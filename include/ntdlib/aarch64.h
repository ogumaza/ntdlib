// SPDX-License-Identifier: MIT

// AArch64's floating-point instructions whose results C++ doesn't give, for models of code built for the Switch's
// processor. fmax and fmin differ from std::fmax and std::fmin in their NaNs and zeros: a NaN operand gives NaN, and -0
// counts as below +0. The reciprocal's estimate and step follow the architecture's pseudocode, with the
// floating-point control register at 0: round to nearest, no flushing to zero and no default NaN.

#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace ntdlib::aarch64
{

// fmax: a NaN gives NaN, and +0 wins over -0.
inline float Fmax(float a, float b)
{
    if (std::isnan(a))
    {
        return a;
    }
    if (std::isnan(b))
    {
        return b;
    }
    if (a == 0.0f && b == 0.0f)
    {
        return std::signbit(a) ? b : a;
    }

    return a > b ? a : b;
}

// fmin: a NaN gives NaN, and -0 wins over +0.
inline float Fmin(float a, float b)
{
    if (std::isnan(a))
    {
        return a;
    }
    if (std::isnan(b))
    {
        return b;
    }
    if (a == 0.0f && b == 0.0f)
    {
        return std::signbit(a) ? a : b;
    }

    return a < b ? a : b;
}

// fminnm against a number `b`: a NaN `a` gives `b`.
inline float Fminnm(float a, float b)
{
    if (std::isnan(a))
    {
        return b;
    }

    return std::min(a, b);
}

// fmaxnm: a NaN operand gives the other operand, and +0 wins over -0.
inline float Fmaxnm(float a, float b)
{
    if (std::isnan(a))
    {
        return b;
    }
    if (std::isnan(b))
    {
        return a;
    }

    return Fmax(a, b);
}

// frecpe: 1 / x to about 8 bits. The leading one of x's significand and the 8 bits below it give a, from 256 to 511.
// The architecture's RecipEstimate works the estimate's significand out as (2^19 / (2a + 1) + 1) / 2. A NaN comes
// back quiet. Infinity gives 0, and 0 or a number below 2^-128 gives infinity, each with x's sign.
inline float Frecpe(float x)
{
    const uint32_t bits = std::bit_cast<uint32_t>(x);
    const uint32_t sign = bits & 0x80000000u;
    if (std::isnan(x))
    {
        return std::bit_cast<float>(bits | 0x00400000u);
    }
    if (std::isinf(x))
    {
        return std::bit_cast<float>(sign);
    }
    if (std::fabs(x) < 0x1p-128f)
    {
        return std::bit_cast<float>(sign | 0x7F800000u);
    }

    // The significand as the pseudocode's 52-bit fraction, normalised for a subnormal x.
    uint64_t fraction = static_cast<uint64_t>(bits & 0x007FFFFFu) << 29;
    int exponent = static_cast<int>(bits >> 23 & 0xFF);
    if (exponent == 0)
    {
        if ((fraction >> 51 & 1) == 0)
        {
            exponent = -1;
            fraction = (fraction << 2) & ((uint64_t{1} << 52) - 1);
        }
        else
        {
            fraction = (fraction << 1) & ((uint64_t{1} << 52) - 1);
        }
    }

    const int a = static_cast<int>(256 | fraction >> 44);
    const int estimate = ((1 << 19) / (a * 2 + 1) + 1) / 2;
    int result_exponent = 253 - exponent;
    fraction = static_cast<uint64_t>(estimate & 0xFF) << 44;
    if (result_exponent == 0)
    {
        fraction = (uint64_t{1} << 51) | fraction >> 1;
    }
    else if (result_exponent == -1)
    {
        fraction = (uint64_t{1} << 50) | fraction >> 2;
        result_exponent = 0;
    }

    return std::bit_cast<float>(sign | static_cast<uint32_t>(result_exponent) << 23 |
                                static_cast<uint32_t>(fraction >> 29));
}

// frecps: 2 - a * b with one rounding, the factor of a Newton step towards 1 / a. Infinity times 0 gives 2. A NaN
// operand gives a quiet NaN. A signalling NaN goes first, then a before b, and a NaN from a has its sign changed.
inline float Frecps(float a, float b)
{
    const float minus_a = -a;
    const auto is_signalling = [](float n)
    {
        return std::isnan(n) && (std::bit_cast<uint32_t>(n) & 0x00400000u) == 0;
    };

    if (std::isnan(minus_a) || std::isnan(b))
    {
        const float nan = std::isnan(minus_a) && (is_signalling(minus_a) || !is_signalling(b)) ? minus_a : b;
        return std::bit_cast<float>(std::bit_cast<uint32_t>(nan) | 0x00400000u);
    }
    if ((std::isinf(minus_a) && b == 0.0f) || (minus_a == 0.0f && std::isinf(b)))
    {
        return 2.0f;
    }
    if (std::isinf(minus_a) || std::isinf(b))
    {
        return std::signbit(minus_a) != std::signbit(b) ? -std::numeric_limits<float>::infinity()
                                                        : std::numeric_limits<float>::infinity();
    }

    return std::fma(minus_a, b, 2.0f);
}

} // namespace ntdlib::aarch64
