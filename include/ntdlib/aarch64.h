// SPDX-License-Identifier: MIT

// AArch64's floating-point minimum and maximum, for models of code built for the Switch's processor. fmax and fmin
// differ from std::fmax and std::fmin in their NaNs and zeros: a NaN operand gives NaN, and -0 counts as below +0.

#pragma once

#include <algorithm>
#include <cmath>

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

} // namespace ntdlib::aarch64
