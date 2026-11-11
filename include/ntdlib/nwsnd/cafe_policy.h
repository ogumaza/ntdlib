// SPDX-License-Identifier: MIT

// The console policy of the sound library's model for the Wii U: nw::snd in NintendoWare for Cafe (see
// console.h). A comment gives the address of the routine that a step follows in the executable of Animal
// Crossing: amiibo Festival.

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "ntdlib/fused.h"
#include "ntdlib/nwsnd/envelope.h"
#include "ntdlib/nwsnd/length_rules.h"
#include "ntdlib/nwsnd/tables.h"
#include "ntdlib/nwsnd/util.h"

namespace ntdlib::nwsnd
{

// nw::snd in NintendoWare for Cafe, on the Wii U's PowerPC.
struct CafePolicy
{
    // The console's policy. A user's policy that derives from it gives this one to the library's builds of the envelope
    // and the LFO.
    using Console = CafePolicy;

    // A sound frame of 3 ms, in 1/65536 ms (UpdateTick, 0x022B7368).
    static constexpr uint64_t kFrameUnits = 0x30000;

    // A millisecond in those units, and their size in milliseconds.
    static constexpr float kUnitsPerMs = 65536.0f;
    static constexpr float kMsPerUnit = 1.0f / 65536.0f;

    // The clock counts in those units. NW4R SND's clock counts in float milliseconds (RevolutionPolicy).
    static constexpr bool kFloatClock = false;

    // The sequence's ticks in a millisecond, from its timebase, tempo and tempo ratio (UpdateTick).
    static float TicksPerMs(int timebase, int tempo, float tempo_ratio)
    {
        return (static_cast<float>(static_cast<int32_t>(timebase * tempo)) * tempo_ratio) / 60000.0f;
    }

    // The library's sizes: a call stack 10 deep, four LFOs and three effect sends.
    static constexpr int kCallStackDepth = 10;
    static constexpr int kLfoCount = 4;
    static constexpr int kFxSendCount = 3;

    // A tick runs no more than this many commands of a track (ParseNextTick, 0x022B6F14). The tick then ends, and the
    // track reads on in the next tick.
    static constexpr int kParseLimit = 10000;
    static constexpr bool kParseLimitEndsTrack = false;

    // After a random prefix (0xA0), a variable prefix (0xA1) is a command that does nothing. A false condition (0xA2)
    // skips the end (0xFF) too.
    static constexpr bool kArgPrefixesChain = false;
    static constexpr bool kEndIsConditional = true;

    // The LFO that a channel plays: five curves, with a phase and a random value.
    using Lfo = CurveLfo<CafePolicy>;

    // A return (0xFD) skips loop entries to the newest call, and a loop end (0xFC) ignores a call's entry. Opening a
    // track clears its wait, its call stack and its wait for notes to end.
    static constexpr bool kReturnSkipsLoops = true;
    static constexpr bool kLoopEndIgnoresCalls = true;
    static constexpr bool kOpenClearsState = true;

    // A channel's update each sound frame (the nameless 0x022C42C8). The channel's volume before the envelope and the
    // volume LFOs is the velocity's volume, squared, times the region's volume, the track's volume and the silence
    // level in 255ths. The channel's pitch in semitones before the pitch LFOs adds the bend to the key's distance from
    // the wave's original key, then the sweep. A channel whose voice ends or is dropped stays among the engine's
    // channels until their next update.
    static constexpr bool kVoiceEndFreesChannel = false;

    static float ChannelVolume(float velocity_volume, float init_volume, float track_volume, uint32_t silence)
    {
        // The game's 1/255 (0x100949F0), the float nearest to it.
        constexpr float kOneOver255 = std::bit_cast<float>(0x3B808081u);

        float volume = velocity_volume * velocity_volume;
        volume = volume * init_volume;
        volume = volume * track_volume;
        volume = volume * static_cast<float>(silence);

        return volume * kOneOver255;
    }

    static float NotePitch(float key, float bend, float sweep)
    {
        return (key + bend) + sweep;
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

    // The library has one build and every command.
    struct Build
    {
    };

    static bool HasCommand(const Build&, uint32_t)
    {
        return true;
    }

    // The argument of a pan, initial pan or surround pan command (0xC0, 0xDC and 0xD7), as read.
    static int32_t PanArgument(const Build&, int32_t value)
    {
        return value;
    }

    // The tempo command's value (0xE1), held to 0-1023.
    static uint16_t TempoOf(const Build&, int32_t value)
    {
        return static_cast<uint16_t>(std::clamp(value, 0, 0x3FF));
    }

    // The sweep command's pitch (0xE3), in 64ths of a semitone.
    static float SweepPitchOf(const Build&, int32_t value)
    {
        return static_cast<float>(value) * 0.015625f;
    }

    // A note in a key group releases the track's other notes in the group at the release rate 0x7E.
    static constexpr bool kKeyGroupFastRelease = true;

    // Whether a track's envelope value (0xD0 to 0xD3) or hold (0xB1) replaces the bank's value at a note's start.
    // Values from 0 to 127 do.
    static bool ReplacesEnvelope(const Build&, int value)
    {
        return value <= 127;
    }

    // The sound's volume, the biquad filter's value (0xB5) and a note's velocity, from 0 to 127 as 0 to 1.
    static float PlayerVolume(uint32_t volume)
    {
        return AtLeastZero(static_cast<float>(volume) / 127.0f);
    }

    static float BiquadValueOf(int32_t value)
    {
        return static_cast<float>(value) / 127.0f;
    }

    static float VelocityVolume(int velocity)
    {
        return static_cast<float>(static_cast<uint8_t>(velocity)) * (1.0f / 127.0f);
    }

    // The channel's track volume (UpdateChannelParam): the product of the track's volume, volume2 and the player's main
    // volume over 127^3, squared, then times the track's volume and the player's volume.
    static float TrackVolume(uint32_t volume, uint32_t volume2, uint32_t main_volume, float track_volume,
                             float player_volume)
    {
        const uint32_t level = volume * volume2 * main_volume;
        float value = static_cast<float>(static_cast<int32_t>(level)) / 2048383.0f;
        value = value * value;
        value = value * track_volume;

        return value * player_volume;
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
        return static_cast<float>(send) / 127.0f;
    }

    // A sequence's 16- and 24-bit arguments are big-endian.
    static constexpr bool kLittleEndianArguments = false;

    // A MultiVoice takes every output parameter that changes, whichever way its channels mix (SetOutputParamImpl,
    // 0x022AA6F0).
    static constexpr bool kOutputParamByMixMode = false;

    // When no MultiVoice is free, the manager drops the one of the lowest priority (MultiVoiceManager::AllocVoice,
    // 0x022AAC0C).
    static constexpr bool kDropVoicesForChannels = false;

    // The step of a channel's envelope, LFOs and sweep in each periodic update, in whole milliseconds: a sound frame
    // (the channel's update, 0x022C42C8).
    static constexpr int kChannelFrameMs = 3;

    // A channel's LFOs start as pitch LFOs, and every LFO steps (Channel::InitParam, 0x022B8E28).
    static constexpr bool kUntypedLfos = false;

    // A note-off releases the channel at once (Channel::NoteOff, 0x022B6E2C).
    static constexpr bool kDeferredNoteOff = false;

    // The PowerPC's conversion of a float to a 32-bit integer (fctiwz).
    static int32_t ToInt32(float x)
    {
        return PowerPcToInt32(x);
    }

    // The PowerPC's fsel: `a` when a - b is 0 or more, else `b`.
    static float Max(float a, float b)
    {
        return a - b >= 0.0f ? a : b;
    }

    // The PowerPC's fsel with 0 as the third operand: `x` when it's 0 or more, else 0.
    static float AtLeastZero(float x)
    {
        return x >= 0.0f ? x : 0.0f;
    }

    // A variable's shift (the sequence command F0 85) by `value` bits, left if it's 0 or more, else right. PowerPC's
    // shifts take the amount's low six bits, and 32 or more gives 0 or the sign.
    static int32_t Shift(int32_t current, int32_t value)
    {
        if (value >= 0)
        {
            const uint32_t shift = static_cast<uint32_t>(value) & 0x3F;
            return shift >= 32 ? 0 : static_cast<int32_t>(static_cast<uint32_t>(current) << shift);
        }

        const uint32_t shift = static_cast<uint32_t>(-value) & 0x3F;
        return shift >= 32 ? (current < 0 ? -1 : 0) : current >> shift;
    }

    // The fall of a decay or a release: `value` less `rate` times `msec`. The fall is one fused step that's negated
    // after it rounds (fnmsubs). A level that ends at exactly 0 is then -0. The game gives -0 too.
    static float Fall(float value, float rate, int msec)
    {
        return NegatedFma(rate, static_cast<float>(msec), -value);
    }

    // Whether a decay that has fallen to `value` has reached the sustain level: below it. A level that lands on it
    // waits for the next update.
    static bool ReachesSustain(float value, float sustain)
    {
        return value < sustain;
    }

    // nw::math's sine of a turn's fraction (CurveSine, 0x022BB420). The turn is in 2^32 steps. The sine is the table's
    // entry for the top 8 bits, plus its step to the next entry for the low 24 bits, in one fused step.
    static float Sine(float turn)
    {
        const uint32_t step = CafeToUint32(turn * 4294967296.0f);
        const float* entry = tables::kSin[step >> 24];

        return std::fma(entry[1] * static_cast<float>(step & 0xFFFFFF), 0x1p-24f, entry[0]);
    }

    // Four straight lines through 0, 1, 0, -1 and 0 (CurveTriangle, 0x022BB50C). The line is the turn's quarters,
    // converted to an unsigned integer and held to 0..4 as a signed one.
    static float Triangle(float turn)
    {
        constexpr std::array<float, 5> kSlopes = {4.0f, -4.0f, -4.0f, 4.0f, 4.0f};
        constexpr std::array<float, 5> kOffsets = {0.0f, 2.0f, 2.0f, -4.0f, 0.0f};
        const auto line = static_cast<int32_t>(CafeToUint32(turn * 4.0f));
        const auto i = static_cast<std::size_t>(std::clamp(line, 0, 4));

        return std::fma(kSlopes[i], turn, kOffsets[i]);
    }

    // 0, then 1 from halfway (CurveSquare, 0x022BB5EC).
    static float Square(float turn)
    {
        return turn - 0.5f >= 0.0f ? 1.0f : 0.0f;
    }

    // Whether an LFO's counter has passed the end of its cycle (CurveLfo::Update, 0x022BB194).
    static bool Wrapped(float counter)
    {
        return !(counter < 1.0f);
    }

    // The place in a pan curve for a pan from -1 to 1 (Util::CalcPanRatio, 0x0229C0DC).
    static int PanPosition(float pan)
    {
        float x = 1.0f;
        if (!(pan > 1.0f))
        {
            const float held = pan - -1.0f >= 0.0f ? pan : -1.0f;
            x = (held + 1.0f) * 0.5f;
        }

        return static_cast<int>(x * 256.0f + 0.5f);
    }

    // The place in a pan curve for a surround pan from 0 to 2 (Util::CalcSurroundPanRatio, 0x0229C228).
    static int SpanPosition(float span)
    {
        float x = 1.0f;
        if (!(span > 2.0f))
        {
            x = (span >= 0.0f ? span : 0.0f) * 0.5f;
        }

        return static_cast<int>(x * 256.0f + 0.5f);
    }

    // Whether surround output takes the surround pan curves where the front doesn't bypass them. It always does.
    template <typename PanInfo>
    static bool SurroundCurves(const PanInfo&)
    {
        return true;
    }

    // The envelope's release rate and its level in decibels (CurveAdshr::CalcRelease and GetValue).
    static float ReleaseRate(float rate)
    {
        return rate / 5.0f;
    }

    static float EnvelopeLevel(float value)
    {
        return value / 10.0f;
    }

    // A low-pass value held to 0 and 1 (Util::CalcLpfFreq, 0x0229CE40).
    static float LpfValue(float lpf)
    {
        float value = 1.0f;
        if (!(lpf > 1.0f))
        {
            value = lpf >= 0.0f ? lpf : 0.0f;
        }

        return value;
    }

    // The steps of kLpfFreq from the first step to a low-pass value `above_first` past it (Util::CalcLpfFreq). The
    // steps are a thirtieth apart. The code divides by 1/30 rather than multiplying by 30.
    static float LpfSteps(float above_first)
    {
        return above_first / 0x1.111112p-5f;
    }

    // The biquad presets that the library registers: types 1 to 5, made for a 32 kHz renderer (see BiquadPresetRow).
    static constexpr int kBiquadPresetTypes = 5;
};

} // namespace ntdlib::nwsnd
