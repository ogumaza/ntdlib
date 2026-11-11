// SPDX-License-Identifier: MIT

// The console policy of the sound library's model for the Wii: NW4R SND (see console.h). A comment gives the
// address of the routine that a step follows in the main.dol of the game that the comment names.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "ntdlib/ax/ax.h"
#include "ntdlib/fused.h"
#include "ntdlib/nwsnd/cafe_policy.h"
#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/envelope.h"
#include "ntdlib/nwsnd/length_rules.h"
#include "ntdlib/nwsnd/tables.h"
#include "ntdlib/nwsnd/util.h"
#include "ntdlib/sound_build.h"

namespace ntdlib::nwsnd
{

// NW4R SND on the Wii's Broadway, after Wii Sports' main.dol. NW4R SND is older than nw::snd: one LFO, no extended
// commands, a call stack 3 deep and three effect sends. The library lacks a channel's silence, the outputs, the pan
// range, the user's pitch, and the player's filters that override the track's filters. NW4R SND's code divides where
// the 3DS's code multiplies by reciprocals. NW4R SND's builds fall into six kinds that differ in their commands
// (SndBuild). WarioWare: Smooth Moves' build of October 2006 also differs in its clock. A sound's build comes from its
// main.dol or its AX, and the player holds it.
struct RevolutionPolicy
{
    // The console's policy. A user's policy that derives from it gives this one to the library's builds of the envelope
    // and the LFO.
    using Console = RevolutionPolicy;

    // A sequence's 16- and 24-bit arguments are big-endian.
    static constexpr bool kLittleEndianArguments = false;

    // The library's sizes: one LFO, a call stack 3 deep, and three effect sends.
    static constexpr int kLfoCount = 1;
    static constexpr int kCallStackDepth = 3;
    static constexpr int kFxSendCount = 3;

    // The LFO that a channel plays: the sine from the table's quarter wave.
    using Lfo = TableLfo<RevolutionPolicy>;

    // A channel's update each sound frame. The frame's step is in whole milliseconds (Channel::Update, 0x800378c8). A
    // channel's LFO starts as a pitch LFO, and it always steps (Channel::InitParam, 0x800377b0). A note-off releases
    // the channel at once (Channel::Release, 0x8003809c). A channel leaves the engine's channels as soon as its voice
    // ends or is dropped (VoiceCallbackFunc, 0x80038244).
    static constexpr int kChannelFrameMs = 3;
    static constexpr bool kVoiceEndFreesChannel = true;
    static constexpr bool kUntypedLfos = false;
    static constexpr bool kDeferredNoteOff = false;

    // The channels: one for each of AX's voices, and one more.
    static constexpr int kChannelCount = ax::kVoiceCount + 1;

    // When no voice is free, the manager drops the one of the lowest priority (VoiceManager::AllocVoice, 0x8004e358).
    static constexpr bool kDropVoicesForChannels = false;

    // What a channel's frame gives its voice. NW4R SND's Voice takes the values in this order: SetPanMode, SetPanCurve,
    // SetVolume, SetVeVolume(ve_target_volume, ve_init_volume), SetPitch, SetPan, SetSurroundPan, SetLpfFreq,
    // SetBiquadFilter, SetMainOutVolume, SetMainSend, and SetFxSend for each bus (0x8004c830 to 0x8004cae0, and
    // SetBiquadFilter at 0x800cc028 in Wii Music's main.dol). AX ramps the voice's envelope across the frame, from the
    // gain before the frame to the gain after it.
    struct ChannelVoiceParam
    {
        uint32_t pan_mode = 0;
        uint32_t pan_curve = 0;
        float volume = 1.0f;
        float ve_target_volume = 1.0f; // the envelope's gain after the frame, times a volume LFO's gain
        float ve_init_volume = 1.0f;   // the same before the frame
        float pitch = 1.0f;            // a ratio
        float pan = 0.0f;
        float surround_pan = 0.0f;
        float lpf_freq = 1.0f;
        uint8_t biquad_type = 0;
        float biquad_value = 0.0f;
        float main_out_volume = 1.0f;
        float main_send = 0.0f;
        std::array<float, kFxSendCount> fx_send{};
    };

    // A channel's frame (Channel::Update, 0x800378c8), in place of the template's frame (see channel.h). The engine
    // supplies Build(), the build of NW4R SND that plays the sound. A voice's SetParam takes a ChannelVoiceParam. A
    // voice without SetParam, such as the template's MultiVoice, takes the pan mode and curve, the volume, the pitch
    // and the filters through its setters (see channel.h). The definition follows the policy.
    template <typename Channel, typename Engine>
    static void UpdateChannel(Channel& channel, Engine& engine, bool periodic);

    // The volume2 command (0xD5) and the main volume (0xC2) set their value at once. A time prefix only takes its
    // bytes (MmlParser::CommandProc, 0x8003bee8). Builds before September 2009 set the pitch bend (0xC4) at once too.
    // Later builds move it over the prefix's ticks (SndBuild::SinceSeptember2009).
    static constexpr bool kVolume2Ramps = false;
    static constexpr bool kMainVolumeRamps = false;

    static bool BendRamps(const SndBuild& build)
    {
        return build.SinceSeptember2009();
    }

    // The random generator doesn't step before a sound starts. The LFO draws no numbers.
    static constexpr int kRandomStepsBeforeStart = 0;

    // NW4R SND's build: its banner's date, and whether it's the build of October 2006 (ntdlib's sound_build.h).
    using Build = SndBuild;

    // The commands that a build lacks. Every build lacks the bank (0xB6), the LFOs' phase, curve and period (0xBD,
    // 0xBE, 0xE4), the front bypass (0xBF) and the extended LFO commands (0xF0 with 0xA0 to 0xBF and 0xE1 to 0xE6). The
    // parser reads no byte after 0xF0 with 0xA0 to 0xBF. The time prefixes (0xA3 to 0xA5), the hold (0xB1) and
    // monophony (0xB2) come in 2008 (Mario Kart Wii, 0x80096240, 0x80096b90 and 0x80096be8). The velocity range (0xB3),
    // the biquad filter (0xB4 and 0xB5) and the envelope's reset (0xFB) come in June 2008 (Wii Music, 0x800b95c4,
    // 0x800b9870 and 0x800b9788). `command` is a byte, or 0xF000 plus the byte after 0xF0.
    static bool HasCommand(const Build& build, uint32_t command)
    {
        if (command >= 0xF000)
        {
            const uint32_t sub = command & 0xFF;
            return !(sub >= 0xA0 && sub <= 0xBF) && !(sub >= 0xE1 && sub <= 0xE6);
        }

        switch (command)
        {
        case 0xA3:
        case 0xA4:
        case 0xA5:
        case 0xB1:
        case 0xB2:
            return build.SinceMarch2008();
        case 0xB3:
        case 0xB4:
        case 0xB5:
        case 0xFB:
            return build.SinceJune2008();
        case 0xB6:
        case 0xBD:
        case 0xBE:
        case 0xBF:
        case 0xE4:
            return false;
        default:
            return true;
        }
    }

    // The PowerPC's conversion of a float to a 32-bit integer (fctiwz).
    static int32_t ToInt32(float x)
    {
        return PowerPcToInt32(x);
    }

    // The Wii's runtime conversion of a float to an unsigned integer: 0 below 0, all ones from 2^32, and truncated
    // between.
    static uint32_t ToUnsigned(float x)
    {
        if (!(x >= 0.0f))
        {
            return 0;
        }
        if (!(x < 4294967296.0f))
        {
            return 0xffffffff;
        }

        return static_cast<uint32_t>(static_cast<double>(x));
    }

    // A variable's shift (the sequence command F0 85), the same as on the Wii U: PowerPC's slw and sraw.
    static int32_t Shift(int32_t current, int32_t value)
    {
        return CafePolicy::Shift(current, value);
    }

    // The channel's track volume (SeqTrack::UpdateChannelParam, 0x800403e0): the squares of the track's volume, volume2
    // and the player's main volume over 127, multiplied in that order from 1, then the track's float volume
    // (SeqTrack::SetVolume) and the sound's volume. The build of October 2006 multiplies them in the same order
    // (0x800e7308 in WarioWare: Smooth Moves' main.dol).
    static float TrackVolume(uint32_t volume, uint32_t volume2, uint32_t main_volume, float track_volume,
                             float player_volume)
    {
        const float track = static_cast<float>(volume) / 127.0f;
        const float track2 = static_cast<float>(volume2) / 127.0f;
        const float main = static_cast<float>(main_volume) / 127.0f;

        float value = 1.0f;
        value *= track * track;
        value *= track2 * track2;
        value *= main * main;
        value *= track_volume;
        value *= player_volume;

        return value;
    }

    // The track's pan and surround pan, from -63 to 63 and from 0 to 126, as -1 to 1 and 0 to 2 (UpdateChannelParam).
    static float PanOfValue(int pan)
    {
        return std::clamp(static_cast<float>(pan) / 63.0f, -1.0f, 1.0f);
    }

    static float SpanOfValue(int span)
    {
        return std::clamp(static_cast<float>(span) / 63.0f, 0.0f, 2.0f);
    }

    // A send from 0 to 127 as 0 to 1. The main send is that less 1.
    static float SendOfValue(uint32_t send)
    {
        return static_cast<float>(send) / 127.0f;
    }

    // The sound's volume from 0 to 127 as 0 to 1, held to 1 (SeqPlayer's setup, from BasicSound::Update).
    static float PlayerVolume(uint32_t volume)
    {
        return std::clamp(static_cast<float>(volume) / 127.0f, 0.0f, 1.0f);
    }

    // The biquad filter's value (the sequence command 0xB5) from 0 to 127 as 0 to 1.
    static float BiquadValueOf(int32_t value)
    {
        return static_cast<float>(value) / 127.0f;
    }

    // The volume that a note gives its channel: the velocity over 127, squared (SeqTrack::NoteOn, 0x80040bc0).
    static float VelocityVolume(int velocity)
    {
        const float value = static_cast<float>(velocity) / 127.0f;
        return value * value;
    }

    // The pan that a note gives its channel at the start: the region's pan less 64, plus the track's initial pan, each
    // over 63.
    static float InitialPan(int region_pan, int track_pan)
    {
        float pan = static_cast<float>(region_pan - 64) / 63.0f;
        pan += static_cast<float>(track_pan) / 63.0f;
        return pan;
    }

    // The sequence's ticks in a millisecond, from its timebase, its tempo and the tempo ratio (SeqPlayer::UpdateTick,
    // 0x8003eec4). The ratio multiplies first.
    static float TicksPerMs(int timebase, int tempo, float tempo_ratio)
    {
        return tempo_ratio * static_cast<float>(timebase * tempo) / 60000.0f;
    }

    // The table LFO's step of its phase in `msec` milliseconds (Lfo::Update, 0x8003b280), and its level from the
    // quarter wave's `sine` (Lfo::GetValue, 0x8003b31c).
    static float LfoPhaseStep(float speed, int msec)
    {
        return speed * static_cast<float>(msec) / 1000.0f;
    }

    static float LfoLevel(int sine, float depth, uint8_t range)
    {
        float value = static_cast<float>(sine) / 127.0f;
        value *= depth;
        value *= static_cast<float>(range);

        return value;
    }

    // The fall of a decay or a release (EnvGenerator::Update, 0x80038fdc): `value` less `rate` times `msec`, the
    // product and the difference rounded on their own. NW4R SND has no fused multiply-adds.
    static float Fall(float value, float rate, int msec)
    {
        return value - rate * static_cast<float>(msec);
    }

    // Whether a decay that has fallen to `value` has reached the sustain level: it isn't above it
    // (EnvGenerator::Update). A level that lands on it starts the sustain at once.
    static bool ReachesSustain(float value, float sustain)
    {
        return !(value > sustain);
    }

    // The envelope's release rate (EnvGenerator::CalcRelease, inlined in SetDecay, 0x80039118, and SetRelease,
    // 0x800391c0) and its level in decibels (EnvGenerator::GetValue, 0x80038fb0). The Wii's code divides by 5 and by
    // 10.
    static float ReleaseRate(float rate)
    {
        return rate / 5.0f;
    }

    static float EnvelopeLevel(float value)
    {
        return value / 10.0f;
    }

    // The larger of two floats, the PowerPC's fsel with 0 as the third operand, and the place in a pan curve, the same
    // as on the Wii U (Util::CalcVolumeRatio, 0x8004e9cc, and Util::CalcPanRatio, 0x8004ea24). For finite arguments,
    // the Wii's code gives the same results.
    static float Max(float a, float b)
    {
        return CafePolicy::Max(a, b);
    }

    static float AtLeastZero(float x)
    {
        return CafePolicy::AtLeastZero(x);
    }

    static int PanPosition(float pan)
    {
        return CafePolicy::PanPosition(pan);
    }

    // The place in a pan curve for a surround pan, the same as on the Wii U. Skyward Sword HD's NW4R SND takes it in
    // its mix for Dolby Pro Logic II (main 0x102a7f8).
    static int SpanPosition(float span)
    {
        return CafePolicy::SpanPosition(span);
    }

    // The sides of the stereo mix that a channel of a wave reaches.
    enum class WaveSide : uint8_t
    {
        kBoth,
        kLeftOnly,
        kRightOnly,
    };

    // Where a channel of a wave goes in the stereo mix (Voice::CalcMixParam, 0x8004d590): the pan that the curve takes,
    // the sides that the channel reaches, and the curve. The left side's gain is the curve's ratio at the pan. The
    // right side's gain is the ratio at the negated pan. A mono wave plays at the pan. In dual mode (pan mode 0), the
    // two channels of a stereo wave pan from their sides, at the pan less 1 and the pan plus 1. In balance mode (pan
    // mode 1), each channel of a wave with more than one reaches one side alone: the first channel the left, and the
    // others the right. The build of October 2006 has neither balance mode nor pan curves. It pans in dual mode by the
    // square-root curve, curve 0 (CalcMixParam, 0x800dd970 in Smooth Moves' main.dol).
    struct WavePlacement
    {
        float pan;
        WaveSide side;
        uint32_t curve;
    };

    static WavePlacement PlacementOfWaveChannel(float pan, int channel, int channels, uint32_t pan_mode,
                                                uint32_t pan_curve, const Build& build)
    {
        const bool october = build.October2006();
        const uint32_t curve = october ? 0 : pan_curve;
        if (channels > 1 && pan_mode == 1 && !october)
        {
            return {pan, channel == 0 ? WaveSide::kLeftOnly : WaveSide::kRightOnly, curve};
        }

        float side = 0.0f;
        if (channels == 2)
        {
            side = channel == 0 ? -1.0f : 1.0f;
        }

        return {pan + side, WaveSide::kBoth, curve};
    }

    // The Wii's stereo mix never takes the surround pan curves.
    template <typename PanInfo>
    static bool SurroundCurves(const PanInfo&)
    {
        return false;
    }

    // The argument of a pan, initial pan or surround pan command. The build of October 2006 raises a value of 0 or 1 by
    // one (0x800e417c in Smooth Moves' main.dol).
    static int32_t PanArgument(const Build& build, int32_t value)
    {
        return build.October2006() && value <= 1 ? value + 1 : value;
    }

    // The tempo command's value (0xE1). Builds from 2008 hold it to 0-1023 (Mario Kart Wii, 0x80096924). Older builds
    // keep 16 bits. A variable's default of -1 then gives 65535.
    static uint16_t TempoOf(const Build& build, int32_t value)
    {
        return static_cast<uint16_t>(build.SinceMarch2008() ? std::clamp(value, 0, 1023) : value);
    }

    // The sweep command's pitch (0xE3): in 32nds of a semitone, and from 2008 in 64ths (Mario Kart Wii, 0x80096b58).
    static float SweepPitchOf(const Build& build, int32_t value)
    {
        return static_cast<float>(value) / (build.SinceMarch2008() ? 64.0f : 32.0f);
    }

    // A note in a key group releases the track's other notes in the group, each at its release rate (Mario Kart Wii,
    // 0x8009bfcc). Builds before 2008 read no key group from the bank.
    static constexpr bool kKeyGroupFastRelease = false;

    // Whether a track's envelope value (0xD0 to 0xD3) or hold (0xB1) replaces the bank's value at a note's start.
    // Builds before 2008 take any value but 0xFF (SeqTrack::NoteOn, 0x80040d40 in Wii Sports' main.dol). Builds from
    // 2008 take 0 to 127 (0x8009c02c in Mario Kart Wii's main.dol). The hold comes in 2008.
    static bool ReplacesEnvelope(const Build& build, int value)
    {
        return build.SinceMarch2008() ? value <= 127 : value != 0xFF;
    }

    // The clock (SeqPlayer::Update, 0x80045d50): a sound frame of 3 ms. UpdateTick (0x8003eec4) counts the frame's time
    // in float milliseconds. The hooks get the time in the Wii U's units, 1/65536 ms.
    static constexpr bool kFloatClock = true;
    static constexpr float kFrameMs = 3.0f;
    static constexpr uint64_t kFrameUnits = 0x30000;
    static constexpr float kUnitsPerMs = 65536.0f;

    // Whether the build's clock counts in 416ths of a tick instead: the build of October 2006 (0x800e6588 in WarioWare:
    // Smooth Moves' main.dol).
    static bool CountsTickParts(const Build& build)
    {
        return build.October2006();
    }

    // NW4R SND runs a track's commands until one waits, and hangs on a sequence that loops without a wait. The model
    // ends the track after this many commands in a tick instead.
    static constexpr int kParseLimit = 100000;
    static constexpr bool kParseLimitEndsTrack = true;

    // A random prefix (0xA0) and a variable prefix (0xA1) after it both apply, and the argument comes from the
    // variable. The end (0xFF) ends the track after a false condition (0xA2) too (MmlParser::Parse, 0x8003b8b8).
    static constexpr bool kArgPrefixesChain = true;
    static constexpr bool kEndIsConditional = false;

    // A return (0xFD) and a loop end (0xFC) take the call stack's top entry, whether a call or a loop start put it
    // there. A loop end on a call's entry counts with the last loop count at that level. Opening a track keeps its
    // wait, its call stack and its wait for notes to end (SeqTrack::Open, 0x8003ffcc). A track that runs and opens
    // again keeps them too.
    static constexpr bool kReturnSkipsLoops = false;
    static constexpr bool kLoopEndIgnoresCalls = false;
    static constexpr bool kOpenClearsState = false;

    // The length analysis on the Wii takes none of the rules that LengthRules can add.
    static constexpr LengthRules kLengthRules{.jump_loops_without_condition = false,
                                              .damper_holds_notes = false,
                                              .timebase_zero_stops = false,
                                              .no_track_passes = false,
                                              .quiet_after_report = false};

    // A low-pass value held to 0 and 1 (Util::CalcLpfFreq, 0x8004eb98). The Wii U holds it the same way. A NaN gives 0.
    // Nobody has read what the Wii's code does with one.
    static float LpfValue(float lpf)
    {
        return CafePolicy::LpfValue(lpf);
    }

    // The low-pass's cutoff in Hz for a value from 0 to 1 in `build` (see CalcLpfFreq). Builds before November 2008
    // work it out: 32000 * 2^(10 (value - 1)), from 31 Hz to 32000 Hz. The game works the power out with its C
    // library's pow, in double precision. Later builds take the cutoff from kLpfFreq, as the Wii U does: 80 Hz below
    // 0.13561438, 16000 Hz from 0.9, and steps a thirtieth apart between them (SndBuild::SinceNovember2008).
    static uint16_t LpfFreq(const SndBuild& build, float value)
    {
        if (build.SinceNovember2008())
        {
            return tables::kLpfFreq[LpfFreqIndex<CafePolicy>(value)];
        }

        return static_cast<uint16_t>(32000.0 * std::pow(2.0, 10.0 * (static_cast<double>(value) - 1.0)));
    }

    // The biquad presets that builds from June 2008 register: types 1 to 5 (from 0x800b3b7c in Wii Music's main.dol).
    static constexpr int kBiquadPresetTypes = 5;
};

// The Wii's channel frame (Channel::Update, 0x800378c8). The sums start from 0 and the products from 1, in the game's
// order. A sum from 0 turns a negative zero positive.
template <typename Channel, typename Engine>
void RevolutionPolicy::UpdateChannel(Channel& channel, Engine& engine, bool periodic)
{
    if (!channel.active)
    {
        return;
    }

    // The LFO's value at the frame's start. The LFO moves the pitch (target 0), the volume (1) or the pan (2).
    const uint8_t lfo_target = channel.lfo_types[0];
    const float lfo_value = channel.lfos[0].GetValue();

    // The silence ramp moves on a frame.
    if (channel.silence_counter < channel.silence_duration)
    {
        ++channel.silence_counter;
    }
    const uint8_t silence =
        channel.silence_counter >= channel.silence_duration
            ? channel.silence_to
            : static_cast<uint8_t>(channel.silence_from + channel.silence_counter *
                                                              (channel.silence_to - channel.silence_from) /
                                                              channel.silence_duration);

    // The Wii keeps a note's volume in velocity_volume: the velocity over 127, squared, times the region's volume. A
    // tie sets it to the squared velocity alone (SeqTrack::NoteOn, 0x80040bc0). From 2008, a monophonic note does too.
    float volume = 1.0f;
    volume *= channel.velocity_volume;
    volume *= channel.track_volume;
    volume *= static_cast<float>(silence) / 255.0f;

    // The envelope's gain at the frame's start, times a volume LFO's gain: where the voice's ramp starts.
    float ve_init_volume = 1.0f;
    ve_init_volume *= CalcVolumeRatio<RevolutionPolicy>(channel.envelope.GetValue());
    if (lfo_target == 1)
    {
        ve_init_volume *= CalcVolumeRatio<RevolutionPolicy>(6.0f * lfo_value);
    }

    // A released note stops once it's silent. A channel with an owner falls silent when the envelope's gain is 0. A
    // channel without one falls silent when the volume times that gain is 0. The build of October 2006 always takes the
    // volume times the gain (0x800e039c in Smooth Moves' main.dol). The channel manager then calls the owner back and
    // frees the channel.
    if (channel.envelope.GetStatus() == AdshrStatus::kRelease)
    {
        const bool owned = channel.owner_player >= 0 && !engine.Build().October2006();
        if (owned ? ve_init_volume == 0.0f : volume * ve_init_volume == 0.0f)
        {
            channel.Stop(engine);

            return;
        }
    }

    // The pitch in semitones: the key's distance from the wave's original key, the sweep, the bend and a pitch LFO.
    float cent = 0.0f;
    cent += static_cast<float>(channel.key - channel.original_key);
    cent += channel.GetSweepValue();
    cent += channel.bend;
    if (lfo_target == 0)
    {
        cent += lfo_value;
    }

    // The pitch's ratio, times the tune times the sound's pitch.
    float pitch_ratio = 1.0f;
    pitch_ratio *= channel.tune;
    pitch_ratio *= channel.user_pitch;
    float pitch = CalcPitchRatio(ToInt32(256.0f * cent));
    pitch *= pitch_ratio;

    // The pan with a pan LFO, and the surround pan, from the note and the track.
    float pan = 0.0f;
    pan += channel.init_pan;
    pan += channel.outputs.pan;
    if (lfo_target == 2)
    {
        pan += lfo_value;
    }
    float surround_pan = 0.0f;
    surround_pan += channel.init_span;
    surround_pan += channel.outputs.span;

    // The low-pass filter's frequency, the main out volume and the sends, from the track.
    float lpf_freq = 1.0f;
    lpf_freq += channel.lpf;
    float main_out_volume = 1.0f;
    main_out_volume *= channel.outputs.volume;
    float main_send = 0.0f;
    main_send += channel.outputs.main_send;
    std::array<float, kFxSendCount> fx_send{};
    for (std::size_t bus = 0; bus < fx_send.size(); ++bus)
    {
        fx_send[bus] += channel.outputs.fx_send[bus];
    }

    // A periodic frame moves the sweep, the LFO and the envelope on by 3 ms.
    if (periodic)
    {
        if (channel.auto_sweep)
        {
            channel.UpdateSweep(kChannelFrameMs);
        }

        channel.lfos[0].Update(kChannelFrameMs);
        channel.envelope.Update(kChannelFrameMs);
    }

    // The envelope's gain after the frame, times a volume LFO's gain: where the voice's ramp ends.
    const float next_lfo_value = channel.lfos[0].GetValue();
    float ve_target_volume = 1.0f;
    ve_target_volume *= CalcVolumeRatio<RevolutionPolicy>(channel.envelope.GetValue());
    if (lfo_target == 1)
    {
        ve_target_volume *= CalcVolumeRatio<RevolutionPolicy>(6.0f * next_lfo_value);
    }

    // A channel without a voice has taken the frame's steps.
    if (channel.voice == kNoVoice)
    {
        return;
    }

    // In builds before September 2009, the voice limits both gains to full level (Voice::SetVeVolume, 0x8004c874).
    // Only a volume LFO lifts them above full level. An engine with the hook follows each frame in which one does.
    // Later builds hold the gains only at 0 (SndBuild::SinceSeptember2009). They call neither hook.
    const bool holds_gains = !engine.Build().SinceSeptember2009();
    if constexpr (requires { engine.OnGainAboveFull(channel); })
    {
        if (holds_gains && lfo_target == 1 && (ve_init_volume > 1.0f || ve_target_volume > 1.0f))
        {
            engine.OnGainAboveFull(channel);
        }
    }

    // An engine with this hook follows every frame of a volume LFO in those builds: the envelope's level and the
    // LFO's, in decibels, where the voice's ramp ends.
    if constexpr (requires { engine.OnVolumeLfo(channel, 0.0f, 0.0f); })
    {
        if (holds_gains && lfo_target == 1)
        {
            engine.OnVolumeLfo(channel, channel.envelope.GetValue(), 6.0f * next_lfo_value);
        }
    }

    // The voice takes the frame's values.
    ChannelVoiceParam param;
    param.pan_mode = channel.pan_mode;
    param.pan_curve = channel.pan_curve;
    param.volume = volume;
    param.ve_target_volume = ve_target_volume;
    param.ve_init_volume = ve_init_volume;
    param.pitch = pitch;
    param.pan = pan;
    param.surround_pan = surround_pan;
    param.lpf_freq = lpf_freq;
    param.biquad_type = channel.biquad_type;
    param.biquad_value = channel.biquad_value;
    param.main_out_volume = main_out_volume;
    param.main_send = main_send;
    param.fx_send = fx_send;

    auto& voice = channel.Voice(engine);
    if constexpr (requires { voice.SetParam(param); })
    {
        voice.SetParam(param);
    }
    else
    {
        voice.SetPanMode(param.pan_mode);
        voice.SetPanCurve(param.pan_curve);
        voice.SetVolume(param.volume);
        voice.SetPitch(param.pitch);
        voice.SetLpfFreq(param.lpf_freq);
        voice.SetBiquadFilter(param.biquad_type, param.biquad_value);
    }
}

} // namespace ntdlib::nwsnd
