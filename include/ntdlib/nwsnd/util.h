// SPDX-License-Identifier: MIT

// The sound library's arithmetic helpers (nw::snd::internal::Util in NintendoWare for Cafe, nn::atk::detail::Util on
// the Switch) and its biquad presets. The functions that differ between the consoles take a console policy (see
// console.h). Each function's comment gives the address of the routine it follows in Animal Crossing: amiibo Festival's
// executable.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "ntdlib/nwsnd/tables.h"

namespace ntdlib::nwsnd
{

// nw::snd's output modes (OutputMode). The Switch's nn::atk has the first three.
enum class OutputMode : uint8_t
{
    kMono = 0,
    kStereo = 1,
    kSurround = 2,
    kDpl2 = 3,
};

// How a pan becomes a gain (Util::PanInfo).
struct PanInfo
{
    uint32_t curve = 0;           // 0 the square root, 1 the sine and cosine, 2 linear
    bool center_zero_db = false;  // scaled so that the centre is unity
    bool clamp_to_unity = false;  // held to 1 rather than 2
    bool front_bypass = false;    // the stereo curves, even for surround output
    bool surround_curves = false; // on the Switch, the surround curves for surround output where the front doesn't
                                  // bypass them
};

// A pan curve's PanInfo (the switch in CalcPreMixVolume, 0x0229C2F4). Curves 0 to 8 are the square root, sine and
// linear curves in turn. Each curve comes plain, then centred at unity, then centred and held to unity. Anything past
// curve 8 is the plain square root.
inline PanInfo PanInfoOf(uint32_t pan_curve)
{
    constexpr std::array<PanInfo, 9> kCurves = {{
        {.curve = 0},
        {.curve = 0, .center_zero_db = true},
        {.curve = 0, .center_zero_db = true, .clamp_to_unity = true},
        {.curve = 1},
        {.curve = 1, .center_zero_db = true},
        {.curve = 1, .center_zero_db = true, .clamp_to_unity = true},
        {.curve = 2},
        {.curve = 2, .center_zero_db = true},
        {.curve = 2, .center_zero_db = true, .clamp_to_unity = true},
    }};

    return pan_curve < kCurves.size() ? kCurves[pan_curve] : kCurves[0];
}

// The C library's conversion of a float to an unsigned 64-bit integer (_f_ftoull, 0x0263486C), the same as AArch64's
// fcvtzu: toward zero, and 0 for what's below 1 or NaN.
uint64_t ToUint64(float x);

// The pitch ratio for a pitch in 1/256 semitones (Util::CalcPitchRatio, 0x022A2640).
float CalcPitchRatio(int pitch);

// The gain for a level in dB, held to -90.4 and +6.0 (Util::CalcVolumeRatio, 0x022A2730). A level above 6 dB is 6 dB.
// The Wii U's fsel holds NaN to -90.4 dB. The Switch's fmax keeps NaN. A NaN level converts to the entry for 0 dB.
template <typename Policy>
float CalcVolumeRatio(float decibel)
{
    const float held = Policy::Max(decibel, -90.4f) * 10.0f;
    const float tenths = decibel > 6.0f ? 60.0f : held;
    return tables::kDecibel[Policy::ToInt32(tenths) + 904];
}

// The gain for a pan from -1 (left) to 1 (right) (Util::CalcPanRatio, 0x0229C0DC). Surround output takes the surround
// curves unless the pan bypasses them.
template <typename Policy>
float CalcPanRatio(float pan, const PanInfo& info, OutputMode mode)
{
    const int position = Policy::PanPosition(pan);

    // A sound's pan curve comes from its archive. Archives give 0 to 2. The game doesn't check the value.
    const bool surround = !info.front_bypass && mode == OutputMode::kSurround && Policy::SurroundCurves(info);
    const float* curve = surround ? tables::kSurroundPan[info.curve % 3] : tables::kStereoPan[info.curve % 3];
    float ratio = curve[position];
    if (info.center_zero_db)
    {
        ratio /= curve[128];
    }

    const float limit = info.clamp_to_unity ? 1.0f : 2.0f;
    if (ratio > limit)
    {
        return limit;
    }

    return Policy::AtLeastZero(ratio);
}

// The gain for a surround pan from 0 (front) to 2 (rear) (Util::CalcSurroundPanRatio, 0x0229C228).
template <typename Policy>
float CalcSurroundPanRatio(float span, const PanInfo& info)
{
    const float ratio = tables::kStereoPan[info.curve % 3][Policy::SpanPosition(span)];
    if (ratio > 2.0f)
    {
        return 2.0f;
    }

    return Policy::AtLeastZero(ratio);
}

// The entry of kLpfFreq for a low-pass value from 0 to 1 (Util::CalcLpfFreq, 0x0229CE40): 0 (80 Hz) below the first
// step at 0.13561438, 23 (16000 Hz) from 0.9, and the steps a thirtieth apart between them. Policy::LpfSteps counts the
// steps. The Wii has no table (see CalcLpfFreq).
template <typename Policy>
int LpfFreqIndex(float lpf)
{
    constexpr float kFirstStep = 0x1.15bcfep-3f; // 0.13561438
    const float value = Policy::LpfValue(lpf);
    if (value < kFirstStep)
    {
        return 0;
    }
    if (value >= 0.9f)
    {
        return 23;
    }

    return Policy::ToInt32(Policy::LpfSteps(value - kFirstStep));
}

// The cutoff in Hz for a low-pass value from 0 to 1 (Util::CalcLpfFreq): kLpfFreq's entry (see LpfFreqIndex). The Wii's
// policy works the cutoff out instead, in LpfFreq.
template <typename Policy>
uint16_t CalcLpfFreq(float lpf)
{
    if constexpr (requires { Policy::LpfFreq(lpf); })
    {
        return Policy::LpfFreq(Policy::LpfValue(lpf));
    }
    else
    {
        return tables::kLpfFreq[LpfFreqIndex<Policy>(lpf)];
    }
}

// nn::atk's low-pass at a cutoff that CalcLpfFreq gives, as a one-pole biquad (b0, b1, b2, a1 and a2 in Q14), for a
// renderer at `sample_rate` (Util::CalcLowPassBiquad, 0xc1a310 in WarioWare: Get It Together!'s main). From the tables,
// the rate must be 32 or 48 kHz, and the cutoff one of the tables' cutoffs. Anything else gives zeros. The game stops
// on such input. Without `from_table`, the coefficients are the exact one-pole for the rate. The Switch's arithmetic
// holds throughout.
std::array<int16_t, 5> CalcLowPassBiquad(uint16_t frequency, int32_t sample_rate, bool from_table);

// A biquad preset: its table of coefficients, with b0, b1, b2, a1 and a2 in each row, and whether it's a band-pass.
struct BiquadPreset
{
    std::span<const int16_t[5]> rows;
    bool band_pass = false;
};

// The library's biquad presets, types 1 to 10: BiquadFilterLpf, BiquadFilterHpf, BiquadFilterBpf512,
// BiquadFilterBpf1024 and BiquadFilterBpf2048 for a 32 kHz renderer, then the same filters for 48 kHz.
inline constexpr BiquadPreset kBiquadPresets[10] = {
    {tables::kBiquadLpf, false},        {tables::kBiquadHpf, false},       {tables::kBiquadBpf512, true},
    {tables::kBiquadBpf1024, true},     {tables::kBiquadBpf2048, true},    {tables::kBiquadLpf48k, false},
    {tables::kBiquadHpf48k, false},     {tables::kBiquadBpf512_48k, true}, {tables::kBiquadBpf1024_48k, true},
    {tables::kBiquadBpf2048_48k, true},
};

// The row of biquad preset `type`'s table for a filter value, or -1 for a type without a preset (the presets'
// GetCoefficients: BiquadFilterLpf's GetCoefficients at 0x022BB710 and the four after it). Every console takes the row
// at the value times the table's last row, truncated towards zero and held to the table. The band-passes (types 3 to 5)
// scale value * (2 - value) instead. Types 1 to 5 are the presets made for a 32 kHz renderer. The Switch adds the same
// filters made for 48 kHz as types 6 to 10 (Policy::kBiquadPresetTypes).
template <typename Policy>
int BiquadPresetRow(int type, float value)
{
    if (type < 1 || type > Policy::kBiquadPresetTypes)
    {
        return -1;
    }

    const BiquadPreset& preset = kBiquadPresets[type - 1];
    const int last = static_cast<int>(preset.rows.size()) - 1;
    const float scale = preset.band_pass ? value * (2.0f - value) : value;
    return std::clamp(Policy::ToInt32(static_cast<float>(last) * scale), 0, last);
}

// The coefficients b0, b1, b2, a1 and a2 of that row, or nullptr for a type without a preset.
template <typename Policy>
const int16_t* BiquadPresetCoefficients(int type, float value)
{
    const int row = BiquadPresetRow<Policy>(type, value);
    return row >= 0 ? kBiquadPresets[type - 1].rows[static_cast<std::size_t>(row)] : nullptr;
}

// The sound library's random numbers (Util::CalcRandom, 0x0229EF64): x = x * 0x19660D + 0x3C6EF35F. Each number is x's
// top 16 bits.
class Random
{
public:
    // The generator starts at `seed`. nw::snd's generator starts at 0x12345678.
    explicit Random(uint32_t seed = 0x12345678) : x_(seed)
    {
    }

    uint16_t Next()
    {
        x_ = x_ * 0x19660Du + 0x3C6EF35Fu;

        return static_cast<uint16_t>(x_ >> 16);
    }

    uint32_t State() const
    {
        return x_;
    }

private:
    uint32_t x_;
};

} // namespace ntdlib::nwsnd
