// SPDX-License-Identifier: MIT

// The integer steps that the renderer's code shares. AArch64's 32-bit integer instructions wrap. A float converts to an
// integer towards zero, held to 32 bits (fcvtzs), as ConvertToInt converts it.

#pragma once

#include <cstdint>

#include "ntdlib/nx/audio/dsp.h"

namespace ntdlib::nx::audio
{

inline int32_t WrapAdd(int32_t a, int32_t b)
{
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}

inline int32_t WrapSub(int32_t a, int32_t b)
{
    return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}

inline int32_t WrapMul(int32_t a, int32_t b)
{
    return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
}

// A float as an integer, and in fixed point with `bits` fraction bits.
inline int32_t ToInt(float x)
{
    int32_t value = 0;
    ConvertToInt(&value, &x, 1);

    return value;
}

inline int32_t ToFixed(float x, int bits)
{
    return ToInt(x * static_cast<float>(1 << bits));
}

} // namespace ntdlib::nx::audio
