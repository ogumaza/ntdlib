// SPDX-License-Identifier: MIT

// The console policies of the sound library's model: the steps where nw::snd in NintendoWare for Cafe (the Wii U),
// nn::atk (the Switch), nw::snd for the 3DS and NW4R SND (the Wii) differ. nn::atk is nw::snd on AArch64. The two run
// the same code, built for the Wii U's PowerPC and for the Switch's AArch64. The compilers chose other instructions,
// and the processors round and convert in other ways. The 3DS has an older version of the library, on the ARM11. NW4R
// SND is older still. Each policy holds one console's version of those steps. A comment gives the address of the
// routine a step follows: in Animal Crossing: amiibo Festival's executable for the Wii U, in WarioWare: Get It
// Together!'s main for the Switch, in Pokemon X's code.bin for the 3DS, and for the Wii in the main.dol of the game
// that the comment names.

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "ntdlib/aarch64.h"
#include "ntdlib/ax/ax.h"
#include "ntdlib/fused.h"
#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/envelope.h"
#include "ntdlib/nwsnd/tables.h"
#include "ntdlib/sound_build.h"

namespace ntdlib::nwsnd
{

// The rules of a rip tool's length analysis that differ between the policies (see sequence.h). Each policy gives them
// as kLengthRules. A user's policy can give others. The Wii U, the Switch and the 3DS take the defaults.
struct LengthRules
{
    // A jump back inside a subroutine is a loop while the track holds a note that sounds until the game stops it. With
    // this rule, it's a loop too when the track has run no conditional command since the jump's target.
    bool jump_loops_without_condition = true;

    // A note while the damper is on isn't a note whose length runs out (SequenceTrack::HasEndingNote).
    bool damper_holds_notes = true;

    // A timebase of 0 stops the sequence like a tempo of 0 (SequenceSoundPlayer::IsSettled and IsStuck).
    bool timebase_zero_stops = true;

    // When no track is open, every open track passes a test (SequenceSoundPlayer::EveryOpenTrack). Without this rule,
    // none does. The sequence then isn't settled or stuck, and it moves.
    bool no_track_passes = true;

    // A pass that ends with a loop is marked quiet after the loop's report (SequenceTrack::CountLoop). Without this
    // rule, the report reads the new mark.
    bool quiet_after_report = true;
};

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
    static constexpr bool kBendRamps = true;

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

    // The sound's volume, the biquad filter's value (0xB5) and a tie's velocity, from 0 to 127 as 0 to 1.
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
        const float steps = turn * 4294967296.0f;
        const uint32_t step = steps < 2147483648.0f
                                  ? static_cast<uint32_t>(ToInt32(steps))
                                  : static_cast<uint32_t>(ToInt32(steps - 2147483648.0f)) + 0x80000000u;
        const float* entry = tables::kSin[step >> 24];

        return std::fma(entry[1] * static_cast<float>(step & 0xFFFFFF), 0x1p-24f, entry[0]);
    }

    // Four straight lines through 0, 1, 0, -1 and 0 (CurveTriangle, 0x022BB50C).
    static float Triangle(float turn)
    {
        constexpr std::array<float, 5> kSlopes = {4.0f, -4.0f, -4.0f, 4.0f, 4.0f};
        constexpr std::array<float, 5> kOffsets = {0.0f, 2.0f, 2.0f, -4.0f, 0.0f};
        const float quarters = turn * 4.0f;
        const int32_t line =
            quarters < 2147483648.0f ? ToInt32(quarters) : ToInt32(quarters - 2147483648.0f) + INT32_MIN;
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
    static constexpr bool kBendRamps = true;

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

    // The sound's volume, the biquad filter's value (0xB5) and a tie's velocity, from 0 to 127 as 0 to 1.
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

    // AArch64's conversion of a float to a 32-bit integer (fcvtzs): toward zero, held to the integer's range, and 0 for
    // NaN.
    static int32_t ToInt32(float x)
    {
        if (std::isnan(x))
        {
            return 0;
        }
        if (x <= -2147483648.0f)
        {
            return INT32_MIN;
        }
        if (x >= 2147483648.0f)
        {
            return INT32_MAX;
        }

        return static_cast<int32_t>(x);
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
        return value < sustain;
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
        return rate / 5.0f;
    }

    static float EnvelopeLevel(float value)
    {
        return value / 10.0f;
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

// nw::snd in the 3DS's SDK, on the ARM11's VFPv2. This library is older than the Wii U's library: one LFO, no extended
// commands, a call stack 3 deep. The library lacks a channel's silence, the outputs, the pan range, the user's pitch,
// and the player's filters that override the track's filters. code.bin keeps reciprocals as floats, and the code
// multiplies by them. The Wii U's code divides instead, and the results differ in the last bit.
struct CtrPolicy
{
    // The console's policy. A user's policy that derives from it gives this one to the library's builds of the envelope
    // and the LFO.
    using Console = CtrPolicy;

    // A sound frame of 160 samples at 32,728 Hz, in units of an ARM11 cycle over 1000 (UpdateTick, 0x31ff50).
    static constexpr uint64_t kFrameUnits = 0x4e200000;

    // A millisecond in those units: the ARM11's clock of 268,111,856 Hz, as code.bin's float, and its reciprocal.
    static constexpr float kUnitsPerMs = std::bit_cast<float>(0x4d7fb0ffu);
    static constexpr float kMsPerUnit = std::bit_cast<float>(0x3180278du);

    // The clock counts in those units. NW4R SND's clock counts in float milliseconds (RevolutionPolicy).
    static constexpr bool kFloatClock = false;

    // A sequence's 16- and 24-bit arguments are big-endian, although the archives are little-endian (ReadArg,
    // 0x4908d4).
    static constexpr bool kLittleEndianArguments = false;

    // code.bin's reciprocals: 1/63, 1/127, 1/127^3 and 1/60000.
    static constexpr float kOneOver63 = std::bit_cast<float>(0x3c820821u);
    static constexpr float kOneOver127 = std::bit_cast<float>(0x3c010204u);
    static constexpr float kOneOver127Cubed = std::bit_cast<float>(0x35030c28u);
    static constexpr float kOneOver60000 = std::bit_cast<float>(0x378bcf65u);

    // The library's sizes: one LFO, a call stack 3 deep, and two effect sends.
    static constexpr int kLfoCount = 1;
    static constexpr int kCallStackDepth = 3;
    static constexpr int kFxSendCount = 2;

    // The model holds a tick to this many commands of a track, as on the Wii U. The tick then ends, and the track reads
    // on in the next tick.
    static constexpr int kParseLimit = 10000;
    static constexpr bool kParseLimitEndsTrack = false;

    // After a random prefix (0xA0), a variable prefix (0xA1) is a command that does nothing. A false condition (0xA2)
    // skips the end (0xFF) too.
    static constexpr bool kArgPrefixesChain = false;
    static constexpr bool kEndIsConditional = true;

    // The LFO that a channel plays: the sine from the table's quarter wave.
    using Lfo = TableLfo<CtrPolicy>;

    // A return (0xFD) skips loop entries to the newest call, and a loop end (0xFC) ignores a call's entry. Opening a
    // track clears its wait, its call stack and its wait for notes to end.
    static constexpr bool kReturnSkipsLoops = true;
    static constexpr bool kLoopEndIgnoresCalls = true;
    static constexpr bool kOpenClearsState = true;

    // A channel's update each sound frame (Channel::Update, 0x17afcc). The channel's envelope, LFO and sweep move on
    // 5 ms. The channel's volume before the envelope and the volume LFO is the silence level in 255ths times the
    // volumes of the note and the track. The 3DS's channel holds the note's volume, from its velocity and its region,
    // in `velocity_volume`, unsquared, and its silence level stays 255. The channel's pitch in semitones before the
    // pitch LFO adds the key's distance from the wave's original key to the sweep, then the bend. A channel leaves the
    // engine's channels as soon as its voice ends or is dropped (VoiceCallbackFunc, 0x17fb84). The channel's LFO starts
    // as a pitch LFO and always steps. A note-off releases the channel at once (Channel::InitParam, 0x320e70, and
    // Channel::NoteOff, 0x320c6c).
    static constexpr int kChannelFrameMs = 5;
    static constexpr bool kVoiceEndFreesChannel = true;
    static constexpr bool kUntypedLfos = false;
    static constexpr bool kDeferredNoteOff = false;

    static float ChannelVolume(float velocity_volume, float init_volume, float track_volume, uint32_t silence)
    {
        // code.bin's 1/255 (0x17b2ec), the float nearest to it.
        constexpr float kOneOver255 = std::bit_cast<float>(0x3B808081u);

        return (static_cast<float>(silence) * kOneOver255) * ((velocity_volume * init_volume) * track_volume);
    }

    static float NotePitch(float key, float bend, float sweep)
    {
        return (sweep + key) + bend;
    }

    // The length analysis's rules: the defaults.
    static constexpr LengthRules kLengthRules{};

    // The volume2 command (0xD5) and the main volume (0xC2) set their value at once. A time prefix only takes its
    // bytes. The pitch bend (0xC4) ramps.
    static constexpr bool kVolume2Ramps = false;
    static constexpr bool kMainVolumeRamps = false;
    static constexpr bool kBendRamps = true;

    // The random generator steps 17 times before a sound's sequence first runs. The LFO draws no numbers.
    static constexpr int kRandomStepsBeforeStart = 17;

    // The library has one build.
    struct Build
    {
    };

    // The commands the 3DS's library lacks: the extended LFO commands (0xBD, 0xBE, 0xE4, 0xF0 0xA0 to 0xBF and 0xF0
    // 0xE1 to 0xE6) and the third send (0xDE). Its parser skips 0xF0 0xA0 to 0xBF without their byte. `command` is a
    // byte, or 0xF000 plus the byte after 0xF0.
    static bool HasCommand(const Build&, uint32_t command)
    {
        if (command >= 0xF000)
        {
            const uint32_t sub = command & 0xFF;
            return !(sub >= 0xA0 && sub <= 0xBF) && !(sub >= 0xE1 && sub <= 0xE6);
        }

        return command != 0xBD && command != 0xBE && command != 0xDE && command != 0xE4;
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

    // The VFP's conversion of a float to a 32-bit integer (ftosizs): toward zero, held to the integer's range, and 0
    // for NaN.
    static int32_t ToInt32(float x)
    {
        if (std::isnan(x))
        {
            return 0;
        }
        if (x <= -2147483648.0f)
        {
            return INT32_MIN;
        }
        if (x >= 2147483648.0f)
        {
            return INT32_MAX;
        }

        return static_cast<int32_t>(x);
    }

    // The larger of two floats for the level of a volume (Util::CalcVolumeRatio, 0x17f8a8): `b` when `a` is below it,
    // else `a`, NaN included.
    static float Max(float a, float b)
    {
        return a < b ? b : a;
    }

    // A gain held at 0 from below: 0 when `x` is below 0, else `x`, NaN included (Util::CalcPanRatio, 0x18b770).
    static float AtLeastZero(float x)
    {
        return x < 0.0f ? 0.0f : x;
    }

    // The place in a pan curve for a pan from -1 to 1 (Util::CalcPanRatio, 0x18b770): the pan held to -1 and 1, then
    // (pan + 1) / 2 in 256ths, rounded. NaN takes the curve's first entry.
    static int PanPosition(float pan)
    {
        if (pan > 1.0f)
        {
            pan = 1.0f;
        }
        else if (pan < -1.0f)
        {
            pan = -1.0f;
        }

        return ToInt32((pan + 1.0f) * 0.5f * 256.0f + 0.5f);
    }

    // The place in a pan curve for a surround pan from 0 to 2 (Util::CalcSurroundPanRatio, 0x18b878): the pan held to 0
    // and 2, then pan / 2 in 256ths, rounded. NaN takes the curve's first entry.
    static int SpanPosition(float span)
    {
        if (span > 2.0f)
        {
            span = 2.0f;
        }
        else if (span < 0.0f)
        {
            span = 0.0f;
        }

        return ToInt32(span * 0.5f * 256.0f + 0.5f);
    }

    // Whether surround output takes the surround pan curves where the front doesn't bypass them. It always does.
    template <typename PanInfo>
    static bool SurroundCurves(const PanInfo&)
    {
        return true;
    }

    // A variable's shift (the sequence command F0 85) by `value` bits, left if it's 0 or more, else right. The ARM's
    // shifts take the amount's low 8 bits, and 32 or more gives 0 or the sign.
    static int32_t Shift(int32_t current, int32_t value)
    {
        if (value >= 0)
        {
            const uint32_t shift = static_cast<uint32_t>(value) & 0xFF;
            return shift >= 32 ? 0 : static_cast<int32_t>(static_cast<uint32_t>(current) << shift);
        }

        const uint32_t shift = static_cast<uint32_t>(-value) & 0xFF;
        return shift >= 32 ? (current < 0 ? -1 : 0) : current >> shift;
    }

    // The fall of a decay or a release: `value` less `rate` times `msec`, the product and the difference rounded on
    // their own.
    static float Fall(float value, float rate, int msec)
    {
        return value - rate * static_cast<float>(msec);
    }

    // Whether a decay that has fallen to `value` has reached the sustain level: below it. A level that lands on it
    // waits for the next update.
    static bool ReachesSustain(float value, float sustain)
    {
        return value < sustain;
    }

    // The channel's track volume (UpdateChannelParam, 0x31de18): the product of the track's volume, volume2 and the
    // player's main volume over 127^3, squared, then the player's volume times it. The 3DS's track has no float volume.
    static float TrackVolume(uint32_t volume, uint32_t volume2, uint32_t main_volume, float, float player_volume)
    {
        const uint32_t level = volume * (volume2 * main_volume);
        float squared = static_cast<float>(level) * kOneOver127Cubed;
        squared = squared * squared;

        return player_volume * squared;
    }

    // The track's pan and surround pan, from -63 to 63 and from 0 to 126, as -1 to 1 and 0 to 2 (UpdateChannelParam).
    static float PanOfValue(int pan)
    {
        const float value = static_cast<float>(pan) * kOneOver63;
        if (value > 1.0f)
        {
            return 1.0f;
        }

        return value < -1.0f ? -1.0f : value;
    }

    static float SpanOfValue(int span)
    {
        const float value = static_cast<float>(span) * kOneOver63;
        if (value > 2.0f)
        {
            return 2.0f;
        }

        return value < 0.0f ? 0.0f : value;
    }

    // A send from 0 to 127 as 0 to 1. The main send is that less 1.
    static float SendOfValue(uint32_t send)
    {
        return static_cast<float>(send) * kOneOver127;
    }

    // The sound's volume from 0 to 127 as 0 to 1 (the player's constructor).
    static float PlayerVolume(uint32_t volume)
    {
        return static_cast<float>(volume) * kOneOver127;
    }

    // The biquad filter's value (the sequence command 0xB5) from 0 to 127 as 0 to 1.
    static float BiquadValueOf(int32_t value)
    {
        return static_cast<float>(value) * kOneOver127;
    }

    // The volume a tie or a mono note gives its channel: the velocity over 127, squared. A new note's channel also has
    // the region's volume. A tie or a mono note loses it.
    static float VelocityVolume(int velocity)
    {
        const float value = static_cast<float>(velocity) * kOneOver127;
        return value * value;
    }

    // The sequence's ticks in a millisecond, from its timebase and tempo. The 3DS has no tempo ratio.
    static float TicksPerMs(int timebase, int tempo, float)
    {
        return static_cast<float>(static_cast<uint32_t>(timebase * tempo)) * kOneOver60000;
    }

    // The envelope's release rate and its level in decibels. The Wii U's code divides by 5 and by 10 instead.
    static float ReleaseRate(float rate)
    {
        return rate * 0.2f;
    }

    static float EnvelopeLevel(float value)
    {
        return value * 0.1f;
    }

    // The table LFO's step of its phase in `msec` milliseconds (Lfo::Update, 0x1811c8), and its level from the quarter
    // wave's `sine` (Lfo::GetValue, 0x181434).
    static float LfoPhaseStep(float speed, int msec)
    {
        return speed * (static_cast<float>(msec) * 0.001f);
    }

    static float LfoLevel(int sine, float depth, uint8_t range)
    {
        return static_cast<float>(range) * (depth * (static_cast<float>(sine) * kOneOver127));
    }

    // A low-pass value held to 1 (Util::CalcLpfFreq, 0x17f6e4). The code doesn't hold it to 0. A value below the first
    // step gives 80 Hz anyway.
    static float LpfValue(float lpf)
    {
        return lpf > 1.0f ? 1.0f : lpf;
    }

    // The steps of kLpfFreq from the first step to a low-pass value `above_first` past it. code.bin multiplies by
    // 29.999998 where the Wii U divides by 1/30.
    static float LpfSteps(float above_first)
    {
        return above_first * 29.999998f;
    }

    // The biquad presets that the library registers (0x1372a8): types 1 to 5.
    static constexpr int kBiquadPresetTypes = 5;
};

// NW4R SND on the Wii's Broadway, after Wii Sports' main.dol. NW4R SND is older than nw::snd: one LFO, no extended
// commands, a call stack 3 deep and three effect sends. The library lacks a channel's silence, the outputs, the pan
// range, the user's pitch, and the player's filters that override the track's filters. NW4R SND's code divides where
// the 3DS's code multiplies by reciprocals. Four builds of it differ in their commands, and WarioWare: Smooth Moves'
// build of October 2006 in its clock too. A sound's build comes from its main.dol or its AX, and the player holds it.
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

    // The volume2 command (0xD5), the main volume (0xC2) and the pitch bend (0xC4) set their value at once. A time
    // prefix only takes its bytes (MmlParser::CommandProc, 0x8003bee8).
    static constexpr bool kVolume2Ramps = false;
    static constexpr bool kMainVolumeRamps = false;
    static constexpr bool kBendRamps = false;

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
            return build.Since2008();
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
        return static_cast<uint16_t>(build.Since2008() ? std::clamp(value, 0, 1023) : value);
    }

    // The sweep command's pitch (0xE3): in 32nds of a semitone, and from 2008 in 64ths (Mario Kart Wii, 0x80096b58).
    static float SweepPitchOf(const Build& build, int32_t value)
    {
        return static_cast<float>(value) / (build.Since2008() ? 64.0f : 32.0f);
    }

    // A note in a key group releases the track's other notes in the group, each at its release rate (Mario Kart Wii,
    // 0x8009bfcc). Builds before 2008 read no key group from the bank.
    static constexpr bool kKeyGroupFastRelease = false;

    // Whether a track's envelope value (0xD0 to 0xD3) or hold (0xB1) replaces the bank's value at a note's start.
    // Builds before 2008 take any value but 0xFF (SeqTrack::NoteOn, 0x80040d40 in Wii Sports' main.dol). Builds from
    // 2008 take 0 to 127 (0x8009c02c in Mario Kart Wii's main.dol). The hold comes in 2008.
    static bool ReplacesEnvelope(const Build& build, int value)
    {
        return build.Since2008() ? value <= 127 : value != 0xFF;
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

    // The low-pass's cutoff in Hz for a value from 0 to 1: 32000 * 2^(10 (value - 1)), from 31 Hz to 32000 Hz. The Wii
    // has no table of cutoffs (see CalcLpfFreq). The game works the power out with its C library's pow, in double
    // precision.
    static uint16_t LpfFreq(float value)
    {
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

    // The voice limits both gains to full level (Voice::SetVeVolume, 0x8004c874). Only a volume LFO lifts them above
    // full level. An engine with the hook follows each frame in which one does.
    if constexpr (requires { engine.OnGainAboveFull(channel); })
    {
        if (lfo_target == 1 && (ve_init_volume > 1.0f || ve_target_volume > 1.0f))
        {
            engine.OnGainAboveFull(channel);
        }
    }

    // An engine with this hook follows every frame of a volume LFO: the envelope's level and the LFO's, in decibels,
    // where the voice's ramp ends.
    if constexpr (requires { engine.OnVolumeLfo(channel, 0.0f, 0.0f); })
    {
        if (lfo_target == 1)
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
