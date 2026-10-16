// SPDX-License-Identifier: MIT

// Sequence execution (see performer.h). A core of nw::snd (nwsnd/) plays the sequence. The performer follows the core's
// hooks and records the notes and controllers. Addresses without another source come from the Wii U's nw::snd in Animal
// Crossing: amiibo Festival's executable. The Switch's nn::atk in WarioWare: Get It Together!'s executable is the same
// player with 5 ms frames, arguments in the file's byte order and the Switch's audio renderer. Settings selects the
// console.

#include "ntdlib/sequence_export/performer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/util.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/envelope.h"
#include "ntdlib/sequence_export/filter.h"
#include "ntdlib/sequence_export/instruments.h"
#include "ntdlib/sequence_export/passes.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/recording_engine.h"
#include "ntdlib/sequence_export/resampling.h"
#include "ntdlib/sequence_export/settings.h"
#include "seqlib/midi/recorder.h"
#include "seqlib/sf2/units.h"
#include "xsflib/binary.h"

namespace ntdlib::sequence_export
{

using sar::Pcm;
using sar::Sequence;
using sar::SoundArchive;
using sar::SoundInfo;
using sar::VelocityRegion;
using seqlib::sf2::SoundFontPan;
using seqlib::sf2::ToTimecents;

namespace
{

constexpr double kNever = 1e300;       // sentinel time in milliseconds
constexpr double kMaxTailMs = 10000.0; // longest release tail kept after the end

// Short loops, such as sustained sound effects, repeat until the performance reaches this duration. The loop markers go
// around the pass that ChooseSteadyPass selects.
constexpr double kMinLoopSeconds = 10.0;

// Longest performance, in seconds of music.
constexpr double kMaxSeconds = 900.0;

// Maximum duration after a held sound stops changing. This covers tracks waiting on endless looping notes and sequences
// whose timing has stopped.
constexpr double kHoldSeconds = 10.0;

// Shortest note that a track waiting it out holds until the game stops the sound. That's 200 beats at the default
// timebase, minutes at a typical tempo. Games use such notes for sounds they stop themselves.
constexpr int32_t kHeldTicks = 9600;

// The notes that a core has room for at once in a conversion that keeps every note. The 3DS's core always has this many
// channels.
constexpr int kEveryNote = 4096;

// A one-shot wave that runs out in frame `f` keeps its channel for the ticks of frames up to `f + ReportFrames`. The
// Switch's renderer reports how far a voice has played two frames late. A one-shot wave's end reaches nn::atk two
// frames after the frame that reads the wave's last sample (LowLevelVoiceAllocator::UpdateAllVoiceStatus). AX on the
// Wii U stops a voice in the frame that reads its last sample. On the 3DS and the Wii, the next frame's voice update
// finds the end, after that frame's ticks.
uint64_t ReportFrames(const Settings& settings)
{
    switch (settings.engine)
    {
    case Engine::kSwitch:
        return 2;
    case Engine::kCtr:
    case Engine::kRevolution:
        return 1;
    default:
        return 0;
    }
}

// The 3DS's nw::snd counts ticks in thousandths of the ARM's cycles. A sound frame is 160 samples of the DSP's clock,
// 160 * 8192 cycles (SequenceSoundPlayer::UpdateTick, code.bin 0x31ff50; see Performer::NextCoreTick).
constexpr float kArmClock = 268111856.0f;
constexpr uint64_t kFrameMilliCycles = 1310720000;

// The units of a sound frame that ticks are scheduled in: 1/65536 ms on the Wii U and the Switch, in sound frames of
// Settings::frame_ms (SequenceSoundPlayer::UpdateTick, 0x022B7368), or the 3DS's thousandths of ARM cycles.
uint64_t FrameUnits(const Settings& settings)
{
    if (settings.engine == Engine::kCtr)
    {
        return kFrameMilliCycles;
    }

    return static_cast<uint64_t>(settings.frame_ms * 65536.0);
}

// A time in the units of FrameUnits, in milliseconds.
double UnitsToMs(uint64_t units, const Settings& settings)
{
    if (settings.engine == Engine::kCtr)
    {
        return static_cast<double>(units) / kArmClock;
    }

    return static_cast<double>(units) / 65536.0;
}

// The Wii's build of October 2006 counts time in 416ths of a tick. A tick lasts 416 / tempo frames.
constexpr int kTickUnits = 416;

// MML gives LFO delays and portamento times in 5 ms units.
constexpr int kMmlUnitMs = 5;

// The deepest call stack of any console's track. Track::calls holds a copy of the core's stack.
constexpr int kCallDepth = std::max({nwsnd::CafePolicy::kCallStackDepth, nwsnd::NxPolicy::kCallStackDepth,
                                     nwsnd::CtrPolicy::kCallStackDepth, nwsnd::RevolutionPolicy::kCallStackDepth});

// A track's LFOs: the plain MML commands set the first, and extended ones (0xf0 0xa0 onwards) the others.
constexpr int kLfoCount = 4;

// The rules of the main loop's passes that this performer plays by (passes.h). A track's first part starts with the SFZ
// low-pass and biquad off. A preset at the loop's first tick doesn't count among the settings there.
const LoopRules kLoopRules{{kSfzLowPassEvent, kSfzBiquadEvent}, false};

// A linear ramp over a number of ticks (nw::snd MoveValue).
class MoveValue
{
public:
    // Sets the value immediately.
    void Init(int v)
    {
        start_ = target_ = v;
        duration_ = counter_ = 0;
    }

    int Get() const
    {
        if (counter_ >= duration_)
        {
            return target_;
        }

        return (target_ - start_) * counter_ / duration_ + start_;
    }

    bool Moving() const
    {
        return counter_ < duration_;
    }

    // Takes the state of `ramp`, a ramp of the core. A negative count there applies at once.
    template <typename T>
    void Take(const nwsnd::MoveValue<T>& ramp)
    {
        start_ = ramp.start;
        target_ = ramp.target;
        duration_ = std::max(0, static_cast<int>(ramp.duration));
        counter_ = ramp.counter;
    }

private:
    int start_ = 0, target_ = 0, duration_ = 0, counter_ = 0;
};

// On the Wii: the swing and the cut that fit the level of a MIDI note whose volume LFO the voice holds at full level
// (PresetKey::lfo_swing and lfo_cut, MidiRecorder::VolumeLfoFits).
struct VolumeLfoFit
{
    bool operator==(const VolumeLfoFit&) const = default;

    uint8_t swing = 16;
    int16_t cut = 0;
};

// The weighted sums of a MIDI note's frames with a volume LFO (MidiRecorder::OnVolumeLfo): the weights, the level that
// the SoundFont and SFZ play the LFO at, its square, the level that the game plays it at, and the product of the two
// levels. `held`: in some frame, the voice held the note at full level.
struct VolumeLfoSums
{
    double w = 0.0, x = 0.0, xx = 0.0, y = 0.0, xy = 0.0;
    bool held = false;
};

// The MIDI note that the performer plays a channel as.
struct MidiNote
{
    uint8_t key = 0;        // the key of the MIDI note
    bool on = false;        // the MIDI note is on
    uint8_t velocity = 0;   // the MIDI note's velocity
    int start_velocity = 0; // the velocity nw::snd played the MIDI note's start at
    bool liftable = false;  // the MIDI note started alone on its part: a later pass may start it higher
    uint8_t part = 0;       // the part of the track the MIDI note is on (TrackEvent::part)
    uint8_t volume = 127;   // volume of the region selected for the MIDI note (see Legato)
    uint32_t id = 0;        // the MIDI note's id (TrackEvent::note)
    uint64_t frame = 0;     // the sound frame the MIDI note started in
    std::optional<int16_t> attack_timecents; // the MIDI note's lengthened attack (PresetKey::attack_timecents)
    PresetKey preset;                        // the MIDI note's preset, for its LFO settings (CheckLfo)
    bool lfo_moved = false;                  // reported as heard with other LFO settings than its preset has
    double quiet_ms = kNever;                // time the held envelope sinks below hearing (QuietMs)
    double faded_ms = kNever;     // time the MIDI note of a channel that its length can't release ends (FadedMs)
    bool sunk = false;            // its MIDI note ended below hearing (MidiRecorder::OnLengthUpdated)
    int16_t lift = 0;             // the Wii: its zones' lift in centibels (PresetKey::lift, ZoneLift)
    float level = -1.0f;          // the Wii: its level before the track's volumes, -1 if CC11 sets it (CheckFullLevel)
    bool over_full_level = false; // reported as played at another level than in the game (CheckFullLevel)
    bool lfo_above_full = false;  // the Wii: reported with a volume LFO above full level (FollowGainAboveFull)
    VolumeLfoFit lfo_fit;         // the Wii: its volume LFO's fit from an earlier pass (VolumeLfoFits)

    // A MIDI note whose pitch LFO changes while it sounds plays on a part of its own (MidiNoteOn). Its preset has no
    // LFO. The pitch bends of the part add the value of the core channel's pitch LFO in each frame (PartPitch).
    // `lfo_crowded`: no part of its own was free, and the preset keeps the LFO. `lfo_random`: the note has reported
    // the random curve of its pitch bends (TakeCoreLfos).
    bool lfo_bends = false;
    bool lfo_crowded = false;
    bool lfo_random = false;
};

// A sounding nw::snd channel and its corresponding MIDI note.
struct Note
{
    // Returns when a one-shot wave's channel is freed, or kNever for a looping wave. The core's position model gives
    // the frame in which the wave runs out (`core_end`). The console reports the end ReportFrames later. The sound
    // library frees the channel before the ticks of the next frame.
    double NaturalEndMs() const
    {
        return one_shot && core_end && *core_end != WaveEnd::kNoEnd
                   ? static_cast<double>(*core_end + 1 + ReportFrames(*settings)) * settings->frame_ms
                   : kNever;
    }

    // Returns the earlier of wave completion and release completion.
    double EndMs() const
    {
        return std::min(NaturalEndMs(), release_end_ms);
    }

    // Returns when the channel's sound ends: where its fade ended its MIDI note (`sunk`), and otherwise when the
    // channel ends.
    double SoundEndMs() const
    {
        return midi.sunk ? midi.faded_ms : EndMs();
    }

    // Returns when the held envelope sinks below hearing (kInaudibleSustain), or kNever if it doesn't.
    double QuietMs() const
    {
        return AttenuationMs(kInaudibleSustain);
    }

    // Returns when the held envelope fades to kFadedAttenuation. If its sustain level stays above that, returns when it
    // sinks below hearing instead.
    double FadedMs() const
    {
        const double faded = AttenuationMs(kFadedAttenuation);
        return faded < kNever ? faded : QuietMs();
    }

    // Returns when the held envelope falls to `attenuation` centibels, or kNever if it doesn't.
    double AttenuationMs(int attenuation) const
    {
        const int64_t frames = FramesToLevel(env, -static_cast<float>(attenuation), *settings);
        return frames >= 0 ? static_cast<double>(on_frame + static_cast<uint64_t>(frames) + 1) * settings->frame_ms
                           : kNever;
    }

    // True if the one-shot wave has run out by the end of frame `frame`.
    bool WaveEndedBy(uint64_t frame) const
    {
        return one_shot && core_end && *core_end <= frame;
    }

    // Like Channel::GetSweepValue (0x022C4204), calculates the sweep offset in semitones for sound frame `frame`. Timed
    // sweeps advance Settings::env_step_ms per frame after pitch calculation, from the initial frame on. Note-length
    // sweeps advance once per tick.
    float SweepAt(uint64_t frame) const
    {
        const int total = static_cast<int>(sweep_length);
        const int gone =
            sweep_in_ms ? static_cast<int>(std::min<uint64_t>((frame - sweep_frame) * settings->env_step_ms, total))
                        : static_cast<int>(std::min(sweep_ticks, sweep_length));
        if (sweep == 0.0f || gone >= total)
        {
            return 0.0f;
        }

        return sweep * static_cast<float>(total - gone) / static_cast<float>(total);
    }

    // The value of the core channel's pitch LFO in sound frame `frame`, in semitones (lfo_pitch). A frame that the core
    // hasn't run yet gives the latest value.
    float LfoPitchAt(uint64_t frame) const
    {
        if (lfo_pitch.empty())
        {
            return 0.0f;
        }

        const uint64_t index = frame > lfo_frame ? frame - lfo_frame : 0;
        return lfo_pitch[std::min<std::size_t>(index, lfo_pitch.size() - 1)];
    }

    // True if `other`'s sweep moves its pitch the same way as this note's sweep from sound frame `frame` on: neither
    // sweeps any more, or both sweep alike.
    bool SweepsLike(const Note& other, uint64_t frame) const
    {
        if (SweepAt(frame) == 0.0f && other.SweepAt(frame) == 0.0f)
        {
            return true;
        }

        return sweep == other.sweep && sweep_length == other.sweep_length && sweep_in_ms == other.sweep_in_ms &&
               (sweep_in_ms ? sweep_frame == other.sweep_frame : sweep_ticks == other.sweep_ticks);
    }

    // True if the channel has a MIDI note. The game plays a note at velocity 0 silently, and MIDI gets no note for it.
    // The MIDI note of a channel that its length can't release ends where the channel's fade ends it (`sunk`). The
    // checks of a part's notes leave out a channel without a MIDI note (Alone, PartSounds, PartPitch and others).
    // Otherwise a silent channel's sweep and settings would move the notes that sound on the same part.
    bool HasMidiNote() const
    {
        return midi.id != 0 && !midi.sunk;
    }

    uint8_t key = 0;             // the key nw::snd plays
    MidiNote midi;               // the MIDI note that the performer plays the channel as
    uint8_t region_volume = 127; // volume of the region the channel started on (tie and mono keep it; see Legato)
    int32_t length = -1;         // ticks until note-off; -1: none
    bool released = false;

    // A note-off came before the channel's first update (Settings::note_off_waits_for_update). nn::atk makes the
    // note-off in that update, after the envelope's first step (Channel::NoteOff, 0xc23a70). The note sounds for its
    // first frame (Performer::ReleasePending).
    bool release_pending = false;
    bool ignore_note_off = false;
    uint8_t key_group = 0;
    uint64_t on_frame = 0; // the sound frame the note started in
    EnvelopeValues env;
    double release_end_ms = kNever;
    uint64_t released_frame = 0; // the sound frame the note was last released in
    float release_level = 0;     // 0.1 dB when released
    float release_rate = 0;      // 0.1 dB per millisecond

    // Whether the wave is a one-shot. The core's position model gives the frame in which a one-shot wave runs out: at
    // the note's start, a prediction at the key's pitch (RecordingEngine::PredictEnd), and once the voice plays, the
    // frame at the voice's rate (Performer::TakeCoreWaves).
    bool one_shot = false;
    std::optional<uint64_t> core_end;

    // The core's channel that plays the note, and the key of its voice (RecordingEngine::PositionKey), or kNoChannel
    // and -1. The channel's envelope gives the level at a release (Performer::ReleaseNote).
    nwsnd::ChannelId core_channel = nwsnd::kNoChannel;
    int core_key = -1;

    // The Wii: the channel's volume before the track's volumes (Channel::SetInitVolume): velocity squared, and the
    // region's volume for a new note. The voice clamps it times the track's volumes to 1 (Voice::SetVolume, main.dol
    // 0x8004c830).
    float init_volume = 1.0f;
    bool stereo = false;

    // The console's settings (Performer::NoteOn sets them).
    const Settings* settings = nullptr;

    // Instrument and initial pan at note-on. Tie/mono continue the channel with these values even if the track changes
    // them.
    uint8_t bank_slot = 0;
    uint32_t program = 0;
    int8_t init_pan = 0;

    // The track's bend when the track detached the note (MidiRecorder::DetachAll).
    float bend = 0.0f;

    // Sweep/portamento starts `sweep` semitones from the target and returns linearly.
    float sweep = 0.0f;
    double sweep_length = 0; // ticks (-1 for a note without a length), or milliseconds when sweep_in_ms
    double sweep_ticks = 0;
    uint64_t sweep_frame = 0; // the sound frame a timed sweep starts in
    bool sweep_in_ms = false;

    // For a MIDI note with lfo_bends: the value of the core channel's pitch LFO in each sound frame from `lfo_frame` on
    // (Performer::TakeCoreLfos).
    std::vector<float> lfo_pitch;
    uint64_t lfo_frame = 0;
};

// Call stack entry: a return address, or a loop start and remaining count.
struct CallEntry
{
    bool is_loop = false;
    uint8_t count = 0;
    uint32_t address = 0;
};

// The settings that the MML commands give a track LFO: CurveLfoParam and its type.
struct LfoSetting
{
    uint8_t depth = 0;   // the LFO's depth is this / 128
    float speed = 6.25f; // Hz
    int32_t delay = 0;   // milliseconds, unsigned in nw::snd: a negative delay never ends
    uint8_t range = 1;
    uint8_t curve = 0; // 0 sine, 1 triangle, 2 saw, 3 square, 4 random
    uint8_t phase = 0; // the cycle's start, in 127ths
    uint8_t type = 0;  // 0 pitch, 1 volume, 2 pan
};

// Returns the CC1 value that holds the depth of LFO `lfo`. A volume LFO changes a channel's level by
// CalcVolumeRatio(6 * its value) dB. That can't rise more than 6 dB. The SoundFont and SFZ LFOs swing as far up as
// down. A deeper LFO therefore keeps to a 6 dB swing and dips less than in the game. The game's LFO value reaches depth
// / 128 * range. Both outputs swing 12 * range * CC1 / 128 dB. A 6 dB swing takes CC1 = 64 / range.
int ModulationValue(const LfoSetting& lfo)
{
    int mod = std::min(127, (lfo.depth + 1) / 2);
    if (lfo.type == 1 && lfo.depth * lfo.range > 128)
    {
        mod = std::min(mod, static_cast<int>(64.0 / lfo.range));
    }

    return mod;
}

// The legato gain of the notes on a part that their track has left (MidiRecorder::Switch).
struct LeftPart
{
    double gain = 1.0;
};

// What the performer has sent to a track's MIDI channels.
struct TrackMidi
{
    // The SoundFont approximation of the track's filters.
    SoundFontFilter filter;

    // Whether the track has played a note. The track's MIDI controllers go out from then on.
    bool played = false;

    // The part that the track's new notes and changes go to (MidiRecorder::Switch), and each part's last values.
    seqlib::midi::TrackOutput output;

    // Legato velocity relative to the continuing MIDI note, and key offset in semitones (see Legato).
    double legato_gain = 1.0;
    float legato_bend = 0.0f;

    // The parts the track has left.
    std::array<LeftPart, 16> left{};

    // The track's volume, pan and sends at the end of its last tick (Performance::mix).
    std::array<int16_t, kMixSettingCount> mix = {127, 0, 0, 0, 0};
};

// A sequence track: its state and settings from the core's track (Performer::TakeCoreTrack), its channels, and the
// records of the loop analysis (Performer::OnLoop).
struct Track
{
    // The pitch bend in semitones.
    float Bend() const
    {
        return static_cast<float>(bend.Get()) / 128.0f * bend_range;
    }

    // Resets to the initial sound state but keeps track allocation.
    void Init()
    {
        const bool was_allocated = allocated;
        const Engine was_engine = engine;
        *this = Track{};
        allocated = was_allocated;
        engine = was_engine;

        vol.Init(127);
        volume2.Init(127);
        pan.Init(0);
        bend.Init(0);
        vars.fill(-1);
    }

    bool allocated = false;
    bool open = false;
    uint32_t current = 0;  // offset of the next command
    bool note_wait = true; // a note makes the track wait for its length (MML notewait)
    bool tie = false;
    bool mono = false;

    // The offset of the command that the track runs, and how the track's next stretch of commands begins, if its next
    // command begins one (Performance::stretches).
    uint32_t command = 0;
    std::optional<Entry> next_stretch = Entry::kOpen;
    std::array<CallEntry, kCallDepth> calls{};
    int call_depth = 0;
    int32_t wait = 0; // ticks until the next command
    bool mute = false;
    bool note_finish_wait = false; // waiting for the track's notes to end
    bool held_wait = false;        // waiting out a note of kHeldTicks or more
    bool holds = false;            // the track's last note is one of kHeldTicks or more
    bool damper = false;
    uint8_t bank_slot = 0;
    uint32_t program = 0;
    std::array<LfoSetting, kLfoCount> lfos{};
    MoveValue vol, volume2, pan, bend;
    uint8_t velocity_range = 127; // velocities are scaled by this / 127
    uint8_t bend_range = 2;
    int8_t init_pan = 0;
    uint8_t attack = 0xff, decay = 0xff, sustain = 0xff, release = 0xff, hold = 0xff;
    uint8_t main_send = 127;
    Engine engine = Engine::kCafe; // the console's engine (Performer sets it)
    uint8_t fx_send_a = 0, fx_send_b = 0, fx_send_c = 0;
    std::array<int16_t, 16> vars{}; // track variables, 32-47 to commands
    std::vector<Note> notes;        // the track's channels, newest last
    std::vector<Note> detached;     // released channels no longer on the track, still sounding

    // Last tick that began a wait (OnLoop), and counts of channels/legatos started in `note_tick` (NoteOn).
    std::optional<uint32_t> last_wait;
    uint32_t note_tick = 0;
    int tick_notes = 0, tick_legatos = 0;

    // Track filter arguments: low-pass (0xd8, initially off), biquad type/value (0xb4/0xb5), and their SoundFont
    // approximation.
    int32_t low_pass = 0x40;
    int32_t biquad_type = 0, biquad_value = 0;

    // Last tick that changed the track's sound, used for main-loop detection (OnLoop). This must agree across Perform's
    // passes even when legato MIDI output differs (Legato). Every legato counts as a change. The comparison takes the
    // game's unrounded level before legato gain, MIDI scaling or clamping; legato gain can make CC11 steps finer. The
    // comparison also takes the filter cutoff independently of controller use, and the pitch without the legato key
    // offset. change_level, change_filter and change_pitch store those values.
    std::optional<uint32_t> last_change;
    double change_level = -1.0;
    int change_filter = -1;
    float change_pitch = 0.0f;
    bool idle = false;   // its last pass of a loop changed nothing; inner and Leavable passes don't count (OnLoop)
    uint32_t opened = 0; // the tick it last opened at

    // When the track last changed a player or global variable that another open track has read and can read again. The
    // value is the number of commands run by then (Performer::commands_run_). Such a change counts as a change of the
    // sound for main-loop detection (OnLoop). The other track may play because of it.
    std::optional<uint64_t> last_signal;

    // The number of commands run (Performer::commands_run_) after the track's last conditional jump, call, return or
    // fin, and after its last note, or 0 before the first (see Leavable).
    uint64_t leave_at = 0;
    uint64_t note_at = 0;

    // The same number after the track last jumped back to each loop start, and the starts of the track's inner loops:
    // loops that the track jumped back to during a pass of another loop (OnLoop).
    std::map<uint32_t, uint64_t> jump_backs;
    std::set<uint32_t> inner_loops;
    double last_pass_ms = 0.0; // the time of the track's last pass of a loop that isn't inner (OnLoop)

    // A serial number for each entry of the call stack, and how many entries have one (Performer::NumberCalls).
    std::array<uint64_t, kCallDepth> call_ids{};
    int numbered_calls = 0;
};

// Builds a preset key for a new note on `tr`. Envelope values outside 0-127 leave the bank value unchanged. Discards
// settings for ineffective LFOs to avoid duplicate presets. These include unsupported types and negative delays.
// nw::snd keeps a delay as unsigned milliseconds. A negative delay therefore wraps to a huge one, and the LFO never
// starts.
PresetKey TrackPresetKey(const Track& tr)
{
    PresetKey pk;
    pk.bank_slot = tr.bank_slot;
    pk.program = tr.program;
    pk.attack = tr.attack <= 127 ? tr.attack : 0xff;
    pk.decay = tr.decay <= 127 ? tr.decay : 0xff;
    pk.sustain = tr.sustain <= 127 ? tr.sustain : 0xff;
    pk.release = tr.release <= 127 ? tr.release : 0xff;
    pk.hold = tr.hold <= 127 ? tr.hold : 0xff;
    const LfoSetting& lfo = tr.lfos[0];
    if (lfo.type <= 2 && lfo.delay >= 0)
    {
        pk.lfo_type = lfo.type;
        pk.lfo_range = lfo.range;
        pk.lfo_speed = lfo.speed;
        pk.lfo_delay = static_cast<uint32_t>(lfo.delay);
        if (tr.engine == Engine::kCtr || tr.engine == Engine::kRevolution)
        {
            // The 3DS's and the Wii's keys hold the bytecode's units: the speed, and the delay in 5 ms steps. Both are
            // exact: the track keeps the speed times 0.390625 and the delay times 5.
            pk.lfo_speed = lfo.speed / 0.390625f;
            pk.lfo_delay = static_cast<uint32_t>(lfo.delay / kMmlUnitMs);
        }
    }
    else
    {
        pk.lfo_type = 3;
    }

    pk.init_pan = tr.init_pan;

    return pk;
}

// Builds channel `n`'s current preset key from track `tr`. The key keeps the channel's original instrument and initial
// pan.
PresetKey ChannelPresetKey(const Track& tr, const Note& n)
{
    PresetKey pk = TrackPresetKey(tr);
    pk.bank_slot = n.bank_slot;
    pk.program = n.program;
    pk.init_pan = n.init_pan;

    return pk;
}

// True if `a` and `b` have the same LFO settings.
bool SameLfo(const PresetKey& a, const PresetKey& b)
{
    return a.lfo_type == b.lfo_type && a.lfo_range == b.lfo_range && a.lfo_speed == b.lfo_speed &&
           a.lfo_delay == b.lfo_delay;
}

// `key` with the LFO settings of `lfo`.
PresetKey WithLfo(PresetKey key, const PresetKey& lfo)
{
    key.lfo_type = lfo.lfo_type;
    key.lfo_range = lfo.lfo_range;
    key.lfo_speed = lfo.lfo_speed;
    key.lfo_delay = lfo.lfo_delay;

    return key;
}

// `key` without an LFO: with the settings of a track whose LFO has no effect (TrackPresetKey).
PresetKey WithoutLfo(const PresetKey& key)
{
    PresetKey none;
    none.lfo_type = 3;

    return WithLfo(key, none);
}

// The regions and decoded waves that the performer plays a sound's notes with. The same region always gives the same
// pointer. Perform's overloads give one over a sound archive's banks: over a BankSet, or over the Wii's RsarBankSet
// with its regions in sar's form.
class Regions
{
public:
    virtual ~Regions() = default;

    // The velocity region of instrument `program` of the bank in `slot` that plays `key` at `velocity`, or nullptr.
    virtual const VelocityRegion* FindRegion(int slot, uint32_t program, int key, int velocity) const = 0;

    // Wave `wave_id_index` of the bank in `slot`, decoded, or nullptr.
    virtual const Pcm* GetWave(int slot, uint32_t wave_id_index) = 0;
};

// The regions and waves of a CSAR or FSAR archive's banks.
class SarRegions final : public Regions
{
public:
    explicit SarRegions(BankSet& banks) : banks_(banks)
    {
    }

    const VelocityRegion* FindRegion(int slot, uint32_t program, int key, int velocity) const override
    {
        return banks_.FindRegion(slot, program, key, velocity);
    }

    const Pcm* GetWave(int slot, uint32_t wave_id_index) override
    {
        return banks_.GetWave(slot, wave_id_index);
    }

private:
    BankSet& banks_;
};

// The regions and waves of a Wii sound's bank, in sar's form. Each region is converted once. A region keeps the same
// pointer. The waves are in sar's form already (wave::Pcm). Builds of NW4R SND before 2008 have no hold, note-off types
// or key groups (Mario Kart Wii's Bank::NoteOn, 0x8008d41c and 0x8008d490).
class RsarRegions final : public Regions
{
public:
    RsarRegions(RsarBankSet& banks, bool since_2008) : banks_(banks), since_2008_(since_2008)
    {
    }

    const VelocityRegion* FindRegion(int, uint32_t program, int key, int velocity) const override
    {
        const rsar::Region* region = banks_.FindRegion(program, key, velocity);
        if (!region)
        {
            return nullptr;
        }

        const auto [it, added] = regions_.try_emplace(region);
        if (added)
        {
            VelocityRegion& v = it->second;
            v.wave_id_index = static_cast<uint32_t>(region->wave_index);
            v.original_key = region->original_key;
            v.volume = region->volume;
            v.pan = region->pan;
            v.pitch = region->tune;
            v.ignore_note_off = since_2008_ && region->note_off_type == 1;
            v.key_group = since_2008_ ? region->key_group : 0;
            const EnvelopeValues env = RegionEnvelope(*region, since_2008_);
            v.adshr = {static_cast<uint8_t>(env.attack), static_cast<uint8_t>(env.decay),
                       static_cast<uint8_t>(env.sustain), static_cast<uint8_t>(env.hold),
                       static_cast<uint8_t>(env.release)};
        }

        return &it->second;
    }

    const Pcm* GetWave(int, uint32_t wave_id_index) override
    {
        return banks_.GetWave(wave_id_index);
    }

private:
    RsarBankSet& banks_;
    bool since_2008_;
    mutable std::map<const rsar::Region*, VelocityRegion> regions_;
};

class Performer;

// The core: a console's sequence player on nw::snd's model (nwsnd/). The core plays the sequence, and the performer
// follows it through the hooks below: each command, its arguments' random draws and variable reads, and each note
// command. The performer runs the core's tick of each track, and the core's frames move on with the frames of the
// performer's ticks. `VoiceLayer` gives the console's voices (see recording_engine.h).
template <typename Console, template <typename> class VoiceLayer = MultiVoiceLayer>
class SequenceCore : public RecordingEngine<Console, SequenceCore<Console, VoiceLayer>, Regions, VoiceLayer>
{
public:
    using Base = RecordingEngine<Console, SequenceCore<Console, VoiceLayer>, Regions, VoiceLayer>;
    using Player = typename Base::Player;
    using Channel = typename Base::Channel;
    using Base::Base;

    // A track that runs the parse limit's commands in a tick without a wait reads on in the next tick. With
    // `stop_runaway_tracks`, the track stops once it has run on that way for kHoldSeconds. Without, it stops at once.
    // `now_ms` is the tick's time. The performer records a track that stops.
    bool OnParseLimit(Player& player, int track);

    // A track that the policy's parse limit ends, on the Wii. The performer records the stop in the same way as for
    // OnParseLimit.
    void OnParseLimitEnd(Player& player, int track);

    // The hooks that the performer follows (Performer::FollowCommandStart and the others).
    void OnCommandStart(Player& player, int track, uint32_t offset, uint32_t command, bool conditional);
    void OnCommand(Player& player, int track, uint32_t command, int32_t arg1, int32_t arg2);
    void OnExtendedCommand(Player& player, int track, uint32_t command, int32_t arg1, int32_t arg2);
    void OnRandomDraw(Player& player, int track);
    void OnVariableRead(Player& player, int track, int index);
    void OnNoteCommand(Player& player, int track, int key, int velocity, int32_t length);
    void OnNoteOn(Player& player, int track, Channel& channel);
    void OnLegato(Player& player, int track, Channel& channel);

    // A channel's release or note-off, and the end of the count of a track's lengths (Performer::FollowLengths).
    void OnRelease(Player& player, int track, Channel& channel);
    void OnLengthsCounted(Player& player, int track);

    // A note that finds no free channel, and a channel whose voice went to another note (RecordingEngine::NoteOn and
    // ChannelEvent).
    void OnNoChannel(int track);
    void OnChannelDropped(int track, Channel& channel);

    // The Wii: a frame in which a channel's volume LFO lifts the envelope's gain above full level
    // (RevolutionPolicy::UpdateChannel). The voice limits the gain to full level.
    void OnGainAboveFull(Channel& channel);

    // The Wii: a frame of a channel's volume LFO, with the envelope's level and the LFO's in decibels
    // (RevolutionPolicy::UpdateChannel).
    void OnVolumeLfo(Channel& channel, float envelope_db, float lfo_db);

    // A pass that stops the performance stops the track's tick at the jump. A copy of the performer then runs the rest
    // of the tick (Performer::PlayOn).
    bool StopsTick(Player& player, int track);

    // Whether the counted loop whose body starts at `address` loops for ever: the loops of Performer::SetEndlessLoops
    // do.
    bool LoopsForEver(const Player& player, int track, uint32_t address) const;

    // The build of the console's sound library that plays the sound. The Wii's channels depend on it
    // (RevolutionPolicy::UpdateChannel).
    const typename Console::Build& Build() const
    {
        return build;
    }

    bool stop_runaway_tracks = true;
    double now_ms = 0.0;

    // The performer that the hooks report to. The performer sets it before each run of the core's tracks. A copy of the
    // performer holds a copy of the core, and the copy sets the pointer to itself.
    Performer* performer = nullptr;

    typename Console::Build build{};

private:
    // Each track's last tick that hit the parse limit, and the time that its run of such ticks began.
    std::array<int64_t, 16> limit_ticks_ = []
    {
        std::array<int64_t, 16> ticks;
        ticks.fill(-2);
        return ticks;
    }();
    std::array<double, 16> run_start_ms_{};
};

// The cores of the Wii U, the Switch, the 3DS and the Wii.
using CafeCore = SequenceCore<nwsnd::CafePolicy>;
using NxCore = SequenceCore<nwsnd::NxPolicy>;
using CtrCore = SequenceCore<nwsnd::CtrPolicy, CtrVoiceLayer>;
using RevolutionCore = SequenceCore<nwsnd::RevolutionPolicy>;

// The performer's rules for recording MIDI (seqlib/midi/recorder.h). nw::snd gives a frame's notes the settings that
// the frame ends with. A setting therefore goes before every event of its tick but a sound off. A part switch drops the
// new part's last values. A pitch that moves 0.0005 semitones or less isn't sent.
seqlib::midi::RecorderOptions RecordingRules()
{
    seqlib::midi::RecorderOptions options;
    options.settings_before_all_events = true;
    options.pitch_tolerance = 0.0005f;

    return options;
}

// The last value of setting `kind` sent to the part that `output`'s track plays on, or -1 if none was.
int LastSent(const seqlib::midi::TrackOutput& output, EventKind kind)
{
    const auto sent = output.sent.find({output.part, kind});
    return sent != output.sent.end() ? sent->second : -1;
}

// When a tick comes: the sound frame it's processed in, and its time. The Wii's build of October 2006 counts time in
// 416ths of a tick (NextWiiCoreTick). There `ticks_left` is the number of the frame's ticks after this one, and
// `nominal_ms` the tick's time at MIDI's tempo.
struct TickTime
{
    uint64_t frame = 0;
    double ms = 0;
    int ticks_left = 0;
    double nominal_ms = 0;
};

// The recording of a sequence's performance: the MIDI events, and the parts of them that the passes use. The recorder's
// methods take the engine that it records, the Performer.
class MidiRecorder
{
public:
    MidiRecorder();

    // Channel `n` of track `t` enters its release in the current frame.
    void OnRelease(Performer& engine, int t, Note& n);

    // Channel `n` of track `t` enters its release a fraction `after` of a tick past the current tick: a note-off that
    // waited for the channel's first update (Settings::note_off_waits_for_update).
    void OnDeferredRelease(Performer& engine, int t, Note& n, double after);

    // Channel `n` of track `t` stops at once (Channel::Stop).
    void OnStop(Performer& engine, int t, Note& n);

    // Track `t` closes. OnTrackClosing comes before its channels are released, and OnTrackClosed after.
    void OnTrackClosing(Performer& engine, int t);
    void OnTrackClosed(Performer& engine, int t);

    // The frames between the current tick and `next` pass.
    void OnBetweenTicks(Performer& engine, const TickTime& next);

    // Channel `n` of track `t` ends: its one-shot wave or its release has finished.
    void OnEnd(Performer& engine, int t, Note& n);

    // Channel `n` of track `t` has counted down its length for the tick.
    void OnLengthUpdated(Performer& engine, int t, Note& n);

    // Track `t` has run command `cmd` with its first argument `arg1`.
    void OnCommand(Performer& engine, int t, uint32_t cmd, int32_t arg1);

    // The timebase has changed.
    void OnTimebase(Performer& engine);

    // Track `t` stops after running without waits (the policy's kParseLimit).
    void OnRunaway(Performer& engine, int t);

    // Track `t`'s commands for the tick have run, and its settings reach its channels.
    void OnTickEnd(Performer& engine, int t);

    // Records the changes of track `t`'s volume, pan and sends in the tick (Performance::mix).
    void RecordMix(const Performer& engine, int t);

    // Track `t` releases all its channels (SequenceTrack::ReleaseAllChannel). OnReleaseAll comes before the channels
    // are released, and OnReleasedAll after.
    void OnReleaseAll(Performer& engine, int t);
    void OnReleasedAll(Performer& engine, int t);

    // Track `t` stops all its channels at once, before they're freed (SequenceTrack::SetMute's mode 3).
    void OnStopAll(Performer& engine, int t);

    // Track `t` plays a note: the first hook of each note, before tie, mono or a new channel.
    void OnNoteOnStart(Performer& engine, int t);

    // Channel `n` of track `t` starts, at velocity `velocity` (the track's velocity range applied), on `region`.
    void OnNoteOn(Performer& engine, int t, Note& n, int velocity, const VelocityRegion* region, bool stereo);

    // A tie or mono note continues channel `n` of track `t` at `key` and `velocity` (Legato). `heard` is false when the
    // channel was below hearing. Returns whether the note plays.
    bool OnLegato(Performer& engine, int t, Note& n, int key, int velocity, bool past_cap, bool heard);

    // Starts/ends channel `n`'s MIDI note. The note plays `region` (null if none plays its key and velocity). Track
    // CC11 is multiplied by `gain` while the note plays (see Legato).
    void MidiNoteOn(Performer& engine, int t, Note& n, int velocity, const VelocityRegion* region, double gain = 1.0);

    void MidiNoteOff(Performer& engine, int t, Note& n);

    // Ends `n`'s MIDI note on release, stop or replacement. Removes unheard notes entirely (see Unheard).
    void EndMidiNote(Performer& engine, int t, Note& n);

    // Records the attack for released channel `n` if the game releases its MIDI note before the SoundFont attack ends,
    // more than 1 dB quieter than that attack has reached (EarlyReleases).
    void RecordEarlyRelease(Performer& engine, const Note& n);

    // Adds a frame of MIDI note `id`'s volume LFO to the note's sums. The envelope stood at `envelope_db` and the LFO
    // at `lfo_db`. The SoundFont and SFZ play the LFO `scale` times as deep (ModulationValue).
    void OnVolumeLfo(uint32_t id, float envelope_db, float lfo_db, double scale);

    // The fits of the MIDI notes whose volume LFO the voice holds at full level, by note id. Unlisted notes keep the
    // LFO's full swing.
    std::map<uint32_t, VolumeLfoFit> VolumeLfoFits() const;

    // Detaches track `t`'s channels. They keep sounding.
    void DetachAll(Performer& engine, int t);

    // After track `t` detaches its channels (DetachAll), moves the track to another part if they still sound on its
    // part. In the game they keep their settings. On a shared MIDI channel, the track's later changes would reach them.
    void LeaveTails(Performer& engine, int t);

    // Moves track `t`'s new notes and changes to a part on which nothing but `except` sounds, within its part limit.
    // The notes on the track's current part keep the settings they have. Returns false if every part sounds.
    bool Switch(Performer& engine, int t, const Note* except = nullptr);

    // Emits changed volume, pan, pitch and other controllers for track `t`.
    void UpdateOutputs(Performer& engine, int t);

    // Sends setting `kind` of track `t` at `value` to `part`, or to the part the track plays on, if that part's last
    // value is another (seqlib::midi::Recorder::SendSetting). Returns true if it was sent.
    bool SendSetting(Performer& engine, int t, EventKind kind, int value, int part = -1);

    // Updates Track::last_change for an event of `kind` that track `t` records. UpdateOutputs handles the three values
    // that can differ between passes: the expression, the filter and the pitch.
    void NoteChange(Performer& engine, int t, EventKind kind);

    // The pitch MIDI gives part `part` of track `t` in sound frame `frame`, in semitones. On the part the track plays
    // on, that's the track's bend and legato key offset with the newest sweep of its notes there that can be heard. A
    // part that the track has left follows the newest note there that can be heard: that note's bend, the offset of its
    // key from the key of its MIDI note, and the newest sweep on the part. Nothing if the part has no such note. A note
    // with lfo_bends adds its pitch LFO.
    std::optional<float> PartPitch(const Performer& engine, int t, uint8_t part, uint64_t frame) const;

    // Sends part `part` of track `t` its pitch in sound frame `frame` if that has changed: at the current tick, or for
    // a frame between ticks, `after` it by a fraction of a tick (TrackEvent::after). Returns true if it was sent.
    bool SendPitch(Performer& engine, int t, uint8_t part, uint64_t frame, double after = 0);

    // True if notes that track `t` has detached still sound on the part that the track plays on. That part takes the
    // track's later changes. No other part was free for the track (LeaveTails).
    bool TailsOnPart(const Performer& engine, int t) const;

    // Sends the pitches that timed sweeps reach in the sound frames between the current tick's frame and `next`'s
    // frame. A part whose pitch bends add a note's pitch LFO gets its pitch in the current tick's frame too.
    void SweepBetweenTicks(Performer& engine, const TickTime& next);

    // The Wii: reports the notes of track `t` that play at another level than in the game. The game limits a channel's
    // volume to full level (TrackLevel, ZoneLift).
    void CheckFullLevel(Performer& engine, int t);

    // At the end of a tick, reports each of track `t`'s notes whose LFO plays with other settings than its preset has.
    // Keeps the notes whose pitch LFO changes for a later pass (LfoChanges).
    void CheckLfo(Performer& engine, int t);

    // Gives the notes that track `t` struck in this tick presets with the track's LFO settings. This tick's preset
    // events are sent again in front of its notes.
    void GiveStruckNotesLfo(Performer& engine, int t);

    // Returns track `t`'s CC11 before legato gain, the level scale and the 127 limit (UpdateOutputs).
    double TrackLevel(const Performer& engine, int t) const;

    // The Wii: the centibels by which the zones of a note at `velocity` on track `t` play louder than their regions'
    // volumes (PresetKey::lift).
    int16_t ZoneLift(const Performer& engine, int t, int velocity) const;

    // Records the current tempo. At tempo or timebase 0, the sequence stops advancing (Run). MIDI keeps its previous
    // tempo. The Wii's build of October 2006 runs tempo / 416 ticks a frame (NextTick).
    void OnTempo(Performer& engine);

    // Records an event for track `t` at the current tick, on `part` or else the part the track plays on, and updates
    // Track::last_change.
    //
    // nw::snd applies track settings at frame end (SequenceTrack::UpdateChannelParam). A note therefore uses settings
    // from the end of its tick, including changes after its note command. Controller changes go before that tick's
    // notes/presets. A change replaces any earlier value for the same setting. SFZ depends on this order to select the
    // biquad filter at note-on.
    void Emit(Performer& engine, int t, TrackEvent e, int part = -1);

    // The SFZ files select a filtered or an unfiltered region at note-on, from the biquad's controller (sfz.cpp). The
    // game gives the notes that a sound frame starts the settings that the frame ends with. When a tick switches the
    // biquad on or off, each note that an earlier tick of its frame started on the part gets a copy of the controller
    // before it. The copies go before notes of earlier ticks only. A loop that starts in the frame therefore keeps its
    // events.
    void GiveTheFrameTheBiquad(Performer& engine, int t, const TrackEvent& e);

    // Counts an approximation required by MIDI or SoundFont.
    void Approximate(const char* what);

    // Records `now_ms_` for the new `tick_`, and its frame start as a fractional tick (Performance::frame_ticks). The
    // Wii's build of October 2006 runs a tick at the start of its frame. MIDI's tempo puts the tick at `nominal_ms_`,
    // up to a frame earlier.
    void RecordTickTime(Performer& engine);

    Performance out;

    // The note ids, the part switches, the parts used and their limits, the levels, and the approximations and
    // warnings.
    seqlib::midi::Recorder rec;

    std::array<bool, 16> filter_controller{};
    std::array<bool, 16> filter_moves{};
    std::map<uint32_t, int> start_velocities; // from an earlier pass's Rises
    std::map<uint32_t, int> rises;
    std::map<uint32_t, int16_t> attacks; // from an earlier pass's EarlyReleases
    std::map<uint32_t, int16_t> early_releases;
    std::map<uint32_t, VolumeLfoFit> lfo_fits; // from an earlier pass's VolumeLfoFits
    std::map<uint32_t, VolumeLfoSums> lfo_sums;
    std::set<uint32_t> lfo_notes; // from an earlier pass's LfoChanges
    std::set<uint32_t> lfo_changes;
    std::array<TrackMidi, 16> tracks{}; // what each track's MIDI channels have been sent
};

// How a track's next stretch of commands begins after `command` moved the track elsewhere (Performance::stretches).
Entry EntryAfter(uint32_t command)
{
    switch (command)
    {
    case 0x8a:
        return Entry::kCall;
    case 0xfd:
        return Entry::kReturn;
    case 0xfc:
        return Entry::kLoop;
    default:
        return Entry::kJump;
    }
}

// A note command that waits for the core's verdict on its channel (Performer::FollowNoteCommand), and where the command
// lies.
struct PendingNote
{
    int t = 0;
    int key = 0;
    int velocity = 0;
    int32_t length = 0;
    NoteCommand command;
};

// Executes a sequence tick by tick, following nw::snd::SequenceSoundPlayer, and records its output.
class Performer
{
    template <typename Console, template <typename> class VoiceLayer>
    friend class SequenceCore;
    friend class MidiRecorder;

public:
    // What MarkSteadyPass selected (passes.h).
    using LoopChoice = sequence_export::LoopChoice;

    // `filter_controller` selects tracks with controller-driven filters (FilterMoves). `start_velocities` gives the
    // velocity to start MIDI notes at, by note id, from an earlier pass (Rises), and `attacks` the attacks of MIDI
    // notes the game releases during their attack (EarlyReleases). `lfo_fits` gives the fits of the MIDI notes whose
    // volume LFO the voice holds at full level (VolumeLfoFits). `lfo_notes` gives the MIDI notes whose pitch LFO
    // changes while they sound (LfoChanges). CC11 is multiplied by `level_scale` (LevelPeak). Each track gets at most
    // `part_limits` parts (Switch). The main loop's choices in `loop_choices` come from an earlier pass (LoopChoices).
    // ChooseSteadyPass selects after them.
    Performer(const Settings& settings, const SoundInfo& sound, const Sequence& seq, Regions& banks,
              const PerformOptions& options, const std::array<bool, 16>& filter_controller,
              const std::map<uint32_t, int>& start_velocities, const std::map<uint32_t, int16_t>& attacks,
              const std::map<uint32_t, VolumeLfoFit>& lfo_fits, const std::set<uint32_t>& lfo_notes, double level_scale,
              const std::array<uint8_t, 16>& part_limits, const std::vector<LoopChoice>& loop_choices)
        : settings_(settings), sound_(sound), seq_(seq), banks_(banks), options_(options), replay_(loop_choices)
    {
        midi_.rec.SetLevelScale(level_scale);
        midi_.filter_controller = filter_controller;
        midi_.start_velocities = start_velocities;
        midi_.attacks = attacks;
        midi_.lfo_fits = lfo_fits;
        midi_.lfo_notes = lfo_notes;
        midi_.rec.SetPartLimits(part_limits);
        for (Track& tr : tracks_)
        {
            tr.engine = settings_.engine;
        }

        player_vars_.fill(-1);
        if (!Wii())
        {
            global_vars_.fill(-1); // on the Wii, 0 until the game first sets one (SeqPlayer::SetGlobalVariable)
        }

        for (const auto& [index, value] : options.variables)
        {
            if (index < 16)
            {
                player_vars_[index] = value;
            }
            else if (index < 32)
            {
                global_vars_[index - 16] = value;
            }
        }

        // The game's changes come in the order of their frames.
        std::ranges::stable_sort(options_.changes, {}, &VariableChange::frame);

        main_volume_.Init(127);

        // The Wii's stereo waves play in balance mode. The builds of 2006 have only dual mode.
        if (Wii())
        {
            balance_ = !settings_.wii_build.October2006() && sound.pan_mode == 1;
            midi_.out.since_2008 = settings_.wii_build.Since2008();
            midi_.out.balance = balance_;
        }

        // The core plays the sequence (MakeCore). On the Wii U and the Switch, each channel has a voice. With voice
        // drops, the cores of the Wii U, the Switch and the Wii have the game's voices. Without, the cores have room
        // for kEveryNote notes at once. With voice drops, the Wii's core has NW4R SND's channels: one per voice, and
        // one more. The 3DS's core has room for kEveryNote channels. A channel there plays only while it holds a voice.
        // Only the voices run out. With voice drops, the 3DS's core has the game's voices. A new note then takes the
        // voice of the lowest priority.
        const int count = VoiceLimit() > 0 ? VoiceLimit() : kEveryNote;
        if (settings_.engine == Engine::kCafe)
        {
            MakeCore<CafeCore>(banks, count, count);
        }
        else if (settings_.engine == Engine::kSwitch)
        {
            MakeCore<NxCore>(banks, count, count);
        }
        else if (settings_.engine == Engine::kCtr)
        {
            MakeCore<CtrCore>(banks, kEveryNote, VoiceLimit() > 0 ? VoiceLimit() : kEveryNote);
        }
        else if (Wii())
        {
            MakeCore<RevolutionCore>(banks, VoiceLimit() > 0 ? nwsnd::RevolutionPolicy::kChannelCount : count, count);
        }
    }

    // Runs the sound and returns its recorded performance.
    Performance Run();

    // Tracks whose filters changed during a sounding note. Their filters require MIDI controllers because presets
    // cannot update an existing note.
    const std::array<bool, 16>& FilterMoves() const
    {
        return midi_.filter_moves;
    }

    // Highest sounding CC11 value before `level_scale` and MIDI clamping. Game volumes can reach 255. This value may
    // therefore exceed 127.
    double LevelPeak() const
    {
        return midi_.rec.LevelPeak();
    }

    // The MIDI notes that tie or mono legatos raise above the velocity they started at, by note id, with the highest
    // velocity each reaches. A later pass starts them there (MidiNoteOn).
    const std::map<uint32_t, int>& Rises() const
    {
        return midi_.rises;
    }

    // The parts each track used: one more than the highest.
    const std::array<uint8_t, 16>& PartsUsed() const
    {
        return midi_.rec.PartsUsed();
    }

    // The MIDI notes the game releases before their SoundFont attack ends, more than 1 dB quieter than the attack has
    // reached then, by note id. Each value is the attack in timecents that reaches the game's level at the release. A
    // later pass gives the notes those attacks (MidiNoteOn).
    const std::map<uint32_t, int16_t>& EarlyReleases() const
    {
        return midi_.early_releases;
    }

    // On the Wii: the fits of the MIDI notes whose volume LFO the voice holds at full level, by note id. A later pass
    // gives the notes presets with those fits (MidiNoteOn).
    std::map<uint32_t, VolumeLfoFit> VolumeLfoFits() const
    {
        return midi_.VolumeLfoFits();
    }

    // The MIDI notes whose pitch LFO changes while they sound, by note id. A later pass gives each of them a part of
    // its own. The pitch bends of that part add the LFO (MidiNoteOn).
    const std::set<uint32_t>& LfoChanges() const
    {
        return midi_.lfo_changes;
    }

    // The choices MarkSteadyPass made for the main loop, for a later pass to make again.
    const std::vector<LoopChoice>& LoopChoices() const
    {
        return choices_;
    }

    // The counted loops that loop for ever in this performance, each a track and its loop's body. Like the passes of a
    // loop of count 0, their passes count as loops of the sequence.
    void SetEndlessLoops(const std::set<std::pair<int, uint32_t>>& loops)
    {
        endless_loops_ = loops;
    }

    // The counted loops that the open tracks were in when the performance was cut at the time limit, each a track and
    // its loop's body. The set is empty when the performance ended before the limit or counted as finished there.
    const std::set<std::pair<int, uint32_t>>& LimitLoops() const
    {
        return limit_loops_;
    }

private:
    // When a track last ran a command: the tick, and the number of commands all tracks had run before it. The number
    // orders the commands of a tick. `call` is the serial number of the innermost call that the command ran in
    // (Track::call_ids), or 0 outside calls.
    struct CommandRun
    {
        uint32_t tick = 0;
        uint64_t order = 0;
        uint64_t call = 0;
    };

    // How a pass of the main loop starts (passes.h).
    using PassStart = sequence_export::PassStart;

    // True for the Wii's engine.
    bool Wii() const
    {
        return settings_.engine == Engine::kRevolution;
    }

    // Sound frame containing `ms`.
    uint64_t FrameOf(double ms) const
    {
        return static_cast<uint64_t>(ms / settings_.frame_ms + 1e-9);
    }

    // When the next tick comes at the current tempo. Nothing when tempo or timebase is 0: either stops nw::snd sequence
    // timing. The core's clock moves on to the tick.
    std::optional<TickTime> NextTick();

    // Makes a core of type `Core` with `channels` channels and `voices` voices to play the sequence. The core's random
    // state starts from PerformOptions::seed. Each global variable starts at -1. On the Wii, each starts at 0
    // (SeqPlayer::SetGlobalVariable), and the core plays as the game's build of NW4R SND does.
    template <typename Core>
    void MakeCore(Regions& banks, int channels, int voices);

    // NextTick on the Wii U, the Switch and the 3DS, from the core's clock (SequenceSoundPlayer::NextTickTime).
    template <typename Core>
    std::optional<TickTime> NextCoreTick(Core& core);

    // Track `t`'s state and settings, taken from the core's track for the performer's analysis and recording. A track
    // that the core's player has freed keeps its state.
    template <typename Core>
    void TakeCoreTrack(Core& core, int t);

    // Track `t`'s volumes, pan and bend, with their moves, from the core's track.
    template <typename Core>
    void TakeCoreRamps(Core& core, int t);

    // The tempo, the timebase, the main volume and the variables from the core's player.
    template <typename Core>
    void TakeCorePlayer(Core& core);

    // What the core's run of command `cmd` on track `t` changed, for the performer's analysis and recording: the pass
    // that a jump back or the end of an endless loop closes, the values of the track and the player, the variables, and
    // the notes that a tie, mono or mute releases or stops.
    template <typename Core>
    void RecordCoreCommand(Core& core, int t, uint32_t cmd, int32_t arg1, int32_t arg2);

    // ParseTracks on the core: the core runs each track's tick, and the performer follows the core's hooks.
    template <typename Core>
    bool ParseCoreTracks(Core& core, int first);

    // For a copy that PlayOn makes at a jump: the copy of the core runs the rest of track `t`'s tick.
    template <typename Core>
    void ContinueCoreTrack(Core& core, int t);

    // The performer's records after the core ran track `t`'s tick, or the rest of it: the close of a track that ended,
    // the start of a wait, and the track's state and settings from the core. `ran` says whether the track ran commands.
    // Returns whether the track is open after the tick.
    template <typename Core>
    bool FinishCoreTrackTick(Core& core, int t, bool was_open, bool open, bool ran);

    // Runs `f` on the core, if the performer has one.
    template <typename F>
    void WithCore(F&& f)
    {
        const auto run = [&](auto& core)
        {
            if constexpr (!std::is_same_v<std::decay_t<decltype(core)>, std::monostate>)
            {
                f(core);
            }
        };
        std::visit(run, core_);
    }

    // What the core's hooks report (SequenceCore). Each records the sequence's analysis: a command's start, a command
    // that has run with `next` the offset after its arguments, an argument's random draw and variable read, and a note
    // command. They do nothing once a pass has stopped the performance.
    void FollowCommandStart(int t, uint32_t offset, uint32_t command, bool conditional);
    void FollowCommand(int t, uint32_t command, int32_t arg1, int32_t arg2, uint32_t next);
    void FollowRandomDraw();
    void FollowVariableRead(int t, int index);
    void FollowNoteCommand(int t, int key, int velocity, int32_t length);

    // The core started channel `channel` for track `t`'s note, on the voice with key `key`
    // (RecordingEngine::PositionKey). The note that NoteOn started for the same command plays there.
    void FollowNoteOn(int t, nwsnd::ChannelId channel, int key);

    // The core continued channel `channel` of track `t` for the note command: a tie or a mono legato.
    void FollowLegato(int t, nwsnd::ChannelId channel);

    // The core found no free channel for track `t`'s note. With voice drops (VoiceLimit), the note plays nothing, as in
    // the game. Without them, the performer keeps the note.
    void FollowNoChannel(int t);

    // The core gave the voice of track `t`'s channel `channel` to another note. With voice drops, the note on that
    // channel stops at once, as in the game.
    void FollowChannelDrop(int t, nwsnd::ChannelId channel);

    // On the Wii, a volume LFO lifted the envelope's gain above full level on channel `channel` of track `t`. The game
    // holds the gain at full level there. The LFOs of the SoundFont and the SFZ swing above it. Each note is reported
    // once.
    void FollowGainAboveFull(int t, nwsnd::ChannelId channel);

    // On the Wii, channel `channel` of track `t` played a frame of its volume LFO. The envelope stood at `envelope_db`
    // and the LFO at `lfo_db`. The frame goes into the sums of the note on the channel (MidiRecorder::OnVolumeLfo).
    void FollowVolumeLfo(int t, nwsnd::ChannelId channel, float envelope_db, float lfo_db);

    // The core released channel `channel`, or ran a note-off on it. While the core counts down a track's lengths
    // (SequenceTrack::UpdateChannelLength, 0x022B6E88), FollowRelease keeps the channels whose lengths ran out. After
    // the count, FollowLengths releases their notes, in the order the notes started. FollowLengths copies each note's
    // length from its channel, or counts the length of a note without one. The channels that have stopped are removed.
    void FollowRelease(nwsnd::ChannelId channel);
    void FollowLengths(int t);

    // Runs the note command that FollowNoteCommand keeps until the core's verdict: NoteOn and the track's record of it.
    // With `drop`, the core found no free channel, and NoteOn starts no channel. A command's start and the end of a
    // track's tick settle a note that the core gave no verdict on, such as one with no region.
    void SettleNote(bool drop);

    // The voices for the notes that the game drops: --voices's count. 0, the default, keeps every note. The drops come
    // from the core.
    int VoiceLimit() const
    {
        return options_.voice_limit;
    }

    // The new notes that a track can start in one tick: the game's count (Settings::voices) with --voices. Without it,
    // the cap is kMaxVoiceLimit, far above any console's count. A loop without waits then makes at most that many MIDI
    // notes in one tick.
    int TickNoteCap() const
    {
        return VoiceLimit() > 0 ? settings_.voices : static_cast<int>(kMaxVoiceLimit);
    }

    // The core's channel that plays note `n`, or nullptr once the core has freed it.
    template <typename Core>
    const typename Core::Channel* CoreChannelOf(Core& core, const Note& n);

    // Each note's frame in which its one-shot wave runs out, from the core's voice layer, after the core's frames.
    template <typename Core>
    void TakeCoreWaves(Core& core);

    // The value of the pitch LFO of each note with lfo_bends in sound frame `frame`, from the core's channel before the
    // frame's update (Note::lfo_pitch).
    template <typename Core>
    void TakeCoreLfos(Core& core, uint64_t frame);

    // The core's frames without ticks, from the current frame up to frame `until`, and each note's one-shot playback
    // and pitch LFO from them. They continue the waves past the sound's last tick. The frames stop once no note's
    // one-shot wave plays on, and no pitch LFO goes on in a note with lfo_bends.
    template <typename Core>
    void RunCoreTail(Core& core, uint64_t until);

    // NextTick on the Wii, from the core's clock: in the clock's float milliseconds, or in 416ths of a tick in the
    // build of October 2006.
    std::optional<TickTime> NextWiiCoreTick();

    // After sequence timing stops, continues sounding channels at the MIDI tempo (OnTempo). Releases and one-shot waves
    // finish normally; held notes last kHoldSeconds. No further sequence commands run.
    void PlayOutStopped();

    // Real milliseconds per tick at the current tempo, or 0 at tempo 0 or timebase 0. A tick of the Wii's build of
    // October 2006 lasts 416 / tempo frames, whatever the timebase.
    double MsPerTick() const
    {
        if (Wii() && settings_.wii_build.October2006())
        {
            return tempo_ > 0 ? settings_.frame_ms * kTickUnits / tempo_ : 0.0;
        }

        const double tpm = static_cast<double>(timebase_) * tempo_; // ticks per minute
        return tpm > 0 ? 60000.0 / tpm : 0.0;
    }

    // First tick at or after `ms`. Extrapolates beyond the last tick at the final tempo, or the MIDI tempo if sequence
    // timing stopped.
    uint32_t MsToTick(double ms) const;

    // Milliseconds per tick at MIDI's tempo (OnTempo).
    double MidiMsPerTick() const
    {
        return 60000.0 / (midi_.out.tempo.back().bpm * midi_.out.timebase);
    }

    // Includes a stopped note's end time in the sound's duration.
    void Ended(const Note& n)
    {
        sound_end_ms_ = std::max(sound_end_ms_, std::min(n.SoundEndMs(), now_ms_));
    }

    // True if `n` hasn't been released and its envelope has sunk below hearing (MidiNote::quiet_ms). Such a note then
    // counts as silence in the search for loops, held sounds and the silence at the time limit.
    bool BelowHearing(const Note& n) const
    {
        return !n.released && now_ms_ >= n.midi.quiet_ms;
    }

    // Allocates track 0, tracks requested by sound flags, and those in a leading alloctrack command.
    void AllocateTracks();

    // Ends all notes and records duration/end tick. `finished` means the sequence ended naturally.
    void EndPerformance(bool finished);

    // Starts track `t` at command offset `offset`.
    void OpenTrack(int t, uint32_t offset);

    // Releases track `t`'s channels and closes the track.
    void CloseTrack(int t);

    // Starts a tick: the core begins it and moves the main volume a tick on. The performer takes the main volume.
    void StartTick();

    // Makes the game's changes of variables that come by the current frame (PerformOptions::changes), before the
    // frame's first tick. The search for the main loop then starts again.
    void ChangeVariables();

    // Runs the open tracks from track `first` on for one tick, and closes those that finish. Returns true if one of
    // them is still open.
    bool ParseTracks(int first);

    // Ends a tick: sends each track's changes, and reports the notes whose LFOs play otherwise than their presets.
    void FinishTick();

    // Moves on to the tick at `next`. Sends the pitches that timed sweeps reach on the way.
    void AdvanceTo(const TickTime& next);

    // How a pass that starts at `tick` starts.
    PassStart PassStartAt(uint32_t tick) const;

    // A copy of this performer that plays on from the tick in which track `t` jumped back to `target` and stopped the
    // performance, up to but not including tick `until`, with no limit on the loops.
    Performer PlayOn(int t, uint32_t target, uint32_t until) const;

    // How the next pass of the main loop starts when track `t` has jumped back to `target` in this tick.
    PassStart NextPassStart(int t, uint32_t target) const;

    // True if a MIDI player that loops on the pass from `start` to `end` plays each repeat's notes as the game plays
    // the notes of the next pass (passes.h). This performer has played that pass.
    bool RepeatsAsTheGame(uint32_t start, uint32_t end) const;

    // Selects the first of the loop owner's passes that plays as the pass after it does (RepeatsAsTheGame). A MIDI
    // player that loops on the markers plays each repeat as the marked pass. Track `t` has just jumped back to
    // `target`.
    LoopChoice ChooseSteadyPass(int t, uint32_t target) const;

    // Marks the pass that ChooseSteadyPass selects. Returns true if the performance can stop, false if it requires
    // another pass first.
    bool MarkSteadyPass(int t, uint32_t target);

    // True if the bytecode's 16- and 24-bit arguments are big-endian (Settings::arguments_follow_byte_order).
    bool ArgumentsBigEndian() const
    {
        return !settings_.arguments_follow_byte_order || seq_.big_endian;
    }

    // Resolves player, global or track variable `index` for track `t`. Returns nullptr above 47.
    int16_t* Variable(int t, uint8_t index);

    // Records that track `t` reads player or global variable `index` (a comparison or an argument), or changes it. A
    // change sets Track::last_signal if another open track has read the variable and can still read on.
    void NoteVariableRead(int t, uint8_t index);
    void NoteVariableChange(int t, uint8_t index);

    // The notes of mute mode `mode` on track `t`: mode 2 releases them, and mode 3 stops them.
    void MuteNotes(int t, uint8_t mode);

    // True if track `t` is in a subroutine and the pass that began with command `first_command` (as commands_run_
    // counts) ran a conditional jump, call, return or fin and played no note. The track must also hold no note that
    // sounds until the game stops it (see OnLoop).
    bool Leavable(int t, uint64_t first_command) const;

    // Gives each entry that track `t`'s call stack gained since the track's last command a serial number
    // (Track::call_ids). A command either pushes entries or pops them. The numbered entries therefore follow the stack.
    void NumberCalls(int t);

    // The serial number of the innermost call on track `t`'s stack, or 0 outside calls.
    uint64_t InnermostCall(int t) const;

    // True if track `t` has returned from the call with serial number `call`. Commands outside calls (0) never are.
    bool ReturnedFrom(int t, uint64_t call) const;

    // Handles track `t` jumping back to `target`. `jump` is true for a jump command and false for the end of an endless
    // loop. Detects the main loop and stops after the requested repetitions.
    void OnLoop(int t, uint32_t target, bool jump);

    // Plays a note on track `t`. Allocates a channel for it, or reuses the newest one for tie/mono. Returns false for a
    // legato on a channel below hearing. Such a legato plays nothing. With `drop`, the game found no free channel for a
    // new note: NoteOn ends after the tie and mono rules, and returns false.
    bool NoteOn(int t, int key, int velocity, int32_t length, bool drop = false);

    // Moves channel `n` to `key` without another attack. Continues its MIDI note where possible, or once this tick has
    // reached the legato cap of Settings::voices (`past_cap`; see NoteOn). Otherwise starts a new one. Returns false if
    // the channel is below hearing (BelowHearing). A legato doesn't start the envelope again. The channel therefore
    // stays there.
    bool Legato(int t, Note& n, int key, int velocity, int32_t length, bool past_cap);

    // Starts `n`'s sweep from the core's channel for the note command (settling_channel_), or no sweep without one.
    void StartSweep(Note& n);

    // True before `n` has been heard. Envelopes start at -90.4 dB and first advance at the end of the note-on frame.
    bool Unheard(const Note& n) const;

    // True if released channel `n` remains above the inaudibility threshold (kInaudibleSustain).
    bool StillAudible(const Note& n) const;

    // True if channel `n` can be heard: if it's released, above the inaudibility threshold (StillAudible).
    bool Heard(const Note& n) const;

    // True if no channel of track `t` but `n` can be heard on the part the track plays on, released ones included. CC11
    // acts on the whole MIDI channel. Only a note that's alone can therefore start higher and be brought down to its
    // velocity by CC11 (MidiNoteOn).
    bool Alone(int t, const Note& n) const;

    // True if a channel of track `t` on `part`, other than `except`, still sounds.
    bool PartSounds(int t, uint8_t part, const Note* except = nullptr) const;

    // Like tie, mono and mute mode 2, releases track `t`'s channels and detaches them. Gives them the track's settings
    // first.
    void ReleaseAndDetach(int t);

    // Releases channel `n`, at MML release rate `release_value` if one is given.
    void ReleaseNote(int t, Note& n, int release_value = -1);

    // Stops channel `n` at once.
    void StopNote(int t, Note& n);

    // Removes stopped channels from track `t`, including detached channels.
    void PruneNotes(int t);

    // Makes the note-offs pending in the current tick's frame (Note::release_pending) if `next` comes in a later frame.
    // nn::atk makes them in the frame's channel update, after the envelopes' first step. So each note sounds for that
    // frame and releases from there in the next. Its MIDI note-off comes where that frame starts.
    void ReleasePending(const TickTime& next);

    // True if a note of track `t` on `part` that sounds at `ms` has a timed sweep still moving after sound frame
    // `frame`, or has lfo_bends.
    bool SweepMoves(int t, uint8_t part, uint64_t frame, double ms) const;

    // Sends the pitches that the timed sweeps of a finished sequence's release tails reach after its last tick. Each
    // pitch goes as far after that tick as its frame starts, at the sequence's last tempo.
    void SweepAfterEnd();

    // The Wii: returns the volume NW4R SND gives track `t`'s channels (SeqTrack::UpdateChannelParam): each volume
    // squared, and the sound's volume, clamped to 1 (SeqPlayer's volume).
    float UserVolume(int t) const;

    // Builds channel `n`'s current preset key. The key uses a controller for the filter if track `t` is in
    // `midi_.filter_controller`.
    PresetKey NoteKey(int t, const Note& n) const;

    // Recalculates track `t`'s filter and records changes affecting sounding notes (FilterMoves). As in the game,
    // detached channels keep their previous filter.
    void SetFilter(int t);

    // Returns true if one of track `t`'s channels is still sounding.
    bool TrackBusy(int t) const;

    // True if any channel of track `t` except `except` is sounding, including detached channels.
    bool OthersSounding(int t, const Note* except) const;

    // True if open track `t` won't read on before the game stops the sound: it waits a negative number of ticks, waits
    // out a note of kHeldTicks or more, or is blocked on held, looping notes that never end.
    bool WaitsForever(int t) const;

    // True if track `t` sounds nothing, and kHoldSeconds have passed since its last change or since it opened,
    // whichever is later.
    bool Quiet(int t) const;

    // The Wii: true if no open track will read on before the game stops the sound and no note sounds: the sequence has
    // ended.
    bool Stuck() const;

    // True if every open track waits for ever (WaitsForever) and no sweep, volume, pan or bend can change. The volumes
    // are the track's volume, volume2 and the main volume.
    bool Steady() const;

    // Returns the time to end the performance at if the main loop holds a sound unchanged. The loop's pass started at
    // tick `start`. Such a pass is longer than kHoldSeconds and has changed nothing for longer than that. The pass
    // sounds only endless notes, each the newest on an open track with tie or mono on. The next pass usually continues
    // them. The sound then holds forever. As with Steady, the performance ends kHoldSeconds after the last change.
    std::optional<double> HeldSoundEnd(uint32_t start) const;

    // True if the marked main loop sounds nothing once the notes before it have ended. No note starts from the loop's
    // start on. No channel has a MIDI note any more. Each channel has ended or ends without the game. Release tails and
    // fades may still play. The game keeps such a sound in silence until it stops the sound, or until a poll leaves the
    // loop.
    bool SilentLoop() const;

    const Settings& settings_;
    const SoundInfo& sound_;
    const Sequence& seq_;
    Regions& banks_;
    PerformOptions options_;

    // The core, on the Wii U, the Switch, the 3DS and the Wii, and whether any of its tracks is open after the tracks
    // that have run this tick.
    std::variant<std::monostate, CafeCore, NxCore, CtrCore, RevolutionCore> core_;
    bool core_open_ = false;

    // The notes that NoteOn has started, and the track whose newest note waits for the core's channel, or -1
    // (FollowNoteOn).
    uint64_t notes_started_ = 0;
    int note_to_pair_ = -1;

    // The note command that waits for the core's verdict (FollowNoteCommand, SettleNote), and the core's channel for it
    // while FollowNoteOn and FollowLegato settle it. The MIDI notes that NoteOn starts while SettleNote settles a note
    // command take `settling_command_` (Performance::note_commands).
    std::optional<PendingNote> pending_note_;
    nwsnd::ChannelId settling_channel_ = nwsnd::kNoChannel;
    NoteCommand settling_command_;

    // The core counts down a track's lengths, and the channels that the count releases (FollowRelease).
    bool counting_lengths_ = false;
    std::vector<nwsnd::ChannelId> length_releases_;

    MidiRecorder midi_; // what the performer records
    std::array<Track, 16> tracks_{};
    std::array<int16_t, 16> player_vars_{};
    std::array<int16_t, 16> global_vars_{};
    uint32_t timebase_ = 48;
    int tempo_ = 120;
    MoveValue main_volume_;
    uint32_t tick_ = 0;
    double now_ms_ = 0;
    uint64_t frame_ = 0; // the sound frame the current tick is processed in

    // The Wii's build of October 2006 (NextWiiCoreTick): the ticks of the current frame after this one, and the tick's
    // time at MIDI's tempo.
    int frame_ticks_left_ = 0;
    double nominal_ms_ = 0;
    bool balance_ = false; // the Wii's stereo waves play in balance mode (Performance::balance)
    std::array<std::map<uint32_t, CommandRun>, 16> last_run_; // when each command offset last ran, per track
    uint64_t commands_run_ = 0;                               // by all tracks
    uint64_t call_serial_ = 0;                                // the last serial number of a call (NumberCalls)
    std::vector<double> tick_ms_;                             // the time of each tick
    double sound_end_ms_ = 0;                                 // end time of the last completed note
    bool stop_ = false;
    int stopped_by_ = -1;                     // the track whose pass of the main loop stopped the performance
    std::optional<double> held_sound_end_ms_; // set when the main loop holds a sound (HeldSoundEnd)
    bool silent_loop_ = false;                // the main loop sounded nothing (SilentLoop)
    std::optional<std::pair<int, uint32_t>> loop_owner_;      // the track and loop start of the marked loop (OnLoop)
    std::vector<std::pair<uint32_t, uint32_t>> owner_passes_; // the owner's counted passes, start and end ticks
    std::size_t marked_ = 0;                                  // the marked one of them (MarkSteadyPass)
    bool extended_ = false;                       // the performance ran past the passes it had to find one to mark
    std::size_t next_change_ = 0;                 // the first of the game's changes still to come (ChangeVariables)
    uint32_t changed_at_ = 0;                     // the tick of the last change
    std::optional<uint32_t> last_draw_;           // the last tick a track drew a random number in
    std::array<uint16_t, 32> variable_readers_{}; // the tracks that have read each player or global variable
    std::vector<LoopChoice> replay_;              // the choices of an earlier pass to make again (MarkSteadyPass)
    std::size_t replayed_ = 0;                    // how many of them have been made
    std::vector<LoopChoice> choices_;             // the choices made
    std::set<std::pair<int, uint32_t>> endless_loops_;
    std::set<std::pair<int, uint32_t>> limit_loops_;
};

template <typename Console, template <typename> class VoiceLayer>
bool SequenceCore<Console, VoiceLayer>::OnParseLimit(Player& player, int track)
{
    bool stop = true;
    if (stop_runaway_tracks)
    {
        const auto t = static_cast<std::size_t>(track);
        const int64_t tick = player.TickCount();
        if (limit_ticks_[t] != tick - 1)
        {
            run_start_ms_[t] = now_ms;
        }
        limit_ticks_[t] = tick;
        stop = now_ms - run_start_ms_[t] >= kHoldSeconds * 1000.0;
    }

    if (stop && !performer->stop_)
    {
        performer->midi_.OnRunaway(*performer, track);
    }

    return stop;
}

template <typename Console, template <typename> class VoiceLayer>
void SequenceCore<Console, VoiceLayer>::OnParseLimitEnd(Player&, int track)
{
    if (!performer->stop_)
    {
        performer->midi_.OnRunaway(*performer, track);
    }
}

template <typename Console, template <typename> class VoiceLayer>
void SequenceCore<Console, VoiceLayer>::OnCommandStart(Player&, int track, uint32_t offset, uint32_t command,
                                                       bool conditional)
{
    performer->FollowCommandStart(track, offset, command, conditional);
}

template <typename Console, template <typename> class VoiceLayer>
void SequenceCore<Console, VoiceLayer>::OnCommand(Player& player, int track, uint32_t command, int32_t arg1,
                                                  int32_t arg2)
{
    performer->FollowCommand(track, command, arg1, arg2, player.Track(track)->CommandEnd());
}

template <typename Console, template <typename> class VoiceLayer>
void SequenceCore<Console, VoiceLayer>::OnExtendedCommand(Player& player, int track, uint32_t command, int32_t arg1,
                                                          int32_t arg2)
{
    performer->FollowCommand(track, command, arg1, arg2, player.Track(track)->CommandEnd());
}

template <typename Console, template <typename> class VoiceLayer>
void SequenceCore<Console, VoiceLayer>::OnRandomDraw(Player&, int)
{
    performer->FollowRandomDraw();
}

template <typename Console, template <typename> class VoiceLayer>
void SequenceCore<Console, VoiceLayer>::OnVariableRead(Player&, int track, int index)
{
    performer->FollowVariableRead(track, index);
}

template <typename Console, template <typename> class VoiceLayer>
void SequenceCore<Console, VoiceLayer>::OnNoteCommand(Player&, int track, int key, int velocity, int32_t length)
{
    performer->FollowNoteCommand(track, key, velocity, length);
}

template <typename Console, template <typename> class VoiceLayer>
void SequenceCore<Console, VoiceLayer>::OnNoteOn(Player&, int track, Channel& channel)
{
    const int key = channel.voice != nwsnd::kNoVoice ? this->PositionKey(channel.voice) : -1;
    performer->FollowNoteOn(track, channel.place, key);
}

template <typename Console, template <typename> class VoiceLayer>
void SequenceCore<Console, VoiceLayer>::OnLegato(Player&, int track, Channel& channel)
{
    performer->FollowLegato(track, channel.place);
}

template <typename Console, template <typename> class VoiceLayer>
void SequenceCore<Console, VoiceLayer>::OnRelease(Player&, int, Channel& channel)
{
    performer->FollowRelease(channel.place);
}

template <typename Console, template <typename> class VoiceLayer>
void SequenceCore<Console, VoiceLayer>::OnLengthsCounted(Player&, int track)
{
    performer->FollowLengths(track);
}

template <typename Console, template <typename> class VoiceLayer>
void SequenceCore<Console, VoiceLayer>::OnNoChannel(int track)
{
    performer->FollowNoChannel(track);
}

template <typename Console, template <typename> class VoiceLayer>
void SequenceCore<Console, VoiceLayer>::OnChannelDropped(int track, Channel& channel)
{
    performer->FollowChannelDrop(track, channel.place);
}

template <typename Console, template <typename> class VoiceLayer>
void SequenceCore<Console, VoiceLayer>::OnGainAboveFull(Channel& channel)
{
    performer->FollowGainAboveFull(channel.owner_track, channel.place);
}

template <typename Console, template <typename> class VoiceLayer>
void SequenceCore<Console, VoiceLayer>::OnVolumeLfo(Channel& channel, float envelope_db, float lfo_db)
{
    performer->FollowVolumeLfo(channel.owner_track, channel.place, envelope_db, lfo_db);
}

template <typename Console, template <typename> class VoiceLayer>
bool SequenceCore<Console, VoiceLayer>::StopsTick(Player&, int)
{
    return performer->stop_;
}

template <typename Console, template <typename> class VoiceLayer>
bool SequenceCore<Console, VoiceLayer>::LoopsForEver(const Player&, int track, uint32_t address) const
{
    return performer->endless_loops_.contains({track, address});
}

MidiRecorder::MidiRecorder() : rec(RecordingRules())
{
    // A track's first part starts with the SFZ filters off, at their controllers' default of 0.
    for (int t = 0; t < 16; t++)
    {
        tracks[t].output.midi_track = static_cast<uint8_t>(t);
        tracks[t].output.sent[{0, kSfzLowPassEvent}] = 0;
        tracks[t].output.sent[{0, kSfzBiquadEvent}] = 0;
    }
}

void MidiRecorder::OnTempo(Performer& engine)
{
    const double bpm =
        engine.Wii() && engine.settings_.wii_build.October2006()
            ? static_cast<double>(engine.tempo_) * (60000.0 / engine.settings_.frame_ms / kTickUnits) / out.timebase
            : static_cast<double>(engine.tempo_) * engine.timebase_ / out.timebase;
    rec.SetTempo(out, engine.tick_, bpm);
}

void MidiRecorder::Emit(Performer& engine, int t, TrackEvent e, int part)
{
    e.tick = engine.tick_;
    e.part = static_cast<uint8_t>(part >= 0 ? part : tracks[t].output.part);
    NoteChange(engine, t, e.kind);
    rec.Emit(out, tracks[t].output, engine.tick_, e, e.part);
    if (IsTrackSetting(e.kind))
    {
        GiveTheFrameTheBiquad(engine, t, e);
    }
}

bool MidiRecorder::SendSetting(Performer& engine, int t, EventKind kind, int value, int part)
{
    if (!rec.SendSetting(out, tracks[t].output, engine.tick_, kind, static_cast<uint8_t>(value), part))
    {
        return false;
    }

    TrackEvent e{engine.tick_, kind, 0, static_cast<uint8_t>(value)};
    e.part = static_cast<uint8_t>(part >= 0 ? part : tracks[t].output.part);
    NoteChange(engine, t, kind);
    GiveTheFrameTheBiquad(engine, t, e);

    return true;
}

void MidiRecorder::NoteChange(Performer& engine, int t, EventKind kind)
{
    if (kind != EventKind::kExpression && kind != kFilterEvent && kind != EventKind::kPitch)
    {
        engine.tracks_[t].last_change = engine.tick_;
    }
}

void MidiRecorder::GiveTheFrameTheBiquad(Performer& engine, int t, const TrackEvent& e)
{
    // The ticks played out after a sequence stops advancing have no recorded time (PlayOutStopped). No command switches
    // the biquad then.
    if (e.kind != kSfzBiquadEvent || engine.tick_ >= engine.tick_ms_.size())
    {
        return;
    }

    // Find the events of the frame's earlier ticks, just before the current tick's events.
    std::vector<TrackEvent>& events = out.tracks[t];
    std::size_t end = events.size();
    while (end > 0 && events[end - 1].tick == engine.tick_)
    {
        end--;
    }

    const uint64_t frame = engine.FrameOf(engine.tick_ms_[engine.tick_]);
    std::size_t begin = end;
    while (begin > 0 && events[begin - 1].after == 0 &&
           engine.FrameOf(engine.tick_ms_[events[begin - 1].tick]) == frame)
    {
        begin--;
    }

    // The controller's value before the frame, 0 (off) if it was never sent.
    int value = 0;
    for (std::size_t k = begin; k > 0; k--)
    {
        if (events[k - 1].kind == kSfzBiquadEvent && events[k - 1].part == e.part)
        {
            value = events[k - 1].value;
            break;
        }
    }

    // Each of those notes on the part that would get the other choice gets a copy of the controller first.
    for (std::size_t k = begin; k < end; k++)
    {
        const TrackEvent& ev = events[k];
        if (ev.part != e.part)
        {
            continue;
        }

        if (ev.kind == kSfzBiquadEvent)
        {
            value = ev.value;
        }
        else if (ev.kind == EventKind::kNoteOn && (value != 0) != (e.value != 0))
        {
            TrackEvent copy = e;
            copy.tick = ev.tick;
            events.insert(events.begin() + static_cast<std::ptrdiff_t>(k), copy);
            k++;
            end++;
            value = e.value;
        }
    }
}

void MidiRecorder::Approximate(const char* what)
{
    rec.Approximate(what);
}

void MidiRecorder::RecordTickTime(Performer& engine)
{
    engine.tick_ms_.push_back(engine.now_ms_);

    // The Wii's build of October 2006 runs a tick at the start of its frame. MIDI's tempo puts the tick at
    // `nominal_ms_`, up to a frame earlier.
    if (engine.Wii() && engine.settings_.wii_build.October2006())
    {
        out.frame_ticks.push_back(static_cast<double>(engine.tick_) +
                                  (engine.now_ms_ - engine.nominal_ms_) / engine.MidiMsPerTick());
        return;
    }

    const double start = static_cast<double>(engine.frame_) * engine.settings_.frame_ms;
    rec.RecordFrameStart(out, engine.tick_ms_, engine.tick_, start);
}

int16_t* Performer::Variable(int t, uint8_t index)
{
    // Player variables 0-15, global 16-31 and track 32-47 (MmlParser::GetVariablePtr, 0x022CB600).
    if (index < 16)
    {
        return &player_vars_[index];
    }
    if (index < 32)
    {
        return &global_vars_[index - 16];
    }
    if (index < 48)
    {
        return &tracks_[t].vars[index - 32];
    }

    return nullptr;
}

PresetKey Performer::NoteKey(int t, const Note& n) const
{
    PresetKey pk = ChannelPresetKey(tracks_[t], n);
    pk.attack_timecents = n.midi.attack_timecents;
    pk.lift = n.midi.lift;
    pk.lfo_swing = n.midi.lfo_fit.swing;
    pk.lfo_cut = n.midi.lfo_fit.cut;
    if (midi_.filter_controller[t])
    {
        pk.filter_controller = true;
    }
    else
    {
        pk.filter = midi_.tracks[t].filter;
    }

    return pk;
}

void Performer::SetFilter(int t)
{
    Track& tr = tracks_[t];
    const SoundFontFilter filter =
        ToSoundFont(TrackFilters(tr.low_pass, tr.biquad_type, tr.biquad_value, settings_), settings_);
    if (filter != midi_.tracks[t].filter && TrackBusy(t))
    {
        midi_.filter_moves[t] = true;
    }

    midi_.tracks[t].filter = filter;
}

void MidiRecorder::GiveStruckNotesLfo(Performer& engine, int t)
{
    // This tick's events, and the notes struck in them.
    Track& tr = engine.tracks_[t];
    std::vector<TrackEvent>& events = out.tracks[t];
    auto first = events.end();
    while (first != events.begin() && std::prev(first)->tick == engine.tick_)
    {
        --first;
    }

    std::set<uint32_t> struck;
    for (auto it = first; it != events.end(); ++it)
    {
        if (it->kind == EventKind::kNoteOn)
        {
            struck.insert(it->note);
        }
    }
    if (struck.empty())
    {
        return;
    }

    // The preset each part had before this tick.
    std::map<uint8_t, int> sent;
    for (auto it = first; it != events.end(); ++it)
    {
        if (it->kind == EventKind::kNoteOn && !sent.contains(it->part))
        {
            const uint8_t part = it->part;
            auto is_part_preset = [part](const TrackEvent& e)
            {
                return e.kind == EventKind::kPreset && e.part == part;
            };
            const auto back = std::find_if(std::make_reverse_iterator(first), events.rend(), is_part_preset);
            sent[part] = back != events.rend() ? static_cast<int>(back->preset) : -1;
        }
    }

    // Each note's preset with the track's LFO settings goes in front of it, in place of this tick's preset events. A
    // note with lfo_bends keeps its preset without the LFO.
    const PresetKey lfo = TrackPresetKey(tr);
    std::set<uint32_t> bends;
    for (const Note& n : tr.notes)
    {
        if (n.midi.lfo_bends)
        {
            bends.insert(n.midi.id);
        }
    }

    std::map<uint8_t, int> struck_with = sent;
    std::vector<TrackEvent> tick_events;
    for (auto it = first; it != events.end(); ++it)
    {
        if (it->kind == EventKind::kPreset)
        {
            struck_with[it->part] = static_cast<int>(it->preset);
            continue;
        }

        const int with = it->kind == EventKind::kNoteOn ? struck_with[it->part] : -1;
        if (with >= 0)
        {
            const int preset = bends.contains(it->note)
                                   ? with
                                   : static_cast<int>(seqlib::midi::PresetIndex(
                                         out.presets, WithLfo(out.presets[static_cast<std::size_t>(with)], lfo)));
            if (preset != sent[it->part])
            {
                TrackEvent ev{engine.tick_, EventKind::kPreset};
                ev.part = it->part;
                ev.preset = static_cast<uint32_t>(preset);
                tick_events.push_back(ev);
                sent[it->part] = preset;
            }
        }
        tick_events.push_back(*it);
    }

    events.erase(first, events.end());
    events.insert(events.end(), tick_events.begin(), tick_events.end());

    if (sent.contains(tracks[t].output.part))
    {
        tracks[t].output.sent[{tracks[t].output.part, EventKind::kPreset}] = sent[tracks[t].output.part];
    }

    for (Note& n : tr.notes)
    {
        if (struck.contains(n.midi.id))
        {
            n.midi.preset = WithLfo(n.midi.preset, lfo);
        }
    }
}

void MidiRecorder::CheckLfo(Performer& engine, int t)
{
    Track& tr = engine.tracks_[t];
    const PresetKey lfo = TrackPresetKey(tr);
    if (tr.lfos[0].depth == 0 || lfo.lfo_type > 2)
    {
        return;
    }

    for (Note& n : tr.notes)
    {
        if (n.midi.on && !n.midi.lfo_moved && n.EndMs() > engine.now_ms_ && !SameLfo(n.midi.preset, lfo))
        {
            // A pitch LFO that changes, without a volume or pan LFO before it, can go out as pitch bends.
            n.midi.lfo_moved = true;
            if (lfo.lfo_type == 0 && (n.midi.preset.lfo_type == 0 || n.midi.preset.lfo_type == 3))
            {
                lfo_changes.insert(n.midi.id);
            }
            if (n.midi.lfo_crowded)
            {
                Approximate(
                    "LFO parameters changed during a note, with no MIDI channel free for its pitch bends (using "
                    "its note-on settings)");
            }
            else if (!n.midi.lfo_bends)
            {
                Approximate("LFO parameters changed during a note (using its note-on settings)");
            }
        }
    }
}

void Performer::OpenTrack(int t, uint32_t offset)
{
    // SetSeqData (0x022B69A8) and Open (0x022B69B8) preserve the other track parameters. On the Wii
    // (SeqTrack::SetSeqData, main.dol 0x8003ffbc, and Open, 0x8003ffcc), a wait and a call stack are among them.
    Track& tr = tracks_[t];
    tr.current = offset;

    if (!Wii())
    {
        tr.note_finish_wait = false;
        tr.call_depth = 0;
        tr.wait = 0;
    }

    tr.open = true;
    tr.opened = tick_;
    tr.next_stretch = Entry::kOpen;
}

void MidiRecorder::MidiNoteOff(Performer& engine, int t, Note& n)
{
    if (!n.midi.on)
    {
        return;
    }

    n.midi.on = false;
    rec.NoteOff(out, static_cast<uint8_t>(t), engine.tick_, n.midi.part, n.midi.key, n.midi.id);
    NoteChange(engine, t, EventKind::kNoteOff);
}

void Performer::ReleaseNote(int t, Note& n, int release_value)
{
    // Channel::Release (0x022B6028) applies a new rate even to channels already releasing. Key groups depend on that.
    //
    // Envelopes advance Settings::env_step_ms per frame, from the note-on frame on. A channel stops in the frame it
    // falls below -90.4 dB and is gone before the next frame's ticks. While the core still has the channel, the core's
    // channel gives the envelope's level.
    const int64_t frame = static_cast<int64_t>(frame_);

    std::optional<float> core_level;
    const auto level_on_core = [&](auto& core)
    {
        if (const auto* channel = CoreChannelOf(core, n))
        {
            core_level = channel->envelope.Level();
        }
    };
    WithCore(level_on_core);

    n.release_pending = false;
    if (n.released)
    {
        if (release_value >= 0 && n.release_end_ms > now_ms_)
        {
            const int64_t steps = frame - static_cast<int64_t>(n.released_frame);
            const float level =
                core_level ? *core_level : ReleasedLevel(n.release_level, n.release_rate, steps, settings_);
            n.release_level = level;
            n.released_frame = frame_;
            n.release_rate = DecayRate(release_value, settings_.engine);
            n.release_end_ms =
                static_cast<double>(frame + ReleaseFrames(level, n.release_rate, settings_) + 1) * settings_.frame_ms;
        }

        return;
    }

    n.released = true;
    n.released_frame = frame_;
    n.release_level = core_level ? *core_level : HeldLevel(n.env, frame - static_cast<int64_t>(n.on_frame), settings_);
    n.release_rate = DecayRate(release_value >= 0 ? release_value : n.env.release, settings_.engine);
    n.release_end_ms =
        static_cast<double>(frame + ReleaseFrames(n.release_level, n.release_rate, settings_) + 1) * settings_.frame_ms;
    midi_.OnRelease(*this, t, n);
}

void MidiRecorder::OnRelease(Performer& engine, int t, Note& n)
{
    RecordEarlyRelease(engine, n);
    EndMidiNote(engine, t, n);
}

void MidiRecorder::RecordEarlyRelease(Performer& engine, const Note& n)
{
    // The SoundFont attack starts with the MIDI note and reaches full level after Sf2AttackMs. A linear ramp to full
    // level over T milliseconds reaches the game's level L dB after t milliseconds when T = t * 10^(-L / 20).
    const uint64_t released_frame = engine.settings_.note_off_waits_for_update ? n.released_frame : engine.frame_;
    const double held_ms = static_cast<double>(released_frame - n.midi.frame) * engine.settings_.frame_ms;
    const double ramp_ms = Sf2AttackMs(n.env, engine.settings_);
    if (!n.midi.on || held_ms <= 0.0 || held_ms >= ramp_ms)
    {
        return;
    }

    const double level_db = n.release_level / 10.0;
    if (20.0 * std::log10(held_ms / ramp_ms) - level_db <= 1.0)
    {
        return;
    }

    const double attack_ms = held_ms * std::pow(10.0, -level_db / 20.0);
    early_releases[n.midi.id] = static_cast<int16_t>(ToTimecents(attack_ms, -12000, 8000));
}

void MidiRecorder::OnVolumeLfo(uint32_t id, float envelope_db, float lfo_db, double scale)
{
    // The SoundFont and SFZ play the LFO at `x` decibels. The game raises a level by 6 dB at most (CalcVolumeRatio).
    // The voice holds the envelope's level plus the LFO's at full level. The game then plays the LFO at `y`. A frame
    // counts by the envelope's power. The note's quiet frames then count for little.
    const double envelope = envelope_db;
    const double game = std::min(static_cast<double>(lfo_db), 6.0);
    const double x = scale * lfo_db;
    const double y = std::min(0.0, envelope + game) - envelope;
    const double w = std::pow(10.0, envelope / 10.0);
    VolumeLfoSums& sums = lfo_sums[id];
    sums.w += w;
    sums.x += w * x;
    sums.xx += w * x * x;
    sums.y += w * y;
    sums.xy += w * x * y;
    sums.held = sums.held || envelope + game > 0.0;
}

std::map<uint32_t, VolumeLfoFit> MidiRecorder::VolumeLfoFits() const
{
    // The SoundFont and SFZ play a fitted note's LFO at `swing` times `x`, `offset` decibels lower: the weighted
    // least-squares fit to the game's `y` (OnVolumeLfo). The swing stays between 0 and 1, and the offset at 0 or
    // below. A note whose envelope stays at full level then swings half as far, a third of the LFO's depth lower. The
    // fit's steps are 16ths of the swing and 5 centibels. Only the notes that the voice holds at full level get a fit.
    std::map<uint32_t, VolumeLfoFit> fits;
    for (const auto& [id, sums] : lfo_sums)
    {
        const double det = sums.w * sums.xx - sums.x * sums.x;
        if (!sums.held || sums.w <= 0.0 || det <= 1e-12 * sums.w * sums.xx)
        {
            continue;
        }

        double swing = std::clamp((sums.w * sums.xy - sums.x * sums.y) / det, 0.0, 1.0);
        double offset = (sums.y - swing * sums.x) / sums.w;
        if (offset > 0.0)
        {
            offset = 0.0;
            swing = std::clamp(sums.xy / sums.xx, 0.0, 1.0);
        }

        VolumeLfoFit fit;
        fit.swing = static_cast<uint8_t>(std::lround(16.0 * swing));
        fit.cut = static_cast<int16_t>(5 * std::lround(-2.0 * offset));
        if (fit != VolumeLfoFit{})
        {
            fits[id] = fit;
        }
    }

    return fits;
}

void MidiRecorder::EndMidiNote(Performer& engine, int t, Note& n)
{
    if (!n.midi.on || !engine.Unheard(n))
    {
        MidiNoteOff(engine, t, n);
        return;
    }

    std::vector<TrackEvent>& events = out.tracks[t];
    auto is_note_on = [&](const TrackEvent& e)
    {
        return e.kind == EventKind::kNoteOn && e.note == n.midi.id;
    };
    const auto on = std::find_if(events.rbegin(), events.rend(), is_note_on);
    if (on != events.rend())
    {
        events.erase(std::next(on).base());
    }

    n.midi.on = false;
}

bool Performer::Unheard(const Note& n) const
{
    return frame_ == n.on_frame;
}

bool Performer::StillAudible(const Note& n) const
{
    // A note-off that waits for the channel's first update starts the release in the frame after the tick's frame
    // (ReleasePending). Before the next tick, the release has taken no step.
    const int64_t frames = static_cast<int64_t>(frame_) - static_cast<int64_t>(n.released_frame);
    const double steps = static_cast<double>(std::max<int64_t>(frames, 0));
    const bool double_rate = settings_.engine == Engine::kCtr || settings_.engine == Engine::kRevolution;
    const double level = double_rate
                             ? n.release_level - static_cast<double>(n.release_rate) * settings_.env_step_ms * steps
                             : n.release_level - n.release_rate * settings_.env_step_ms * steps;
    return n.EndMs() > now_ms_ && level > -kInaudibleSustain;
}

bool Performer::Heard(const Note& n) const
{
    return n.released ? StillAudible(n) : n.EndMs() > now_ms_;
}

bool Performer::Alone(int t, const Note& n) const
{
    const Track& tr = tracks_[t];
    auto other_heard = [&](const Note& other)
    {
        return &other != &n && other.HasMidiNote() && other.midi.part == midi_.tracks[t].output.part && Heard(other);
    };

    return std::ranges::none_of(tr.notes, other_heard) && std::ranges::none_of(tr.detached, other_heard);
}

bool Performer::PartSounds(int t, uint8_t part, const Note* except) const
{
    const Track& tr = tracks_[t];
    auto sounds = [&](const Note& n)
    {
        return &n != except && n.HasMidiNote() && n.midi.part == part && n.EndMs() > now_ms_;
    };

    return std::ranges::any_of(tr.notes, sounds) || std::ranges::any_of(tr.detached, sounds);
}

bool MidiRecorder::Switch(Performer& engine, int t, const Note* except)
{
    // The new part's channel may hold another part's values from earlier. Its last values are dropped, and every
    // controller goes out again.
    const uint8_t old = tracks[t].output.part;
    auto sounds = [&](uint8_t part)
    {
        return engine.PartSounds(t, part, except);
    };
    if (!rec.Switch(out, tracks[t].output, engine.tick_, sounds))
    {
        return false;
    }

    tracks[t].left[old].gain = tracks[t].legato_gain;

    return true;
}

void MidiRecorder::OnStopAll(Performer& engine, int t)
{
    // CC120 stops the parts that the track's notes sounded on. Tails that the track detached on other parts go on.
    std::set<uint8_t> sounding;
    for (Note& n : engine.tracks_[t].notes)
    {
        if (n.HasMidiNote() && n.EndMs() > engine.now_ms_ && !engine.Unheard(n))
        {
            sounding.insert(n.midi.part);
        }
        EndMidiNote(engine, t, n);
    }

    for (const uint8_t part : sounding)
    {
        Emit(engine, t, {0, EventKind::kSoundOff}, part);
    }
}

void Performer::ReleaseAndDetach(int t)
{
    // SequenceTrack::ReleaseAllChannel (0x022B608C) gives the channels the track's settings before it releases them.
    // The end of a tick does the same. So the MIDI part the notes sound on gets the tick's changes so far before the
    // track leaves it.
    midi_.OnReleaseAll(*this, t);
    for (Note& n : tracks_[t].notes)
    {
        ReleaseNote(t, n);
    }
    midi_.OnReleasedAll(*this, t);
}

void MidiRecorder::OnReleaseAll(Performer& engine, int t)
{
    UpdateOutputs(engine, t);
}

void MidiRecorder::OnReleasedAll(Performer& engine, int t)
{
    DetachAll(engine, t);
    LeaveTails(engine, t);
}

void MidiRecorder::LeaveTails(Performer& engine, int t)
{
    const Track& tr = engine.tracks_[t];
    auto tail = [&](const Note& n)
    {
        return n.HasMidiNote() && n.midi.part == tracks[t].output.part && engine.StillAudible(n);
    };
    if (std::ranges::any_of(tr.detached, tail))
    {
        Switch(engine, t);
    }
}

void MidiRecorder::DetachAll(Performer& engine, int t)
{
    // FreeAllChannel (0x022B6114). SequenceTrack::UpdateChannelParam (0x022B55B4) gives the channels the track's bend
    // before it detaches them. A detached note keeps that bend (PartPitch).
    Track& tr = engine.tracks_[t];
    for (Note& n : tr.notes)
    {
        n.bend = tr.Bend();
        tr.detached.push_back(n);
    }
    tr.notes.clear();
}

bool Performer::OthersSounding(int t, const Note* except) const
{
    const Track& tr = tracks_[t];
    for (const Note& n : tr.notes)
    {
        if (&n != except && n.EndMs() > now_ms_ && !BelowHearing(n))
        {
            return true;
        }
    }

    for (const Note& n : tr.detached)
    {
        if (n.EndMs() > now_ms_)
        {
            return true;
        }
    }

    return false;
}

void Performer::StopNote(int t, Note& n)
{
    // Channel::Stop (0x022C4278) silences immediately.
    midi_.OnStop(*this, t, n);
    n.released = true;
    n.release_end_ms = now_ms_;
}

void MidiRecorder::OnStop(Performer& engine, int t, Note& n)
{
    // MIDI CC120 cuts the whole channel. Use it only for an audible note with no other sounds remaining on its part.
    const bool heard = n.HasMidiNote() && !engine.Unheard(n);
    EndMidiNote(engine, t, n);
    if (heard && n.EndMs() > engine.now_ms_)
    {
        if (!engine.PartSounds(t, n.midi.part, &n))
        {
            Emit(engine, t, {0, EventKind::kSoundOff}, n.midi.part);
        }
        else
        {
            Approximate("immediate channel stop (MIDI release tail remains)");
        }
    }
}

void Performer::CloseTrack(int t)
{
    // Like the end of a tick, SequenceTrack::Close (0x022B6158) first gives the channels the track's settings. The
    // release tails start from the closing tick's volume, pan, pitch and filter. Close then releases every channel,
    // even one that ignores note-off.
    Track& tr = tracks_[t];
    midi_.OnTrackClosing(*this, t);
    for (Note& n : tr.notes)
    {
        ReleaseNote(t, n);
    }

    midi_.OnTrackClosed(*this, t);
    tr.open = false;
}

void MidiRecorder::OnTrackClosing(Performer& engine, int t)
{
    if (engine.TrackBusy(t))
    {
        UpdateOutputs(engine, t);
        rec.RaiseLevelPeak(TrackLevel(engine, t) * tracks[t].legato_gain);
    }
}

void MidiRecorder::OnTrackClosed(Performer& engine, int t)
{
    DetachAll(engine, t);
}

bool Performer::TrackBusy(int t) const
{
    for (const Note& n : tracks_[t].notes)
    {
        if (n.EndMs() > now_ms_)
        {
            return true;
        }
    }

    return false;
}

bool Performer::Quiet(int t) const
{
    const Track& tr = tracks_[t];
    const uint32_t since = std::max(tr.last_change.value_or(0), tr.opened);
    return !OthersSounding(t, nullptr) && now_ms_ - tick_ms_[since] > kHoldSeconds * 1000.0;
}

bool Performer::WaitsForever(int t) const
{
    // A track that waits a negative number of ticks never reads on. That happens to a note or wait whose length comes
    // from a variable left at -1. A tick counts down only a positive wait. The track reads commands only while its
    // wait is 0 (SequenceTrack::ParseNextTick, 0x022B6F14).
    const Track& tr = tracks_[t];
    if (tr.held_wait || tr.wait < 0)
    {
        return true;
    }
    if (!tr.note_finish_wait || tr.notes.empty())
    {
        return false;
    }

    for (const Note& n : tr.notes)
    {
        if (n.length > 0 || n.EndMs() < kNever)
        {
            return false;
        }
    }

    return true;
}

bool Performer::Stuck() const
{
    // A note that hasn't been released sounds here until it has faded (MidiNote::faded_ms). The MIDI note of a tie ends
    // at the same point. The sequence's sound then ends where the note's MIDI note ends (EndPerformance).
    const auto sounding = [this](const Note& n)
    {
        return n.EndMs() > now_ms_ && (n.released || now_ms_ < n.midi.faded_ms);
    };
    const auto ends_later = [this](const Note& n)
    {
        return n.EndMs() > now_ms_;
    };
    for (int t = 0; t < 16; t++)
    {
        const Track& tr = tracks_[t];
        if ((tr.open && tr.wait >= 0 && !tr.held_wait) || std::ranges::any_of(tr.notes, sounding) ||
            std::ranges::any_of(tr.detached, ends_later))
        {
            return false;
        }
    }

    return true;
}

bool Performer::Steady() const
{
    // The main volume and volume2 ramp on the Wii U and the Switch (SequenceSoundPlayer::ParseNextTick, 0x022B70AC, and
    // SequenceTrack::ParseNextTick, 0x022B6F14).
    if (main_volume_.Moving())
    {
        return false;
    }

    bool any = false;
    for (int t = 0; t < 16; t++)
    {
        const Track& tr = tracks_[t];
        if (!tr.open)
        {
            continue;
        }

        any = true;
        if (!WaitsForever(t) || tr.vol.Moving() || tr.volume2.Moving() || tr.pan.Moving() || tr.bend.Moving())
        {
            return false;
        }

        for (const Note& n : tr.notes)
        {
            if (n.SweepAt(frame_) != 0.0f)
            {
                return false;
            }
        }
    }

    return any;
}

void MidiRecorder::OnEnd(Performer& engine, int t, Note& n)
{
    MidiNoteOff(engine, t, n); // the wave ended by itself
}

void Performer::PruneNotes(int t)
{
    // Remove channels when their one-shot wave or release finishes (SequenceTrack::ChannelCallbackFunc, 0x022CB06C).
    auto ended = [&](Note& n)
    {
        if (n.EndMs() > now_ms_)
        {
            return false;
        }

        midi_.OnEnd(*this, t, n);
        Ended(n);

        return true;
    };
    std::erase_if(tracks_[t].notes, ended);
    std::erase_if(tracks_[t].detached, ended);
}

void MidiRecorder::OnLengthUpdated(Performer& engine, int t, Note& n)
{
    // The MIDI note of a channel that its length can't release, such as a tie, ends where the channel has faded to
    // kFadedAttenuation. The channel's sound ends there.
    if (!n.midi.sunk && (n.ignore_note_off || n.length < 0) && !n.released && engine.now_ms_ >= n.midi.faded_ms)
    {
        MidiNoteOff(engine, t, n);
        n.midi.sunk = true;
        engine.sound_end_ms_ = std::max(engine.sound_end_ms_, n.midi.faded_ms);
    }
}

void Performer::FollowRelease(nwsnd::ChannelId channel)
{
    if (counting_lengths_)
    {
        length_releases_.push_back(channel);
    }
}

void Performer::FollowLengths(int t)
{
    // The count runs a note-off on a channel whose length has run out if the track's damper doesn't hold it. A region
    // that ignores note-offs plays on. Its MIDI note ends on the channel's release or stop, the wave's end, or where it
    // falls below hearing. In the game, silent channels keep their voices until the voice allocator reuses them.
    //
    // A note can be without a channel in the core. A one-shot wave can end in the frames before the tick in which the
    // note's length runs out. The core frees the channel at the wave's end. The performer also keeps some notes that
    // have no voice in the core (FollowNoChannel, FollowChannelDrop). FollowLengths counts the length and the sweep's
    // ticks of such a note itself. The note then ends in its place among the tick's note-offs. Without that count, a
    // note whose wave has ended would end after them (PruneNotes).
    counting_lengths_ = false;
    Track& tr = tracks_[t];
    for (Note& n : tr.notes)
    {
        bool has_channel = false;
        bool ran_out = false;
        const auto take_length = [&](auto& core)
        {
            if (const auto* channel = CoreChannelOf(core, n))
            {
                has_channel = true;
                n.length = channel->length;
                n.sweep_ticks = channel->sweep_counter;
                ran_out = std::ranges::find(length_releases_, n.core_channel) != length_releases_.end();
            }
        };
        WithCore(take_length);
        if (!has_channel)
        {
            if (n.length > 0)
            {
                n.length--;
            }
            ran_out = n.length == 0 && !tr.damper;

            if (!n.sweep_in_ms)
            {
                n.sweep_ticks += 1.0;
            }
        }

        if (ran_out && !n.released && !n.release_pending && !n.ignore_note_off)
        {
            if (settings_.note_off_waits_for_update && frame_ == n.on_frame)
            {
                n.release_pending = true;
            }
            else
            {
                ReleaseNote(t, n);
            }
        }

        midi_.OnLengthUpdated(*this, t, n);
    }

    length_releases_.clear();
    PruneNotes(t);
}

void MidiRecorder::UpdateOutputs(Performer& engine, int t)
{
    // The values SequenceTrack::UpdateChannelParam (0x022B55B4) gives the track's channels.
    Track& tr = engine.tracks_[t];
    if (!tracks[t].played)
    {
        return;
    }

    // A closed track's channels take nothing more from it, but go on with their timed sweeps (Channel::Update).
    if (!tr.open)
    {
        for (uint8_t part = 0; part < 16; part++)
        {
            if (engine.frame_ > 0 && engine.SweepMoves(t, part, engine.frame_ - 1, engine.now_ms_))
            {
                SendPitch(engine, t, part, engine.frame_);
            }
        }

        return;
    }

    // Detached notes keep their old settings in the game. In MIDI, those the track couldn't leave on another part
    // (LeaveTails) still share its channel and receive later controller changes.
    const bool tails = TailsOnPart(engine, t);
    auto control = [&](int value, EventKind kind)
    {
        if (SendSetting(engine, t, kind, value) && tails)
        {
            Approximate("detached notes (still receive track controller changes in MIDI)");
        }
    };

    // Channel gain is (vol * volume2 * main volume / 127^3)^2 * sound volume / 127 * (velocity / 127)^2. Each volume
    // can reach 255. SoundFont CC7, CC11 and velocity all act squared: put up to 127 of track volume in CC7, and the
    // excess, other volumes and legato velocity change in CC11. If CC11 exceeds 127, rerun with all CC11 values scaled
    // equally (Perform).
    control(std::clamp(tr.vol.Get(), 0, 127), EventKind::kVolume);
    const double level = TrackLevel(engine, t);
    control(rec.Expression(level * tracks[t].legato_gain), EventKind::kExpression);
    const int change_filter = FilterControllerValue(tracks[t].filter.cutoff);
    if (level != tr.change_level || change_filter != tr.change_filter)
    {
        if (engine.Wii() && level != tr.change_level)
        {
            CheckFullLevel(engine, t);
        }

        tr.change_level = level;
        tr.change_filter = change_filter;
        tr.last_change = engine.tick_;
    }

    // nw::snd pan is value / 63, and the SoundFont CC10 modulator is (value - 64) / 64, on the synth's pan curve (see
    // pan.h).
    const int pan = std::clamp(static_cast<int>(std::lround(64.0 + SoundFontPan(tr.pan.Get() / 63.0) * 64.0)), 0, 127);
    auto stereo_on_part = [&](const Note& n)
    {
        return n.stereo && n.midi.part == tracks[t].output.part && n.EndMs() > engine.now_ms_;
    };
    if (pan != LastSent(tracks[t].output, EventKind::kPan) && pan != 64 && engine.balance_ &&
        std::ranges::any_of(tr.notes, stereo_on_part))
    {
        Approximate("track pan on stereo waves in balance mode (they pan as in dual mode)");
    }
    control(pan, EventKind::kPan);

    const int mod = ModulationValue(tr.lfos[0]);
    control(mod, EventKind::kModulation);
    // The game's code sets up the effects on the aux buses, and a send to a bus without an effect is lost (sndcore2).
    // The games put an effect on bus A. Bus A becomes the synth's reverb. Bus B usually has none, and CC93 stays at 0.
    // A synth's default chorus send therefore adds nothing either.
    control(engine.settings_.sends == Sends::kNone ? 0 : std::min<int>(127, tr.fx_send_a), EventKind::kReverb);
    if (engine.settings_.sends == Sends::kReverb || engine.settings_.sends == Sends::kNone)
    {
        control(0, EventKind::kChorus);
    }
    else if (engine.settings_.sends == Sends::kReverbChorus)
    {
        control(std::min<int>(127, tr.fx_send_b), EventKind::kChorus);
    }

    if (filter_controller[t])
    {
        control(FilterControllerValue(tracks[t].filter.cutoff), kFilterEvent);
    }

    // SFZ controllers hold the original filter settings. Each track file uses its first enabled biquad type; omit later
    // types.
    control(SfzLowPassValue(tr.low_pass), kSfzLowPassEvent);
    uint8_t biquad = SfzBiquadValue(tr.biquad_type, tr.biquad_value, engine.settings_);
    if (biquad != 0 && out.biquad_types[t] == 0)
    {
        out.biquad_types[t] = static_cast<uint8_t>(tr.biquad_type);
    }
    if (tr.biquad_type != out.biquad_types[t])
    {
        biquad = 0;
    }
    control(biquad, kSfzBiquadEvent);

    // As in the game, a part that the track has left but whose notes it still holds follows the track's changes. The
    // part takes them at the level its notes have (their legato gain). A part that holds only notes that the track
    // detached keeps their settings.
    for (uint8_t part = 0; part < 16; part++)
    {
        auto held = [&](const Note& n)
        {
            return n.HasMidiNote() && n.midi.part == part && engine.Heard(n);
        };
        if (part == tracks[t].output.part || !std::ranges::any_of(tr.notes, held))
        {
            continue;
        }

        LeftPart& left = tracks[t].left[part];
        auto follow = [&](int value, EventKind kind)
        {
            SendSetting(engine, t, kind, value, part);
        };
        follow(std::clamp(tr.vol.Get(), 0, 127), EventKind::kVolume);
        follow(rec.Expression(level * left.gain), EventKind::kExpression);
        rec.RaiseLevelPeak(level * left.gain);
        follow(pan, EventKind::kPan);
        follow(mod, EventKind::kModulation);
        follow(engine.settings_.sends == Sends::kNone ? 0 : std::min<int>(127, tr.fx_send_a), EventKind::kReverb);
        if (engine.settings_.sends == Sends::kReverb || engine.settings_.sends == Sends::kNone)
        {
            follow(0, EventKind::kChorus);
        }
        else if (engine.settings_.sends == Sends::kReverbChorus)
        {
            follow(std::min<int>(127, tr.fx_send_b), EventKind::kChorus);
        }
        if (filter_controller[t])
        {
            follow(FilterControllerValue(tracks[t].filter.cutoff), kFilterEvent);
        }
        follow(SfzLowPassValue(tr.low_pass), kSfzLowPassEvent);
        follow(biquad, kSfzBiquadEvent);
    }

    // MIDI has one pitch per channel; nw::snd has one per note. For change detection, take the track's bend and the
    // newest sweep of any part, without the legato key offset. The game's notes have no parts (Track::last_change). A
    // note below hearing changes nothing that can be heard.
    float game_pitch = tr.Bend();
    for (auto it = tr.notes.rbegin(); it != tr.notes.rend(); ++it)
    {
        const float s = it->SweepAt(engine.frame_);
        if (s != 0.0f && !engine.BelowHearing(*it))
        {
            game_pitch += s;
            break;
        }
    }
    if (std::fabs(game_pitch - tr.change_pitch) > 0.0005f || (game_pitch != tr.change_pitch && game_pitch == 0.0f))
    {
        tr.change_pitch = game_pitch;
        tr.last_change = engine.tick_;
    }

    // Each part gets the pitch of its notes, the track's first.
    if (SendPitch(engine, t, tracks[t].output.part, engine.frame_) && tails)
    {
        Approximate("detached notes (still receive track controller changes in MIDI)");
    }
    for (uint8_t part = 0; part < 16; part++)
    {
        if (part != tracks[t].output.part)
        {
            SendPitch(engine, t, part, engine.frame_);
        }
    }
}

std::optional<float> MidiRecorder::PartPitch(const Performer& engine, int t, uint8_t part, uint64_t frame) const
{
    const Track& tr = engine.tracks_[t];
    if (part == tracks[t].output.part && tr.open)
    {
        const float pitch = tr.Bend() + tracks[t].legato_bend;
        for (auto it = tr.notes.rbegin(); it != tr.notes.rend(); ++it)
        {
            const float s = it->SweepAt(frame);
            if ((s != 0.0f || it->midi.lfo_bends) && it->midi.part == part && it->HasMidiNote() && engine.Heard(*it))
            {
                return it->midi.lfo_bends ? pitch + s + it->LfoPitchAt(frame) : pitch + s;
            }
        }

        return pitch;
    }

    const Note* newest = nullptr;
    bool held = false;
    float sweep = 0.0f;
    auto look = [&](const Note& n, bool holds)
    {
        if (!n.HasMidiNote() || n.midi.part != part || !engine.Heard(n))
        {
            return;
        }

        if (!newest)
        {
            newest = &n;
            held = holds;
        }
        sweep = sweep != 0.0f ? sweep : n.SweepAt(frame);
    };
    std::for_each(tr.notes.rbegin(), tr.notes.rend(), [&](const Note& n) { look(n, true); });
    std::for_each(tr.detached.rbegin(), tr.detached.rend(), [&](const Note& n) { look(n, false); });
    if (!newest)
    {
        return std::nullopt;
    }

    // The game gives the notes a track holds its bend in the frame of the tick that changes it, after this tick's
    // events (SequenceTrack::UpdateChannelParam). Detached notes keep theirs.
    const float bend = held ? tr.Bend() : newest->bend;
    const float pitch = bend + static_cast<float>(newest->key - newest->midi.key) + sweep;
    return newest->midi.lfo_bends ? pitch + newest->LfoPitchAt(frame) : pitch;
}

bool MidiRecorder::SendPitch(Performer& engine, int t, uint8_t part, uint64_t frame, double after)
{
    const std::optional<float> pitch = PartPitch(engine, t, part, frame);
    return pitch && rec.SendPitch(out, tracks[t].output, engine.tick_, *pitch, part, after);
}

bool MidiRecorder::TailsOnPart(const Performer& engine, int t) const
{
    const Track& tr = engine.tracks_[t];
    auto shares_part = [&](const Note& n)
    {
        return n.HasMidiNote() && n.midi.part == tracks[t].output.part && engine.StillAudible(n);
    };

    return std::ranges::any_of(tr.detached, shares_part);
}

void MidiRecorder::SweepBetweenTicks(Performer& engine, const TickTime& next)
{
    // Channel::Update moves a timed sweep and an LFO on every sound frame. That includes the frames between ticks and
    // the frames after the track has closed. The track's settings hold still between ticks. Find the parts with a sweep
    // under way, and the parts whose pitch bends add a note's pitch LFO.
    std::array<std::array<bool, 16>, 16> sweeping{};
    std::array<std::array<bool, 16>, 16> lfo{};
    bool any = false;
    for (int t = 0; t < 16; t++)
    {
        const Track& tr = engine.tracks_[t];
        if (!tracks[t].played)
        {
            continue;
        }

        for (const auto* list : {&tr.notes, &tr.detached})
        {
            for (const Note& n : *list)
            {
                if (n.HasMidiNote() && n.sweep_in_ms && n.SweepAt(engine.frame_) != 0.0f)
                {
                    sweeping[t][n.midi.part] = true;
                    any = true;
                }
                if (n.HasMidiNote() && n.midi.lfo_bends && engine.Heard(n))
                {
                    sweeping[t][n.midi.part] = true;
                    lfo[t][n.midi.part] = true;
                    any = true;
                }
            }
        }
    }
    if (!any)
    {
        return;
    }

    // The core has run the current tick's frame. The LFO's value in that frame goes at the tick, in place of the latest
    // value that the tick sent.
    for (int t = 0; t < 16; t++)
    {
        for (uint8_t part = 0; part < 16; part++)
        {
            if (lfo[t][part] && SendPitch(engine, t, part, engine.frame_) && engine.tracks_[t].open &&
                part == tracks[t].output.part && TailsOnPart(engine, t))
            {
                Approximate("detached notes (still receive track controller changes in MIDI)");
            }
        }
    }

    // Send each frame's pitch where the frame starts. Give its time as a fraction of the way to the next tick.
    for (uint64_t f = engine.frame_ + 1; f < next.frame; f++)
    {
        // On the Wii's build of October 2006, the fraction counts at MIDI's tempo from the tick's nominal time
        // (RecordTickTime).
        const double start = static_cast<double>(f) * engine.settings_.frame_ms;
        const double after = engine.Wii() && engine.settings_.wii_build.October2006()
                                 ? (start - engine.nominal_ms_) / engine.MidiMsPerTick()
                                 : (start - engine.now_ms_) / (next.ms - engine.now_ms_);
        for (int t = 0; t < 16; t++)
        {
            for (uint8_t part = 0; part < 16; part++)
            {
                const Track& tr = engine.tracks_[t];
                if (sweeping[t][part] && SendPitch(engine, t, part, f, after) && tr.open &&
                    part == tracks[t].output.part && TailsOnPart(engine, t))
                {
                    Approximate("detached notes (still receive track controller changes in MIDI)");
                }
            }
        }
    }
}

bool Performer::SweepMoves(int t, uint8_t part, uint64_t frame, double ms) const
{
    const Track& tr = tracks_[t];
    auto moves = [&](const Note& n)
    {
        return n.HasMidiNote() && n.midi.part == part &&
               ((n.sweep_in_ms && n.SweepAt(frame) != 0.0f) || n.midi.lfo_bends) && n.EndMs() > ms;
    };

    return std::ranges::any_of(tr.notes, moves) || std::ranges::any_of(tr.detached, moves);
}

void Performer::SweepAfterEnd()
{
    // As MsToTick counts, MIDI goes on at the last tick's tempo. Follow the sweeps for as long as the release tails
    // last (EndPerformance).
    const double per_tick = MsPerTick();
    if (per_tick <= 0)
    {
        return;
    }

    for (uint64_t f = frame_ + 1; static_cast<double>(f) * settings_.frame_ms < now_ms_ + kMaxTailMs; f++)
    {
        const double ms = static_cast<double>(f) * settings_.frame_ms;
        bool any = false;
        for (int t = 0; t < 16; t++)
        {
            for (uint8_t part = 0; part < 16; part++)
            {
                if (SweepMoves(t, part, f - 1, ms))
                {
                    midi_.SendPitch(*this, t, part, f,
                                    Wii() && settings_.wii_build.October2006() ? (ms - nominal_ms_) / MidiMsPerTick()
                                                                               : (ms - now_ms_) / per_tick);
                    any = true;
                }
            }
        }
        if (!any)
        {
            break;
        }
    }
}

double MidiRecorder::TrackLevel(const Performer& engine, int t) const
{
    // On the Switch, a voice's main bus takes the main send, and its aux buses B and C the effect sends. Sub-mix 0
    // passes every bus to the final mix at 1.0. A bus with no effect reaches the mix dry, in phase with the main bus
    // (MultiVoice::CalcMixVolumeForBuses, 0xc25fc0). Those sends therefore add up to the note's level. Bus A's send
    // goes through the game's reverb. CC91 stands in for that reverb.
    const Track& tr = engine.tracks_[t];
    if (engine.settings_.sends == Sends::kReverbDry)
    {
        const double dry = (tr.main_send + tr.fx_send_b + tr.fx_send_c) / 127.0;
        return std::max(tr.vol.Get(), 127) * (tr.volume2.Get() / 127.0) * (engine.main_volume_.Get() / 127.0) *
               std::sqrt(engine.sound_.volume / 127.0 * dry);
    }

    // The Wii's voice clamps a channel's volume to 1 (Voice::SetVolume), and the sound's volume to 1 too. The track's
    // volumes never take a note above full level. Limit them to full level, the most CC7 and CC11 can give together. A
    // note's zones take the rest (ZoneLift).
    if (engine.Wii())
    {
        const double level = std::max(tr.vol.Get(), 127) * (tr.volume2.Get() / 127.0) *
                             (engine.main_volume_.Get() / 127.0) *
                             std::sqrt(std::min<int>(engine.sound_.volume, 127) / 127.0);
        return std::min(level, 127.0 * 127.0 / std::clamp(tr.vol.Get(), 1, 127));
    }

    return std::max(tr.vol.Get(), 127) * (tr.volume2.Get() / 127.0) * (engine.main_volume_.Get() / 127.0) *
           std::sqrt(engine.sound_.volume / 127.0);
}

int16_t MidiRecorder::ZoneLift(const Performer& engine, int t, int velocity) const
{
    // A channel's volume is the velocity squared, the region's volume and the track's volumes (UserVolume) multiplied
    // together. MIDI's velocities stop at 127. Its track volumes stop at full level (TrackLevel). The zones take the
    // rest.
    const double above = std::max(velocity, 127) / 127.0;
    const double rest = above * above * std::max(1.0f, engine.UserVolume(t));
    return static_cast<int16_t>(std::lround(200.0 * std::log10(rest)));
}

float Performer::UserVolume(int t) const
{
    const Track& tr = tracks_[t];
    const float volume = static_cast<float>(tr.vol.Get()) / 127.0f;
    const float volume2 = static_cast<float>(tr.volume2.Get()) / 127.0f;
    const float main_volume = static_cast<float>(main_volume_.Get()) / 127.0f;
    float user = 1.0f;
    user *= volume * volume;
    user *= volume2 * volume2;
    user *= main_volume * main_volume;
    user *= std::clamp(static_cast<float>(sound_.volume) / 127.0f, 0.0f, 1.0f);

    return user;
}

void MidiRecorder::CheckFullLevel(Performer& engine, int t)
{
    // The game limits a channel's volume to full level (Note::init_volume). MIDI plays a note at its level before the
    // track's volumes (MidiNote::level), times the track's volumes up to full level (TrackLevel). Report a note whose
    // level differs from its level in the game by more than 0.1 dB. That happens to a note below velocity 127 whose
    // zone reaches full level before the note does (ZoneLift). It also happens to a note whose track's volumes change
    // while it plays. After a legato, CC11 sets the level. A note with lifted zones then plays at another level. So
    // does a quieter note while the track's volumes are above full level.
    constexpr double kTenthDb = 1.0115794542598986; // 10^(0.1 / 20)
    Track& tr = engine.tracks_[t];
    const float user = engine.UserVolume(t);
    for (Note& n : tr.notes)
    {
        if (n.midi.over_full_level || !n.midi.on)
        {
            continue;
        }

        const double game = std::min(1.0, static_cast<double>(n.init_volume) * user);
        const double midi = static_cast<double>(n.midi.level) * std::min(1.0f, user);
        const bool differs = n.midi.level >= 0.0f ? midi > game * kTenthDb || game > midi * kTenthDb
                                                  : n.midi.lift > 0 || (user > 1.0f && n.init_volume < 1.0f);
        if (differs)
        {
            n.midi.over_full_level = true;
            Approximate("channel volume above full level (limited in the game; MIDI plays the note at another level)");
        }
    }
}

void Performer::StartSweep(Note& n)
{
    // SequenceTrack::NoteOn has given the channel its sweep (Channel::SetSweepParam, 0x022C487C): the track's sweep
    // pitch, plus the distance from the porta key with portamento, over the note's length or over the porta time.
    n.sweep = 0.0f;
    n.sweep_length = 0;
    n.sweep_in_ms = false;
    n.sweep_ticks = 0;
    n.sweep_frame = frame_;

    const auto take_sweep = [&](auto& core)
    {
        if (settling_channel_ != nwsnd::kNoChannel)
        {
            const auto& channel = core.Channels().At(settling_channel_);
            n.sweep = channel.sweep_pitch;
            n.sweep_length = channel.sweep_length;
            n.sweep_in_ms = channel.auto_sweep;
        }
    };
    WithCore(take_sweep);
}

void MidiRecorder::MidiNoteOn(Performer& engine, int t, Note& n, int velocity, const VelocityRegion* region,
                              double gain)
{
    Track& tr = engine.tracks_[t];
    n.midi.key = n.key;
    n.midi.lfo_bends = false;
    n.midi.lfo_crowded = false;
    if (velocity <= 0)
    {
        return; // silent in nw::snd; MIDI velocity 0 would be a note-off
    }

    // On the Wii, the voice limits the channel's volume to full level (Note::init_volume). The note's zones take the
    // part of the volume above what MIDI's velocity and track controllers reach (ZoneLift). A new note that a legato
    // starts takes its level from CC11 instead (OnLegato).
    n.midi.lift = engine.Wii() && gain == 1.0 ? ZoneLift(engine, t, velocity) : 0;

    // A note that requires a pitch or level that the other notes sounding on the track's part don't share moves the
    // track to another part (Switch), and the note plays there. Each part has a MIDI channel of its own. A note that
    // sweeps, or that starts beside one still sweeping, moves the track. So does a note above velocity 127 whose zones
    // don't take the rest. A note that later legatos raise moves the track too. Such a note starts higher (below). A
    // note moves the track if its CC11, or the CC11 a legato lowered, would change the level of the others. Last, a
    // note whose pitch LFO an earlier pass found changing (LfoChanges) moves the track. So does a note that starts
    // beside one. Alone on its part, the note plays without the LFO in its preset. The pitch bends of the part add the
    // core's LFO (lfo_bends).
    const auto rise = start_velocities.find(rec.NextNote());
    const bool lfo_changes_here = lfo_notes.contains(rec.NextNote());
    auto sweeps_otherwise = [&](const Note& other)
    {
        return &other != &n && other.HasMidiNote() && other.midi.part == tracks[t].output.part && engine.Heard(other) &&
               (!n.SweepsLike(other, engine.frame_) || other.midi.lfo_bends);
    };
    const bool own_settings = std::ranges::any_of(tr.notes, sweeps_otherwise) ||
                              std::ranges::any_of(tr.detached, sweeps_otherwise) || rise != start_velocities.end() ||
                              (velocity > 127 && n.midi.lift == 0) || gain != 1.0 || tracks[t].legato_gain != 1.0 ||
                              lfo_changes_here;
    if (own_settings && !engine.Alone(t, n))
    {
        Switch(engine, t, &n);
    }
    n.midi.part = tracks[t].output.part;
    n.midi.lfo_bends = lfo_changes_here && engine.Alone(t, n);
    n.midi.lfo_crowded = lfo_changes_here && !n.midi.lfo_bends;

    // A note that legatos raise later (Rises) starts at the highest velocity that they reach and that its region still
    // plays. That way the synth selects the same zone, and CC11 brings the note down to the velocity it plays at. The
    // legatos then continue the note (Legato). That works only for a note that's alone on its part (Alone).
    const int capped = std::min(velocity, 127);
    int start = capped;
    n.midi.liftable = engine.Alone(t, n);
    if (n.midi.liftable && rise != start_velocities.end() && region)
    {
        for (int v = std::min(rise->second, 127); v > capped; v--)
        {
            if (engine.banks_.FindRegion(n.bank_slot, n.program, n.key, v) == region)
            {
                start = v;
                break;
            }
        }
    }

    // A note above velocity 127 plays at 127. If the note is alone on its part, CC11 raises it the rest of the way. On
    // the Wii, the zones do.
    double above = 1.0;
    if (velocity > 127 && n.midi.lift == 0)
    {
        if (n.midi.liftable)
        {
            above = static_cast<double>(velocity) / 127.0;
        }
        else
        {
            Approximate("velocity above 127 (played at 127)");
        }
    }

    tracks[t].legato_gain = gain * capped / start * above;
    tracks[t].legato_bend = 0.0f;
    UpdateOutputs(engine, t); // the controllers, and the pitch of a sweeping note, come first

    const LfoSetting& lfo = tr.lfos[0];
    if (lfo.type == 2 && lfo.depth != 0 && lfo.delay >= 0)
    {
        Approximate("pan LFO (omitted from SoundFont)");
    }
    if (lfo.type == 1 && lfo.depth * lfo.range > 128 && lfo.delay >= 0)
    {
        Approximate("volume LFO deeper than 6 dB (limited to 6 dB: it dips less than in the game)");
    }
    // The pitch bends of a note with lfo_bends follow the curve and the phase of the core's LFO. TakeCoreLfos reports a
    // random curve there.
    if (lfo.depth != 0 && lfo.delay >= 0 && (lfo.curve != 0 || lfo.phase != 0) && !n.midi.lfo_bends)
    {
        Approximate("LFO curve other than a sine, or a phase (using a sine from the cycle's start)");
    }
    if (std::any_of(tr.lfos.begin() + 1, tr.lfos.end(), [](const LfoSetting& s) { return s.depth != 0; }))
    {
        Approximate("second to fourth LFOs (omitted)");
    }
    if (engine.settings_.sends == Sends::kReverb && tr.fx_send_b != 0)
    {
        Approximate("effect send B (omitted)");
    }
    if ((engine.settings_.sends == Sends::kReverb || engine.settings_.sends == Sends::kReverbChorus) &&
        tr.fx_send_c != 0)
    {
        Approximate("effect send C (omitted)");
    }

    const GameFilters filters = TrackFilters(tr.low_pass, tr.biquad_type, tr.biquad_value, engine.settings_);
    if (filters.low_pass >= 0)
    {
        Approximate("low-pass filter (one-pole in the game, two-pole in the SoundFont)");
    }
    if (BiquadShape(filters.biquad_type, engine.settings_) >= 2)
    {
        Approximate("biquad high-pass or band-pass (omitted from SoundFont)");
    }
    if (filters.low_pass >= 0 && BiquadShape(filters.biquad_type, engine.settings_) == 1)
    {
        Approximate("combined low-pass filters (SoundFont uses the lower cutoff)");
    }
    if (filters.biquad_type != 0 && filters.biquad_type != out.biquad_types[t])
    {
        Approximate("biquad type change (SFZ supports one type per track)");
    }

    const auto attack = attacks.find(rec.NextNote());
    n.midi.attack_timecents = attack != attacks.end() ? std::optional<int16_t>(attack->second) : std::nullopt;
    const auto fit = lfo_fits.find(rec.NextNote());
    n.midi.lfo_fit = fit != lfo_fits.end() ? fit->second : VolumeLfoFit{};
    n.midi.preset = engine.NoteKey(t, n);
    n.midi.lfo_moved = false;

    // The SoundFont leaves the zone's low-pass for the console's resampling filter out of a preset whose filter changes
    // during notes (ZoneSpans).
    const Pcm* wave = region && n.midi.preset.filter_controller
                          ? engine.banks_.GetWave(n.bank_slot, region->wave_id_index)
                          : nullptr;
    if (wave && ResamplingFilterOf(n.midi.key,
                                   {region->original_key, region->pitch, wave->sample_rate, region->interpolation},
                                   engine.settings_))
    {
        Approximate("resampling filter on a preset whose filter changes during notes (omitted from SoundFont)");
    }
    const PresetKey sent = n.midi.lfo_bends ? WithoutLfo(n.midi.preset) : n.midi.preset;
    if (rec.SendPreset(out, tracks[t].output, engine.tick_, seqlib::midi::PresetIndex(out.presets, sent)))
    {
        NoteChange(engine, t, EventKind::kPreset);
    }

    // Assign IDs to overlapping notes of the same key so the MIDI writer can distribute them across channels.
    n.midi.on = true;
    n.midi.velocity = static_cast<uint8_t>(start);
    n.midi.start_velocity = velocity;
    n.midi.frame = engine.frame_;
    n.midi.id = rec.NoteOn(out, tracks[t].output, engine.tick_, n.midi.key, n.midi.velocity);
    out.note_commands.resize(n.midi.id);
    out.note_commands[n.midi.id - 1] = engine.settling_command_;
    NoteChange(engine, t, EventKind::kNoteOn);
    if (engine.Wii())
    {
        // The note's level before the track's volumes: the velocity's level times the zone's level, up to full level.
        // CC11 sets the level of a note that a legato starts.
        const double zone = region ? region->volume / 127.0 * std::pow(10.0, n.midi.lift / 200.0) : 0.0;
        n.midi.level =
            gain == 1.0 ? static_cast<float>(capped * capped / (127.0 * 127.0) * std::min(1.0, zone)) : -1.0f;
        CheckFullLevel(engine, t);
    }
}

bool Performer::NoteOn(int t, int key, int velocity, int32_t length, bool drop)
{
    // SequenceTrack::NoteOn (0x022C9A4C).
    Track& tr = tracks_[t];

    // At most TickNoteCap() new channels can sound in one tick, fewer with stereo waves. The cap avoids thousands of
    // MIDI notes from loops without waits. Legatos past Settings::voices in one tick continue their existing MIDI note,
    // with or without --voices.
    if (tr.note_tick != tick_)
    {
        tr.note_tick = tick_;
        tr.tick_notes = 0;
        tr.tick_legatos = 0;
    }

    const int vel = velocity * tr.velocity_range / 127;
    midi_.OnNoteOnStart(*this, t);

    // Tie/mono continue the newest channel without another attack (see Legato for MIDI handling).
    if (!tr.notes.empty() && (tr.tie || tr.mono))
    {
        Note& head = tr.notes.back();
        if (!tr.mono || !head.released)
        {
            return Legato(t, head, key, vel, length, ++tr.tick_legatos > settings_.voices);
        }

        StopNote(t, head); // nw::snd stops a releasing channel and starts a new one
        Ended(head);
        tr.notes.pop_back();
    }

    // The game found no free channel for the new note (FollowNoChannel).
    if (drop)
    {
        return false;
    }

    // Bank::NoteOn (0x022C4A0C) finds the new channel's region and wave, or leaves it silent.
    const VelocityRegion* region = banks_.FindRegion(tr.bank_slot, tr.program, key, vel);
    if (!region)
    {
        return true;
    }

    const Pcm* pcm = banks_.GetWave(tr.bank_slot, region->wave_id_index);
    if (!pcm)
    {
        return true;
    }

    if (++tr.tick_notes > TickNoteCap())
    {
        const std::string what = "more than " + std::to_string(TickNoteCap()) +
                                 " new channels on a track in one tick (excess omitted; the game keeps the newest)";
        midi_.Approximate(what.c_str());
        return true;
    }

    const bool stereo = pcm->channels.size() > 1;
    Note n;
    n.settings = &settings_;
    n.key = static_cast<uint8_t>(key);
    n.length = tr.tie ? -1 : length;
    n.on_frame = frame_;
    n.ignore_note_off = region->ignore_note_off;
    n.key_group = region->key_group;
    n.bank_slot = tr.bank_slot;
    n.program = tr.program;
    n.init_pan = tr.init_pan;
    n.stereo = stereo;
    const float init_volume = static_cast<float>(vel) / 127.0f;
    n.init_volume = init_volume * init_volume * (static_cast<float>(region->volume) / 127.0f);
    n.env = OverrideEnvelope(
        {region->adshr.attack, region->adshr.decay, region->adshr.sustain, region->adshr.hold, region->adshr.release},
        TrackPresetKey(tr));

    n.midi.quiet_ms = n.QuietMs();
    n.midi.faded_ms = n.FadedMs();

    n.midi.volume = region->volume;
    n.region_volume = region->volume;
    if (!pcm->loop)
    {
        const float ratio = nwsnd::CalcPitchRatio((key - region->original_key) * 256) * region->pitch;
        n.one_shot = true;
        WithCore([&](auto& core) { n.core_end = core.PredictEnd(frame_, pcm->Length(), ratio, pcm->sample_rate); });
    }

    StartSweep(n);

    // Release other channels in the key group at rate 126. SoundFont uses exclusiveClass for this. From 2008, the Wii
    // releases each of them at its release rate (Mario Kart Wii, 0x8009bfcc).
    if (n.key_group != 0)
    {
        for (Note& other : tr.notes)
        {
            if (other.key_group == n.key_group)
            {
                ReleaseNote(t, other, Wii() ? -1 : 126);
            }
        }
    }

    tr.notes.push_back(n);
    notes_started_++;
    midi_.OnNoteOn(*this, t, tr.notes.back(), vel, region, stereo);

    return true;
}

void MidiRecorder::OnNoteOnStart(Performer& engine, int t)
{
    if (!tracks[t].played)
    {
        tracks[t].played = true;
        UpdateOutputs(engine, t);
    }
}

void MidiRecorder::OnNoteOn(Performer& engine, int t, Note& n, int velocity, const VelocityRegion* region, bool stereo)
{
    // Use nw::snd's default pan curve and dual mode for stereo waves. On the Wii, the default pan curve, with the
    // sound's pan mode.
    if (engine.Wii())
    {
        if (velocity > 0 && engine.sound_.pan_curve != 0)
        {
            Approximate("custom pan curve (using the default)");
        }
    }
    else if (velocity > 0 && (engine.sound_.pan_curve != 0 || (engine.sound_.pan_mode != 0 && stereo)))
    {
        Approximate("custom pan curve or mode (using defaults)");
    }

    // nw::snd adds region pan, track initial pan and track pan before applying its pan law. The synth receives the
    // first two as zone pan and the third as CC10, then combines them by the synth's pan law.
    if (velocity > 0 && region->pan + engine.tracks_[t].init_pan != 64 && engine.tracks_[t].pan.Get() != 0)
    {
        Approximate("combined region and track pan (using the synth pan law)");
    }

    // In the Wii's balance mode a stereo wave's channels keep to their sides, but CC10 pans them as in dual mode.
    if (velocity > 0 && stereo && engine.balance_ && engine.tracks_[t].pan.Get() != 0)
    {
        Approximate("track pan on stereo waves in balance mode (they pan as in dual mode)");
    }

    // AX uses the region's interpolation mode. The SFZ files follow linear interpolation. SoundFont synths use their
    // usual interpolation. That interpolation most closely matches the default mode.
    if ((engine.settings_.engine == Engine::kCafe || engine.settings_.engine == Engine::kCtr) && velocity > 0 &&
        region->interpolation == 1)
    {
        Approximate("linear sample interpolation (SoundFont uses the synth's interpolation)");
    }
    else if (engine.settings_.engine == Engine::kCafe && velocity > 0 && region->interpolation != 0)
    {
        Approximate("uninterpolated sample playback (using synth interpolation)");
    }
    else if (engine.settings_.engine == Engine::kCtr && velocity > 0 && region->interpolation == 2)
    {
        Approximate("uninterpolated sample playback (SoundFont uses the synth's interpolation)");
    }

    MidiNoteOn(engine, t, n, velocity, region);
}

bool Performer::Legato(int t, Note& n, int key, int velocity, int32_t length, bool past_cap)
{
    Track& tr = tracks_[t];
    const bool heard = !BelowHearing(n);
    n.key = static_cast<uint8_t>(key);
    if (tr.mono)
    {
        n.length = length;
    }

    // The channel takes the velocity squared as its volume (the Wii's Note::init_volume).
    const float init_volume = static_cast<float>(velocity) / 127.0f;
    n.init_volume = init_volume * init_volume;
    n.env = OverrideEnvelope(n.env, TrackPresetKey(tr));
    StartSweep(n);

    // A legato on a channel below hearing changes nothing that can be heard. Count every other legato as a change,
    // regardless of MIDI representation: that can differ between passes.
    if (heard)
    {
        tr.last_change = tick_;
    }

    return midi_.OnLegato(*this, t, n, key, velocity, past_cap, heard);
}

bool MidiRecorder::OnLegato(Performer& engine, int t, Note& n, int key, int velocity, bool past_cap, bool heard)
{
    if (heard)
    {
        n.midi.quiet_ms = n.QuietMs();
        n.midi.faded_ms = n.FadedMs();
    }

    // A channel that has faded far enough goes on silent. Its MIDI note has ended (MidiRecorder::OnLengthUpdated).
    // Nothing starts another.
    if (!heard && !n.midi.on)
    {
        return false;
    }

    // The tie path of SequenceTrack::NoteOn (0x022C9A4C) sets only the channel's velocity volume. The channel keeps the
    // volume of the region it started on. The 3DS's and the Wii's nw::snd drop that volume. The synth zone applies the
    // volume of the region the MIDI note started on. Once a legato has started a new MIDI note on another region, CC11
    // makes up the difference. A zero-volume zone can't be made audible this way.
    //
    // The 3DS's and the Wii's SequenceTrack::NoteOn set the gain from the new velocity without the region's volume
    // (Settings::keep_first_region_volume). CC11 then takes out the volume of the region that the synth plays.
    const int first_volume = engine.settings_.keep_first_region_volume ? n.region_volume : 127;
    auto with_first_region_volume = [&](double gain)
    {
        if (n.midi.volume == 0 && first_volume > 0 && velocity > 0)
        {
            Approximate("legato on a zero-volume region (audible in the game, silent in the synth)");
        }

        return n.midi.volume > 0 ? gain * std::sqrt(static_cast<double>(first_volume) / n.midi.volume) : gain;
    };

    // Continue the MIDI note if its preset matches. CC11 handles the velocity and pitch bend the key (UpdateOutputs).
    // The note keeps its sample position and original region. At velocity 0, keep the note silent through CC11
    // regardless of preset; ending it would leave a release tail.
    //
    // MIDI can't raise a note's velocity. A legato above the velocity that a MIDI note alone on its track started at
    // therefore raises CC11 above the note's start. Record it. A later pass then starts the note higher (MidiNoteOn);
    // CC11 above 127, past the top velocity of the note's region, lowers the whole sequence (Perform). A note that
    // isn't alone requires a new MIDI note. Lowering CC11 for it would lower the others too.
    //
    // After Settings::voices legatos in a tick, continue regardless of preset, capped at the starting velocity. Count
    // only legatos that would otherwise require a new MIDI note as approximations.
    //
    // A legato on a channel below hearing continues its MIDI note the same way. The game's legato doesn't start the
    // envelope again. A new MIDI note, or a later pass that starts the note higher, would change what can be heard. The
    // gain of such a legato therefore stays at or below the note's start, and the legato records no rise.
    //
    // Envelope settings that the track leaves to the bank (0xff), such as those after an envelope reset, leave the
    // channel's settings as they were (SequenceTrack::NoteOn). They therefore count as the MIDI note's settings.
    auto same_preset = [&]()
    {
        const PresetKey& sent = out.presets[static_cast<std::size_t>(LastSent(tracks[t].output, EventKind::kPreset))];
        PresetKey current = n.midi.lfo_bends ? WithoutLfo(engine.NoteKey(t, n)) : engine.NoteKey(t, n);
        for (auto [kept, from] : {std::pair{&current.attack, &sent.attack}, std::pair{&current.decay, &sent.decay},
                                  std::pair{&current.sustain, &sent.sustain},
                                  std::pair{&current.release, &sent.release}, std::pair{&current.hold, &sent.hold}})
        {
            if (*kept == 0xff)
            {
                *kept = *from;
            }
        }

        return current == sent;
    };
    const bool carries_on =
        n.midi.on && (velocity <= n.midi.velocity || n.midi.liftable) && (velocity <= 0 || same_preset());
    if (carries_on || (n.midi.on && (past_cap || !heard)))
    {
        if (!carries_on && heard)
        {
            const std::string what =
                "more than " + std::to_string(engine.settings_.voices) +
                " legatos in a tick (keeping the MIDI preset and limiting gain to the starting level)";
            Approximate(what.c_str());
        }
        else if (carries_on && heard && velocity > n.midi.start_velocity)
        {
            int& top = rises[n.midi.id];
            top = std::max(top, velocity);
        }

        const int kept = carries_on && heard ? std::max(velocity, 0) : std::clamp<int>(velocity, 0, n.midi.velocity);
        tracks[t].legato_gain = with_first_region_volume(static_cast<double>(kept) / n.midi.velocity);
        tracks[t].legato_bend = static_cast<float>(key - n.midi.key);
        if (engine.Wii())
        {
            n.midi.level = -1.0f; // CC11 sets the level from here (CheckFullLevel)
            CheckFullLevel(engine, t);
        }

        return heard;
    }

    // Start a new MIDI note. Select its region by the new key and velocity. A legato at velocity 0 starts none.
    const VelocityRegion* played = engine.banks_.FindRegion(n.bank_slot, n.program, key, velocity);
    n.midi.volume = played ? played->volume : 127;
    EndMidiNote(engine, t, n);
    MidiNoteOn(engine, t, n, velocity, played, with_first_region_volume(1.0));
    if (n.midi.on)
    {
        Approximate("legato (tie or mono) played as a new note");
    }

    return heard;
}

void MidiRecorder::OnCommand(Performer& engine, int /*t*/, uint32_t cmd, int32_t arg1)
{
    const uint8_t a8 = static_cast<uint8_t>(arg1);
    switch (cmd)
    {
    case 0xd7: // surround pan: the Wii's stereo mix leaves it out
        if (!engine.settings_.surround_pan && arg1 != 0)
        {
            Approximate("surround pan (omitted)");
        }
        break;

    case 0xd9:
        if (engine.settings_.sends == Sends::kNone && a8 != 0)
        {
            Approximate("effect send A (omitted)");
        }
        break;

    case 0xda:
        if (engine.settings_.sends == Sends::kNone && a8 != 0)
        {
            Approximate("effect send B (omitted)");
        }
        break;

    case 0xdb:
        if (engine.settings_.sends != Sends::kReverbDry && a8 != 127)
        {
            Approximate("main send (omitted)");
        }
        break;

    default:
        break;
    }
}

void MidiRecorder::OnTimebase(Performer& engine)
{
    if (engine.tick_ == 0 && engine.timebase_ > 0)
    {
        out.timebase = engine.timebase_;
    }

    OnTempo(engine);
}

void MidiRecorder::OnRunaway(Performer& /*engine*/, int t)
{
    rec.Warn("track " + std::to_string(t) + " stopped after running without waits");
}

void Performer::NoteVariableRead(int t, uint8_t index)
{
    if (index < 32)
    {
        variable_readers_[index] |= static_cast<uint16_t>(1u << t);
    }
}

void Performer::NoteVariableChange(int t, uint8_t index)
{
    if (index >= 32)
    {
        return;
    }

    for (int i = 0; i < 16; i++)
    {
        if (i != t && (variable_readers_[index] >> i & 1) && tracks_[i].open && !WaitsForever(i))
        {
            tracks_[t].last_signal = commands_run_;
            return;
        }
    }
}

void Performer::MuteNotes(int t, uint8_t mode)
{
    Track& tr = tracks_[t];
    if (mode == 2)
    {
        ReleaseAndDetach(t);
    }
    else if (mode == 3)
    {
        midi_.OnStopAll(*this, t);
        for (Note& n : tr.notes)
        {
            n.released = true;
            n.release_end_ms = now_ms_;
            Ended(n);
        }

        tr.notes.clear();
    }
}

bool Performer::Leavable(int t, uint64_t first_command) const
{
    const Track& tr = tracks_[t];
    const auto call = [](const CallEntry& e)
    {
        return !e.is_loop;
    };
    if (std::none_of(tr.calls.begin(), tr.calls.begin() + tr.call_depth, call) || tr.leave_at <= first_command ||
        tr.note_at > first_command)
    {
        return false;
    }

    // A note of kHeldTicks or more on a looping wave sounds until the game stops it. A note below hearing doesn't
    // count.
    const auto sustained = [&](const Note& n)
    {
        return !n.released && !n.one_shot && !BelowHearing(n);
    };
    return !tr.holds || std::ranges::none_of(tr.notes, sustained);
}

void Performer::NumberCalls(int t)
{
    Track& tr = tracks_[t];
    tr.numbered_calls = std::min(tr.numbered_calls, tr.call_depth);
    while (tr.numbered_calls < tr.call_depth)
    {
        tr.call_ids[tr.numbered_calls++] = ++call_serial_;
    }
}

uint64_t Performer::InnermostCall(int t) const
{
    const Track& tr = tracks_[t];
    for (int i = tr.call_depth - 1; i >= 0; i--)
    {
        if (!tr.calls[i].is_loop)
        {
            return tr.call_ids[i];
        }
    }

    return 0;
}

bool Performer::ReturnedFrom(int t, uint64_t call) const
{
    const Track& tr = tracks_[t];
    return call != 0 && std::find(tr.call_ids.begin(), tr.call_ids.begin() + tr.call_depth, call) ==
                            tr.call_ids.begin() + tr.call_depth;
}

void Performer::OnLoop(int t, uint32_t target, bool jump)
{
    // A backward jump to unvisited commands is not a loop; shared code may precede its callers. Also ignore passes
    // without waits, such as random retries or variable polling. A jump back to commands that last ran in a call that
    // the track has since returned from doesn't end a pass either. A random retry can jump into another subroutine's
    // copy of its draw. That copy last ran in an earlier pass.
    const auto run = last_run_[t].find(target);
    Track& tr = tracks_[t];
    const std::optional<uint32_t> last_wait = tr.last_wait;
    if (run == last_run_[t].end() || !last_wait || *last_wait < run->second.tick || ReturnedFrom(t, run->second.call))
    {
        return;
    }

    // A loop that the track jumps back to during a pass of another loop is an inner loop, such as a countdown of waits
    // between the notes of an outer loop. The outer loop repeats the inner loop with everything else. Later passes of
    // an inner loop therefore don't count. The first pass of the outer loop shows which loops are inner. These passes,
    // and passes that can leave their loop (below), don't set Track::idle. It keeps the value from the other passes of
    // the track. An inner loop counts again once the track has played no pass of another loop for kMinLoopSeconds. The
    // outer loop may then take minutes to repeat. A countdown can compare a counter that no command resets with a
    // constant. Its later passes then run until the counter wraps.
    const uint32_t start = run->second.tick;
    const uint64_t first_command = run->second.order;
    for (const auto& [other, at] : tr.jump_backs)
    {
        if (other != target && at > first_command)
        {
            tr.inner_loops.insert(other);
        }
    }

    tr.jump_backs[target] = commands_run_;
    if (tr.inner_loops.contains(target))
    {
        if (now_ms_ - tr.last_pass_ms < kMinLoopSeconds * 1000.0)
        {
            return;
        }

        tr.inner_loops.erase(target);
    }

    // A jump back inside a subroutine isn't a loop when its pass ran a conditional command that can leave the loop and
    // played no note. A subroutine that polls a variable after a one-shot sound does that. A stricter rule would leave
    // out every pass that runs a conditional command. But a game may loop a melody, or move the pitch of a note it has
    // played, in such a subroutine until it stops the sound. And a conditional setvar can't leave the loop.
    if (jump && settings_.subroutine_loop_rule && Leavable(t, first_command))
    {
        return;
    }

    tr.last_pass_ms = now_ms_;

    // A pass that changed nothing is idle: the track only waited, polled or counted. Changing a variable that another
    // open track reads counts as a change (Track::last_signal). It belongs to the pass if it came after the pass's
    // first command. The tick doesn't show that: a loop's last commands run in the tick that its next pass starts in.
    auto changed = [&](int i)
    {
        const Track& other = tracks_[i];
        return (other.last_change && *other.last_change >= start) ||
               (other.last_signal && *other.last_signal > first_command);
    };
    tr.idle = !changed(t);

    // Use the lowest-numbered track that can still advance: track 0 while active, or a later track for continuing sound
    // effects. A pass counts only if no earlier track contributed during it. The marked section then contains only
    // repeating material. A track whose last pass was idle doesn't count as advancing. An idle pass of this track
    // counts only if no later track advances or contributed either. A later track that has been quiet for kHoldSeconds
    // doesn't count as advancing: it may wait minutes in silence before it loops or closes.
    auto plays_on = [&](int i)
    {
        return tracks_[i].open && !WaitsForever(i) && !tracks_[i].idle && (i < t || !Quiet(i));
    };
    for (int i = 0; i < 16; i++)
    {
        if (i != t && (i < t || tr.idle) && (plays_on(i) || changed(i)))
        {
            return;
        }
    }

    // A pass that started before the game's last change of a variable can play otherwise than the passes after it
    // (ChangeVariables). It doesn't count.
    if (start < changed_at_)
    {
        return;
    }

    // A pass that changed something, of another loop than the marked one, means the marked pass doesn't repeat what
    // plays now. That happens when an earlier track has gone idle while a later track's loop plays on. Mark this pass
    // instead. An idle pass counts only while nothing else changes. Any idle pass then repeats the sound. Such a pass
    // therefore keeps the marked loop. It doesn't when the pass plays on the track of the marked loop, or when that
    // track has closed. The marked loop then no longer plays.
    const std::pair<int, uint32_t> owner{t, target};
    const int marked_track = loop_owner_ ? loop_owner_->first : t;
    if (!loop_owner_ || (*loop_owner_ != owner && (!tr.idle || marked_track == t || !tracks_[marked_track].open)))
    {
        midi_.out.loop.reset();
        midi_.out.loops = 0;
        loop_owner_ = owner;
        owner_passes_.clear();
        marked_ = 0;
        extended_ = false;
    }

    if (owner == *loop_owner_)
    {
        owner_passes_.emplace_back(start, tick_);
    }

    midi_.out.loops++;
    if (!midi_.out.loop)
    {
        midi_.out.loop = Performance::Loop{start, tick_};
    }

    // The performance stops at a pass that repeats, once it has played the passes asked for and kMinLoopSeconds. It
    // plays on while a change of a variable is still to come. The change can change what the loop plays.
    if (midi_.out.loops >= options_.loops && now_ms_ >= kMinLoopSeconds * 1000.0 - settings_.early_ticks_ms &&
        next_change_ == options_.changes.size() && MarkSteadyPass(t, target))
    {
        stop_ = true;
        stopped_by_ = t;
        held_sound_end_ms_ = HeldSoundEnd(start);
    }
}

std::optional<double> Performer::HeldSoundEnd(uint32_t start) const
{
    if (now_ms_ - tick_ms_[start] <= kHoldSeconds * 1000.0)
    {
        return std::nullopt;
    }

    // Find the last change: the last MIDI event or tempo change.
    uint32_t last = 0;
    for (const std::vector<TrackEvent>& events : midi_.out.tracks)
    {
        if (!events.empty())
        {
            last = std::max(last, events.back().tick);
        }
    }
    if (!midi_.out.tempo.empty())
    {
        last = std::max(last, midi_.out.tempo.back().tick);
    }
    if (now_ms_ - tick_ms_[last] <= kHoldSeconds * 1000.0)
    {
        return std::nullopt;
    }

    // Every sounding note must be endless, unreleased and the newest note of an open track with tie or mono on.
    bool sounding = false;
    for (const Track& tr : tracks_)
    {
        for (const Note& n : tr.detached)
        {
            if (n.EndMs() > now_ms_)
            {
                return std::nullopt;
            }
        }

        for (std::size_t k = 0; k < tr.notes.size(); k++)
        {
            const Note& n = tr.notes[k];
            if (n.EndMs() <= now_ms_ || BelowHearing(n))
            {
                continue;
            }
            if (!tr.open || !(tr.tie || tr.mono) || n.released || n.EndMs() < kNever || k + 1 != tr.notes.size())
            {
                return std::nullopt;
            }

            sounding = true;
        }
    }
    if (!sounding)
    {
        return std::nullopt;
    }

    return tick_ms_[last] + kHoldSeconds * 1000.0;
}

bool Performer::SilentLoop() const
{
    const uint32_t start = midi_.out.loop->start;
    for (const std::vector<TrackEvent>& events : midi_.out.tracks)
    {
        const auto starts_note = [start](const TrackEvent& e)
        {
            return e.kind == EventKind::kNoteOn && e.tick >= start;
        };
        if (std::ranges::any_of(events, starts_note))
        {
            return false;
        }
    }

    // A sunk channel (MidiNote::sunk) has ended its MIDI note, and its sound ends at its fade.
    for (const Track& tr : tracks_)
    {
        for (const auto* list : {&tr.notes, &tr.detached})
        {
            for (const Note& n : *list)
            {
                if (n.midi.on || n.SoundEndMs() >= kNever)
                {
                    return false;
                }
            }
        }
    }

    return true;
}

void Performer::AllocateTracks()
{
    for (int t = 0; t < 16; t++)
    {
        tracks_[t].Init();
        tracks_[t].allocated = (sound_.allocate_track_flags >> t) & 1;
    }

    // The Wii allocates only the tracks that the sound's flags select (SeqPlayer::Setup, main.dol 0x8003e1f4).
    if (Wii())
    {
        return;
    }

    tracks_[0].allocated = true;

    // A leading alloctrack (0xfe, 16-bit mask) also allocates tracks. The start offset can be 0xffffffff. Guard against
    // addition wrapping.
    const std::size_t start = sound_.start_offset;
    if (start < seq_.data.size() && seq_.data.size() - start > 2 && seq_.data[start] == 0xfe)
    {
        const uint32_t first = seq_.data[start + 1], second = seq_.data[start + 2];
        const uint32_t mask = ArgumentsBigEndian() ? first << 8 | second : second << 8 | first;
        for (int t = 0; t < 16; t++)
        {
            if ((mask >> t) & 1)
            {
                tracks_[t].allocated = true;
            }
        }
    }
}

template <typename Core>
void Performer::MakeCore(Regions& banks, int channels, int voices)
{
    Core& core = core_.template emplace<Core>(banks, channels, voices);
    core.stop_runaway_tracks = settings_.stop_runaway_tracks;

    // The seed is the random state when the game starts the sound. On the Switch, a sound starts in the second sound
    // frame after that: the first takes the start to the sound thread. The state steps once a frame
    // (SoundThread::FrameProcess, 0xc18590). The first tick draws from the state one step on.
    core.Rng() = nwsnd::Random(options_.seed);
    for (int i = 0; i < settings_.random_steps_before_start; i++)
    {
        core.Rng().Next();
    }

    core.GlobalVariables().fill(Wii() ? 0 : -1);
    if constexpr (std::is_same_v<Core, RevolutionCore>)
    {
        core.build = settings_.wii_build;
    }
}

void Performer::StartTick()
{
    ChangeVariables();

    // The core begins its tick: SequenceSoundPlayer::ParseNextTick (0x022B70AC) moves the main volume a tick on before
    // the tracks' ticks. The performer takes the main volume from the core.
    const auto begin_tick = [&](auto& core)
    {
        core.now_ms = now_ms_;
        core.GetPlayer().BeginTick();
        core_open_ = false;
        main_volume_.Take(core.GetPlayer().MainVolume());
    };
    WithCore(begin_tick);
}

void Performer::ChangeVariables()
{
    const std::vector<VariableChange>& changes = options_.changes;
    if (next_change_ == changes.size() || changes[next_change_].frame > frame_)
    {
        return;
    }

    // The core and the performer take each change of a player or a global variable.
    while (next_change_ < changes.size() && changes[next_change_].frame <= frame_)
    {
        const VariableChange& change = changes[next_change_++];
        if (change.index < 32)
        {
            *Variable(0, change.index) = change.value;
            WithCore([&](auto& core) { core.GetPlayer().SetVariable(core, change.index, change.value); });
        }
    }

    // The passes of the main loop so far can play otherwise than the passes after the change. The search for the main
    // loop starts again with the first pass that starts after it (OnLoop).
    changed_at_ = tick_;
    midi_.out.loop.reset();
    midi_.out.loops = 0;
    loop_owner_.reset();
    owner_passes_.clear();
    marked_ = 0;
    extended_ = false;
}

bool Performer::ParseTracks(int first)
{
    bool any_open = false;
    WithCore([&](auto& core) { any_open = ParseCoreTracks(core, first); });

    return any_open;
}

template <typename Core>
bool Performer::ParseCoreTracks(Core& core, int first)
{
    core.performer = this;
    bool any_open = false;
    for (int t = first; t < 16 && !stop_; t++)
    {
        // The core's tick of the track counts its channels' lengths down (FollowLengths), moves the track's ramps on
        // and runs the commands. The performer takes the ramps from the core (FollowCommandStart).
        Track& tr = tracks_[t];
        const bool was_open = tr.open;
        const uint64_t commands = commands_run_;
        counting_lengths_ = true;
        const bool open = core.GetPlayer().RunTrackTick(core, t);
        counting_lengths_ = false;
        core_open_ = open || core_open_;

        // A pass that stops the performance stops in the middle of the track's tick, at a jump (Run). The core's track
        // stops at the same command (StopsTick). The performer's track keeps its state.
        if (stop_)
        {
            break;
        }

        any_open = FinishCoreTrackTick(core, t, was_open, open, commands_run_ != commands) || any_open;
    }

    return any_open;
}

template <typename Core>
void Performer::ContinueCoreTrack(Core& core, int t)
{
    core.performer = this;
    const uint64_t commands = commands_run_;
    const bool open = core.GetPlayer().ContinueTrackTick(core, t);
    core_open_ = open || core_open_;
    FinishCoreTrackTick(core, t, true, open, commands_run_ != commands);
}

template <typename Core>
bool Performer::FinishCoreTrackTick(Core& core, int t, bool was_open, bool open, bool ran)
{
    SettleNote(false);
    Track& tr = tracks_[t];
    if (was_open && !open)
    {
        CloseTrack(t);
        tr.allocated = false; // the player frees a finished track
    }
    else if (ran)
    {
        // A track that ran its commands up to a wait waits again. One that stopped at the parse limit reads on in the
        // next tick.
        const auto* track = core.GetPlayer().Track(t);
        if (track && (track->Wait() != 0 || track->NoteFinishWait()))
        {
            tr.last_wait = tick_;
        }
    }

    TakeCoreTrack(core, t);
    TakeCorePlayer(core);

    return was_open && open;
}

void Performer::FollowCommandStart(int t, uint32_t offset, uint32_t command, bool conditional)
{
    SettleNote(false);
    if (stop_)
    {
        return;
    }

    Track& tr = tracks_[t];
    NumberCalls(t);
    last_run_[t][offset] = {tick_, commands_run_++, InnermostCall(t)};
    tr.held_wait = false;

    // The command runs on in the track's stretch of commands, or begins the next stretch (Performance::stretches).
    std::vector<Stretch>& stretches = midi_.out.stretches[t];
    tr.command = offset;
    if (tr.next_stretch)
    {
        stretches.push_back({.tick = tick_,
                             .first = offset,
                             .last = offset,
                             .entry = *tr.next_stretch,
                             .depth = static_cast<uint8_t>(tr.call_depth),
                             .note_wait = tr.note_wait});
        tr.next_stretch.reset();
    }
    else
    {
        stretches.back().last = offset;
    }

    // A conditional jump, call, return or fin can leave a loop (see OnLoop).
    if (conditional && (command == 0x89 || command == 0x8a || command == 0xfd || command == 0xff))
    {
        tr.leave_at = commands_run_;
    }

    // The core moved the track's ramps on at the start of its tick.
    WithCore([&](auto& core) { TakeCoreRamps(core, t); });
}

void Performer::FollowCommand(int t, uint32_t command, int32_t arg1, int32_t arg2, uint32_t next)
{
    // The extended command 0xe0 calls into the game. The performer records nothing for it.
    if (stop_ || command == 0xf0e0)
    {
        return;
    }

    tracks_[t].current = next;
    WithCore([&](auto& core) { RecordCoreCommand(core, t, command, arg1, arg2); });

    // A command that moved the track elsewhere ends the track's stretch of commands.
    if (tracks_[t].current != next)
    {
        tracks_[t].next_stretch = EntryAfter(command);
    }
}

template <typename Core>
void Performer::RecordCoreCommand(Core& core, int t, uint32_t cmd, int32_t arg1, int32_t arg2)
{
    // A jump back, and the end of an endless loop, close a pass of a loop (OnLoop). The analysis takes the track's
    // position and loops from before the command.
    Track& tr = tracks_[t];
    if (cmd == 0x89 && static_cast<uint32_t>(arg1) < tr.current)
    {
        OnLoop(t, static_cast<uint32_t>(arg1), true);
    }
    if (cmd == 0xfc && tr.call_depth != 0)
    {
        // The Wii takes the top of the call stack as the loop, whether a loop start or a call put it there
        // (kLoopEndIgnoresCalls in nwsnd's policies). A counted loop in endless_loops_ closes a pass like a loop of
        // count 0.
        const CallEntry& e = tr.calls[tr.call_depth - 1];
        if ((e.is_loop || Wii()) && (e.count == 0 || endless_loops_.contains({t, e.address})))
        {
            OnLoop(t, e.address, false);
        }
    }

    // A variable command's variable before the command, and the track's LFOs.
    const uint32_t op = cmd & 0xff;
    const bool lfo_command = (op >= 0xa0 && op <= 0xb1) || (op >= 0xe1 && op <= 0xe6);
    const uint8_t index = static_cast<uint8_t>(arg1);
    const int16_t* variable = cmd > 0xff && !lfo_command ? Variable(t, index) : nullptr;
    const int16_t before = variable ? *variable : 0;
    const PresetKey preset_before = TrackPresetKey(tr);

    TakeCoreTrack(core, t);
    TakeCorePlayer(core);
    if (cmd > 0xff)
    {
        if (!variable)
        {
            return;
        }

        if (op >= 0x90)
        {
            NoteVariableRead(t, index);
        }
        if (op == 0x86)
        {
            last_draw_ = tick_;
        }
        if (*variable != before)
        {
            NoteVariableChange(t, index);
        }

        return;
    }

    switch (cmd)
    {
    case 0x88: // opentrack: a track that reached FIN was freed and can't be opened again
        if (arg1 < 16 && arg1 != t && tracks_[arg1].allocated)
        {
            CloseTrack(arg1);
            OpenTrack(arg1, static_cast<uint32_t>(arg2));
        }
        break;

    case 0xb0: // timebase: the Wii's build of October 2006 has no such command
        if (!Wii() || !settings_.wii_build.October2006())
        {
            midi_.OnTimebase(*this);
        }
        break;

    case 0xb2:
        if (tr.mono)
        {
            ReleaseAndDetach(t);
        }
        break;

    case 0xc8:
        ReleaseAndDetach(t);
        break;

    case 0xb4:
    case 0xb5:
    case 0xd8:
        SetFilter(t);
        break;

    case 0xbd:
    case 0xbe:
    case 0xcb:
    case 0xcc:
    case 0xcd:
    case 0xe0:
    case 0xe4:
        if (!SameLfo(TrackPresetKey(tr), preset_before))
        {
            midi_.GiveStruckNotesLfo(*this, t);
        }
        break;

    case 0xdd:
        MuteNotes(t, static_cast<uint8_t>(arg1));
        break;

    case 0xe1:
        midi_.OnTempo(*this);
        break;

    default:
        break;
    }

    midi_.OnCommand(*this, t, cmd, arg1);
}

void Performer::FollowNoteOn(int t, nwsnd::ChannelId channel, int key)
{
    if (pending_note_ && pending_note_->t == t)
    {
        settling_channel_ = channel;
        SettleNote(false);
        settling_channel_ = nwsnd::kNoChannel;
    }

    if (note_to_pair_ == t && !tracks_[t].notes.empty())
    {
        tracks_[t].notes.back().core_channel = channel;
        tracks_[t].notes.back().core_key = key;
    }
    note_to_pair_ = -1;
}

void Performer::FollowLegato(int t, nwsnd::ChannelId channel)
{
    if (pending_note_ && pending_note_->t == t)
    {
        settling_channel_ = channel;
        SettleNote(false);
        settling_channel_ = nwsnd::kNoChannel;
    }
}

void Performer::FollowNoChannel(int t)
{
    if (pending_note_ && pending_note_->t == t)
    {
        SettleNote(VoiceLimit() > 0);
    }
}

void Performer::FollowChannelDrop(int t, nwsnd::ChannelId channel)
{
    if (stop_ || VoiceLimit() == 0)
    {
        return;
    }

    // A note past its track's cap for one tick takes a voice from a note that the performer keeps. NoteOn omits the
    // newer note: the cap keeps a tick's first notes, and the game keeps the newest.
    if (pending_note_)
    {
        const Track& taker = tracks_[pending_note_->t];
        const int started = taker.note_tick == tick_ ? taker.tick_notes : 0;
        if (started >= TickNoteCap())
        {
            return;
        }
    }

    // The track's newest note on the channel. An older note's channel has ended, and a newer channel can take its
    // place.
    auto& notes = tracks_[t].notes;
    const auto on_channel = [channel](const Note& n)
    {
        return n.core_channel == channel;
    };
    const auto found = std::find_if(notes.rbegin(), notes.rend(), on_channel);
    if (found != notes.rend())
    {
        StopNote(t, *found);
        found->core_channel = nwsnd::kNoChannel;
    }
}

void Performer::FollowGainAboveFull(int t, nwsnd::ChannelId channel)
{
    if (t < 0 || static_cast<std::size_t>(t) >= tracks_.size())
    {
        return;
    }

    // The track's newest note on the channel, on the track or let go. An older note's channel has ended, and a newer
    // channel can take its place.
    const auto on_channel = [channel](const Note& n)
    {
        return n.core_channel == channel;
    };
    for (auto* list : {&tracks_[t].notes, &tracks_[t].detached})
    {
        const auto found = std::find_if(list->rbegin(), list->rend(), on_channel);
        if (found == list->rend())
        {
            continue;
        }

        if (!found->midi.lfo_above_full)
        {
            found->midi.lfo_above_full = true;
            midi_.Approximate(found->midi.lfo_fit != VolumeLfoFit{}
                                  ? "volume LFO above full level (limited in the game; the SoundFont and SFZ swing "
                                    "less, from a lower level)"
                                  : "volume LFO above full level (limited in the game; the SoundFont and SFZ swing "
                                    "above it)");
        }

        return;
    }
}

void Performer::FollowVolumeLfo(int t, nwsnd::ChannelId channel, float envelope_db, float lfo_db)
{
    if (t < 0 || static_cast<std::size_t>(t) >= tracks_.size())
    {
        return;
    }

    // The track's newest note on the channel, on the track or let go (FollowGainAboveFull). A note without a MIDI note
    // has id 0.
    const auto on_channel = [channel](const Note& n)
    {
        return n.core_channel == channel;
    };
    for (auto* list : {&tracks_[t].notes, &tracks_[t].detached})
    {
        const auto found = std::find_if(list->rbegin(), list->rend(), on_channel);
        if (found == list->rend())
        {
            continue;
        }

        if (found->midi.id != 0)
        {
            const LfoSetting& lfo = tracks_[t].lfos[0];
            const double scale = lfo.depth != 0 ? 2.0 * ModulationValue(lfo) / lfo.depth : 0.0;
            midi_.OnVolumeLfo(found->midi.id, envelope_db, lfo_db, scale);
        }

        return;
    }
}

template <typename Core>
const typename Core::Channel* Performer::CoreChannelOf(Core& core, const Note& n)
{
    if (n.core_channel == nwsnd::kNoChannel || n.core_key < 0)
    {
        return nullptr;
    }

    // A freed channel's place can hold a newer channel. The note's channel still plays the voice with the note's key.
    const auto& channels = core.Channels();
    const auto& in_use = channels.InUse();
    if (std::find(in_use.begin(), in_use.end(), n.core_channel) == in_use.end())
    {
        return nullptr;
    }

    const auto& channel = channels.At(n.core_channel);
    if (channel.voice == nwsnd::kNoVoice)
    {
        return nullptr;
    }

    return core.PositionKey(channel.voice) == n.core_key ? &channel : nullptr;
}

template <typename Core>
void Performer::TakeCoreWaves(Core& core)
{
    for (Track& tr : tracks_)
    {
        for (auto* list : {&tr.notes, &tr.detached})
        {
            for (Note& n : *list)
            {
                const auto end = n.core_key >= 0 ? core.PositionOf(n.core_key) : std::nullopt;
                if (end)
                {
                    n.one_shot = end->one_shot;
                    n.core_end = end->frame;
                }
            }
        }
    }
}

template <typename Core>
void Performer::TakeCoreLfos(Core& core, uint64_t frame)
{
    if (midi_.lfo_notes.empty())
    {
        return;
    }

    for (Track& tr : tracks_)
    {
        for (auto* list : {&tr.notes, &tr.detached})
        {
            for (Note& n : *list)
            {
                const auto* channel = n.midi.lfo_bends ? CoreChannelOf(core, n) : nullptr;
                if (!channel)
                {
                    continue;
                }

                // Channel::Update adds the pitch LFO's value to the frame's pitch before the LFO steps on. A copy of
                // the LFO gives the value. The copy draws from a copy of the random generator. The core's generator
                // stays as it was. A random curve's numbers come from that copy. They can differ from those in the
                // game. A note reports such a curve once, when the curve first moves the note's pitch.
                float value = 0.0f;
                if (channel->lfo_types[0] == 0)
                {
                    auto lfo = channel->lfos[0];
                    nwsnd::Random random = core.Rng();
                    value = lfo.GetValue(random);
                    if (lfo.param.curve == 4 && value != 0.0f && !n.midi.lfo_random)
                    {
                        n.midi.lfo_random = true;
                        midi_.Approximate(
                            "random LFO curve in pitch bends (its numbers can differ from those in the game)");
                    }
                }

                // The note keeps the values from frame_ - 1 on.
                const uint64_t kept = frame_ > 0 ? frame_ - 1 : 0;
                if (n.lfo_pitch.empty() || n.lfo_frame + n.lfo_pitch.size() != frame)
                {
                    n.lfo_pitch.clear();
                    n.lfo_frame = frame;
                }
                else if (n.lfo_frame < kept)
                {
                    const uint64_t gone = std::min<uint64_t>(kept - n.lfo_frame, n.lfo_pitch.size());
                    n.lfo_pitch.erase(n.lfo_pitch.begin(), n.lfo_pitch.begin() + static_cast<std::ptrdiff_t>(gone));
                    n.lfo_frame += gone;
                }
                n.lfo_pitch.push_back(value);
            }
        }
    }
}

template <typename Core>
void Performer::RunCoreTail(Core& core, uint64_t until)
{
    const auto playing = [this, &core](uint64_t frame)
    {
        for (const Track& tr : tracks_)
        {
            for (const auto* list : {&tr.notes, &tr.detached})
            {
                for (const Note& n : *list)
                {
                    if (n.core_key >= 0 && n.one_shot && !n.WaveEndedBy(frame))
                    {
                        return true;
                    }
                    if (n.midi.lfo_bends && static_cast<double>(frame) * settings_.frame_ms < n.EndMs() &&
                        CoreChannelOf(core, n))
                    {
                        return true;
                    }
                }
            }
        }

        return false;
    };

    for (uint64_t frame = frame_; frame < until && playing(frame); frame++)
    {
        if (frame > frame_)
        {
            core.BeginFrame();
        }
        if (core.GetPlayer().IsActive())
        {
            core.GetPlayer().UpdateTrackParams(core);
        }
        TakeCoreLfos(core, frame);
        core.FinishFrame();
        TakeCoreWaves(core);
    }
}

void Performer::FollowRandomDraw()
{
    if (stop_)
    {
        return;
    }

    last_draw_ = tick_;
}

void Performer::FollowVariableRead(int t, int index)
{
    if (stop_)
    {
        return;
    }

    NoteVariableRead(t, static_cast<uint8_t>(index));
}

void Performer::FollowNoteCommand(int t, int key, int velocity, int32_t length)
{
    if (stop_)
    {
        return;
    }

    // The note waits for the core's verdict on its channel (SettleNote).
    Track& tr = tracks_[t];
    tr.holds = length >= kHeldTicks;
    note_to_pair_ = -1;
    if (!tr.mute)
    {
        const auto stretch = static_cast<uint32_t>(midi_.out.stretches[t].size() - 1);
        pending_note_ = PendingNote{t, key, velocity, length, {tr.command, stretch}};
    }

    if (tr.note_wait)
    {
        tr.wait = length;
        tr.held_wait = length >= kHeldTicks;
        if (length == 0)
        {
            tr.note_finish_wait = true;
        }
    }
}

void Performer::SettleNote(bool drop)
{
    if (!pending_note_)
    {
        return;
    }

    // A note that NoteOn starts waits for the core's channel (FollowNoteOn).
    const PendingNote note = *pending_note_;
    pending_note_.reset();
    settling_command_ = note.command;
    const uint64_t started = notes_started_;
    if (NoteOn(note.t, note.key, note.velocity, note.length > 0 ? note.length : -1, drop))
    {
        tracks_[note.t].note_at = commands_run_;
    }
    if (notes_started_ != started)
    {
        note_to_pair_ = note.t;
    }
}

template <typename Core>
void Performer::TakeCoreTrack(Core& core, int t)
{
    const auto* track = core.GetPlayer().Track(t);
    if (!track)
    {
        return;
    }

    Track& tr = tracks_[t];
    tr.open = track->IsOpen();
    tr.current = track->Current();
    tr.wait = track->Wait();
    tr.note_finish_wait = track->NoteFinishWait();
    tr.holds = track->Holds();
    tr.held_wait = track->HeldWait();
    const auto calls = track->CallStack();
    tr.call_depth = static_cast<int>(calls.size());
    for (std::size_t i = 0; i < calls.size(); i++)
    {
        tr.calls[i] = {.is_loop = calls[i].is_loop, .count = calls[i].loop_count, .address = calls[i].address};
    }

    tr.tie = track->Tie();
    tr.mono = track->Mono();
    tr.bank_slot = track->BankIndex();
    tr.program = track->ProgramNumber();
    TakeCoreRamps(core, t);
    tr.bend_range = track->BendRange();
    tr.init_pan = track->InitPan();

    for (int i = 0; i < kLfoCount; i++)
    {
        const nwsnd::CurveLfoParam& lfo = track->LfoParams()[static_cast<std::size_t>(i)];
        tr.lfos[i] = {.depth = static_cast<uint8_t>(lfo.depth * 128.0f),
                      .speed = lfo.speed,
                      .delay = static_cast<int32_t>(lfo.delay),
                      .range = lfo.range,
                      .curve = lfo.curve,
                      .phase = lfo.phase,
                      .type = track->LfoTypes()[static_cast<std::size_t>(i)]};
    }

    tr.attack = track->Attack();
    tr.decay = track->Decay();
    tr.sustain = track->Sustain();
    tr.release = track->Release();
    tr.hold = static_cast<uint8_t>(track->Hold());
    tr.main_send = track->MainSend();
    tr.fx_send_a = track->FxSends()[0];
    tr.fx_send_b = track->FxSends()[1];
    tr.fx_send_c = track->FxSends()[2];

    // The core keeps the filters' values as nw::snd uses them. The performer keeps the commands' arguments.
    tr.low_pass = static_cast<int32_t>(track->Lpf() * 64.0f) + 0x40;
    tr.biquad_type = static_cast<uint8_t>(track->BiquadType());
    tr.biquad_value = static_cast<int32_t>(std::lround(track->BiquadValue() * 127.0f));

    tr.note_wait = track->NoteWait();
    tr.mute = track->IsMuted();
    tr.damper = track->Damper();
    tr.velocity_range = track->VelocityRange();
    tr.vars = track->Variables();
}

template <typename Core>
void Performer::TakeCoreRamps(Core& core, int t)
{
    const auto* track = core.GetPlayer().Track(t);
    if (!track)
    {
        return;
    }

    Track& tr = tracks_[t];
    tr.vol.Take(track->Volume());
    tr.volume2.Take(track->Volume2());
    tr.pan.Take(track->Pan());
    tr.bend.Take(track->Bend());
}

template <typename Core>
void Performer::TakeCorePlayer(Core& core)
{
    const auto& player = core.GetPlayer();
    tempo_ = player.Tempo();
    timebase_ = player.Timebase();
    main_volume_.Take(player.MainVolume());
    player_vars_ = player.LocalVariables();
    global_vars_ = core.GlobalVariables();
}

void MidiRecorder::OnTickEnd(Performer& engine, int t)
{
    RecordMix(engine, t);
    UpdateOutputs(engine, t);
    CheckLfo(engine, t);
    if (tracks[t].played && engine.tracks_[t].open && engine.TrackBusy(t))
    {
        rec.RaiseLevelPeak(TrackLevel(engine, t) * tracks[t].legato_gain);
    }
}

void MidiRecorder::RecordMix(const Performer& engine, int t)
{
    const Track& tr = engine.tracks_[t];
    const std::array<int16_t, kMixSettingCount> values = {static_cast<int16_t>(tr.vol.Get()),
                                                          static_cast<int16_t>(tr.pan.Get()), tr.fx_send_a,
                                                          tr.fx_send_b, tr.fx_send_c};
    std::array<int16_t, kMixSettingCount>& last = tracks[t].mix;
    for (int setting = 0; setting < kMixSettingCount; setting++)
    {
        if (values[setting] != last[setting])
        {
            out.mix[t].push_back({engine.tick_, static_cast<Mix>(setting), values[setting]});
            last[setting] = values[setting];
        }
    }
}

void Performer::FinishTick()
{
    WithCore([&](auto& core) { core.GetPlayer().EndTick(core, core_open_); });

    for (int t = 0; t < 16; t++)
    {
        midi_.OnTickEnd(*this, t);
        if (!tracks_[t].open)
        {
            PruneNotes(t);
        }
    }

    // The Wii's build of October 2006 stops a released note once its volume times its envelope is 0, whether a track
    // owns it or not (Channel::Update, 0x800e039c in Smooth Moves' main.dol). The later builds stop a note that a track
    // owns only when its envelope is silent. The channels update after the frame's last tick. The performer stops only
    // the notes that a track owns.
    if (Wii() && settings_.wii_build.October2006() && frame_ticks_left_ == 0)
    {
        for (int t = 0; t < 16; t++)
        {
            const float user = UserVolume(t);
            for (Note& n : tracks_[t].notes)
            {
                if (n.released && n.EndMs() > now_ms_ && n.init_volume * user == 0.0f)
                {
                    StopNote(t, n);
                }
            }
        }
    }
}

void Performer::ReleasePending(const TickTime& next)
{
    if (!settings_.note_off_waits_for_update || next.frame == frame_)
    {
        return;
    }

    const uint64_t frame = frame_ + 1;
    const double after = (static_cast<double>(frame) * settings_.frame_ms - now_ms_) / (next.ms - now_ms_);
    for (int t = 0; t < 16; t++)
    {
        for (Note& n : tracks_[t].notes)
        {
            if (!n.release_pending)
            {
                continue;
            }

            n.release_pending = false;
            n.released = true;
            n.released_frame = frame;
            n.release_level = HeldLevel(n.env, 1, settings_);
            n.release_rate = DecayRate(n.env.release, settings_.engine);
            n.release_end_ms =
                static_cast<double>(frame + ReleaseFrames(n.release_level, n.release_rate, settings_) + 1) *
                settings_.frame_ms;
            tracks_[t].last_change = tick_;
            midi_.OnDeferredRelease(*this, t, n, after);
        }
    }
}

void MidiRecorder::OnDeferredRelease(Performer& engine, int t, Note& n, double after)
{
    RecordEarlyRelease(engine, n);
    if (n.midi.on)
    {
        n.midi.on = false;
        rec.NoteOff(out, static_cast<uint8_t>(t), engine.tick_, n.midi.part, n.midi.key, n.midi.id, after);
    }
}

std::optional<TickTime> Performer::NextWiiCoreTick()
{
    // The core's clock goes on from the last tick through the frames after it, a frame at a time, until a tick falls in
    // one. The tick's time comes from the milliseconds left in its frame, in the clock's floats. The build of October
    // 2006 runs its ticks at the frame's start.
    auto& player = std::get<RevolutionCore>(core_).GetPlayer();
    TickTime next;
    next.frame = frame_;
    std::optional<uint64_t> left = player.NextTickTime();
    while (!left)
    {
        if (player.ClockStopped())
        {
            return std::nullopt;
        }

        next.frame++;
        player.StartClock(1);
        left = player.NextTickTime();
    }

    next.ms = static_cast<double>(next.frame) * settings_.frame_ms +
              (settings_.frame_ms - static_cast<double>(player.ClockRestMs()));
    if (settings_.wii_build.October2006())
    {
        next.ticks_left = player.ClockTicksLeft();
        next.nominal_ms = nominal_ms_ + MidiMsPerTick();
    }

    return next;
}

void MidiRecorder::OnBetweenTicks(Performer& engine, const TickTime& next)
{
    SweepBetweenTicks(engine, next);
}

void Performer::AdvanceTo(const TickTime& next)
{
    // The core ends the last tick's frame, plays the frames without a tick, and begins the next tick's frame.
    const auto play_frames = [&](auto& core)
    {
        for (uint64_t frame = frame_; frame < next.frame; frame++)
        {
            if (frame > frame_)
            {
                core.BeginFrame();
            }
            if (core.GetPlayer().IsActive())
            {
                core.GetPlayer().UpdateTrackParams(core);
            }
            TakeCoreLfos(core, frame);
            core.FinishFrame();
        }

        TakeCoreWaves(core);
        core.BeginFrame();
    };
    if (next.frame > frame_)
    {
        WithCore(play_frames);
    }

    ReleasePending(next);
    midi_.OnBetweenTicks(*this, next);

    frame_ = next.frame;
    now_ms_ = next.ms;
    if (Wii() && settings_.wii_build.October2006())
    {
        frame_ticks_left_ = next.ticks_left;
        nominal_ms_ = next.nominal_ms;
    }

    tick_++;
    midi_.RecordTickTime(*this);
}

Performance Performer::Run()
{
    // The game's player opens track 0, if the sound allocates it, at the sound's start offset (on the Wii,
    // SeqPlayer::Start, main.dol 0x8003e504). The others always allocate it.
    AllocateTracks();
    if (tracks_[0].allocated)
    {
        OpenTrack(0, sound_.start_offset);
    }

    midi_.OnTempo(*this);
    midi_.RecordTickTime(*this);

    // The core's sound starts in the first tick's frame. The core's clock gives the first tick at the frame's start.
    // The core allocates the tracks that AllocateTracks does. The core alone would allocate only those that the sound's
    // flags select. A sound archive's flags give the tracks of the sequence's alloctrack.
    SoundInfo info = sound_;
    info.allocate_track_flags = 0;
    for (int t = 0; t < 16; t++)
    {
        info.allocate_track_flags |= tracks_[t].allocated ? 1u << t : 0u;
    }

    const auto start = [&](auto& core)
    {
        core.Play(info, seq_.data, 0);
        core.GetPlayer().SetBuild(core.Build());
        for (const auto& [index, value] : options_.variables)
        {
            core.GetPlayer().SetVariable(core, static_cast<int>(index), value);
        }

        core.BeginFrame();
        core.GetPlayer().StartClock(1);
        core.GetPlayer().NextTickTime();
    };
    WithCore(start);

    // The Wii's build of October 2006 starts its count at a whole tick, and its first frame runs one tick.
    if (Wii() && settings_.wii_build.October2006())
    {
        frame_ticks_left_ = std::get<RevolutionCore>(core_).GetPlayer().ClockTicksLeft();
    }

    bool finished = false;
    double steady_since = -1.0;
    PassStart next_pass;
    for (;;)
    {
        StartTick();
        const bool any_open = ParseTracks(0);
        if (stop_)
        {
            // A main loop that sounds nothing is no loop. The sequence ends with its last sound, like one that ends.
            if (midi_.out.loop && !held_sound_end_ms_ && SilentLoop())
            {
                finished = true;
                silent_loop_ = true;
                midi_.out.loop.reset();
                midi_.out.loops = 0;
            }
            else if (midi_.out.loop && !held_sound_end_ms_)
            {
                next_pass = tick_ > midi_.out.loop->end ? PassStartAt(midi_.out.loop->end)
                                                        : NextPassStart(stopped_by_, tracks_[stopped_by_].current);
            }

            break;
        }

        FinishTick();

        // Every track has ended. The game repeats none of the passes before the end, such as a state machine's jumps
        // back to its dispatcher. Drop the loop marked on the way. On the Wii, a sequence also ends when no track will
        // read on and no note sounds (Stuck).
        if (!any_open || (Wii() && Stuck()))
        {
            finished = true;
            midi_.out.loop.reset();
            midi_.out.loops = 0;
            break;
        }

        if (steady_since < 0 && Steady())
        {
            steady_since = now_ms_;
        }
        if (steady_since >= 0 && now_ms_ - steady_since >= kHoldSeconds * 1000.0)
        {
            midi_.out.holds = true;
            break;
        }

        // Treat a sequence that has been silent for longer than kHoldSeconds at the time limit as finished. Such a
        // sequence ends with its last sound and, like a sequence that ends, has no main loop.
        if (now_ms_ >= kMaxSeconds * 1000.0)
        {
            bool sounding = false;
            for (int t = 0; t < 16; t++)
            {
                sounding = sounding || OthersSounding(t, nullptr);
            }
            finished = !sounding && now_ms_ - sound_end_ms_ > kHoldSeconds * 1000.0;
            midi_.out.truncated = !finished;

            // When the performance is cut here, the counted loops that the open tracks are in (LimitLoops). A
            // performance that counts as finished ends with its last sound.
            for (int t = 0; midi_.out.truncated && t < 16; t++)
            {
                const Track& tr = tracks_[t];
                for (int i = 0; tr.open && i < tr.call_depth; i++)
                {
                    if (tr.calls[i].is_loop && tr.calls[i].count != 0)
                    {
                        limit_loops_.insert({t, tr.calls[i].address});
                    }
                }
            }

            if (finished && settings_.drop_loop_when_finished)
            {
                midi_.out.loop.reset();
                midi_.out.loops = 0;
            }

            break;
        }

        const std::optional<TickTime> next = NextTick();
        if (!next)
        {
            midi_.rec.Warn(timebase_ == 0 ? "timebase 0: the sequence stops advancing"
                                          : "tempo 0: the sequence stops advancing");
            PlayOutStopped();
            break;
        }

        AdvanceTo(*next);
    }

    EndPerformance(finished);
    if (midi_.out.loop)
    {
        RestoreAtLoopStart(midi_.out, midi_.rec.PartSwitches(), next_pass, kLoopRules);
    }

    midi_.out.approximations = midi_.rec.Approximations();
    midi_.out.warnings = midi_.rec.Warnings();

    return midi_.out;
}

Performer::PassStart Performer::PassStartAt(uint32_t tick) const
{
    return sequence_export::PassStartAt(midi_.out, midi_.rec.PartSwitches(), tick);
}

Performer Performer::PlayOn(int t, uint32_t target, uint32_t until) const
{
    // Track `t` has jumped back, and the tracks after it haven't run this tick. The copy runs them, then the ticks
    // after, with no limit on the loops. No pass then stops the copy again.
    Performer probe = *this;
    probe.stop_ = false;
    probe.options_.loops = std::numeric_limits<int>::max();
    probe.tracks_[t].current = target;
    probe.WithCore([&](auto& core) { probe.ContinueCoreTrack(core, t); });
    probe.ParseTracks(t + 1);
    probe.FinishTick();

    while (probe.tick_ + 1 < until)
    {
        const std::optional<TickTime> next = probe.NextTick();
        if (!next)
        {
            break;
        }

        probe.AdvanceTo(*next);
        probe.StartTick();
        const bool any_open = probe.ParseTracks(0);
        probe.FinishTick();
        if (!any_open)
        {
            break;
        }
    }

    return probe;
}

Performer::PassStart Performer::NextPassStart(int t, uint32_t target) const
{
    return PlayOn(t, target, tick_ + 1).PassStartAt(tick_);
}

bool Performer::RepeatsAsTheGame(uint32_t start, uint32_t end) const
{
    return sequence_export::RepeatsAsTheGame(midi_.out, midi_.rec.PartSwitches(), start, end, kLoopRules);
}

Performer::LoopChoice Performer::ChooseSteadyPass(int t, uint32_t target) const
{
    // A copy of this performer plays on from the jump back. Its passes are compared.
    auto play_on = [&](uint32_t until)
    {
        return [probe = PlayOn(t, target, until)](uint32_t start, uint32_t end)
        {
            return probe.RepeatsAsTheGame(start, end);
        };
    };

    return sequence_export::ChooseSteadyPass(owner_passes_, marked_, last_draw_, extended_, options_.loops, play_on);
}

bool Performer::MarkSteadyPass(int t, uint32_t target)
{
    // A later run of the sequence makes the choices of the first (Perform). The first run compared its passes without
    // the details that later runs give the notes it played, such as the attacks of early releases (EarlyReleases).
    // Notes that only a copy played lack those details.
    const LoopChoice choice = replayed_ < replay_.size() ? replay_[replayed_++] : ChooseSteadyPass(t, target);
    choices_.push_back(choice);
    if (choice.marked && *choice.marked < owner_passes_.size())
    {
        marked_ = *choice.marked;
        const auto [start, end] = owner_passes_[marked_];
        midi_.out.loop = Performance::Loop{start, end};
        midi_.out.loops = static_cast<int>(owner_passes_.size() - marked_);
    }

    extended_ = extended_ || choice.extend;

    return choice.stop;
}

std::optional<TickTime> Performer::NextTick()
{
    if (Wii())
    {
        return NextWiiCoreTick();
    }

    std::optional<TickTime> next;
    WithCore([&](auto& core) { next = NextCoreTick(core); });

    return next;
}

template <typename Core>
std::optional<TickTime> Performer::NextCoreTick(Core& core)
{
    // The clock goes on from the last tick through the frames after it, a frame at a time, until a tick falls in one.
    auto& player = core.GetPlayer();
    TickTime next{frame_};
    std::optional<uint64_t> left = player.NextTickTime();
    while (!left)
    {
        if (player.ClockStopped())
        {
            return std::nullopt;
        }

        next.frame++;
        player.StartClock(1);
        left = player.NextTickTime();
    }

    const uint64_t frame_units = FrameUnits(settings_);
    next.ms = UnitsToMs(next.frame * frame_units + (frame_units - *left), settings_);

    return next;
}

void Performer::PlayOutStopped()
{
    // The frame's channel update makes the note-offs still pending. Their MIDI note-offs come at most a tick after the
    // last, at the MIDI tempo. As in EndPerformance, the core's frames finish the one-shot waves.
    const double ticks_per_second = midi_.out.tempo.back().bpm * midi_.out.timebase / 60.0;
    const double next_frame_ms = static_cast<double>(frame_ + 1) * settings_.frame_ms;
    ReleasePending({frame_ + 1, std::max(now_ms_ + 1000.0 / ticks_per_second, next_frame_ms)});
    WithCore([&](auto& core) { RunCoreTail(core, FrameOf(now_ms_ + kMaxTailMs) + 1); });

    double end_ms = now_ms_;
    for (const Track& tr : tracks_)
    {
        for (const auto* list : {&tr.notes, &tr.detached})
        {
            for (const Note& n : *list)
            {
                midi_.out.holds = midi_.out.holds || n.EndMs() >= kNever;
                end_ms = std::max(end_ms, std::min(n.EndMs(), now_ms_ + kMaxTailMs));
            }
        }
    }
    if (midi_.out.holds)
    {
        end_ms = now_ms_ + kHoldSeconds * 1000.0;
    }

    // MIDI ticks continue after sequence timing stops. Millisecond sweeps keep moving; note-length sweeps remain frozen
    // with the sequence's ticks (UpdateOutputs).
    const auto ticks = static_cast<uint32_t>(std::ceil((end_ms - now_ms_) / 1000.0 * ticks_per_second));
    const double start_ms = now_ms_;
    for (uint32_t i = 1; i <= ticks; i++)
    {
        const double ms = start_ms + i / ticks_per_second * 1000.0;
        midi_.SweepBetweenTicks(*this, {FrameOf(ms), ms});

        tick_++;
        now_ms_ = ms;
        if (Wii())
        {
            nominal_ms_ += 1000.0 / ticks_per_second;
        }
        frame_ = FrameOf(now_ms_);

        for (int t = 0; t < 16; t++)
        {
            PruneNotes(t); // finished notes have no active sweep
            midi_.UpdateOutputs(*this, t);
        }
    }
}

void Performer::EndPerformance(bool finished)
{
    // A main loop that holds a sound ends kHoldSeconds after its last change, without loop markers. Nothing changed in
    // the ticks after that.
    if (held_sound_end_ms_)
    {
        tick_ = std::min(MsToTick(*held_sound_end_ms_), tick_);
        now_ms_ = tick_ms_[tick_];
        midi_.out.holds = true;
        midi_.out.loop.reset();
        midi_.out.loops = 0;
    }

    // Stop immediately at a loop boundary, time limit or held-sound limit. A naturally finished sequence ends with its
    // final sound, including release tails; nw::snd can leave finished tracks waiting long after silence. The core's
    // frames finish the one-shot waves.
    if (finished)
    {
        WithCore([&](auto& core) { RunCoreTail(core, FrameOf(now_ms_ + kMaxTailMs) + 1); });
    }

    for (int t = 0; t < 16; t++)
    {
        Track& tr = tracks_[t];
        for (auto* list : {&tr.notes, &tr.detached})
        {
            for (Note& n : *list)
            {
                // On the Wii, a note below hearing ends the sequence's sound where it has faded (MidiNote::faded_ms).
                // Stuck can end a sequence there while its tracks wait out held notes.
                if (finished)
                {
                    const double end = Wii() && BelowHearing(n) ? n.midi.faded_ms : n.SoundEndMs();
                    sound_end_ms_ = std::max(sound_end_ms_, std::min(end, now_ms_ + kMaxTailMs));
                }
                midi_.MidiNoteOff(*this, t, n);
            }
        }
    }

    // The tails' timed sweeps go on after the last tick, and their pitch events follow its note-offs.
    if (finished)
    {
        SweepAfterEnd();
    }

    for (Track& tr : tracks_)
    {
        tr.notes.clear();
        tr.detached.clear();
    }

    if (finished)
    {
        // After a main loop that sounded nothing (SilentLoop), what comes after the last sound can't be heard.
        if (silent_loop_)
        {
            const uint32_t end = MsToTick(sound_end_ms_);
            for (std::vector<TrackEvent>& events : midi_.out.tracks)
            {
                std::erase_if(events, [end](const TrackEvent& e) { return e.tick > end; });
            }
            std::erase_if(midi_.out.tempo, [end](const Performance::Tempo& t) { return t.tick > end; });
        }

        uint32_t last_event = 0;
        for (const auto& events : midi_.out.tracks)
        {
            if (!events.empty())
            {
                last_event = std::max(last_event, events.back().tick);
            }
        }
        if (!midi_.out.tempo.empty())
        {
            last_event = std::max(last_event, midi_.out.tempo.back().tick);
        }

        midi_.out.end_tick = std::max(last_event, MsToTick(sound_end_ms_));
        midi_.out.seconds = std::max(sound_end_ms_, tick_ms_[last_event]) / 1000.0;
    }
    else
    {
        midi_.out.end_tick = tick_;
        midi_.out.seconds = now_ms_ / 1000.0;
    }
}

uint32_t Performer::MsToTick(double ms) const
{
    const auto it = std::lower_bound(tick_ms_.begin(), tick_ms_.end(), ms);
    if (it != tick_ms_.end())
    {
        return static_cast<uint32_t>(it - tick_ms_.begin());
    }

    // Extrapolate at the last tempo, or the MIDI tempo if sequence timing stopped.
    const double per_tick = MsPerTick() > 0 ? MsPerTick() : 60000.0 / (midi_.out.tempo.back().bpm * midi_.out.timebase);
    return static_cast<uint32_t>(tick_ms_.size() - 1 + std::ceil((ms - tick_ms_.back()) / per_tick));
}

// Runs as many passes of a sequence's performance as it takes. The counted loops in `endless_loops` loop for ever.
Performance PerformPasses(const SoundInfo& sound, const Sequence& seq, Regions& banks, const PerformOptions& options,
                          const Settings& settings, const std::set<std::pair<int, uint32_t>>& endless_loops = {});

// Finds, reads and performs a CSAR or FSAR archive's sequence.
Performance PerformSar(const SoundArchive& archive, uint32_t sound_index, BankSet& banks, const PerformOptions& options,
                       const Settings& settings)
{
    const SoundInfo& sound = archive.Sounds()[sound_index];
    const auto file = archive.FileData(sound.file_id);
    if (file.empty() && !archive.FileMissing(sound.file_id))
    {
        throw xsflib::FormatError(archive.ExternalGroups().empty()
                                      ? "sequence file is not stored in the archive"
                                      : "sequence file is not stored in the archive or a group file beside it");
    }
    if (file.empty())
    {
        throw xsflib::FormatError(archive.Truncated() ? "sequence extends past the truncated archive"
                                                      : "sequence extends past the archive");
    }

    const Sequence seq = Sequence::Parse(file, settings.archive);
    SarRegions regions(banks);
    return PerformPasses(sound, seq, regions, options, settings);
}

// Finds, reads and performs a Wii archive's sequence. The sound's fields and the sequence's bytecode go to the
// performer in sar's form. The Wii's arguments are big-endian.
Performance PerformRsar(const rsar::SoundArchive& archive, uint32_t sound_index, RsarBankSet& banks,
                        const PerformOptions& options, const Settings& settings)
{
    const rsar::SoundInfo& sound = archive.Sounds()[sound_index];
    const auto file = archive.FileData(sound.file_id);
    if (file.empty() && !archive.FileMissing(sound.file_id))
    {
        throw xsflib::FormatError("sequence file is not stored in the archive");
    }
    if (file.empty())
    {
        throw xsflib::FormatError(archive.Truncated() ? "sequence extends past the truncated archive"
                                                      : "sequence extends past the archive");
    }

    Sequence seq;
    seq.data = rsar::Sequence::Parse(file).data;
    seq.big_endian = true;

    SoundInfo info;
    info.name = sound.name;
    info.file_id = sound.file_id;
    info.volume = sound.volume;
    info.pan_mode = sound.pan_mode;
    info.pan_curve = sound.pan_curve;
    info.allocate_track_flags = sound.allocate_track_flags;
    info.start_offset = sound.start_offset;

    RsarRegions regions(banks, settings.wii_build.Since2008());
    return PerformPasses(info, seq, regions, options, settings);
}

Performance PerformPasses(const SoundInfo& sound, const Sequence& seq, Regions& banks, const PerformOptions& options,
                          const Settings& settings, const std::set<std::pair<int, uint32_t>>& endless_loops)
{
    // Rerun sequences in these cases. When filters change during notes, the rerun moves those filters from presets to
    // controllers. When legatos raise MIDI notes above the velocities they start at, the rerun starts those notes
    // higher (Rises). When rounded CC11 exceeds 127, the rerun scales every value equally, and the tracks keep their
    // balance. When a note's pitch LFO changes while it sounds, the rerun gives the note a part of its own. The pitch
    // bends of that part add the LFO (LfoChanges). On the Wii, when the voice holds a note's volume LFO at full level,
    // the rerun gives the note a preset that fits its level (VolumeLfoFits).
    //
    // Only MIDI events and presets change between passes, but each change can move the others. With a controller-driven
    // filter, a legato may continue its MIDI note (Legato). That numbers the notes differently and changes the peak
    // CC11. A higher start lowers the peak. Note ids stay valid only between passes with the same filters. So run
    // passes until one leaves nothing to change, four at most. A pass whose parts don't fit MIDI's channels also
    // requires another pass. That pass keeps each track to the parts that fit (FitParts). Every later pass does the
    // same.
    std::array<bool, 16> filters{};
    std::map<uint32_t, int> starts;
    std::map<uint32_t, int16_t> attacks;
    std::map<uint32_t, VolumeLfoFit> lfo_fits;
    std::set<uint32_t> lfo_notes;
    double peak = 0.0;    // the CC11 peak the next pass scales for
    double applied = 0.0; // the peak the last pass scaled for
    std::array<uint8_t, 16> part_limits;
    part_limits.fill(16);
    bool limited = false;
    std::vector<Performer::LoopChoice> loop_choices;
    Performance performance;
    for (int pass = 0; pass < 4; pass++)
    {
        Performer performer(settings, sound, seq, banks, options, filters, starts, attacks, lfo_fits, lfo_notes,
                            LevelScale(peak), part_limits, loop_choices);
        performer.SetEndlessLoops(endless_loops);
        performance = performer.Run();

        // A sequence can repeat its music in a counted loop that the game plays for longer than the limit. When the
        // performance is cut at the limit inside counted loops, the sequence is performed again with those loops
        // endless. Their passes then count as loops of the sequence.
        if (endless_loops.empty() && !performer.LimitLoops().empty())
        {
            return PerformPasses(sound, seq, banks, options, settings, performer.LimitLoops());
        }

        applied = peak;
        loop_choices = performer.LoopChoices();

        bool parts_fit = true;
        if (!limited)
        {
            const std::array<uint8_t, 16> fitted = FitParts(performance, performer.PartsUsed());
            for (int t = 0; t < 16; t++)
            {
                parts_fit = parts_fit && fitted[t] >= performer.PartsUsed()[t];
            }
            if (!parts_fit)
            {
                part_limits = fitted;
                limited = true;
            }
        }

        const bool same_filters = performer.FilterMoves() == filters;
        const std::map<uint32_t, int> rises = same_filters ? performer.Rises() : std::map<uint32_t, int>{};
        const std::map<uint32_t, int16_t> early =
            same_filters ? performer.EarlyReleases() : std::map<uint32_t, int16_t>{};
        const std::map<uint32_t, VolumeLfoFit> fits =
            same_filters ? performer.VolumeLfoFits() : std::map<uint32_t, VolumeLfoFit>{};
        const std::set<uint32_t> lfo = same_filters ? performer.LfoChanges() : std::set<uint32_t>{};
        if (parts_fit && same_filters && rises == starts && early == attacks && fits == lfo_fits && lfo == lfo_notes &&
            LevelScale(performer.LevelPeak()) == LevelScale(peak))
        {
            break;
        }

        filters = performer.FilterMoves();
        starts = rises;
        attacks = early;
        lfo_fits = fits;
        lfo_notes = lfo;
        peak = performer.LevelPeak();
    }

    if (LevelScale(applied) < 1.0)
    {
        // CC11 acts squared. Scaling it by 127 / peak makes every note 40 log10(peak / 127) dB quieter.
        performance.level_cut_db = 40.0 * std::log10(applied / 127.0);
        char what[160];
        std::snprintf(what, sizeof(what),
                      "volume exceeds MIDI range (SoundFont playback attenuated by %.1f dB; "
                      "SFZ restores the gain)",
                      performance.level_cut_db);
        performance.approximations[what] = NoteCount(performance);
    }

    return performance;
}

} // namespace

Performance Perform(const SoundArchive& archive, uint32_t sound_index, BankSet& banks, const PerformOptions& options,
                    const Settings& settings)
{
    return PerformSar(archive, sound_index, banks, options, settings);
}

Performance Perform(const rsar::SoundArchive& archive, uint32_t sound_index, RsarBankSet& banks,
                    const PerformOptions& options, const Settings& settings)
{
    return PerformRsar(archive, sound_index, banks, options, settings);
}

} // namespace ntdlib::sequence_export
