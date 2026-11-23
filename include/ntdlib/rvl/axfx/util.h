// SPDX-License-Identifier: MIT

// What the effects' models share: the games' sample rate, their runtime's conversion of a float to an unsigned integer,
// the reverbs' range check, comb gains, allpasses and low-pass weight, and the lines' steps and clearing.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "ntdlib/ax/axfx.h"

namespace ntdlib::rvl::axfx
{

// The sample rate that the games' code converts times with.
constexpr float kSampleRate = 32000.0f;

// The runtime's conversion of a float to an unsigned integer (__cvt_fp2unsigned, 0x800b1c1c in Wii Sports, 0x804be79c
// in WarioWare: Smooth Moves): 0 below 0, all ones from 2^32 and for NaN, and truncated between.
inline uint32_t ToUnsigned(float value)
{
    if (value < 0.0f)
    {
        return 0;
    }

    if (!(value < 4294967296.0f))
    {
        return 0xffffffff;
    }

    return static_cast<uint32_t>(value);
}

// The range test of a gain, and the clear, the release and the step of a line. The Wii U's effects use the same
// (ax/axfx.h). A NaN gain passes the range test, as it passes the games' comparisons.
using ntdlib::ax::axfx::Clear;
using ntdlib::ax::axfx::Free;
using ntdlib::ax::axfx::OutOfRange;
using ntdlib::ax::axfx::Step;

// The gain of a reverb's comb `length` samples long that decays by 60 dB in `time` seconds. The games call MSL's pow
// (0x800c0e80 in Wii Sports, 0x804cb4dc in Smooth Moves) in double.
inline float CombCoef(uint32_t length, float time)
{
    const float exponent = -3.0f * static_cast<float>(length) / (kSampleRate * time);
    return static_cast<float>(std::pow(10.0, static_cast<double>(exponent)));
}

// One sample of a reverb's allpass of gain `coef` (the coloration), whose line holds `delayed`, the sample from one
// line's length ago. Stores the new sample in its place and returns the output.
inline float Allpass(float& delayed, float input, float coef)
{
    const float output = delayed;
    const float stored = input + output * coef;
    delayed = stored;

    return output - stored * coef;
}

// The weight that a reverb's low-pass gives its last output: 1 - damping, capped at 0.95.
inline float LowpassCoef(float damping)
{
    return std::min(1.0f - damping, 0.95f);
}

} // namespace ntdlib::rvl::axfx
