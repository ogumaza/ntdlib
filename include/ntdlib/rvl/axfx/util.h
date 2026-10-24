// SPDX-License-Identifier: MIT

// What the effects' models share: the games' sample rate, their runtime's conversion of a float to an unsigned integer,
// the reverbs' range check, comb gains, allpasses and low-pass weight, the lines' steps, and the reverbs' line
// clearing.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

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

// Whether a gain lies outside 0 to 1. A NaN gain doesn't. The games' comparisons let NaN through too.
inline bool OutOfRange(float value)
{
    return value < 0.0f || value > 1.0f;
}

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

// Moves a line's position on by a sample. The position wraps to 0 at the line's length.
inline void Step(uint32_t& position, uint32_t length)
{
    position++;
    if (position >= length)
    {
        position = 0;
    }
}

// What the reverbs' __BzeroDelayLines and __FreeDelayLine do to each delay line.
inline void ClearLine(std::vector<float>& line)
{
    std::fill(line.begin(), line.end(), 0.0f);
}

inline void ReleaseLine(std::vector<float>& line)
{
    line = {};
}

} // namespace ntdlib::rvl::axfx
