// SPDX-License-Identifier: MIT

// Definitions that the parts of the DS's performer share (performance.cpp, performance_recording.cpp and
// performance_loop.cpp): the performer, its state and the helpers that read the driver. The three files build as one
// translation unit. The definitions therefore keep internal linkage. Only those files include this header.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "seqlib/midi/performance.h"
#include "seqlib/midi/recorder.h"
#include "xsflib/binary.h"

#include "ntdlib/commands/sequence.h"
#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/ntr/snd/commands.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sequence_export/ntr/instruments.h"
#include "ntdlib/sequence_export/ntr/performance.h"
#include "ntdlib/sequence_export/passes.h"
#include "ntdlib/sequence_export/performance_base.h"
#include "sequence_export/bend_changes.h"
#include "sequence_export/lengths.h"
#include "sequence_export/ntr/frame_performer.h"
#include "sequence_export/ntr/stands.h"

namespace ntdlib::sequence_export::ntr
{

namespace
{

namespace snd = ntdlib::ntr::snd;
using ntdlib::ntr::ArchivePlayer;
using ntdlib::sequence_export::BendChanges;
using ntdlib::sequence_export::LoopChoice;
using ntdlib::sequence_export::LoopReplay;
using ntdlib::sequence_export::MainLoopPasses;
using ntdlib::sequence_export::PassStart;
using ntdlib::sequence_export::detail::FrameProbe;
using ntdlib::sequence_export::detail::FrameState;
using ntdlib::sequence_export::detail::kFrameLoopRules;
using ntdlib::sequence_export::detail::PendingLoop;
using seqlib::midi::TrackOutput;

// How much longer or shorter a note of a settled pass may last than its twin in the next pass
// (ntdlib::sequence_export::PassTest::kSettled), in the driver's ticks. Where passes repeat, the frames place the ends
// of their notes up to a tick and a half apart.
constexpr double kSettledLengthTicks = 6.0;

// The driver's timebase: 48 ticks a quarter note. A tick takes 240 of the tempo counter.
constexpr uint32_t kTimebase = 48;
constexpr uint32_t kTickCounter = 240;

// A frame of the driver in milliseconds.
constexpr double kFrameMs = 1000.0 * snd::kFrameClocks / ntdlib::ntr::kSystemClockHz;

// The volume that a track starts with (Driver::InitTrack).
constexpr int16_t kStartVolume = 127;

// The driver's values: a channel's envelope stage while it releases (snd::Channel), the target of a pan LFO
// (snd::LfoParameters), and a player's slot without a track (snd::Player).
constexpr uint8_t kReleaseStage = 3;
constexpr uint8_t kPanTarget = 2;
constexpr uint8_t kNoTrack = 0xFF;

// A track's pitch from its bend, in 64ths of a semitone (Driver::UpdateTrackChannels).
int32_t BendUnits(const snd::Track& track)
{
    int32_t pitch = track.pitch_bend;
    pitch *= track.bend_range << 6;
    pitch >>= 7;
    return pitch + track.external_pitch;
}

// A track's pan, from 0 to 127 with 64 at the centre (Driver::UpdateTrackChannels).
int TrackPan(const snd::Track& track)
{
    int32_t pan = track.pan;
    if (track.pan_range != 127)
    {
        pan = (pan * track.pan_range + 0x40) >> 7;
    }
    pan += track.external_pan;
    return std::clamp(64 + pan, 0, 127);
}

// The faders of a track and its player, in 0.1 dB (Driver::UpdateTrackChannels). The player's fader holds the level
// that the SDK gives a sequence as it starts it, with the archive's volume of the sequence (ArchivePlayer). A track's
// fader holds what a game's code sets with NNS_SndPlayerSetTrackVolume.
int32_t TrackFader(const snd::Track& track, const snd::Player& player)
{
    return std::max(track.external_fader + player.external_fader, -0x8000);
}

// The bank of `sequence`. Throws xsflib::FormatError when the sequence is missing from the archive.
uint16_t BankOf(const sdat::Archive& archive, const ntdlib::ntr::SequenceRef& sequence)
{
    if (!sequence.archived)
    {
        if (sequence.index >= archive.sequences.size() || !archive.sequences[sequence.index])
        {
            throw xsflib::FormatError("the archive has no sequence " + std::to_string(sequence.index));
        }
        return archive.sequences[sequence.index]->bank;
    }

    if (sequence.index >= archive.sequence_archives.size() || !archive.sequence_archives[sequence.index])
    {
        throw xsflib::FormatError("the archive has no sequence archive " + std::to_string(sequence.index));
    }
    const sdat::SequenceArchive parsed =
        sdat::SequenceArchive::Parse(archive.File(archive.sequence_archives[sequence.index]->file_id));
    if (*sequence.archived >= parsed.sequences.size())
    {
        throw xsflib::FormatError("sequence archive " + std::to_string(sequence.index) + " has no sequence " +
                                  std::to_string(*sequence.archived));
    }
    return parsed.sequences[*sequence.archived].bank;
}

// A note of the performance on a channel of the driver.
struct Voice
{
    bool sounding = false; // the channel plays the note, or its release
    bool held = false;     // the MIDI note is on: the channel hasn't released
    int track = 0;
    uint8_t part = 0;
    uint8_t key = 0;      // the MIDI note's key. A tie to another key moves the channel's key, and the part's pitch
    uint8_t velocity = 0; // the MIDI note's velocity
    uint32_t note = 0;    // the MIDI note's id
    uint32_t ends = 0;    // the tick where the channel's length runs out, or UINT32_MAX for a channel without one
    int32_t sweep_counter = 0; // the channel's sweep counter at the last frame's end, or at a later note or tie
    PresetKey preset;
    bool lfo_reported = false;     // the change of the track's LFO during the note is counted
    bool pan_lfo_reported = false; // the note's pan LFO on a part with another note is counted
    // The track's expression and the player's volume at the release. The driver keeps a released channel's volume.
    int released_expression = 0;
    int released_volume = 0;

    // For the pass test's slides (Strikes::slides): the tick of the note's strike, the track's bend range then, and the
    // track's bend in 64ths of a semitone when SyncTrack last saw the voice.
    uint32_t struck = 0;
    int strike_range = 0;
    int32_t last_bend = 0;
};

// The command that a track ran last, for the editor's stretches: whether it ran one, the offset after it, and its kind.
struct LastCommand
{
    bool ran = false;
    uint32_t next = 0;
    ntdlib::commands::Kind kind = ntdlib::commands::Kind::kOther;
};

// What the performer records, apart from the player: FrameState, with the performance and what the DS's driver adds. A
// probe copies it and puts it back (Performer::PlayOn).
struct State : FrameState
{
    State(const LoopReplay& replay, double level_scale, const std::array<uint8_t, 16>& part_limits)
        : FrameState(replay, kFrameMs, kFrameMs * kTickCounter / 240.0, level_scale, part_limits)
    {
    }

    Performance out;
    std::array<Voice, snd::kChannelCount> voices{};

    // Each track's commands, by their offset, with the tick in which the track last started each.
    std::array<std::map<uint32_t, uint32_t>, snd::kTracksPerPlayer> runs;

    // The ticks of each track's last loop, 0 for a track that hasn't looped (OnTrackLoop).
    std::array<uint32_t, snd::kTracksPerPlayer> loop_lengths{};

    // For the editor: each track's last command, and the volume and pan that its mix changes gave last.
    std::array<LastCommand, snd::kTracksPerPlayer> last_commands{};
    std::array<std::array<int16_t, 2>, snd::kTracksPerPlayer> mixed{};

    std::optional<uint32_t> last_draw;
    std::size_t next_change = 0; // the first of the game's changes still to come (ChangeVariables)
    std::size_t next_lock = 0;   // the first of the game's locks still to come (LockChannels)
    uint32_t changed_at = 0;     // the tick of the last change
    double changed_ms = 0.0;     // the time of the last change

    // How far each of the game's tick commands has got (SendTickCommands).
    ntdlib::ntr::TickListProgress tick_progress;

    // The bend, in semitones, that each part's last pitch stands for, by track and part. A part that holds one note
    // plays the timer's pitch for its bend (TimerPitch).
    std::map<std::pair<int, uint8_t>, float> part_bends;
    BendChanges bend_changes; // each part's changes of part_bends, for the pass test (PlayOn)

    // For the pass test (stands.h): what CC10, CC11 and CC1 stand for, the last stand recorded for each track, part and
    // controller, and what each strike plays.
    StandLists stands;
    std::map<std::tuple<int, uint8_t, EventKind>, uint8_t> stood;
    Strikes strikes;
};

class Performer
{
public:
    // Multiply CC11 by `level_scale` (LevelPeak). Give each MIDI track at most `part_limits` parts. Make the main
    // loop's first choices as an earlier run made them (`replay`, from LoopChoices). Then choose the rest.
    Performer(const sdat::Archive& archive, const ntdlib::ntr::SequenceRef& sequence, const PerformOptions& options,
              double level_scale, const std::array<uint8_t, 16>& part_limits, const LoopReplay& replay);

    Performance Run();

    // Highest CC11 value before the level scale and MIDI's clamp.
    double LevelPeak() const
    {
        return s_.recorder.LevelPeak();
    }

    // The parts each MIDI track used: one more than the highest.
    const std::array<uint8_t, 16>& PartsUsed() const
    {
        return s_.recorder.PartsUsed();
    }

    // The choices MarkSteadyPass made for the main loop, for a later run to make again.
    const std::vector<LoopChoice>& LoopChoices() const
    {
        return s_.main_loop.Choices();
    }

private:
    const snd::Driver& Driver() const
    {
        return player_.Driver();
    }

    // Track `number` of player 0, or null for none.
    const snd::Track* TrackOf(int number) const;

    // The whole period of the tracks that play, over the owner's last pass (ntdlib::sequence_export::WholeSpan).
    std::size_t WholeSpan() const;

    // --- The driver's hooks ---
    void OnTick();
    void OnNote(const snd::NoteEvent& e);
    void OnCommandStart(int number);
    void OnLoop(int number, uint32_t target);
    void OnTrackLoop(int number, uint32_t target);

    // --- Recording ---
    // Records the editor's stretch of the command that track `number` starts, and the track's volume and pan.
    void RecordCommand(int number, const snd::Track& track);
    void RecordMix(int number, const snd::Track& track);

    // Records the settings of each track after the commands of the last tick.
    void SyncTracks();
    void SyncTrack(int number, const snd::Track& track);
    void SyncTempo();

    // The part's pitch in semitones that `channel`'s voice plays at the frame's end: its bend, its sweep, and the
    // key that a tie has moved it to.
    float VoicePitch(int channel) const;

    // VoicePitch at the rate at which the channel's timer plays it (TimerBend), for a part that plays the voice alone.
    // A pitch of 0 stays 0: the zone's tuning gives the timer's rate there.
    float TimerPitch(int channel) const;

    // The most by which TimerPitch can differ from VoicePitch through the timer's rounding (TimerRounding). 0 where
    // VoicePitch is 0.
    double TimerPitchRounding(int channel) const;

    // The channel of the voice whose LFO moves the pan of track `number`'s part `part`, or -1 for none: the part's one
    // sounding note, while the channel's LFO moves the pan.
    int PanLfoChannel(int number, uint8_t part) const;

    // The pan, from 0 to 127, that CC10 gives `channel`'s voice: the track's pan in the channel, moved as far as the
    // channel's LFO moves the channel's pan.
    int LfoPan(int channel) const;

    // Records `kind` at `value` for track `number`'s part `part`, `after` the last tick, if the part's last value of it
    // is another (Recorder::SendSetting). Returns true if it was recorded.
    bool SendSettingAfter(int number, uint8_t part, EventKind kind, uint8_t value, double after);

    // Whether the last event of `kind` on part `part` of track `number` in the last tick comes after the tick's start:
    // a value that a later frame of the tick sent (SendPanLfos).
    bool SentLaterInTick(int number, uint8_t part, EventKind kind) const;

    // Records that CC10, CC11 or CC1 (`kind`) on part `part` of track `number` stands for `value` from the last tick's
    // start, if the part's last recorded stand of it is another (State::stands).
    void SetStand(int number, uint8_t part, EventKind kind, uint8_t value);

    // Records that the part sends `kind` `after` the last tick, with the stand that it last recorded: the pass test
    // then counts the controller as sent there.
    void MarkStandSent(int number, uint8_t part, EventKind kind, double after);

    // Records CC10, CC11 and CC1 of each part whose pan a channel's LFO moves (PanLfoChannel), `after` the last tick.
    // A release tail keeps the level from its release.
    void SendPanLfos(double after);

    // The sweep that the update of the current frame applies to `channel`, in 64ths of a semitone (Driver::StepSweep).
    // In the frame's ticks, the update is still to come.
    int32_t SweepApplied(int channel) const;

    // Whether a voice of track `number` sounds on part `part`, or one is held there.
    bool PartSounds(int number, uint8_t part) const;
    bool PartHolds(int number, uint8_t part) const;

    // Whether a new note of track `number` has to go to another part than `part`: a held note there bends away from
    // the track's pitch, or the new note sweeps while a note sounds there.
    bool NeedsOtherPart(int number, uint8_t part, bool sweeps) const;

    // Whether part `part` of track `number` plays `bend`, in semitones: its last pitch is `bend`, or the timer's pitch
    // for it (State::part_bends).
    bool PlaysBend(int number, uint8_t part, float bend) const;

    // Records that part `part` of track `number` plays `bend`, in semitones, from `after` the last tick, or its pitch
    // itself without `bend`, with the timer's `rounding` of it (State::part_bends, State::bend_changes).
    void SetPartBend(int number, uint8_t part, std::optional<float> bend, double after, double rounding);

    // Switches the new notes of track `number` to a free part (Recorder::Switch). The new part takes the old part's
    // pitch, and the bend that it stands for.
    bool SwitchPart(int number, TrackOutput& output);

    // Records the note-off of `channel`'s voice, `after` the last tick.
    void EndVoice(int channel, double after);

    // The frame's end: the settings, the voices' releases and ends, and the parts' pitches.
    void EndFrame();

    // Makes the game's changes of variables that come by the frame that runs next (PerformOptions::changes).
    void ChangeVariables();

    // Stops the voice of `channel` at once, as the console stops the channel. CC120 at `after` stops it where it sounds
    // alone on its part. Otherwise MIDI plays its release, or lets its release ring on, and the approximation `what`
    // counts it.
    void CutVoice(int channel, double after, const std::string& what);

    // Sends the game's locks of channels that come by the frame that runs next (PerformOptions::channel_locks), and
    // stops the voices on the channels that a lock takes.
    void LockChannels();

    // Runs the frames of the game's code that start by the frame that runs next, for the game's lists of commands that
    // follow a tick (PerformOptions::tick_commands). Each frame of the game's code after the one that reads the tick
    // sends the next list.
    void SendTickCommands();

    // Whether the game's changes, tick commands and locks have all been made.
    bool GameChangesDone() const;

    // Runs a frame. Returns false once the sequence has ended.
    bool Advance();

    bool AnySounding() const;

    // The search for the main loop starts again after a change of the game's: a change of a variable, a lock or the
    // last of the lists of commands that follow a tick. The passes so far can play otherwise than the passes after
    // it.
    void RestartSearch();

    // --- The main loop ---
    // A copy of the performer's state plays on up to `until`, or until it has played the owner's first `enough` passes
    // to their end where `enough` isn't 0. Returns the function that tests the copy's passes (RepeatsAsTheGame).
    FrameProbe PlayOn(uint32_t until, std::size_t enough);
    bool MarkSteadyPass();
    void DecideLoop();

    // The end of a held sound: kHoldSeconds after the last change, when the pass that the loop stops on starts no
    // note and a note is held.
    std::optional<double> HeldSoundEnd(uint32_t start) const;

    void EndPerformance(bool finished);

    void Approximate(const std::string& what)
    {
        s_.out.approximations[what]++;
    }

    ntdlib::ntr::SequenceRef sequence_;
    PerformOptions options_;
    ArchivePlayer player_;
    State s_;
    bool probing_ = false;
    bool stop_ = false;
    std::optional<uint32_t> stop_tick_;
    std::optional<double> held_sound_end_;
    // By the start and end of a pass that passed the pass test: the strikes that the test left out, whose twins got no
    // channel (Strikes::unstarted).
    std::map<std::pair<uint32_t, uint32_t>, std::size_t> excused_;
};

} // namespace

} // namespace ntdlib::sequence_export::ntr
