// SPDX-License-Identifier: MIT

// What the effects' models share: the library's conversions of integers to floats, the PowerPC's fused multiply-adds,
// the test of a gain's range, the clear and the position's step of a line, the renderer's samples per millisecond, and
// the steps of Settings in the reverbs and the chorus.
// The library converts a float to an unsigned integer as the rest of the Wii U's code does (ntdlib::CafeToUint32).

#pragma once

#include <cmath>
#include <cstdint>

#include "ntdlib/ax/axfx.h"
#include "ntdlib/cafe/axfx/axfx.h"
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
// precision first, then to single. The two differ only where the result in double precision is halfway between two
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

// The range test of a gain, and the clear, the release and the step of a line. The Wii's effects use the same
// (ax/axfx.h).
using ntdlib::ax::axfx::Clear;
using ntdlib::ax::axfx::Free;
using ntdlib::ax::axfx::OutOfRange;
using ntdlib::ax::axfx::Step;

// The renderer's samples per millisecond: the rate divided by 1,000 (mulhwu and srwi).
inline uint32_t SamplesPerMs(uint32_t rate)
{
    return rate / 1000;
}

// Settings in the reverbs and the chorus: `off` turns the effect off and frees its lines, and `init` runs Init. A
// failed Init leaves the effect off. After Init succeeds, the next Process leaves its frame as it is.
template <typename Off, typename Init>
bool Settings(uint32_t& active, const Off& off, const Init& init)
{
    off();
    if (!init())
    {
        off();
        return false;
    }

    active = (active | kSkip) & ~kOff;

    return true;
}

} // namespace ntdlib::cafe::axfx
