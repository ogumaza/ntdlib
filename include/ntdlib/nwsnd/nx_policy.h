// SPDX-License-Identifier: MIT

// The console policy of the sound library's model for the Switch: nn::atk (see console.h). A comment gives the
// address of the routine that a step follows in WarioWare: Get It Together!'s main.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "ntdlib/aarch64.h"
#include "ntdlib/fused.h"
#include "ntdlib/nwsnd/cafe_policy.h"
#include "ntdlib/nwsnd/envelope.h"
#include "ntdlib/nwsnd/length_rules.h"
#include "ntdlib/nwsnd/tables.h"
#include "ntdlib/nwsnd/util.h"

namespace ntdlib::nwsnd
{

// nn::atk, on the Switch's AArch64.
struct NxPolicy
{
    // The console's policy. A user's policy that derives from it gives this one to the library's builds of the envelope
    // and the LFO.
    using Console = NxPolicy;

    // A sound frame of 5 ms, in 1/65536 ms.
    static constexpr uint64_t kFrameUnits = 0x50000;

    // A millisecond in those units, and their size in milliseconds.
    static constexpr float kUnitsPerMs = 65536.0f;
    static constexpr float kMsPerUnit = 1.0f / 65536.0f;

    // The clock counts in those units. NW4R SND's clock counts in float milliseconds (RevolutionPolicy).
    static constexpr bool kFloatClock = false;

    // The sequence's ticks in a millisecond, from its timebase, tempo and tempo ratio (UpdateTick), as on the Wii U.
    static float TicksPerMs(int timebase, int tempo, float tempo_ratio)
    {
        return CafePolicy::TicksPerMs(timebase, tempo, tempo_ratio);
    }

    // The library's sizes: a call stack 10 deep, four LFOs and three effect sends.
    static constexpr int kCallStackDepth = 10;
    static constexpr int kLfoCount = 4;
    static constexpr int kFxSendCount = 3;

    // A tick runs no more than this many commands of a track (ParseNextTick, 0x022B6F14 on the Wii U). The tick then
    // ends, and the track reads on in the next tick.
    static constexpr int kParseLimit = 10000;
    static constexpr bool kParseLimitEndsTrack = false;

    // After a random prefix (0xA0), a variable prefix (0xA1) is a command that does nothing. A false condition (0xA2)
    // skips the end (0xFF) too.
    static constexpr bool kArgPrefixesChain = false;
    static constexpr bool kEndIsConditional = true;

    // The LFO that a channel plays: five curves, with a phase and a random value.
    using Lfo = CurveLfo<NxPolicy>;

    // A return (0xFD) skips loop entries to the newest call, and a loop end (0xFC) ignores a call's entry. Opening a
    // track clears its wait, its call stack and its wait for notes to end.
    static constexpr bool kReturnSkipsLoops = true;
    static constexpr bool kLoopEndIgnoresCalls = true;
    static constexpr bool kOpenClearsState = true;

    // A channel's volume, pitch and end with its voice work as on the Wii U (see CafePolicy).
    static constexpr bool kVoiceEndFreesChannel = CafePolicy::kVoiceEndFreesChannel;

    static float ChannelVolume(float velocity_volume, float init_volume, float track_volume, uint32_t silence)
    {
        return CafePolicy::ChannelVolume(velocity_volume, init_volume, track_volume, silence);
    }

    static float NotePitch(float key, float bend, float sweep)
    {
        return CafePolicy::NotePitch(key, bend, sweep);
    }

    // The length analysis's rules: the defaults.
    static constexpr LengthRules kLengthRules{};

    // The volume2 command (0xD5), the main volume (0xC2) and the pitch bend (0xC4) ramp over a time prefix.
    static constexpr bool kVolume2Ramps = true;
    static constexpr bool kMainVolumeRamps = true;

    static constexpr bool BendRamps(const auto&)
    {
        return true;
    }

    // The library has one build and every command, as on the Wii U. The commands' values are the Wii U's too.
    using Build = CafePolicy::Build;

    static bool HasCommand(const Build& build, uint32_t command)
    {
        return CafePolicy::HasCommand(build, command);
    }

    // The argument of a pan, initial pan or surround pan command (0xC0, 0xDC and 0xD7), as read.
    static int32_t PanArgument(const Build& build, int32_t value)
    {
        return CafePolicy::PanArgument(build, value);
    }

    // The tempo command's value (0xE1), held to 0-1023.
    static uint16_t TempoOf(const Build& build, int32_t value)
    {
        return CafePolicy::TempoOf(build, value);
    }

    // The sweep command's pitch (0xE3), in 64ths of a semitone.
    static float SweepPitchOf(const Build& build, int32_t value)
    {
        return CafePolicy::SweepPitchOf(build, value);
    }

    // A note in a key group releases the track's other notes in the group at the release rate 0x7E.
    static constexpr bool kKeyGroupFastRelease = true;

    // Whether a track's envelope value (0xD0 to 0xD3) or hold (0xB1) replaces the bank's value at a note's start.
    // Values from 0 to 127 do.
    static bool ReplacesEnvelope(const Build& build, int value)
    {
        return CafePolicy::ReplacesEnvelope(build, value);
    }

    // The sound's volume, the biquad filter's value (0xB5) and a note's velocity, from 0 to 127 as 0 to 1.
    static float PlayerVolume(uint32_t volume)
    {
        return AtLeastZero(static_cast<float>(volume) / 127.0f);
    }

    static float BiquadValueOf(int32_t value)
    {
        return CafePolicy::BiquadValueOf(value);
    }

    static float VelocityVolume(int velocity)
    {
        return CafePolicy::VelocityVolume(velocity);
    }

    // The channel's track volume (UpdateChannelParam): the product of the track's volume, volume2 and the player's main
    // volume over 127^3, squared, then times the track's volume and the player's volume.
    static float TrackVolume(uint32_t volume, uint32_t volume2, uint32_t main_volume, float track_volume,
                             float player_volume)
    {
        return CafePolicy::TrackVolume(volume, volume2, main_volume, track_volume, player_volume);
    }

    // The track's pan and surround pan, from -63 to 63 and from 0 to 126, as -1 to 1 and 0 to 2.
    static float PanOfValue(int pan)
    {
        const float value = static_cast<float>(pan) / 63.0f;
        return value > 1.0f ? 1.0f : Max(value, -1.0f);
    }

    static float SpanOfValue(int span)
    {
        const float value = static_cast<float>(span) / 63.0f;
        return value > 2.0f ? 2.0f : AtLeastZero(value);
    }

    // A send from 0 to 127 as 0 to 1. The main send is that less 1.
    static float SendOfValue(uint32_t send)
    {
        return CafePolicy::SendOfValue(send);
    }

    // A sequence's 16- and 24-bit arguments are little-endian.
    static constexpr bool kLittleEndianArguments = true;

    // A MultiVoice takes only the inputs of the way its channels mix: the pan and the surround pan in mix mode 0, and
    // the mix parameters in mode 1 (SetOutputParamImpl, 0xc25230).
    static constexpr bool kOutputParamByMixMode = true;

    // When no MultiVoice or no renderer voice is free, the manager drops the voices of the lowest priority until their
    // channels make room. The manager drops none unless each of them has a priority of at most the new voice's priority
    // and below 255. If the renderer still has too few voices, the manager drops voices once more and retries the
    // allocation (MultiVoiceManager::AllocVoice, 0xc268d0).
    static constexpr bool kDropVoicesForChannels = true;

    // The step of a channel's envelope, LFOs and sweep in each periodic update, in whole milliseconds: a sound frame
    // (Channel::Update, 0xc23120).
    static constexpr int kChannelFrameMs = 5;

    // A channel's LFOs start without a type (0xFF). An LFO without a type doesn't step (Channel::InitParam, 0xc22fd0,
    // and Channel::Update).
    static constexpr bool kUntypedLfos = true;

    // A note-off that comes before the channel's first update waits for the channel's next periodic update. The release
    // then follows the envelope's step (Channel::NoteOff, 0xc23a70, and Channel::Update).
    static constexpr bool kDeferredNoteOff = true;

    // AArch64's conversion of a float to a 32-bit integer (fcvtzs).
    static int32_t ToInt32(float x)
    {
        return ArmToInt32(x);
    }

    // AArch64's fmax, fminnm and fmin (see ntdlib/aarch64.h).
    static float Fmax(float a, float b)
    {
        return aarch64::Fmax(a, b);
    }

    static float Fminnm(float a, float b)
    {
        return aarch64::Fminnm(a, b);
    }

    static float Fmin(float a, float b)
    {
        return aarch64::Fmin(a, b);
    }

    // fmax. The Wii U's code uses fsel instead.
    static float Max(float a, float b)
    {
        return Fmax(a, b);
    }

    static float AtLeastZero(float x)
    {
        return Fmax(x, 0.0f);
    }

    // A variable's shift by `value` bits, left if it's 0 or more, else right. AArch64's shifts take the amount
    // modulo 32.
    static int32_t Shift(int32_t current, int32_t value)
    {
        if (value >= 0)
        {
            return static_cast<int32_t>(static_cast<uint32_t>(current) << (static_cast<uint32_t>(value) & 31));
        }

        return current >> (static_cast<uint32_t>(-value) & 31);
    }

    // The fall of a decay or a release: `value` less `rate` times `msec`, the product and the difference rounded on
    // their own (fmul, then fsub). The Wii U's code fuses them.
    static float Fall(float value, float rate, int msec)
    {
        return value - rate * static_cast<float>(msec);
    }

    // Whether a decay that has fallen to `value` has reached the sustain level: below it. A level that lands on it
    // waits for the next update.
    static bool ReachesSustain(float value, float sustain)
    {
        return CafePolicy::ReachesSustain(value, sustain);
    }

    // AArch64's fcvtzu to a 32-bit integer with `fbits` fraction bits: toward zero, held to the integer's range, 0 for
    // NaN.
    static uint32_t ToFixedU32(float x, int fbits)
    {
        const double scaled = std::ldexp(static_cast<double>(x), fbits);
        if (!(scaled >= 1.0))
        {
            return 0;
        }
        if (scaled >= 4294967296.0)
        {
            return 0xFFFFFFFF;
        }

        return static_cast<uint32_t>(scaled);
    }

    // nn::util's sine of a turn's fraction (CurveSine, 0xc19c10): the turn in 2^32 steps, the table's row for the top 8
    // bits, and its sine plus its step to the next row times the low 24 bits, each product and sum rounded on its own.
    static float Sine(float turn)
    {
        const uint32_t steps = ToFixedU32(turn, 32);
        const float fraction = static_cast<float>(steps & 0xFFFFFF) * 0x1p-24f;
        const float* row = tables::kSinCos[steps >> 24];
        return row[1] + fraction * row[3];
    }

    // Four straight lines through 0, 1, 0, -1 and 0 (CurveTriangle, 0xc19c50), the product and the sum rounded on their
    // own. The game takes the line from the quarter without a check; a turn below 1 gives 0 to 3.
    static float Triangle(float turn)
    {
        const size_t line = std::min<size_t>(ToFixedU32(turn, 2), 4);
        return tables::kTriangleSlope[line] * turn + tables::kTriangleOffset[line];
    }

    // 0, then 1 from halfway (CurveSquare, 0xc19c90).
    static float Square(float turn)
    {
        return turn < 0.5f ? 0.0f : 1.0f;
    }

    static bool Wrapped(float counter)
    {
        return counter >= 1.0f;
    }

    // fminnm holds the pan to 1, and a pan below -1, or NaN, takes the curve's first entry (Util::CalcPanRatio,
    // 0xc1a480).
    static int PanPosition(float pan)
    {
        const float x = (Fminnm(pan, 1.0f) + 1.0f) * 0.5f;
        const float position = pan < -1.0f || std::isnan(pan) ? 0.5f : x * 256.0f + 0.5f;

        return ToInt32(position);
    }

    // Util::CalcSurroundPanRatio, 0xc1a520.
    static int SpanPosition(float span)
    {
        const float x = Fminnm(span, 2.0f) * 0.5f;
        const float position = span < 0.0f || std::isnan(span) ? 0.5f : x * 256.0f + 0.5f;

        return ToInt32(position);
    }

    // Surround output takes the surround curves only where the sound's settings select them.
    template <typename PanInfo>
    static bool SurroundCurves(const PanInfo& info)
    {
        return info.surround_curves;
    }

    // The envelope's release rate and its level in decibels (CurveAdshr::CalcRelease and GetValue).
    static float ReleaseRate(float rate)
    {
        return CafePolicy::ReleaseRate(rate);
    }

    static float EnvelopeLevel(float value)
    {
        return CafePolicy::EnvelopeLevel(value);
    }

    // Util::CalcLpfFreq, 0xc1a2a0. The steps are those of the Wii U.
    static float LpfValue(float lpf)
    {
        return lpf < 0.0f || std::isnan(lpf) ? 0.0f : Fminnm(lpf, 1.0f);
    }

    static float LpfSteps(float above_first)
    {
        return CafePolicy::LpfSteps(above_first);
    }

    // The biquad presets that the library registers (from 0xc06520): types 1 to 5, the Wii U's presets for a 32 kHz
    // renderer, and types 6 to 10, the same filters made for 48 kHz.
    static constexpr int kBiquadPresetTypes = 10;
};

} // namespace ntdlib::nwsnd
