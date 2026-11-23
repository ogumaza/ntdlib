// SPDX-License-Identifier: MIT

// Definitions that the parts of DSE's performer share (performance.cpp, performance_recording.cpp and
// performance_loop.cpp): the performer, its state and the helpers that read the driver. The three files build as one
// translation unit. The definitions therefore keep internal linkage. Only those files include this header.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/midi/performance.h"
#include "seqlib/midi/recorder.h"

#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/player.h"
#include "ntdlib/dse/tables.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/sequence_export/dse/instruments.h"
#include "ntdlib/sequence_export/dse/performance.h"
#include "ntdlib/sequence_export/linear_pan.h"
#include "ntdlib/sequence_export/passes.h"
#include "ntdlib/sequence_export/performance_base.h"
#include "sequence_export/bend_changes.h"
#include "sequence_export/lengths.h"
#include "sequence_export/ntr/frame_performer.h"

namespace ntdlib::sequence_export::dse
{

namespace
{

using ntdlib::dse::ChannelView;
using ntdlib::dse::EventStart;
using ntdlib::dse::NoteEvent;
using ntdlib::dse::Player;
using ntdlib::dse::VoiceView;
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

// Leaves out what a performance that ends at tick `end` doesn't reach: the notes that never start, and the cuts.
void DropEventsFrom(Performance& out, uint32_t end)
{
    std::erase_if(out.unstarted, [end](const UnstartedNote& note) { return note.tick >= end; });

    for (NoteVoices& note : out.note_channels)
    {
        if (note.cut_tick >= end)
        {
            note.cut_tick.reset();
        }
    }
}

// How much longer or shorter a note of a settled pass may last than its twin in the next pass
// (ntdlib::sequence_export::PassTest::kSettled): an eighth of the sequence's timebase. That is 6 ticks at 48 a quarter
// note. The driver's ticks place the ends of repeating notes up to a tick apart.
constexpr double kSettledLengthShare = 1.0 / 8.0;

constexpr int kVoices = ntdlib::dse::Driver::kVoices;
constexpr int kMidiTracks = 16;

// An LFO's output that changes the pan (LfoValues::output).
constexpr uint8_t kPanOutput = 3;

// The event that ends a pass of a repeat.
constexpr uint8_t kRepeatEnd = 0x9D;

// The volume and the pan from which the editor's changes count (Performance::mix).
constexpr std::array<int16_t, 2> kStartMix = {127, 0};

// The envelope's full level, and the level below which a release falls silent at the latest. The driver squares the
// level: 12 of 127 plays 41 dB down, and 11 gives every voice a hardware volume of 0 (VoiceView::volume).
constexpr uint8_t kFullLevel = 127;
constexpr uint8_t kInaudibleLevel = 12;

// The highest hardware volume of a voice.
constexpr uint8_t kFullVolume = 127;

// The CC11 that a part would take without its mutes (PartMuted): an event of the performer alone. The pass test
// compares the passes by these values. A mute starts where a voice's hardware volume falls to 0, and in each pass that
// can fall in another driver's tick of the sequence's tick. The values of a fade during a mute would then differ from
// pass to pass, and so would the value at a note that a mute covers in one pass only. The performance leaves the
// events out (Performer::Run).
constexpr seqlib::midi::EventKind kUnmutedExpression = seqlib::midi::ControlKind(3);

// The largest difference in semitones between the timer's pitches of a note's voices that counts as one pitch, and the
// pass test's margin on the timer's departure from a bend: half a cent, for the zones' whole cents.
constexpr double kHalfCent = 0.005;

// The departure of a note's timer from its bend at its start, in semitones, above which the note takes a part to
// itself.
constexpr double kOwnPartDeparture = 0.05;

// A note of the sequence, while a voice plays it: its MIDI note and its voices.
struct MidiNote
{
    bool held = false; // the MIDI note is on
    int track = 0;     // the MIDI track: the sequence's channel
    uint8_t part = 0;
    uint8_t key = 0;       // the MIDI note's key. A legato to another key moves the voices' key, and the part's pitch
    uint32_t id = 0;       // the recorder's id
    uint16_t voices = 0;   // the voices that play it
    uint32_t preset = 0;   // the part's preset at its note-on
    bool own_part = false; // the note takes its part to itself (kOwnPartDeparture)
    bool stopped = false;  // CC120 stopped its part while its voices fell silent (StopSilentParts)
};

// The pitch in semitones that a part sends, the bend that the pitch stands for, and the most by which the two differ.
struct PartSend
{
    float pitch = 0.0f;
    float bend = 0.0f;
    double rounding = 0.0;
};

// The event that a track ran last, for the editor's stretches: whether it ran one, its offset, the offset after it, and
// its code.
struct LastEvent
{
    bool ran = false;
    uint32_t offset = 0;
    uint32_t next = 0;
    uint8_t code = 0;
};

// What the performer records, apart from the player: FrameState, with the performance and what DSE's driver adds. A
// frame of FrameState is a tick of the driver, `frame_ms` long. A probe copies the state and puts it back
// (Performer::PlayOn).
struct State : FrameState
{
    State(const LoopReplay& replay, double frame_ms, double level_scale, const std::array<uint8_t, 16>& part_limits)
        : FrameState(replay, frame_ms, 10.0, level_scale, part_limits)
    {
        mixed.fill(kStartMix);
    }

    Performance out;

    std::map<uint32_t, MidiNote> notes;     // by the driver's number of the note
    std::array<uint32_t, kVoices> voices{}; // the note that each voice plays, or 0
    std::array<bool, kVoices> started{};    // the voice's envelope has run since its note took it
    std::array<uint8_t, kVoices> levels{};  // the voice's envelope level at the end of the last driver's tick
    std::array<uint8_t, kVoices> volumes{}; // the voice's hardware volume then (VoiceView::volume)
    std::array<std::optional<int32_t>, kVoices> release_ticks{}; // the voice's VoiceView::release_ticks then
    std::array<uint8_t, kVoices> exclusive{}; // the key group of the voice's note where it allows one voice, or 0
    std::map<std::pair<int, uint8_t>, int> part_pans; // the oldest build: the pan of the notes that sound on a part
    std::map<int, uint32_t> pass_starts;              // each track's tick at the start of its pass of its loop
    std::map<int, uint32_t> loop_lengths;             // the ticks of each track's last pass of its loop
    BendChanges bend_changes;                         // the bends that each part's pitches stand for, for the pass test
    std::set<int> looped;                             // the tracks that have gone back to their loop's start
    std::set<int> ended;                              // the tracks that have ended
    std::optional<uint32_t> owner_length;             // the length of the owner's passes
    bool reported_channels = false;

    // For the editor: each track's last event, and each channel's changes of its volume and pan, with the values that
    // they gave last.
    std::vector<LastEvent> last_events;
    std::array<std::vector<MixChange>, kMidiTracks> channel_mix{};
    std::array<std::array<int16_t, 2>, kMidiTracks> mixed{};

    double last_note_ms = 0.0; // the time of the last note-on
};

class Performer
{
public:
    // Multiply CC11 by `level_scale` (LevelPeak). Give each MIDI track at most `part_limits` parts. Make the main
    // loop's first choices as an earlier run made them (`replay`, from LoopChoices). Then choose the rest.
    Performer(const SequenceFiles& files, const PerformOptions& options, double level_scale,
              const std::array<uint8_t, 16>& part_limits, const LoopReplay& replay);

    Performance Run();

    double LevelPeak() const
    {
        return s_.recorder.LevelPeak();
    }

    const std::array<uint8_t, 16>& PartsUsed() const
    {
        return s_.recorder.PartsUsed();
    }

    const std::vector<LoopChoice>& LoopChoices() const
    {
        return s_.main_loop.Choices();
    }

private:
    const ntdlib::dse::Driver& Driver() const
    {
        return player_->Driver();
    }

    // --- The driver's hooks ---
    void OnTick();
    void OnEvent(const EventStart& e);
    void OnNote(const NoteEvent& e);
    void OnRelease(int voice);
    void OnLoopStart(int track);
    void OnLoop(int track);

    // --- Recording ---
    void SyncTempo();
    void SyncChannels(double after);
    void SyncChannel(int channel, double after);

    // Records the editor's event of MIDI note `id` (`e`), and channel `number`'s volume and pan.
    void RecordNote(uint32_t id, const NoteEvent& e);
    void RecordMix(int number, const ChannelView& channel);

    // The pitch in semitones that note `number` plays at against its MIDI note: the channel's bend, the voice's key
    // bend and jitter, and the key that a legato has moved it to.
    float NotePitch(uint32_t number) const;

    // The pitch of a new note's voice `voice` in semitones (NotePitch).
    float VoicePitch(int voice, uint8_t key) const;

    // Whether a note of MIDI track `track` sounds on part `part`.
    bool PartSounds(int track, uint8_t part) const;

    // Whether a note that takes its part to itself sounds on part `part` of MIDI track `track` (MidiNote::own_part).
    bool PartOwned(int track, uint8_t part) const;

    // The hardware pan of channel `track`'s pan (dse::tables::kPanCurve).
    int ChannelPan(int track) const;

    // The pan that part `part` of channel `track` plays: `channel_pan`, or in the oldest build, the pan that the notes
    // sounding on the part started with. The oldest build's writer never pans a voice again once it has started it
    // (0x020C4DDC in Luminous Arc's ARM9 binary).
    int PartPan(int track, uint8_t part, int channel_pan) const;

    // Whether voice `voice` can be heard: its envelope hasn't run since its note took it, or its hardware volume wasn't
    // 0 at the end of the last driver's tick. For a voice that the driver hasn't updated yet, that is the first
    // update's volume (VoiceView::volume). The first update comes in the note's driver's tick in some passes and in the
    // next tick in others.
    bool VoiceAudible(int voice) const;

    // Whether a note of MIDI track `track` that can be heard sounds on part `part`: a voice of the note is audible
    // (VoiceAudible).
    bool PartAudible(int track, uint8_t part) const;

    // Whether a note is held on part `part` of MIDI track `track`.
    bool PartHolds(int track, uint8_t part) const;

    // Whether a note is held on part `part` of MIDI track `track`, and none of the part's notes can be heard. The part
    // then takes a CC11 of 0.
    bool PartMuted(int track, uint8_t part) const;

    // Stops each part whose notes are all released and none of them can be heard, with CC120 `after` the last tick.
    // The notes stay until their voices end: the parts that later notes take don't depend on the driver's tick in
    // which a release falls silent.
    void StopSilentParts(double after);

    // The newest note that sounds on the part, or the newest that is held there where `held`. 0 for none.
    uint32_t NewestOn(int track, uint8_t part, bool held) const;

    // The part's pitch in semitones: that of its newest held note, else of its newest note, else the channel's bend.
    float PartPitch(int track, uint8_t part) const;

    // The pitch in semitones at which voice `voice` plays MIDI key `key` through its zone at its timer's rate
    // (TimerBend), or nothing where the timer gives no rate. A pitch of 0 stays 0: the zone's tuning gives the timer's
    // rate there.
    std::optional<double> VoiceTimerPitch(int voice, uint8_t key) const;

    // The timer's pitch of note `number` (VoiceTimerPitch) for MIDI key `key`, where `voices` play it: nothing where
    // the voices' pitches differ by more than kHalfCent, or where one has none.
    std::optional<double> TimerPitch(uint16_t voices, uint32_t number, uint8_t key) const;

    // The pitch that a part sends for note `note` alone, numbered `number`, from its pitch `plain` (NotePitch): the
    // timer's pitch, or `plain` where the note has none. The timers' rates span 117 semitones at most, within MIDI's
    // bend range of 127.
    PartSend AlonePitch(uint32_t number, const MidiNote& note, float plain) const;

    // The pitch that part `part` of track `track` sends: AlonePitch where one note sounds on it, else PartPitch.
    // Nothing where no note sounds on the part and its last pitch stands for the channel's bend already.
    std::optional<PartSend> PartPitchToSend(int track, uint8_t part) const;

    // Records that part `part` of track `track` sends a pitch for `send.bend`, from `after` the last tick, if it
    // differs from the part's last record (State::bend_changes).
    void SetPartBend(int track, uint8_t part, const PartSend& send, double after);

    // The voice `voice` no longer plays its note: the note ends when it has no other voice. A note that ends stops its
    // part where nothing else there sounds or is held. Returns a cut note where it rings on in its release instead.
    std::optional<MidiNote> LeaveVoice(int voice, bool cut, double after);

    // The end of the driver's tick: the settings and the voices' ends.
    void EndFrame();

    // Runs a tick of the driver. Returns false once the sequence has ended.
    bool Advance();

    bool AnySounding() const;

    // --- The main loop ---
    // A copy of the performer's state plays on up to `until`, or until it has played the owner's first `enough` passes
    // to their end where `enough` isn't 0. Returns the test of the copy's passes (RepeatsAsTheGame).
    FrameProbe PlayOn(uint32_t until, std::size_t enough);
    bool RestartAtTrackEnds();
    bool EveryTrackLooped() const;

    // The whole period of the tracks that play, over the owner's last pass (ntdlib::sequence_export::WholeSpan).
    std::size_t WholeSpan() const;
    bool MarkSteadyPass();
    bool HoldsSound(uint32_t start) const;
    void DecideLoop();
    std::optional<double> HeldSoundEnd(uint32_t start) const;
    void EndPerformance(bool finished);

    void Approximate(const std::string& what)
    {
        s_.out.approximations[what]++;
    }

    SequenceFiles files_;
    std::vector<uint8_t> track_channels_; // the channel of each of the sequence's tracks, by the tracks' slots
    PerformOptions options_;
    std::unique_ptr<Player> player_;

    // The driver's nominal tick in milliseconds, for which a sequence's tempo counts. State::frame_ms holds the
    // driver's tick.
    double nominal_tick_ms_ = 0.0;
    State s_;
    bool stop_ = false;
    std::optional<uint32_t> stop_tick_;
    std::optional<double> held_sound_end_;
};

} // namespace

} // namespace ntdlib::sequence_export::dse
