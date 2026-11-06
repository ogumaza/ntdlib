// SPDX-License-Identifier: MIT

// The DS's performer (see performance.h). An ArchivePlayer plays the sequence on the driver's player 0. The performer
// takes the driver's notes through the player's hooks, and reads the tracks' settings and the channels' states from
// the driver. It records each tick's settings when the next tick starts, and when the frame ends. A note's channel
// ends its MIDI note when the channel releases or stops.
//
// The driver runs a player's ticks at the start of its frames. A player's tempo adds to a counter each frame, and a
// tick runs each time the counter passes 240. MIDI's tempo puts each tick at its nominal time, up to a frame before the
// frame that runs it. The events of a tick go at the start of that frame (Performance::frame_ticks). The changes that
// the end of a frame makes, such as a note's release or a sweep's pitch, go at the start of their frame too
// (TrackEvent::after).

#include "ntdlib/sequence_export/ntr/performance.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
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
#include "ntdlib/sequence_export/passes.h"
#include "ntdlib/sequence_export/performance_base.h"
#include "sequence_export/lengths.h"

namespace ntdlib::sequence_export::ntr
{

namespace
{

namespace snd = ntdlib::ntr::snd;
using ntdlib::ntr::ArchivePlayer;
using ntdlib::sequence_export::FitParts;
using ntdlib::sequence_export::LevelScale;
using ntdlib::sequence_export::LoopChoice;
using ntdlib::sequence_export::MainLoopPasses;
using ntdlib::sequence_export::PassStart;
using ntdlib::sequence_export::SpareChannels;
using seqlib::midi::TrackOutput;

// How the recorder places events (seqlib/midi/recorder.h). A setting replaces an earlier value of its kind anywhere in
// its tick. A part switch sends the new part the old part's values. No tick's frame starts before tick 0.
constexpr seqlib::midi::RecorderOptions kRecorderOptions{
    .replace_anywhere_in_tick = true, .resend_on_switch = true, .frame_ticks_from_zero = true};

// The rules of the main loop's passes (ntdlib/sequence_export/passes.h). The DS's notes take no settings that MIDI
// seeds.
const ntdlib::sequence_export::LoopRules kLoopRules{{}, false};

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

// The bank of `sequence`. Throws xsflib::FormatError when the archive lacks the sequence.
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
    int32_t sweep_counter = 0; // the channel's sweep counter at the last frame's end
    PresetKey preset;
    bool lfo_reported = false; // the change of the track's LFO during the note is counted
};

// The command that a track ran last, for the editor's stretches: whether it ran one, the offset after it, and its kind.
struct LastCommand
{
    bool ran = false;
    uint32_t next = 0;
    ntdlib::commands::Kind kind = ntdlib::commands::Kind::kOther;
};

// A pass of the main loop that waits for the frame's end: the track, and the ticks it started and ended in.
struct PendingLoop
{
    int track = 0;
    uint32_t start = 0;
    uint32_t end = 0;
};

// What the performer records, apart from the player. A probe copies it and puts it back (Performer::PlayOn).
struct State
{
    explicit State(const std::vector<LoopChoice>& loop_choices) : main_loop(loop_choices)
    {
    }

    Performance out;
    seqlib::midi::Recorder recorder{kRecorderOptions};
    std::array<TrackOutput, snd::kTracksPerPlayer> outputs{};
    std::array<Voice, snd::kChannelCount> voices{};

    // Each track's commands, by their offset, with the tick in which the track last started each.
    std::array<std::map<uint32_t, uint32_t>, snd::kTracksPerPlayer> runs;

    // For the editor: each track's last command, and the volume and pan that its mix changes gave last.
    std::array<LastCommand, snd::kTracksPerPlayer> last_commands{};
    std::array<std::array<int16_t, 2>, snd::kTracksPerPlayer> mixed{};

    MainLoopPasses main_loop;
    std::optional<PendingLoop> pending;
    std::optional<uint32_t> last_draw;
    std::size_t next_change = 0; // the first of the game's changes still to come (ChangeVariables)
    uint32_t changed_at = 0;     // the tick of the last change
    double changed_ms = 0.0;     // the time of the last change

    bool ticked = false;      // the player has run its first tick
    uint32_t tick = 0;        // the last tick
    uint64_t frame = 0;       // the frame that runs, or the frames run between frames
    uint64_t first_frame = 0; // the frame of tick 0
    uint64_t tick_frame = 0;  // the frame of the last tick
    double nominal_ms = 0.0;  // the last tick's time at MIDI's tempo
    double ms_per_tick = kFrameMs * kTickCounter / 240.0;
    std::vector<double> tick_ms; // each tick's nominal time
    uint64_t last_sound_frame = 0;
    double last_change_ms = 0.0; // the time of the last note-on or setting
};

class Performer
{
public:
    // Multiply CC11 by `level_scale` (LevelPeak). Give each MIDI track at most `part_limits` parts. Make the main
    // loop's first choices as an earlier run made them (`loop_choices`, from LoopChoices). Then choose the rest.
    Performer(const sdat::Archive& archive, const ntdlib::ntr::SequenceRef& sequence, const PerformOptions& options,
              double level_scale, const std::array<uint8_t, 16>& part_limits,
              const std::vector<LoopChoice>& loop_choices);

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

    // --- The driver's hooks ---
    void OnTick();
    void OnNote(const snd::NoteEvent& e);
    void OnCommandStart(int number);
    void OnLoop(int number, uint32_t target);

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

    // The sweep that `channel`'s last update applied, in 64ths of a semitone (Driver::StepSweep).
    int32_t SweepApplied(int channel) const;

    // Whether a voice of track `number` sounds on part `part`, or one is held there.
    bool PartSounds(int number, uint8_t part) const;
    bool PartHolds(int number, uint8_t part) const;

    // Whether a new note of track `number` has to go to another part than `part`: a held note there bends away from
    // the track's pitch, or the new note sweeps while a note sounds there.
    bool NeedsOtherPart(int number, uint8_t part, bool sweeps) const;

    // Records the note-off of `channel`'s voice, `after` the last tick.
    void EndVoice(int channel, double after);

    // The frame's end: the settings, the voices' releases and ends, and the parts' pitches.
    void EndFrame();

    // Makes the game's changes of variables that come by the frame that runs next (PerformOptions::changes).
    void ChangeVariables();

    // Runs a frame. Returns false once the sequence has ended.
    bool Advance();

    bool AnySounding() const;
    double FrameMs(uint64_t frame) const;
    double TickMs(uint32_t tick) const;

    // The first tick at or after `ms`.
    uint32_t TickAt(double ms) const;

    // How far after the last tick the frame that has run starts, in ticks at MIDI's tempo.
    double FrameAfter() const;

    // --- The main loop ---
    // A copy of the performer's state plays on up to `until`. Returns the function that tests the copy's passes
    // (RepeatsAsTheGame).
    auto PlayOn(uint32_t until);
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
};

Performer::Performer(const sdat::Archive& archive, const ntdlib::ntr::SequenceRef& sequence,
                     const PerformOptions& options, double level_scale, const std::array<uint8_t, 16>& part_limits,
                     const std::vector<LoopChoice>& loop_choices)
    : sequence_(sequence),
      options_(options),
      player_(archive, {.build = options.build, .random_state = options.seed}),
      s_(loop_choices)
{
    for (std::array<int16_t, 2>& mixed : s_.mixed)
    {
        mixed = {kStartVolume, 0};
    }
    s_.recorder.SetLevelScale(level_scale);
    s_.recorder.SetPartLimits(part_limits);
    for (std::size_t t = 0; t < s_.outputs.size(); t++)
    {
        s_.outputs[t].midi_track = static_cast<uint8_t>(t);
    }
    s_.out.timebase = kTimebase;
    s_.out.bank = BankOf(archive, sequence);

    player_.on_tick = [this](int player)
    {
        if (player == 0)
        {
            OnTick();
        }
    };
    player_.on_note = [this](const snd::NoteEvent& e)
    {
        if (e.player == 0)
        {
            OnNote(e);
        }
    };
    snd::LengthHooks hooks;
    hooks.on_loop = [this](int player, int track, uint32_t target)
    {
        if (player == 0)
        {
            OnLoop(track, target);
        }
    };
    hooks.on_command_start = [this](int player, int track)
    {
        if (player == 0)
        {
            OnCommandStart(track);
        }
    };
    hooks.on_draw = [this](int player, int)
    {
        if (player == 0)
        {
            s_.last_draw = s_.tick;
        }
    };
    player_.FollowLengths(std::move(hooks));
}

const snd::Track* Performer::TrackOf(int number) const
{
    const uint8_t index = Driver().GetPlayer(0).tracks[static_cast<std::size_t>(number)];
    return index == kNoTrack ? nullptr : &Driver().GetTrack(index);
}

// --- The driver's hooks ---

void Performer::OnTick()
{
    if (!s_.ticked)
    {
        s_.ticked = true;
        s_.first_frame = s_.frame;
        s_.tick_frame = s_.frame;
        s_.tick_ms.push_back(0.0);
        s_.out.frame_ticks.push_back(0.0);
        SyncTempo();
        return;
    }

    // The settings of the last tick, then the next tick at MIDI's tempo.
    SyncTracks();
    s_.tick++;
    s_.tick_frame = s_.frame;
    s_.nominal_ms += s_.ms_per_tick;
    s_.tick_ms.push_back(s_.nominal_ms);
    const double start = static_cast<double>(s_.tick) + (FrameMs(s_.frame) - s_.nominal_ms) / s_.ms_per_tick;
    s_.out.frame_ticks.push_back(std::max(start, s_.out.frame_ticks.back()));
}

void Performer::OnCommandStart(int number)
{
    if (const snd::Track* track = TrackOf(number))
    {
        s_.runs[static_cast<std::size_t>(number)][track->position - track->base] = s_.tick;
        RecordCommand(number, *track);
    }
}

void Performer::OnLoop(int number, uint32_t target)
{
    // The pass started when the track last ran `target`. A pass within one tick waited for nothing.
    const std::map<uint32_t, uint32_t>& runs = s_.runs[static_cast<std::size_t>(number)];
    const auto run = runs.find(target);
    if (run == runs.end() || run->second >= s_.tick)
    {
        return;
    }

    // The driver gives the passes that changed the sound, of the track that owns the sequence's loop. A pass of
    // another loop than the marked one moves the markers to it.
    const uint32_t start = run->second;
    if (start < s_.changed_at)
    {
        return;
    }

    const std::pair<int, uint32_t> owner{number, target};
    if (!s_.main_loop.Owner() || *s_.main_loop.Owner() != owner)
    {
        s_.main_loop.Restart(s_.out, owner);
    }

    s_.main_loop.CountPass(s_.out, owner, start, s_.tick);
    if (s_.out.loops >= options_.loops && TickMs(s_.tick) >= kMinLoopSeconds * 1000.0)
    {
        s_.pending = PendingLoop{number, start, s_.tick};
    }
}

void Performer::OnNote(const snd::NoteEvent& e)
{
    if (!s_.ticked)
    {
        return;
    }

    const snd::Track* track = TrackOf(e.track);
    if (track == nullptr)
    {
        return;
    }

    // A note that gets no channel plays nothing on the console. It gets no MIDI note. So does a note whose program
    // plays nothing at its key.
    if (e.channel < 0)
    {
        s_.out.unstarted.push_back({.tick = s_.tick,
                                    .length = e.length < 0 ? 0 : static_cast<uint32_t>(e.length),
                                    .track = static_cast<uint8_t>(e.track),
                                    .key = e.key,
                                    .velocity = e.velocity,
                                    .offset = e.offset});
        return;
    }

    const auto c = static_cast<std::size_t>(e.channel);
    Voice& v = s_.voices[c];
    TrackOutput& output = s_.outputs[static_cast<std::size_t>(e.track)];

    // A tie goes on in the note's channel. A tie to another key moves the part's pitch (VoicePitch). The channel keeps
    // the velocity of the note that it started with.
    if (e.tie && v.sounding && v.held && v.track == e.track)
    {
        const snd::Channel& channel = Driver().GetChannel(e.channel);
        if (channel.key != v.key)
        {
            for (std::size_t other = 0; other < s_.voices.size(); other++)
            {
                const Voice& o = s_.voices[other];
                if (other != c && o.sounding && o.track == v.track && o.part == v.part)
                {
                    Approximate("tie to another key on a part with other notes (they follow its pitch)");
                    break;
                }
            }
        }
        if (e.velocity != v.velocity)
        {
            Approximate("tie at another velocity (keeping the first note's)");
        }
        return;
    }

    // The channel's last note stops at once on the console. CC120 stops it where it sounds alone on its part. Otherwise
    // MIDI plays its release, or lets its release ring on.
    if (v.sounding)
    {
        if (v.held && s_.tick < v.ends && v.note >= 1 && v.note <= s_.out.note_channels.size())
        {
            s_.out.note_channels[v.note - 1].cut_tick = s_.tick;
        }
        EndVoice(e.channel, 0.0);
        v.sounding = false;
        if (PartSounds(v.track, v.part))
        {
            Approximate("note cut short by another note's channel (rings on in its release)");
        }
        else
        {
            s_.recorder.Emit(s_.out, s_.outputs[static_cast<std::size_t>(v.track)], s_.tick,
                             {.kind = EventKind::kSoundOff}, v.part);
        }
    }

    // The settings that the tick's commands have changed so far, then a part for the note.
    SyncTrack(e.track, *track);
    const int32_t sweep = SweepApplied(e.channel);
    if (NeedsOtherPart(e.track, output.part, sweep != 0) &&
        !s_.recorder.Switch(s_.out, output, s_.tick, [this, &e](uint8_t part) { return PartSounds(e.track, part); }))
    {
        Approximate("note on a part whose pitch another note moves (no MIDI channel free)");
    }

    PresetKey key;
    key.program = track->program;
    key.attack = track->attack;
    key.decay = track->decay;
    key.sustain = track->sustain;
    key.release = track->release;
    key.lfo_target = track->modulation.target;
    key.lfo_speed = track->modulation.speed;
    key.lfo_range = track->modulation.range;
    key.lfo_delay = track->modulation.delay;
    if (key.lfo_target == kPanTarget && track->modulation.depth != 0)
    {
        Approximate("pan LFO (omitted from SoundFont)");
    }

    const uint32_t preset = seqlib::midi::PresetIndex(s_.out.presets, key);
    s_.recorder.SendPreset(s_.out, output, s_.tick, preset);
    s_.recorder.SendPitch(s_.out, output, s_.tick, static_cast<float>(BendUnits(*track) + sweep) / 64.0f);

    // A tie's channel and the channel of an instrument whose release is 0xFF have no length (Driver::NoteOn). The note
    // then plays past its command's length.
    const auto velocity = static_cast<uint8_t>(std::clamp<int>(e.velocity, 1, 127));
    const int32_t length = Driver().GetChannel(e.channel).length;
    v = Voice{.sounding = true,
              .held = true,
              .track = e.track,
              .part = output.part,
              .key = e.key,
              .velocity = e.velocity,
              .note = s_.recorder.NoteOn(s_.out, output, s_.tick, e.key, velocity),
              .ends = length < 0 ? UINT32_MAX : s_.tick + static_cast<uint32_t>(length),
              .sweep_counter = Driver().GetChannel(e.channel).sweep_counter,
              .preset = key};
    s_.last_change_ms = TickMs(s_.tick);

    // The note command ran in the track's last stretch.
    std::vector<NoteCommand>& commands = s_.out.note_commands;
    if (commands.size() < v.note)
    {
        commands.resize(v.note);
        s_.out.note_channels.resize(v.note);
    }
    const std::vector<Stretch>& stretches = s_.out.stretches[static_cast<std::size_t>(e.track)];
    commands[v.note - 1] = {e.offset, stretches.empty() ? 0 : static_cast<uint32_t>(stretches.size() - 1)};
    s_.out.note_channels[v.note - 1] = {.channel = static_cast<int16_t>(e.channel)};
}

// --- Recording ---

void Performer::RecordCommand(int number, const snd::Track& track)
{
    // A command that follows the one before it goes on with the stretch.
    const auto t = static_cast<std::size_t>(number);
    const uint32_t offset = track.position - track.base;
    LastCommand& last = s_.last_commands[t];
    std::vector<Stretch>& stretches = s_.out.stretches[t];
    if (last.ran && offset == last.next && !stretches.empty())
    {
        stretches.back().last = offset;
    }
    else
    {
        Stretch stretch;
        stretch.tick = s_.tick;
        stretch.first = offset;
        stretch.last = offset;
        stretch.depth = track.call_depth;
        stretch.note_wait = track.note_wait;
        if (last.ran)
        {
            switch (last.kind)
            {
            case ntdlib::commands::Kind::kCall:
                stretch.entry = Entry::kCall;
                break;
            case ntdlib::commands::Kind::kReturn:
                stretch.entry = Entry::kReturn;
                break;
            case ntdlib::commands::Kind::kLoopEnd:
                stretch.entry = Entry::kLoop;
                break;
            default:
                stretch.entry = Entry::kJump;
                break;
            }
        }
        stretches.push_back(stretch);
    }

    // The command as the driver reads it, from main memory.
    const std::span<const uint8_t> memory = player_.MainMemory();
    const auto byte_at = [memory](uint32_t address)
    {
        return memory[(address - ArchivePlayer::kMainMemory) % memory.size()];
    };
    const snd::PlayedCommand played = snd::ReadPlayedCommand(byte_at, track.position);
    last = {true, played.next - track.base, played.command.kind};
}

void Performer::RecordMix(int number, const snd::Track& track)
{
    const auto t = static_cast<std::size_t>(number);
    const std::array<std::pair<Mix, int16_t>, 2> values = {
        {{Mix::kVolume, static_cast<int16_t>(track.volume)}, {Mix::kPan, static_cast<int16_t>(track.pan)}}};
    std::vector<MixChange>& changes = s_.out.mix[t];
    for (std::size_t i = 0; i < values.size(); i++)
    {
        const auto [setting, value] = values[i];
        if (value == s_.mixed[t][i])
        {
            continue;
        }

        s_.mixed[t][i] = value;
        const auto same = std::find_if(changes.rbegin(), changes.rend(), [&](const MixChange& change)
                                       { return change.tick == s_.tick && change.setting == setting; });
        if (same != changes.rend())
        {
            same->value = value;
        }
        else
        {
            changes.push_back({s_.tick, setting, value});
        }
    }
}

void Performer::SyncTempo()
{
    const snd::Player& player = Driver().GetPlayer(0);
    const uint32_t step = (static_cast<uint32_t>(player.tempo) * player.tempo_ratio) >> 8;
    if (step == 0)
    {
        return;
    }

    s_.ms_per_tick = kFrameMs * kTickCounter / step;
    s_.recorder.SetTempo(s_.out, s_.tick, 60000.0 / s_.ms_per_tick / kTimebase);
}

void Performer::SyncTracks()
{
    SyncTempo();
    for (int number = 0; number < snd::kTracksPerPlayer; number++)
    {
        if (const snd::Track* track = TrackOf(number); track != nullptr && track->active)
        {
            SyncTrack(number, *track);
            RecordMix(number, *track);
        }
    }
}

void Performer::SyncTrack(int number, const snd::Track& track)
{
    using seqlib::midi::EventKind;
    TrackOutput& output = s_.outputs[static_cast<std::size_t>(number)];
    const snd::Player& player = Driver().GetPlayer(0);

    // CC7 and CC11 take the square of their value. So do the driver's volumes (SND_CalcDecibelSquare). CC11 holds the
    // expression, the player's volume, the faders and the level of the track's pan (PanGain). A fader of f gives the
    // channels f / 10 dB, and CC11 the square root of that gain. The zones hold the rest of the pan's level. CC10 holds
    // the pan's position.
    const int pan = TrackPan(track);
    const double level = std::min<int>(track.expression, 127) * std::min<int>(player.volume, 127) / 127.0 *
                         std::sqrt(PanGain(pan)) * std::pow(10.0, TrackFader(track, player) / 400.0);
    s_.recorder.RaiseLevelPeak(level);
    if (track.volume > 127 || track.expression > 127 || player.volume > 127)
    {
        Approximate("volume above 127 (played at 127)");
    }
    if (track.modulation.depth > 127)
    {
        Approximate("LFO depth above 127 (CC1 holds 127)");
    }

    // A game's reverb gives every part a reverb send of the reverb's volume.
    const std::array<std::pair<EventKind, uint8_t>, 5> all_settings = {{
        {EventKind::kVolume, static_cast<uint8_t>(std::min<int>(track.volume, 127))},
        {EventKind::kExpression, s_.recorder.Expression(level)},
        {EventKind::kPan, static_cast<uint8_t>(std::clamp<long>(std::lround(64.0 + 64.0 * PanPosition(pan)), 0, 127))},
        {EventKind::kModulation, static_cast<uint8_t>(std::min<int>(track.modulation.depth, 127))},
        {EventKind::kReverb, static_cast<uint8_t>(options_.reverb ? std::clamp(options_.reverb->volume, 0, 127) : 0)},
    }};
    const std::span settings = std::span(all_settings).first(options_.reverb ? 5 : 4);
    const float bend = static_cast<float>(BendUnits(track)) / 64.0f;
    const auto changes = [&](uint8_t part)
    {
        for (const auto& [kind, value] : settings)
        {
            const auto sent = output.sent.find({part, kind});
            if (sent == output.sent.end() || sent->second != value)
            {
                return true;
            }
        }
        const auto pitch = output.pitch.find(part);
        return !PartHolds(number, part) && (pitch == output.pitch.end() ? bend != 0.0f : pitch->second != bend);
    };

    // The channels that release keep their settings. A part with only release tails gives way to a free part before a
    // change. A part that also holds a note can't.
    if (PartSounds(number, output.part) && !PartHolds(number, output.part) && changes(output.part))
    {
        s_.recorder.Switch(s_.out, output, s_.tick, [this, number](uint8_t part) { return PartSounds(number, part); });
    }

    // The part that new notes go to, and every part that holds a note of the track.
    std::set<uint8_t> parts = {output.part};
    for (const Voice& v : s_.voices)
    {
        if (v.sounding && v.held && v.track == number)
        {
            parts.insert(v.part);
        }
    }

    bool tails_follow = false;
    for (const uint8_t part : parts)
    {
        bool changed = false;
        for (const auto& [kind, value] : settings)
        {
            changed = s_.recorder.SendSetting(s_.out, output, s_.tick, kind, value, part) || changed;
        }
        if (!PartHolds(number, part))
        {
            changed = s_.recorder.SendPitch(s_.out, output, s_.tick, bend, part) || changed;
        }
        if (changed)
        {
            s_.last_change_ms = TickMs(s_.tick);
            for (const Voice& v : s_.voices)
            {
                tails_follow = tails_follow || (v.sounding && !v.held && v.track == number && v.part == part);
            }
        }
    }
    if (tails_follow)
    {
        Approximate("release tail follows its track's later settings");
    }

    // A channel takes the track's LFO every frame. The note's preset keeps the LFO's speed, range and delay at its
    // start.
    for (Voice& v : s_.voices)
    {
        if (v.sounding && v.held && v.track == number && !v.lfo_reported && track.modulation.depth != 0 &&
            (v.preset.lfo_target != track.modulation.target || v.preset.lfo_speed != track.modulation.speed ||
             v.preset.lfo_range != track.modulation.range || v.preset.lfo_delay != track.modulation.delay))
        {
            v.lfo_reported = true;
            Approximate("LFO parameters changed during a note (using its note-on settings)");
        }
    }
}

int32_t Performer::SweepApplied(int channel) const
{
    const snd::Channel& ch = Driver().GetChannel(channel);
    const Voice& v = s_.voices[static_cast<std::size_t>(channel)];
    if (ch.sweep_pitch == 0)
    {
        return 0;
    }

    // An automatic sweep steps its counter after each frame's update. Its counter has moved on since the last frame's
    // end only when the update applied the sweep. A sweep over the note's length steps in the ticks, before the update.
    int32_t counter = ch.sweep_counter;
    if (ch.auto_sweep && v.sounding && v.held)
    {
        if (counter == v.sweep_counter)
        {
            return 0;
        }
        counter--;
    }
    if (counter >= ch.sweep_length)
    {
        return 0;
    }

    return static_cast<int32_t>(static_cast<int64_t>(ch.sweep_pitch) * (ch.sweep_length - counter) / ch.sweep_length);
}

float Performer::VoicePitch(int channel) const
{
    const snd::Channel& ch = Driver().GetChannel(channel);
    const Voice& v = s_.voices[static_cast<std::size_t>(channel)];
    return static_cast<float>(ch.user_pitch + SweepApplied(channel)) / 64.0f + static_cast<float>(ch.key - v.key);
}

bool Performer::PartSounds(int number, uint8_t part) const
{
    return std::ranges::any_of(s_.voices,
                               [&](const Voice& v) { return v.sounding && v.track == number && v.part == part; });
}

bool Performer::PartHolds(int number, uint8_t part) const
{
    return std::ranges::any_of(
        s_.voices, [&](const Voice& v) { return v.sounding && v.held && v.track == number && v.part == part; });
}

bool Performer::NeedsOtherPart(int number, uint8_t part, bool sweeps) const
{
    const snd::Track* track = TrackOf(number);
    const float bend = track != nullptr ? static_cast<float>(BendUnits(*track)) / 64.0f : 0.0f;
    for (std::size_t c = 0; c < s_.voices.size(); c++)
    {
        const Voice& v = s_.voices[c];
        if (!v.sounding || v.track != number || v.part != part)
        {
            continue;
        }
        if (sweeps || (v.held && VoicePitch(static_cast<int>(c)) != bend))
        {
            return true;
        }
    }

    return false;
}

void Performer::EndVoice(int channel, double after)
{
    Voice& v = s_.voices[static_cast<std::size_t>(channel)];
    if (v.held)
    {
        s_.recorder.NoteOff(s_.out, static_cast<uint8_t>(v.track), s_.tick, v.part, v.key, v.note, after);
        v.held = false;
    }
}

void Performer::EndFrame()
{
    if (!s_.ticked)
    {
        s_.frame++;
        return;
    }

    SyncTracks();

    // A channel that releases ends its MIDI note. A channel that stops ends its voice. The release comes at the end of
    // the frame whose ticks end the note's length.
    const double after = FrameAfter();
    for (std::size_t c = 0; c < s_.voices.size(); c++)
    {
        Voice& v = s_.voices[c];
        if (!v.sounding)
        {
            continue;
        }

        const snd::Channel& ch = Driver().GetChannel(static_cast<int>(c));
        if (!ch.active || ch.envelope_stage == kReleaseStage)
        {
            EndVoice(static_cast<int>(c), after);
        }
        v.sounding = ch.active;
    }

    // Each part that holds a note takes the pitch of its newest held note.
    std::map<std::pair<int, uint8_t>, std::size_t> newest;
    for (std::size_t c = 0; c < s_.voices.size(); c++)
    {
        const Voice& v = s_.voices[c];
        if (!v.sounding || !v.held)
        {
            continue;
        }
        const auto [it, added] = newest.try_emplace({v.track, v.part}, c);
        if (!added && s_.voices[it->second].note < v.note)
        {
            it->second = c;
        }
    }
    for (const auto& [place, c] : newest)
    {
        TrackOutput& output = s_.outputs[static_cast<std::size_t>(place.first)];
        if (s_.recorder.SendPitch(s_.out, output, s_.tick, VoicePitch(static_cast<int>(c)), place.second, after))
        {
            s_.last_change_ms = FrameMs(s_.frame);
        }
    }

    for (std::size_t c = 0; c < s_.voices.size(); c++)
    {
        s_.voices[c].sweep_counter = Driver().GetChannel(static_cast<int>(c)).sweep_counter;
    }
    if (AnySounding())
    {
        s_.last_sound_frame = s_.frame;
    }
    s_.frame++;
}

void Performer::ChangeVariables()
{
    const std::vector<VariableChange>& changes = options_.changes;
    if (s_.next_change == changes.size() || changes[s_.next_change].frame > s_.frame)
    {
        return;
    }

    ntdlib::ntr::Variables variables;
    for (; s_.next_change < changes.size() && changes[s_.next_change].frame <= s_.frame; s_.next_change++)
    {
        const VariableChange& change = changes[s_.next_change];
        if (change.index < 16)
        {
            variables.local[change.index] = change.value;
        }
        else if (change.index < 32)
        {
            variables.global[change.index - 16u] = change.value;
        }
    }
    player_.Send(ntdlib::ntr::VariableCommands(variables));

    // The passes so far can play otherwise than the passes after the change. The search for the main loop starts again
    // with the first pass that starts after it (OnLoop).
    s_.changed_at = s_.tick;
    s_.changed_ms = FrameMs(s_.frame);
    s_.main_loop.Restart(s_.out, std::nullopt);
}

bool Performer::Advance()
{
    ChangeVariables();
    player_.RunFrame();
    EndFrame();
    return Driver().GetPlayer(0).active;
}

bool Performer::AnySounding() const
{
    for (int c = 0; c < snd::kChannelCount; c++)
    {
        if (Driver().GetChannel(c).active)
        {
            return true;
        }
    }

    return false;
}

double Performer::FrameMs(uint64_t frame) const
{
    return static_cast<double>(frame) * kFrameMs - static_cast<double>(s_.first_frame) * kFrameMs;
}

double Performer::TickMs(uint32_t tick) const
{
    if (tick < s_.tick_ms.size())
    {
        return s_.tick_ms[tick];
    }

    return s_.nominal_ms + static_cast<double>(tick - s_.tick) * s_.ms_per_tick;
}

uint32_t Performer::TickAt(double ms) const
{
    if (ms <= s_.nominal_ms)
    {
        return static_cast<uint32_t>(std::lower_bound(s_.tick_ms.begin(), s_.tick_ms.end(), ms) - s_.tick_ms.begin());
    }

    return s_.tick + static_cast<uint32_t>(std::ceil((ms - s_.nominal_ms) / s_.ms_per_tick));
}

double Performer::FrameAfter() const
{
    // The events of the tick's frame go where the frame starts (Performance::frame_ticks).
    if (s_.frame == s_.tick_frame)
    {
        return 0.0;
    }

    return std::max((FrameMs(s_.frame) - s_.nominal_ms) / s_.ms_per_tick, 0.0);
}

// --- The main loop ---

auto Performer::PlayOn(uint32_t until)
{
    // The player and the performer's state go back to where they are once the copy has played.
    const ArchivePlayer::Snapshot snapshot = player_.Save(nullptr);
    State saved = s_;
    probing_ = true;
    while (s_.tick < until && Advance())
    {
        s_.pending.reset();
        if (FrameMs(s_.frame) >= kMaxSeconds * 1000.0)
        {
            break;
        }
    }
    probing_ = false;

    std::vector<seqlib::midi::PartSwitch> probe_switches = s_.recorder.PartSwitches();
    seqlib::midi::Performance probe = std::move(s_.out);
    player_.Restore(snapshot);
    s_ = std::move(saved);

    return [played = std::move(probe), switches = std::move(probe_switches)](uint32_t start, uint32_t end)
    {
        return ntdlib::sequence_export::RepeatsAsTheGame(played, switches, start, end, kLoopRules);
    };
}

bool Performer::MarkSteadyPass()
{
    return s_.main_loop.MarkSteadyPass(s_.out, s_.last_draw, options_.loops,
                                       [this](uint32_t until) { return PlayOn(until); });
}

void Performer::DecideLoop()
{
    const PendingLoop pending = *s_.pending;
    s_.pending.reset();
    if (s_.next_change == options_.changes.size() && MarkSteadyPass())
    {
        stop_ = true;
        stop_tick_ = pending.end;
        held_sound_end_ = HeldSoundEnd(pending.start);
    }
}

std::optional<double> Performer::HeldSoundEnd(uint32_t start) const
{
    const bool held = std::ranges::any_of(s_.voices, [](const Voice& v) { return v.sounding && v.held; });
    for (const std::vector<TrackEvent>& events : s_.out.tracks)
    {
        for (const TrackEvent& e : events)
        {
            if (e.kind == EventKind::kNoteOn && e.tick >= start)
            {
                return std::nullopt;
            }
        }
    }

    return held ? std::optional<double>(std::min(s_.last_change_ms + kHoldSeconds * 1000.0, TickMs(s_.tick)))
                : std::nullopt;
}

void Performer::EndPerformance(bool finished)
{
    Performance& out = s_.out;

    // A main loop that holds a sound ends kHoldSeconds after its last change, without loop markers.
    if (held_sound_end_)
    {
        out.holds = true;
        out.DropLoop();
    }

    // A performance that stops at the end of a pass leaves out what the next pass started in its last tick.
    const std::optional<uint32_t> end_tick =
        held_sound_end_ ? std::optional<uint32_t>(std::min(TickAt(*held_sound_end_), s_.tick)) : stop_tick_;
    if (!finished && end_tick)
    {
        seqlib::midi::ClampNoteOffs(out, *end_tick);
        const std::set<uint32_t> late = seqlib::midi::CutAt(out, *end_tick);
        std::erase_if(out.unstarted, [&end_tick](const UnstartedNote& note) { return note.tick >= *end_tick; });
        for (Voice& v : s_.voices)
        {
            if (late.contains(v.note))
            {
                v.held = false;
            }
        }
        for (int c = 0; c < snd::kChannelCount; c++)
        {
            Voice& v = s_.voices[static_cast<std::size_t>(c)];
            if (v.held)
            {
                s_.recorder.NoteOff(out, static_cast<uint8_t>(v.track), *end_tick, v.part, v.key, v.note);
                v.held = false;
            }
        }
        out.end_tick = *end_tick;
        out.seconds = TickMs(*end_tick) / 1000.0;
        seqlib::midi::ClampNoteOffs(out, out.end_tick);

        // The stretches that begin in the end's tick, and the changes there, belong to the next pass.
        for (std::size_t t = 0; t < out.stretches.size(); t++)
        {
            std::erase_if(out.stretches[t], [&](const Stretch& stretch) { return stretch.tick >= *end_tick; });
            std::erase_if(out.mix[t], [&](const MixChange& change) { return change.tick >= *end_tick; });
        }
        return;
    }

    // A finished sequence plays its tails out, up to kMaxTailSeconds.
    double end_ms = FrameMs(s_.frame);
    if (finished)
    {
        const auto limit = s_.frame + static_cast<uint64_t>(kMaxTailSeconds * 1000.0 / kFrameMs);
        while (AnySounding() && s_.frame < limit)
        {
            player_.RunFrame();
            EndFrame();
        }
        end_ms = FrameMs(AnySounding() ? s_.frame : s_.last_sound_frame + 1);
    }

    // The notes still on end at the end.
    const double after = std::max((end_ms - s_.nominal_ms) / s_.ms_per_tick, 0.0);
    for (int c = 0; c < snd::kChannelCount; c++)
    {
        EndVoice(c, after);
    }

    uint32_t last_event = 0;
    for (const std::vector<TrackEvent>& events : out.tracks)
    {
        for (const TrackEvent& e : events)
        {
            last_event = std::max(last_event, e.tick + static_cast<uint32_t>(std::ceil(e.after)));
        }
    }
    out.end_tick = std::max({last_event, s_.tick + static_cast<uint32_t>(std::ceil(after))});
    out.seconds = std::max(end_ms, TickMs(s_.tick)) / 1000.0;

    // A sequence whose last change comes long after its last sound ends where the sound dies away.
    if (finished && EndsAtLastSound(end_ms / 1000.0, std::max(TickMs(last_event), TickMs(s_.tick)) / 1000.0))
    {
        const uint32_t end = TickAt(end_ms);
        seqlib::midi::CutAt(out, end);
        std::erase_if(out.unstarted, [end](const UnstartedNote& note) { return note.tick >= end; });
        std::erase_if(out.tempo, [end](const auto& tempo) { return tempo.tick > end; });
        out.end_tick = end;
        out.seconds = end_ms / 1000.0;
    }
    seqlib::midi::ClampNoteOffs(out, out.end_tick);
}

Performance Performer::Run()
{
    ntdlib::ntr::GameSettings settings = {
        .channels = options_.player_channels, .track_channels = options_.track_channels, .reverb = options_.reverb};
    for (const auto& [index, value] : options_.variables)
    {
        if (index < 16)
        {
            settings.variables.local[index] = value;
        }
        else if (index < 32)
        {
            settings.variables.global[index - 16u] = value;
        }
    }
    player_.Start(sequence_, settings);
    if (!options_.commands.empty())
    {
        player_.Send(options_.commands);
    }

    // The game's changes come in the order of their frames.
    std::ranges::stable_sort(options_.changes, {}, &VariableChange::frame);

    bool finished = false;
    std::optional<double> steady_since;
    for (;;)
    {
        if (!Advance())
        {
            finished = true;
            break;
        }

        if (s_.pending)
        {
            DecideLoop();
            if (stop_)
            {
                break;
            }
        }

        // A sequence whose tracks only wait has ended once nothing sounds. So has one whose tracks pass still. A held
        // sound ends kHoldSeconds after it stops changing, without loop markers. A track that waits for ever holds such
        // a sound. So does a track that passes still while its notes don't end by themselves, such as a tie struck
        // again in each pass. A player that loops on a pass would strike the notes again. A game's change that is still
        // to come can set such a track going. So can the last change, once the track's pass reads it. The driver's
        // rules count that pass as still until it ends, and it gets kHoldSeconds.
        const double now = FrameMs(s_.frame);
        const bool changes_settled = s_.next_change == options_.changes.size() &&
                                     (options_.changes.empty() || now - s_.changed_ms >= kHoldSeconds * 1000.0);
        if (changes_settled && (Driver().IsStuck(0) || Driver().IsHolding(0)))
        {
            if (!AnySounding())
            {
                finished = true;
                break;
            }
            if (!steady_since)
            {
                steady_since = std::max(now, s_.last_change_ms);
            }
            if (now - *steady_since >= kHoldSeconds * 1000.0)
            {
                s_.out.holds = true;
                s_.out.DropLoop();
                break;
            }
        }
        else
        {
            steady_since.reset();
        }

        // A sequence that has been silent for longer than kHoldSeconds at the time limit has finished. It ends with
        // its last sound.
        if (now >= kMaxSeconds * 1000.0)
        {
            finished = !AnySounding() && now - FrameMs(s_.last_sound_frame) > kHoldSeconds * 1000.0;
            s_.out.truncated = !finished;
            break;
        }

        const snd::Player& player = Driver().GetPlayer(0);
        if (s_.ticked && ((static_cast<uint32_t>(player.tempo) * player.tempo_ratio) >> 8) == 0)
        {
            s_.out.warnings.push_back("tempo 0: the sequence stops advancing");
            s_.out.holds = AnySounding();
            break;
        }
    }

    const PassStart next_pass =
        s_.out.loop && stop_ && !held_sound_end_
            ? ntdlib::sequence_export::PassStartAt(s_.out, s_.recorder.PartSwitches(), s_.out.loop->end)
            : PassStart{};
    EndPerformance(finished);
    if (finished)
    {
        s_.out.DropLoop();
    }
    if (s_.out.loop)
    {
        ntdlib::sequence_export::RestoreAtLoopStart(s_.out, s_.recorder.PartSwitches(), next_pass, kLoopRules);
    }
    for (const auto& [what, count] : s_.recorder.Approximations())
    {
        s_.out.approximations[what] += count;
    }
    s_.out.warnings.insert(s_.out.warnings.end(), s_.recorder.Warnings().begin(), s_.recorder.Warnings().end());

    return std::move(s_.out);
}

} // namespace

Performance Perform(const sdat::Archive& archive, const ntdlib::ntr::SequenceRef& sequence,
                    const PerformOptions& options)
{
    // Run again when CC11 would pass 127, scaling every value equally, or when the tracks' parts don't fit MIDI's
    // channels. Four runs at most.
    std::array<uint8_t, 16> part_limits;
    part_limits.fill(16);
    double peak = 0.0;
    double applied = 0.0;
    bool limited = false;
    std::vector<LoopChoice> loop_choices;
    Performance performance;
    for (int pass = 0; pass < 4; pass++)
    {
        Performer performer(archive, sequence, options, LevelScale(peak), part_limits, loop_choices);
        performance = performer.Run();
        applied = peak;
        loop_choices = performer.LoopChoices();

        bool parts_fit = true;
        if (!limited)
        {
            const std::array<uint8_t, 16> fitted =
                FitParts(performance, performer.PartsUsed(), SpareChannels::kForLayers);
            for (std::size_t t = 0; t < 16; t++)
            {
                parts_fit = parts_fit && fitted[t] >= performer.PartsUsed()[t];
            }
            if (!parts_fit)
            {
                part_limits = fitted;
                limited = true;
            }
        }
        if (parts_fit && LevelScale(performer.LevelPeak()) == LevelScale(peak))
        {
            break;
        }

        peak = performer.LevelPeak();
    }

    ntdlib::sequence_export::SetLevelCut(performance, applied);

    return performance;
}

} // namespace ntdlib::sequence_export::ntr
