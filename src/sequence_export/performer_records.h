// SPDX-License-Identifier: MIT

// The performer's records (see performer_engine.h): each channel that it follows with the MIDI note that plays the
// channel (Note, MidiNote), each track's state and MIDI output (Track, TrackMidi), and the regions and waves that the
// notes play (Regions). Addresses without another source come from the Wii U's nw::snd in Animal Crossing: amiibo
// Festival's executable.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "seqlib/midi/recorder.h"

#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/envelope.h"
#include "ntdlib/nwsnd/sequence.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/envelope.h"
#include "ntdlib/sequence_export/filter.h"
#include "ntdlib/sequence_export/instruments.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/recording_engine.h"
#include "ntdlib/sequence_export/rsar_instruments.h"
#include "ntdlib/sequence_export/settings.h"

namespace ntdlib::sequence_export
{

using sar::Pcm;
using sar::Sequence;
using sar::SoundArchive;
using sar::SoundInfo;
using sar::VelocityRegion;

inline constexpr double kNever = 1e300; // sentinel time in milliseconds

// A one-shot wave that runs out in frame `f` keeps its channel for the ticks of frames up to `f + ReportFrames`. The
// Switch's renderer reports how far a voice has played two frames late. A one-shot wave's end reaches nn::atk two
// frames after the frame that reads the wave's last sample (LowLevelVoiceAllocator::UpdateAllVoiceStatus). AX on the
// Wii U stops a voice in the frame that reads its last sample. On the 3DS and the Wii, the next frame's voice update
// finds the end, after that frame's ticks.
inline uint64_t ReportFrames(const Settings& settings)
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

// The Wii's build of October 2006 counts time in 416ths of a tick. A tick lasts 416 / tempo frames.
inline constexpr int kTickUnits = 416;

// MML gives LFO delays and portamento times in 5 ms units.
inline constexpr int kMmlUnitMs = 5;

// The deepest call stack of any console's track. Track::calls holds a copy of the core's stack.
inline constexpr int kCallDepth =
    std::max({nwsnd::CafePolicy::kCallStackDepth, nwsnd::NxPolicy::kCallStackDepth, nwsnd::CtrPolicy::kCallStackDepth,
              nwsnd::RevolutionPolicy::kCallStackDepth});

// A track's LFOs: the plain MML commands set the first, and extended ones (0xf0 0xa0 onwards) the others.
inline constexpr int kLfoCount = 4;

// A player's tracks, and the parts of a track's MIDI track: one for each of MIDI's 16 channels at most.
using nwsnd::kTrackCount;
inline constexpr uint8_t kPartCount = 16;

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
    VolumeLfoFit lfo_fit;         // the Wii: its volume LFO's fit from an earlier pass (PassFindings::lfo_fits)

    // A MIDI note whose pitch LFO changes while it sounds plays on a part of its own (MidiNoteOn). Its preset has no
    // LFO. The pitch bends of the part add the value of the core channel's pitch LFO in each frame (PartPitch).
    // `lfo_crowded`: no part of its own was free, and the preset keeps the LFO. `lfo_random`: the note has reported
    // the random curve of its pitch bends (TakeCoreLfos).
    bool lfo_bends = false;
    bool lfo_crowded = false;
    bool lfo_random = false;

    // On the Wii U and the Switch: the MIDI note starts past one of the console's pitch limits (PastPitchLimit).
    // `at_limits`: an earlier pass found its pitch still (PassFindings::pitch_moves), and its preset plays it at the
    // limit (PresetKey::at_pitch_limits).
    bool past_limit = false;
    bool at_limits = false;
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

    // The sweep's offset in semitones in sound frame `frame` (nwsnd::SweepValue, as in Channel::GetSweepValue). Timed
    // sweeps advance Settings::env_step_ms per frame after pitch calculation, from the initial frame on. Note-length
    // sweeps advance once per tick.
    float SweepAt(uint64_t frame) const
    {
        const int32_t gone =
            sweep_in_ms
                ? static_cast<int32_t>(std::min<uint64_t>((frame - sweep_frame) * settings->env_step_ms, sweep_length))
                : std::min(sweep_ticks, sweep_length);

        return nwsnd::SweepValue(sweep, sweep_length, gone);
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
    // the note's start, a prediction at the key's pitch times PerformOptions::pitch (RecordingEngine::PredictEnd), and
    // once the voice plays, the frame at the voice's rate (Performer::TakeCoreWaves).
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
    int32_t sweep_length = 0; // ticks (-1 for a note without a length), or milliseconds when sweep_in_ms
    int32_t sweep_ticks = 0;
    uint64_t sweep_frame = 0; // the sound frame a timed sweep starts in
    bool sweep_in_ms = false;

    // For a MIDI note with lfo_bends: the value of the core channel's pitch LFO in each sound frame from `lfo_frame` on
    // (Performer::TakeCoreLfos).
    std::vector<float> lfo_pitch;
    uint64_t lfo_frame = 0;
};

// The depth of a track's LFO in the bytecode's 128ths. The core keeps the depth as a fraction.
inline uint8_t LfoDepth(const nwsnd::CurveLfoParam& lfo)
{
    return static_cast<uint8_t>(lfo.depth * 128.0f);
}

// Whether a track's LFO starts. nw::snd keeps a delay as unsigned milliseconds. A negative delay therefore wraps to a
// huge one, and the LFO never starts.
inline bool LfoStarts(const nwsnd::CurveLfoParam& lfo)
{
    return static_cast<int32_t>(lfo.delay) >= 0;
}

// Returns the CC1 value that holds the depth of LFO `lfo` of type `type` (0 pitch, 1 volume, 2 pan). A volume LFO
// changes a channel's level by CalcVolumeRatio(6 * its value) dB. That can't rise more than 6 dB. The SoundFont and SFZ
// LFOs swing as far up as down. A deeper LFO therefore keeps to a 6 dB swing and dips less than in the game. The game's
// LFO value reaches depth / 128 * range. Both outputs swing 12 * range * CC1 / 128 dB. A 6 dB swing takes CC1 = 64 /
// range.
inline int ModulationValue(const nwsnd::CurveLfoParam& lfo, uint8_t type)
{
    const int depth = LfoDepth(lfo);
    int mod = std::min(127, (depth + 1) / 2);
    if (type == 1 && depth * lfo.range > 128)
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
    std::array<LeftPart, kPartCount> left{};

    // The track's volume, pan and sends at the end of its last tick (Performance::mix).
    std::array<int16_t, kMixSettingCount> mix = {127, 0, 0, 0, 0};
};

// The controllers that a track's settings give each part that takes them, apart from the pitch
// (MidiRecorder::UpdateOutputs). CC11 takes `level` times the part's legato gain.
struct PartControllers
{
    int volume = 0;
    double level = 0.0;
    int pan = 64;
    int modulation = 0;
    int reverb = 0;
    std::optional<int> chorus; // none where the sends leave CC93 alone (Sends::kReverbDry)
    std::optional<int> filter; // only for a filter that changes during notes (PassFindings::filter_moves)
    int sfz_low_pass = 0;
    int sfz_biquad = 0;
};

// Sends the controllers `c` through `send`, which takes a controller's value and its kind, with CC11 at `expression`.
template <typename Send>
void SendControllers(const PartControllers& c, int expression, const Send& send)
{
    send(c.volume, EventKind::kVolume);
    send(expression, EventKind::kExpression);
    send(c.pan, EventKind::kPan);
    send(c.modulation, EventKind::kModulation);
    send(c.reverb, EventKind::kReverb);
    if (c.chorus)
    {
        send(*c.chorus, EventKind::kChorus);
    }
    if (c.filter)
    {
        send(*c.filter, kFilterEvent);
    }
    send(c.sfz_low_pass, kSfzLowPassEvent);
    send(c.sfz_biquad, kSfzBiquadEvent);
}

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
    std::array<nwsnd::CallStackEntry, kCallDepth> calls{};
    int call_depth = 0;
    int32_t wait = 0; // ticks until the next command
    bool mute = false;
    bool note_finish_wait = false; // waiting for the track's notes to end
    bool held_wait = false;        // waiting out a note of kHeldTicks or more
    bool holds = false;            // the track's last note is one of kHeldTicks or more
    bool damper = false;
    uint8_t bank_slot = 0;
    uint32_t program = 0;
    std::array<nwsnd::CurveLfoParam, kLfoCount> lfos{};
    std::array<uint8_t, kLfoCount> lfo_types{}; // 0 pitch, 1 volume, 2 pan
    nwsnd::MoveValue<uint8_t> vol, volume2;
    nwsnd::MoveValue<int8_t> pan, bend;
    uint8_t velocity_range = 127; // velocities are scaled by this / 127
    uint8_t bend_range = 2;
    int8_t init_pan = 0;
    uint8_t attack = 0xff, decay = 0xff, sustain = 0xff, release = 0xff, hold = 0xff;
    uint8_t main_send = 127;
    Engine engine = Engine::kCafe; // the console's engine (Performer sets it)
    uint8_t fx_send_a = 0, fx_send_b = 0, fx_send_c = 0;
    float handle_volume = 1.0f;     // the volume that the game sets through the sound's handle (PerformOptions::tracks)
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

    // Whether the track's last pass of a loop changed nothing. Inner passes don't count. A Leavable pass counts only
    // when it changed nothing, no variable included, and drew no random number (OnLoop).
    bool idle = false;
    uint32_t opened = 0; // the tick it last opened at

    // When the track last changed a player or global variable that another open track has read and can read again. The
    // value is the number of commands run by then (Performer::commands_run_). Such a change counts as a change of the
    // sound for main-loop detection (OnLoop). The other track may play because of it.
    std::optional<uint64_t> last_signal;

    // The number of commands run (Performer::commands_run_) when the track last changed a variable, or 0 before its
    // first change (see OnLoop).
    uint64_t last_variable_change = 0;

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
// settings for ineffective LFOs to avoid duplicate presets. These include unsupported types and LFOs that never start
// (LfoStarts).
inline PresetKey TrackPresetKey(const Track& tr)
{
    PresetKey pk;
    pk.bank_slot = tr.bank_slot;
    pk.program = tr.program;
    pk.attack = tr.attack <= 127 ? tr.attack : 0xff;
    pk.decay = tr.decay <= 127 ? tr.decay : 0xff;
    pk.sustain = tr.sustain <= 127 ? tr.sustain : 0xff;
    pk.release = tr.release <= 127 ? tr.release : 0xff;
    pk.hold = tr.hold <= 127 ? tr.hold : 0xff;
    const nwsnd::CurveLfoParam& lfo = tr.lfos[0];
    if (tr.lfo_types[0] <= 2 && LfoStarts(lfo))
    {
        pk.lfo_type = tr.lfo_types[0];
        pk.lfo_range = lfo.range;
        pk.lfo_speed = lfo.speed;
        pk.lfo_delay = lfo.delay;
        if (LfoInBytecodeUnits(tr.engine))
        {
            // The 3DS's and the Wii's keys hold the bytecode's units: the speed, and the delay in 5 ms steps. Both are
            // exact: the track keeps the speed times 0.390625 and the delay times 5.
            pk.lfo_speed = lfo.speed / 0.390625f;
            pk.lfo_delay = lfo.delay / kMmlUnitMs;
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
inline PresetKey ChannelPresetKey(const Track& tr, const Note& n)
{
    PresetKey pk = TrackPresetKey(tr);
    pk.bank_slot = n.bank_slot;
    pk.program = n.program;
    pk.init_pan = n.init_pan;

    return pk;
}

// True if `a` and `b` have the same LFO settings.
inline bool SameLfo(const PresetKey& a, const PresetKey& b)
{
    return a.lfo_type == b.lfo_type && a.lfo_range == b.lfo_range && a.lfo_speed == b.lfo_speed &&
           a.lfo_delay == b.lfo_delay;
}

// `key` with the LFO settings of `lfo`.
inline PresetKey WithLfo(PresetKey key, const PresetKey& lfo)
{
    key.lfo_type = lfo.lfo_type;
    key.lfo_range = lfo.lfo_range;
    key.lfo_speed = lfo.lfo_speed;
    key.lfo_delay = lfo.lfo_delay;

    return key;
}

// `key` without an LFO: with the settings of a track whose LFO has no effect (TrackPresetKey).
inline PresetKey WithoutLfo(const PresetKey& key)
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
    RsarRegions(RsarBankSet& banks, bool since_march_2008) : banks_(banks), since_march_2008_(since_march_2008)
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
            v.ignore_note_off = since_march_2008_ && region->note_off_type == 1;
            v.key_group = since_march_2008_ ? region->key_group : 0;
            const EnvelopeValues env = RegionEnvelope(*region, since_march_2008_);
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
    bool since_march_2008_;
    mutable std::map<const rsar::Region*, VelocityRegion> regions_;
};

} // namespace ntdlib::sequence_export
