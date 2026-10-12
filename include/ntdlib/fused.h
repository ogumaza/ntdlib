// SPDX-License-Identifier: MIT

// The PowerPC's arithmetic where a cast or a compiler's instructions would give another result on some hosts: the
// negated fused multiply-add, and the conversion of a float to a 32-bit integer.
//
// The negated fused multiply-add (fnmadd and fnmsub, in their double, single and paired-single forms) is a * c + b,
// rounded once, then negated. Compilers turn a negated fma into one instruction that negates before it rounds instead
// (Arm's FNMADD or x86's VFNMSUB, for example). Those instructions give +0 for an exact zero of operands with opposite
// signs. The PowerPC gives -0. The sum passes through a volatile. The two steps then stay apart, and every build gives
// the PowerPC's result.

#pragma once

#include <cmath>
#include <cstdint>

namespace ntdlib
{

inline double NegatedFma(double a, double c, double b)
{
    volatile double sum = std::fma(a, c, b);
    return -sum;
}

inline float NegatedFma(float a, float c, float b)
{
    volatile float sum = std::fma(a, c, b);
    return -sum;
}

// The PowerPC's conversion of a float to a 32-bit integer (fctiwz): toward zero, held to the integer's range, and the
// most negative integer for NaN. A cast out of the range is undefined in C++, and x86 gives the most negative integer
// for every value out of it. The range comes first. A model that converts every sample then pays one test for it.
inline int32_t PowerPcToInt32(float x)
{
    if (x >= -2147483648.0f && x < 2147483648.0f)
    {
        return static_cast<int32_t>(x);
    }

    return x >= 2147483648.0f ? INT32_MAX : INT32_MIN;
}

} // namespace ntdlib
