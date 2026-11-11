// SPDX-License-Identifier: MIT

// DSE's performer (see performance.h). dse::Player plays the song or the sound effect, and the driver's views follow
// its sequence. The performer takes the sequence's notes, releases, ticks and loops through the driver's hooks, and
// reads the channels' settings and the voices' states from the driver. It records each tick's settings when the next
// tick starts, and when the driver's tick ends. A MIDI note ends when its voices release, or when each of its voices
// has ended or gone to another note.
//
// The driver runs a tick about every 10 ms. A sequence's tempo adds to a counter each tick, and a tick of the sequence
// runs each time the counter passes the length of a quarter note. MIDI's tempo puts each of the sequence's ticks at its
// nominal time, up to a driver's tick before the driver's tick that runs it. The events of a sequence's tick go at the
// start of that driver's tick (Performance::frame_ticks). The ends of voices that a driver's tick finds go at its start
// too (TrackEvent::after).

#include "ntdlib/sequence_export/dse/performance.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
#include "ntdlib/sequence_export/linear_pan.h"
#include "ntdlib/sequence_export/passes.h"
#include "ntdlib/sequence_export/performance_base.h"
#include "sequence_export/lengths.h"

namespace ntdlib::sequence_export::dse
{
namespace
{

using ntdlib::dse::ChannelView;
using ntdlib::dse::EventStart;
using ntdlib::dse::NoteEvent;
using ntdlib::dse::Player;
using ntdlib::dse::VoiceView;
using ntdlib::sequence_export::FittedParts;
using ntdlib::sequence_export::LevelScale;
using ntdlib::sequence_export::LoopChoice;
using ntdlib::sequence_export::MainLoopPasses;
using ntdlib::sequence_export::PassStart;
using ntdlib::sequence_export::SpareChannels;
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

// How the recorder places events (seqlib/midi/recorder.h). A setting replaces an earlier value of its kind anywhere in
// its tick. A part switch sends the new part the old part's values. No tick's frame starts before tick 0.
constexpr seqlib::midi::RecorderOptions kRecorderOptions{
    .replace_anywhere_in_tick = true, .resend_on_switch = true, .frame_ticks_from_zero = true};

// The rules of the main loop's passes (ntdlib/sequence_export/passes.h). DSE's notes take no settings that MIDI seeds.
const ntdlib::sequence_export::LoopRules kLoopRules{{}, false};

constexpr int kVoices = ntdlib::dse::Driver::kVoices;
constexpr int kMidiTracks = 16;

// A tick of the driver in milliseconds, and the driver's nominal tick, for which a sequence's tempo counts.
constexpr double kTickMs = 1000.0 * Player::kTickClocks / ntdlib::ntr::kSystemClockHz;
constexpr double kNominalTickMs = ntdlib::dse::Driver::kTickMicroseconds / 1000.0;

// An LFO's output that changes the pan (LfoValues::output).
constexpr uint8_t kPanOutput = 3;

// The event that ends a pass of a repeat.
constexpr uint8_t kRepeatEnd = 0x9D;

// The volume and the pan from which the editor's changes count (Performance::mix).
constexpr std::array<int16_t, 2> kStartMix = {127, 0};

// The envelope's full level, and the highest level below hearing. The driver squares the level: 12 of 127 plays
// 41 dB down.
constexpr uint8_t kFullLevel = 127;
constexpr uint8_t kInaudibleLevel = 12;

// A note of the sequence, while a voice plays it: its MIDI note and its voices.
struct MidiNote
{
    bool held = false; // the MIDI note is on
    int track = 0;     // the MIDI track: the sequence's channel
    uint8_t part = 0;
    uint8_t key = 0;     // the MIDI note's key. A legato to another key moves the voices' key, and the part's pitch
    uint32_t id = 0;     // the recorder's id
    uint16_t voices = 0; // the voices that play it
    uint32_t preset = 0; // the part's preset at its note-on
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

// A pass of the main loop that waits for the driver's tick to end: the track, and the ticks it started and ended in.
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
        mixed.fill(kStartMix);
    }

    Performance out;
    seqlib::midi::Recorder recorder{kRecorderOptions};
    std::array<TrackOutput, kMidiTracks> outputs{};

    std::map<uint32_t, MidiNote> notes;     // by the driver's number of the note
    std::array<uint32_t, kVoices> voices{}; // the note that each voice plays, or 0
    std::array<bool, kVoices> started{};    // the voice's envelope has run since its note took it
    std::array<uint8_t, kVoices> levels{};  // the voice's envelope level at the end of the last driver's tick
    std::array<std::optional<int32_t>, kVoices> release_ticks{}; // the voice's VoiceView::release_ticks then
    std::array<uint8_t, kVoices> exclusive{}; // the key group of the voice's note where it allows one voice, or 0
    std::map<int, uint32_t> pass_starts;      // each track's tick at the start of its pass of its loop
    std::set<int> looped;                     // the tracks that have gone back to their loop's start
    std::set<int> ended;                      // the tracks that have ended
    std::optional<uint32_t> owner_length;     // the length of the owner's passes
    MainLoopPasses main_loop;
    std::optional<PendingLoop> pending;
    bool reported_channels = false;

    // For the editor: each track's last event, and each channel's changes of its volume and pan, with the values that
    // they gave last.
    std::vector<LastEvent> last_events;
    std::array<std::vector<MixChange>, kMidiTracks> channel_mix{};
    std::array<std::array<int16_t, 2>, kMidiTracks> mixed{};

    bool ticked = false;      // the sequence has run its first tick
    uint32_t tick = 0;        // the last tick
    uint64_t frame = 0;       // the driver's tick that runs, or the driver's ticks run so far between them
    uint64_t first_frame = 0; // the driver's tick of the sequence's tick 0
    uint64_t tick_frame = 0;  // the driver's tick of the last tick
    double nominal_ms = 0.0;  // the last tick's time at MIDI's tempo
    double ms_per_tick = 10.0;
    std::vector<double> tick_ms; // each tick's nominal time
    uint64_t last_sound_frame = 0;
    double last_change_ms = 0.0; // the time of the last note-on or setting
    double last_note_ms = 0.0;   // the time of the last note-on
};

class Performer
{
public:
    // Multiply CC11 by `level_scale` (LevelPeak). Give each MIDI track at most `part_limits` parts. Make the main
    // loop's first choices as an earlier run made them (`loop_choices`, from LoopChoices). Then choose the rest.
    Performer(const SequenceFiles& files, const PerformOptions& options, double level_scale,
              const std::array<uint8_t, 16>& part_limits, const std::vector<LoopChoice>& loop_choices);

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

    // Whether a note of MIDI track `track` that can be heard sounds on part `part`: a voice of the note was above
    // kInaudibleLevel at the end of the last driver's tick.
    bool PartAudible(int track, uint8_t part) const;

    // The newest note that sounds on the part, or the newest that is held there where `held`. 0 for none.
    uint32_t NewestOn(int track, uint8_t part, bool held) const;

    // The part's pitch in semitones: that of its newest held note, else of its newest note, else the channel's bend.
    float PartPitch(int track, uint8_t part) const;

    // The voice `voice` no longer plays its note: the note ends when it has no other voice. A note that a new note cut
    // stops where it sounds alone on its part. Returns the cut note where it rings on in its release instead.
    std::optional<MidiNote> LeaveVoice(int voice, bool cut, double after);

    // The driver's tick's end: the settings and the voices' ends.
    void EndFrame();

    // Runs a tick of the driver. Returns false once the sequence has ended.
    bool Advance();

    bool AnySounding() const;
    double FrameMs(uint64_t frame) const;
    double TickMs(uint32_t tick) const;
    uint32_t TickAt(double ms) const;
    double FrameAfter() const;

    // --- The main loop ---
    auto PlayOn(uint32_t until);
    bool RestartAtTrackEnds();
    bool EveryTrackLooped() const;
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
    State s_;
    bool stop_ = false;
    std::optional<uint32_t> stop_tick_;
    std::optional<double> held_sound_end_;
};

Performer::Performer(const SequenceFiles& files, const PerformOptions& options, double level_scale,
                     const std::array<uint8_t, 16>& part_limits, const std::vector<LoopChoice>& loop_choices)
    : files_(files),
      options_(options),
      player_(std::make_unique<Player>(ntdlib::dse::PlayerOptions{.build = options.build})),
      s_(loop_choices)
{
    const std::vector<ntdlib::dse::Track>* tracks = nullptr;
    if (files.song != nullptr)
    {
        tracks = &files.song->tracks;
    }
    else if (files.effect < files.effects->sequences.size() && files.effects->sequences[files.effect])
    {
        tracks = &files.effects->sequences[files.effect]->tracks;
    }
    if (tracks != nullptr)
    {
        for (const ntdlib::dse::Track& track : *tracks)
        {
            track_channels_.push_back(track.channel);
        }
    }

    s_.recorder.SetLevelScale(level_scale);
    s_.recorder.SetPartLimits(part_limits);

    for (std::size_t t = 0; t < s_.outputs.size(); t++)
    {
        s_.outputs[t].midi_track = static_cast<uint8_t>(t);
    }

    // The game's loads: the banks, then the waves of each bank without samples that the main bank holds.
    for (const uint16_t id : player_->LoadBanks(files))
    {
        s_.out.warnings.push_back("a bank's id " + std::to_string(id) +
                                  " is taken: the driver plays the bank loaded first");
    }

    ntdlib::dse::SequenceHooks hooks;
    hooks.on_tick = [this]
    {
        OnTick();
    };
    hooks.on_event = [this](const EventStart& e)
    {
        OnEvent(e);
    };
    hooks.on_note = [this](const NoteEvent& e)
    {
        OnNote(e);
    };
    hooks.on_release = [this](int voice)
    {
        OnRelease(voice);
    };
    hooks.on_loop_start = [this](int track)
    {
        OnLoopStart(track);
    };
    hooks.on_loop = [this](int track)
    {
        OnLoop(track);
    };
    player_->Driver().SetHooks(std::move(hooks));
}

// --- The driver's hooks ---

void Performer::OnTick()
{
    if (!s_.ticked)
    {
        s_.ticked = true;
        s_.first_frame = s_.frame;
        s_.tick_frame = s_.frame;
        s_.out.timebase = Driver().SequenceTimebase();
        s_.tick_ms.push_back(0.0);
        s_.out.frame_ticks.push_back(0.0);
        SyncTempo();
        return;
    }

    RestartAtTrackEnds();

    // The settings of the last tick, then the next tick at MIDI's tempo.
    SyncChannels(0.0);
    SyncTempo();
    s_.tick++;
    s_.tick_frame = s_.frame;
    s_.nominal_ms += s_.ms_per_tick;
    s_.tick_ms.push_back(s_.nominal_ms);
    const double start = static_cast<double>(s_.tick) + (FrameMs(s_.frame) - s_.nominal_ms) / s_.ms_per_tick;
    s_.out.frame_ticks.push_back(std::max(start, s_.out.frame_ticks.back()));
}

void Performer::OnEvent(const EventStart& e)
{
    // An event that follows the one before it goes on with the stretch. So does an event that runs again.
    if (e.track < 0)
    {
        return;
    }

    const auto t = static_cast<std::size_t>(e.track);
    if (t >= s_.out.stretches.size())
    {
        s_.out.stretches.resize(t + 1);
        s_.last_events.resize(t + 1);
    }

    LastEvent& last = s_.last_events[t];
    std::vector<Stretch>& stretches = s_.out.stretches[t];
    if (last.ran && (e.offset == last.next || e.offset == last.offset) && !stretches.empty())
    {
        stretches.back().last = e.offset;
    }
    else
    {
        Stretch stretch;
        stretch.tick = s_.tick;
        stretch.first = e.offset;
        stretch.last = e.offset;
        stretch.depth = e.repeats;
        if (last.ran)
        {
            stretch.entry = last.code == kRepeatEnd ? Entry::kLoop : Entry::kJump;
        }
        stretches.push_back(stretch);
    }

    last = {true, e.offset, e.next, e.code};
}

void Performer::OnLoopStart(int track)
{
    s_.pass_starts[track] = s_.tick;
}

void Performer::OnLoop(int track)
{
    // The pass started when the track last passed its loop's start. A pass within one tick waited for nothing.
    const auto found = s_.pass_starts.find(track);
    if (found == s_.pass_starts.end() || found->second >= s_.tick)
    {
        return;
    }

    const uint32_t start = found->second;
    found->second = s_.tick;
    s_.looped.insert(track);

    // The longest loop of the sequence's tracks owns the main loop. A song's tracks usually loop over the same ticks.
    const std::pair<int, uint32_t> owner{track, 0};
    const uint32_t length = s_.tick - start;
    if (!s_.main_loop.Owner() || (*s_.main_loop.Owner() != owner && length > *s_.owner_length))
    {
        s_.main_loop.Restart(s_.out, owner);
        s_.owner_length = length;
    }
    if (*s_.main_loop.Owner() != owner)
    {
        return;
    }

    s_.main_loop.CountPass(s_.out, owner, start, s_.tick);
    if (s_.out.loops >= options_.loops && TickMs(s_.tick) >= kMinLoopSeconds * 1000.0)
    {
        s_.pending = PendingLoop{track, start, s_.tick};
    }
}

void Performer::OnNote(const NoteEvent& e)
{
    if (!s_.ticked)
    {
        return;
    }
    if (e.channel >= kMidiTracks)
    {
        if (!s_.reported_channels)
        {
            s_.reported_channels = true;
            s_.out.warnings.push_back("notes on channels past 15 are left out");
        }
        return;
    }

    // A legato goes on in the voices of the previous key, and moves them to the new key (NotePitch). The voices keep
    // the velocity of the note that started them.
    TrackOutput& output = s_.outputs[static_cast<std::size_t>(e.channel)];
    if (e.legato)
    {
        std::set<uint32_t> moved;
        for (int v = 0; v < kVoices; v++)
        {
            const uint32_t number = s_.voices[static_cast<std::size_t>(v)];
            const VoiceView view = Driver().GetVoice(v);
            if (number != 0 && view.channel == e.channel && view.key == e.key && s_.notes.at(number).held)
            {
                moved.insert(number);
            }
        }

        for (const uint32_t number : moved)
        {
            const MidiNote& note = s_.notes.at(number);
            if (note.key != e.key && NewestOn(note.track, note.part, true) != number)
            {
                Approximate("legato to another key on a part with newer notes (they keep its pitch)");
            }
        }

        SyncChannel(e.channel, 0.0);
        return;
    }

    const ChannelView channel = Driver().GetChannel(e.channel);

    // A voice that another note takes stops at once on the console. A note of a key group that allows one voice takes
    // the voice of the group's last note on the channel. Its exclusive class cuts that note where both share a part
    // and a preset (PresetZone::exclusive_class).
    std::vector<MidiNote> exclusive_cuts;
    for (int v = 0; v < kVoices; v++)
    {
        const auto index = static_cast<std::size_t>(v);
        if ((e.voices & (1u << v)) == 0 || s_.voices[index] == 0)
        {
            continue;
        }

        const auto found = s_.notes.find(s_.voices[index]);
        const bool exclusive = s_.exclusive[index] != 0 && Driver().GetVoice(v).key_group == s_.exclusive[index] &&
                               found != s_.notes.end() && found->second.track == e.channel;
        if (const std::optional<MidiNote> rings = LeaveVoice(v, true, 0.0); rings && exclusive)
        {
            exclusive_cuts.push_back(*rings);
        }
        else if (rings)
        {
            Approximate("note cut short by another note's voice (rings on in its release)");
        }
    }

    // A note that gets no voice plays nothing on the console. It gets no MIDI note.
    if (e.voices == 0)
    {
        s_.out.unstarted.push_back({.tick = s_.tick,
                                    .length = e.length,
                                    .track = static_cast<uint8_t>(e.track),
                                    .key = e.key,
                                    .velocity = e.velocity,
                                    .offset = e.offset});
        return;
    }

    int first_voice = 0;
    while ((e.voices & (1u << first_voice)) == 0)
    {
        first_voice++;
    }

    // The note's key is its voices' key: a channel with random keys gives the note another key than the event's. The
    // settings that the tick's events have changed so far come first. The note then goes to another part when a note
    // held there plays at another pitch. The pitches of a channel's notes differ by their random changes of pitch, the
    // ranges of their key bends and the keys that legatos have moved them to.
    const uint8_t midi_key = Driver().GetVoice(first_voice).key;
    SyncChannel(e.channel, 0.0);
    const float pitch = VoicePitch(first_voice, midi_key);
    const bool other_pitch = std::ranges::any_of(s_.notes,
                                                 [&](const auto& entry)
                                                 {
                                                     const MidiNote& n = entry.second;
                                                     return n.held && n.track == e.channel && n.part == output.part &&
                                                            NotePitch(entry.first) != pitch;
                                                 });
    if (other_pitch &&
        !s_.recorder.Switch(s_.out, output, s_.tick, [this, &e](uint8_t part) { return PartSounds(e.channel, part); }))
    {
        Approximate("note on a part whose pitch another note moves (no MIDI channel free)");
    }

    PresetKey key;
    key.bank = channel.bank;
    key.program = channel.program;
    key.envelope = channel.envelope;
    key.lfos = channel.lfos;
    key.lfo_level = channel.lfo_level;

    // SoundFont holds a pitch LFO and a volume LFO, each a triangle without a fade (convert.h).
    std::set<uint8_t> outputs;
    for (const LfoShape& lfo : PresetLfos(key))
    {
        if (lfo.output == kPanOutput)
        {
            Approximate("pan LFO (omitted from SoundFont)");
        }
        else if (!outputs.insert(lfo.output).second)
        {
            Approximate("second LFO of the pitch or the volume (omitted from SoundFont)");
        }
        else
        {
            if (!lfo.triangle)
            {
                Approximate("LFO waveform other than a triangle (a triangle in SoundFont)");
            }
            if (lfo.fade > 0.0)
            {
                Approximate("LFO fade-in (omitted from SoundFont)");
            }
        }
    }

    const uint32_t preset = seqlib::midi::PresetIndex(s_.out.presets, key);
    s_.recorder.SendPreset(s_.out, output, s_.tick, preset);
    s_.recorder.SendPitch(s_.out, output, s_.tick, pitch);

    for (const MidiNote& cut : exclusive_cuts)
    {
        if (cut.part != output.part || cut.preset != preset)
        {
            Approximate("note cut short by another note's voice (rings on in its release)");
        }
    }

    const auto velocity = static_cast<uint8_t>(std::clamp<int>(e.velocity, 1, 127));
    const MidiNote note{.held = true,
                        .track = e.channel,
                        .part = output.part,
                        .key = midi_key,
                        .id = s_.recorder.NoteOn(s_.out, output, s_.tick, midi_key, velocity),
                        .voices = e.voices,
                        .preset = preset};
    s_.notes[e.number] = note;
    RecordNote(note.id, e);

    for (int v = 0; v < kVoices; v++)
    {
        if ((e.voices & (1u << v)) != 0)
        {
            s_.voices[static_cast<std::size_t>(v)] = e.number;
            s_.started[static_cast<std::size_t>(v)] = false;
            s_.levels[static_cast<std::size_t>(v)] = kFullLevel;
            const VoiceView view = Driver().GetVoice(v);
            s_.release_ticks[static_cast<std::size_t>(v)] = view.release_ticks;
            s_.exclusive[static_cast<std::size_t>(v)] = view.key_group_voices == 1 ? view.key_group : 0;
        }
    }

    s_.last_change_ms = TickMs(s_.tick);
    s_.last_note_ms = s_.last_change_ms;
}

void Performer::OnRelease(int voice)
{
    const uint32_t number = s_.voices[static_cast<std::size_t>(voice)];
    const auto found = s_.notes.find(number);
    if (number == 0 || found == s_.notes.end() || !found->second.held)
    {
        return;
    }

    MidiNote& note = found->second;
    s_.recorder.NoteOff(s_.out, static_cast<uint8_t>(note.track), s_.tick, note.part, note.key, note.id);
    note.held = false;
}

// --- Recording ---

void Performer::SyncTempo()
{
    const int32_t tempo = Driver().SequenceTempo();
    if (tempo <= 0 || s_.out.timebase == 0)
    {
        return;
    }

    // A tick of the sequence takes tempo / timebase microseconds of the driver's nominal ticks.
    s_.ms_per_tick = tempo / 1000.0 / s_.out.timebase * (kTickMs / kNominalTickMs);
    s_.recorder.SetTempo(s_.out, s_.tick, 60000.0 / s_.ms_per_tick / s_.out.timebase);
}

void Performer::SyncChannels(double after)
{
    const int count = std::min(Driver().ChannelCount(), kMidiTracks);
    for (int c = 0; c < count; c++)
    {
        SyncChannel(c, after);
    }
}

void Performer::SyncChannel(int number, double after)
{
    using seqlib::midi::EventKind;
    TrackOutput& output = s_.outputs[static_cast<std::size_t>(number)];
    const ChannelView channel = Driver().GetChannel(number);

    // CC7 and CC11 take the square of their value. So do the driver's volumes: a voice's volume is the square of the
    // product of the channel's fade, expression and synth volume, the note's volume and the envelope's level. CC11
    // holds the expression, the synth's volume and the level of the channel's pan (PanGain). The zones hold the rest of
    // the pan's level. CC10 holds the pan's position. The driver gives the sound unit the hardware pan of its pan.
    RecordMix(number, channel);
    const int pan = ntdlib::dse::tables::kPanCurve[std::clamp<int>(channel.pan, 0, 127)];
    const double level =
        channel.expression * std::clamp<int>(channel.synth_volume, 0, 127) / 127.0 * std::sqrt(PanGain(pan));
    s_.recorder.RaiseLevelPeak(level);

    const std::array<std::pair<EventKind, uint8_t>, 3> settings = {{
        {EventKind::kVolume, static_cast<uint8_t>(std::min<int>(channel.volume, 127))},
        {EventKind::kExpression, s_.recorder.Expression(level)},
        {EventKind::kPan, static_cast<uint8_t>(std::clamp<long>(std::lround(64.0 + 64.0 * PanPosition(pan)), 0, 127))},
    }};

    // The part that new notes go to, and every part on which a note of the channel sounds. The voices that release
    // follow the channel's settings, as MIDI's notes do.
    std::set<uint8_t> parts = {output.part};
    for (const auto& [number_of_note, note] : s_.notes)
    {
        if (note.track == number)
        {
            parts.insert(note.part);
        }
    }

    for (const uint8_t part : parts)
    {
        bool changed = false;
        for (const auto& [kind, value] : settings)
        {
            changed = s_.recorder.SendSetting(s_.out, output, s_.tick, kind, value, part) || changed;
        }
        changed = s_.recorder.SendPitch(s_.out, output, s_.tick, PartPitch(number, part), part, after) || changed;
        if (changed)
        {
            s_.last_change_ms = TickMs(s_.tick);
        }
    }
}

void Performer::RecordNote(uint32_t id, const NoteEvent& e)
{
    // The note's event ran in its track's last stretch.
    std::vector<NoteCommand>& commands = s_.out.note_commands;
    if (commands.size() < id)
    {
        commands.resize(id);
        s_.out.note_tracks.resize(id);
        s_.out.note_channels.resize(id);
    }

    const auto t = static_cast<std::size_t>(e.track);
    const std::size_t stretches = t < s_.out.stretches.size() ? s_.out.stretches[t].size() : 0;
    commands[id - 1] = {e.offset, static_cast<uint32_t>(stretches > 0 ? stretches - 1 : 0)};
    s_.out.note_tracks[id - 1] = static_cast<uint8_t>(e.track);
}

void Performer::RecordMix(int number, const ChannelView& channel)
{
    const auto c = static_cast<std::size_t>(number);
    const std::array<std::pair<Mix, int16_t>, 2> values = {
        {{Mix::kVolume, static_cast<int16_t>(std::min<int>(channel.volume, 127))},
         {Mix::kPan, static_cast<int16_t>(std::clamp<int>(channel.pan - 0x40, -64, 63))}}};
    std::vector<MixChange>& changes = s_.channel_mix[c];
    for (std::size_t i = 0; i < values.size(); i++)
    {
        const auto [setting, value] = values[i];
        if (value == s_.mixed[c][i])
        {
            continue;
        }

        s_.mixed[c][i] = value;

        const auto matches = [&](const MixChange& change)
        {
            return change.tick == s_.tick && change.setting == setting;
        };
        const auto same = std::find_if(changes.rbegin(), changes.rend(), matches);
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

float Performer::VoicePitch(int voice, uint8_t key) const
{
    const VoiceView view = Driver().GetVoice(voice);
    const ChannelView channel = Driver().GetChannel(view.channel);
    const int32_t bend = static_cast<int16_t>(channel.bend) + view.key_bend + view.jitter;
    return static_cast<float>(bend) / 256.0f + static_cast<float>(view.key - key);
}

float Performer::NotePitch(uint32_t number) const
{
    // A voice that the driver has freed in this tick still counts among the note's until the tick ends (EndFrame).
    const MidiNote& note = s_.notes.at(number);
    for (int v = 0; v < kVoices; v++)
    {
        if ((note.voices & (1u << v)) == 0)
        {
            continue;
        }

        const VoiceView view = Driver().GetVoice(v);
        if (view.allocated && view.followed && view.note == number)
        {
            return VoicePitch(v, note.key);
        }
    }

    return static_cast<float>(static_cast<int16_t>(Driver().GetChannel(note.track).bend)) / 256.0f;
}

bool Performer::PartSounds(int track, uint8_t part) const
{
    return std::ranges::any_of(
        s_.notes, [&](const auto& entry) { return entry.second.track == track && entry.second.part == part; });
}

bool Performer::PartAudible(int track, uint8_t part) const
{
    return std::ranges::any_of(
        s_.notes,
        [&](const auto& entry)
        {
            const MidiNote& note = entry.second;
            if (note.track != track || note.part != part)
            {
                return false;
            }

            for (int v = 0; v < kVoices; v++)
            {
                if ((note.voices & (1u << v)) != 0 && s_.levels[static_cast<std::size_t>(v)] > kInaudibleLevel)
                {
                    return true;
                }
            }

            return false;
        });
}

uint32_t Performer::NewestOn(int track, uint8_t part, bool held) const
{
    uint32_t newest = 0;
    for (const auto& [number, note] : s_.notes)
    {
        if (note.track == track && note.part == part && (note.held || !held))
        {
            newest = std::max(newest, number);
        }
    }

    return newest;
}

float Performer::PartPitch(int track, uint8_t part) const
{
    if (const uint32_t held = NewestOn(track, part, true); held != 0)
    {
        return NotePitch(held);
    }
    if (const uint32_t newest = NewestOn(track, part, false); newest != 0)
    {
        return NotePitch(newest);
    }

    return static_cast<float>(static_cast<int16_t>(Driver().GetChannel(track).bend)) / 256.0f;
}

std::optional<MidiNote> Performer::LeaveVoice(int voice, bool cut, double after)
{
    uint32_t& number = s_.voices[static_cast<std::size_t>(voice)];
    const auto found = s_.notes.find(number);
    number = 0;
    if (found == s_.notes.end())
    {
        return std::nullopt;
    }

    MidiNote& note = found->second;
    note.voices = static_cast<uint16_t>(note.voices & ~(1u << voice));
    if (note.voices != 0)
    {
        return std::nullopt;
    }

    const MidiNote ended = note;
    s_.notes.erase(found);

    if (ended.held)
    {
        s_.recorder.NoteOff(s_.out, static_cast<uint8_t>(ended.track), s_.tick, ended.part, ended.key, ended.id, after);
    }
    if (cut && ended.held && ended.id >= 1 && ended.id <= s_.out.note_channels.size())
    {
        s_.out.note_channels[ended.id - 1].cut_tick = s_.tick;
    }

    if (cut)
    {
        // The cut note stops at once. CC120 stops it where no other note that can be heard sounds on its part, and
        // stops those below hearing with it. Otherwise MIDI plays its release. A release below hearing doesn't count,
        // and neither does one that falls below hearing within a tick: the level falls in a straight line.
        const uint8_t level = s_.levels[static_cast<std::size_t>(voice)];
        if (level <= kInaudibleLevel)
        {
            return std::nullopt;
        }

        if (!PartAudible(ended.track, ended.part))
        {
            s_.recorder.Emit(s_.out, s_.outputs[static_cast<std::size_t>(ended.track)], s_.tick,
                             {.kind = EventKind::kSoundOff}, ended.part);
            return std::nullopt;
        }

        const std::optional<int32_t> release = s_.release_ticks[static_cast<std::size_t>(voice)];
        if (!release || int64_t{*release} * (level - kInaudibleLevel) > level)
        {
            return ended;
        }
    }

    return std::nullopt;
}

void Performer::EndFrame()
{
    if (!s_.ticked)
    {
        s_.frame++;
        return;
    }

    // A voice that the driver has freed, stopped or given to another note no longer plays its note.
    const double after = FrameAfter();
    for (int v = 0; v < kVoices; v++)
    {
        const auto index = static_cast<std::size_t>(v);
        if (s_.voices[index] == 0)
        {
            continue;
        }

        const VoiceView view = Driver().GetVoice(v);
        if (!view.allocated || !view.followed || view.note != s_.voices[index] || (s_.started[index] && !view.started))
        {
            LeaveVoice(v, false, after);
            continue;
        }

        s_.started[index] = s_.started[index] || view.started;
        s_.levels[index] = view.level;
        s_.release_ticks[index] = view.release_ticks;
    }

    SyncChannels(after);

    if (AnySounding())
    {
        s_.last_sound_frame = s_.frame;
    }

    s_.frame++;
}

bool Performer::Advance()
{
    player_->RunTick();
    EndFrame();
    return Driver().SequencePlays();
}

bool Performer::AnySounding() const
{
    return player_->BusyChannels() != 0;
}

double Performer::FrameMs(uint64_t frame) const
{
    return static_cast<double>(frame) * kTickMs - static_cast<double>(s_.first_frame) * kTickMs;
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
    // The events of the tick's driver's tick go where it starts (Performance::frame_ticks).
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
    const Player::Snapshot snapshot = player_->Save();
    State saved = s_;
    while (s_.tick < until && Advance())
    {
        s_.pending.reset();
        if (FrameMs(s_.frame) >= kMaxSeconds * 1000.0)
        {
            break;
        }
    }

    std::vector<seqlib::midi::PartSwitch> part_switches = s_.recorder.PartSwitches();
    seqlib::midi::Performance probe_out = std::move(s_.out);
    player_->Restore(snapshot);
    s_ = std::move(saved);

    return [played = std::move(probe_out), switches = std::move(part_switches)](uint32_t start, uint32_t end)
    {
        return ntdlib::sequence_export::RepeatsAsTheGame(played, switches, start, end, kLoopRules);
    };
}

// A track that ends leaves music that the passes before its end didn't play. The main loop's passes count again from
// there. Returns true if a track has ended since the last call and the passes start again.
bool Performer::RestartAtTrackEnds()
{
    bool restarted = false;
    for (int track = 0; track < Driver().TrackCount(); track++)
    {
        if (!Driver().TrackPlays(track) && s_.ended.insert(track).second && s_.main_loop.Owner())
        {
            s_.main_loop.Restart(s_.out, s_.main_loop.Owner());
            restarted = true;
        }
    }

    return restarted;
}

// Until each track that still plays has gone back to its loop's start, a track with a longer loop can still take the
// main loop, and a track without a loop can still play on.
bool Performer::EveryTrackLooped() const
{
    for (int track = 0; track < Driver().TrackCount(); track++)
    {
        if (Driver().TrackPlays(track) && !s_.looped.contains(track))
        {
            return false;
        }
    }

    return true;
}

bool Performer::MarkSteadyPass()
{
    return s_.main_loop.MarkSteadyPass(s_.out, std::nullopt, options_.loops,
                                       [this](uint32_t until) { return PlayOn(until); });
}

void Performer::DecideLoop()
{
    const PendingLoop pending = *s_.pending;
    s_.pending.reset();
    if (RestartAtTrackEnds() || !EveryTrackLooped())
    {
        return;
    }

    // A pass that holds a sound ends the performance only once no note has started for kHoldSeconds.
    if (HoldsSound(pending.start) && TickMs(s_.tick) - s_.last_note_ms <= kHoldSeconds * 1000.0)
    {
        return;
    }

    if (MarkSteadyPass())
    {
        stop_ = true;
        stop_tick_ = pending.end;
        held_sound_end_ = HeldSoundEnd(pending.start);
    }
}

// True if a note sounds and no note has started since tick `start`.
bool Performer::HoldsSound(uint32_t start) const
{
    for (const std::vector<TrackEvent>& events : s_.out.tracks)
    {
        for (const TrackEvent& e : events)
        {
            if (e.kind == EventKind::kNoteOn && e.tick >= start)
            {
                return false;
            }
        }
    }

    return std::ranges::any_of(s_.notes, [](const auto& entry) { return entry.second.held; });
}

std::optional<double> Performer::HeldSoundEnd(uint32_t start) const
{
    return HoldsSound(start)
               ? std::optional<double>(std::min(s_.last_change_ms + kHoldSeconds * 1000.0, TickMs(s_.tick)))
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
        DropEventsFrom(out, *end_tick);

        for (auto& [number, note] : s_.notes)
        {
            if (late.contains(note.id))
            {
                note.held = false;
            }
        }

        for (auto& [number, note] : s_.notes)
        {
            if (note.held)
            {
                s_.recorder.NoteOff(out, static_cast<uint8_t>(note.track), *end_tick, note.part, note.key, note.id);
                note.held = false;
            }
        }

        out.end_tick = *end_tick;
        out.seconds = TickMs(*end_tick) / 1000.0;
        seqlib::midi::ClampNoteOffs(out, out.end_tick);

        // The stretches that begin in the end's tick, and the changes there, belong to the next pass.
        for (std::vector<Stretch>& stretches : out.stretches)
        {
            std::erase_if(stretches, [&](const Stretch& stretch) { return stretch.tick >= *end_tick; });
        }
        for (std::vector<MixChange>& changes : s_.channel_mix)
        {
            std::erase_if(changes, [&](const MixChange& change) { return change.tick >= *end_tick; });
        }
        return;
    }

    // A finished sequence plays its tails out, up to kMaxTailSeconds.
    double end_ms = FrameMs(s_.frame);
    if (finished)
    {
        const auto limit = s_.frame + static_cast<uint64_t>(kMaxTailSeconds * 1000.0 / kTickMs);
        while (AnySounding() && s_.frame < limit)
        {
            player_->RunTick();
            EndFrame();
        }

        end_ms = FrameMs(AnySounding() ? s_.frame : s_.last_sound_frame + 1);
    }

    // The notes still on end at the end.
    const double after = std::max((end_ms - s_.nominal_ms) / s_.ms_per_tick, 0.0);
    for (auto& [number, note] : s_.notes)
    {
        if (note.held)
        {
            s_.recorder.NoteOff(out, static_cast<uint8_t>(note.track), s_.tick, note.part, note.key, note.id, after);
            note.held = false;
        }
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
        DropEventsFrom(out, end);
        std::erase_if(out.tempo, [end](const auto& tempo) { return tempo.tick > end; });
        out.end_tick = end;
        out.seconds = end_ms / 1000.0;
    }

    seqlib::midi::ClampNoteOffs(out, out.end_tick);
}

Performance Performer::Run()
{
    // A sound effect plays on the first sequence of the pool that doesn't play.
    if (!player_->Start(files_))
    {
        s_.out.warnings.push_back("the driver refuses the sound effect");
        return std::move(s_.out);
    }

    bool finished = false;
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

        // A sequence that has been silent for longer than kHoldSeconds at the time limit has finished. It ends with its
        // last sound.
        const double now = FrameMs(s_.frame);
        if (now >= kMaxSeconds * 1000.0)
        {
            finished = !AnySounding() && now - FrameMs(s_.last_sound_frame) > kHoldSeconds * 1000.0;
            s_.out.truncated = !finished;
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

    // Each track takes the changes of its channel.
    s_.out.stretches.resize(std::max(s_.out.stretches.size(), track_channels_.size()));
    s_.out.mix.resize(s_.out.stretches.size());
    for (std::size_t t = 0; t < track_channels_.size(); t++)
    {
        if (track_channels_[t] < s_.channel_mix.size())
        {
            s_.out.mix[t] = s_.channel_mix[track_channels_[t]];
        }
    }

    return std::move(s_.out);
}

} // namespace

Performance Perform(const SequenceFiles& files, const PerformOptions& options)
{
    if (files.song == nullptr && files.effects == nullptr)
    {
        throw std::invalid_argument("the files hold no song or sound effect");
    }

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
        Performer performer(files, options, LevelScale(peak), part_limits, loop_choices);
        performance = performer.Run();
        applied = peak;
        loop_choices = performer.LoopChoices();

        bool parts_fit = true;
        if (!limited)
        {
            if (const auto fitted = FittedParts(performance, performer.PartsUsed(), SpareChannels::kForLayers))
            {
                part_limits = *fitted;
                limited = true;
                parts_fit = false;
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

} // namespace ntdlib::sequence_export::dse
