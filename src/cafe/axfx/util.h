// SPDX-License-Identifier: MIT

// What the effects' models share: the library's conversions of integers to floats, the PowerPC's fused multiply-adds,
// the test of a gain's range, the clear and the position's step of a line, and the renderer's samples per millisecond.
// The library converts a float to an unsigned integer as the rest of the Wii U's code does (ntdlib::CafeToUint32).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "ntdlib/fused.h"

namespace ntdlib::cafe::axfx
{

// The library's conversions of an integer to a float: through a double (exact), then rounded to single precision.
inline float ToFloat(uint32_t value)
{
    return static_cast<float>(value);
}

inline float ToFloat(int32_t value)
{
    return static_cast<float>(value);
}

// The PowerPC's fmadds: a * c + b, rounded once to single precision. Game mode's interpreter rounds it to double
// precision first, then to single. The two differ only where the result in double precision lies halfway between two
// floats. Which of the two the Wii U's CPU does isn't known.
inline float Fmadds(float a, float c, float b)
{
    return std::fma(a, c, b);
}

// The PowerPC's fnmsubs: -(a * c - b), rounded once, then negated. An exact zero comes out as -0.
inline float NegatedFms(float a, float c, float b)
{
    return ntdlib::NegatedFma(a, c, -b);
}

// Whether a gain is below 0 or above 1. A NaN gain passes the library's tests.
inline bool OutOfRange(float value)
{
    return value < 0.0f || value > 1.0f;
}

// Sets every sample of a line to 0.
template <typename Sample>
void Clear(std::vector<Sample>& line)
{
    std::fill(line.begin(), line.end(), Sample{});
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

// The renderer's samples per millisecond: the rate divided by 1,000 (mulhwu and srwi).
inline uint32_t SamplesPerMs(uint32_t rate)
{
    return rate / 1000;
}

} // namespace ntdlib::cafe::axfx
