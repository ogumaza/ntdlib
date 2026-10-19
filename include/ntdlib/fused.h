// SPDX-License-Identifier: MIT

// The consoles' arithmetic where a cast or a compiler's instructions would give another result on some hosts: the
// PowerPC's negated fused multiply-add, the PowerPC's and ARM's conversions of a float to a 32-bit integer, and the
// Wii U's conversion of a float to an unsigned one.
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

// The Wii U's conversion of a float to a 32-bit unsigned integer, as its compiler makes it from two fctiwz: below 2^31,
// PowerPcToInt32's result taken as unsigned, and from 2^31, PowerPcToInt32 of the value less 2^31 with 2^31 added back.
// From 2^32 up, that gives 0xffffffff. A NaN fails the comparison with 2^31 and goes the second way. PowerPcToInt32
// gives the most negative integer for it. The sum then wraps to 0. The Wii's library converts with a different routine
// (nwsnd::RevolutionPolicy::ToUnsigned).
inline uint32_t CafeToUint32(float x)
{
    if (x < 2147483648.0f)
    {
        return static_cast<uint32_t>(PowerPcToInt32(x));
    }

    return static_cast<uint32_t>(PowerPcToInt32(x - 2147483648.0f)) + 0x80000000u;
}

// ARM's conversion of a float to a 32-bit integer, as both AArch64's fcvtzs and the VFP's ftosizs make it: toward zero,
// held to the integer's range, and 0 for NaN. nwsnd's NxPolicy and CtrPolicy convert with it. CafePolicy and
// RevolutionPolicy convert with PowerPcToInt32.
inline int32_t ArmToInt32(float x)
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

} // namespace ntdlib
