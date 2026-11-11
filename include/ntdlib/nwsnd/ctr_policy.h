// SPDX-License-Identifier: MIT

// The console policy of the sound library's model for the 3DS: nw::snd in the SDK of the 3DS (see console.h). A
// comment gives the address of the routine that a step follows in Pokemon X's code.bin.

#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>

#include "ntdlib/fused.h"
#include "ntdlib/nwsnd/cafe_policy.h"
#include "ntdlib/nwsnd/envelope.h"
#include "ntdlib/nwsnd/length_rules.h"
#include "ntdlib/nwsnd/util.h"

namespace ntdlib::nwsnd
{

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

    static constexpr bool BendRamps(const auto&)
    {
        return true;
    }

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

    // The VFP's conversion of a float to a 32-bit integer (ftosizs).
    static int32_t ToInt32(float x)
    {
        return ArmToInt32(x);
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
    // player's main volume over 127^3, squared, then times the product of the track's float volume and the player's
    // volume. The float volume is +8 of the track, which the sound's handle sets (SequenceSoundPlayer::SetTrackVolume:
    // driver command 0x19 and 0x2776cc in Mario Kart 7's code.bin). UpdateChannelParam reads it at 0x31de74 and
    // multiplies it by the player's volume at 0x31de88. A Unicorn run of 0x31de18 at track volumes of 1, 0.8, 0.75, 0.5
    // and 0 gives this product, bit for bit. At a volume of 1 it's the player's volume times the square.
    static float TrackVolume(uint32_t volume, uint32_t volume2, uint32_t main_volume, float track_volume,
                             float player_volume)
    {
        const uint32_t level = volume * (volume2 * main_volume);
        float squared = static_cast<float>(level) * kOneOver127Cubed;
        squared = squared * squared;

        return (track_volume * player_volume) * squared;
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

    // The sequence's ticks in a millisecond, from its timebase, tempo and tempo ratio: the product of timebase and
    // tempo, times the ratio, times 1/60000, in that order (StreetPass Mii Plaza's update v14.6.0, 0x342444). The ratio
    // stays 1 unless the game sets it, and a ratio of 1 changes nothing.
    static float TicksPerMs(int timebase, int tempo, float tempo_ratio)
    {
        return static_cast<float>(static_cast<uint32_t>(timebase * tempo)) * tempo_ratio * kOneOver60000;
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

} // namespace ntdlib::nwsnd
