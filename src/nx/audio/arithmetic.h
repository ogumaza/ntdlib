// SPDX-License-Identifier: MIT

// The integer steps that the renderer's code shares. AArch64's 32-bit integer instructions wrap. A float converts to an
// integer towards zero, held to 32 bits (fcvtzs), as ConvertToInt converts it.

#pragma once

#include <cstdint>

#include "ntdlib/nx/audio/dsp.h"
#include "ntdlib/wave/adpcm.h"

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

// The low 32 bits of a * gain shifted right by 14, the product in 64 bits. The delay and the reverb take their products
// so.
inline int32_t MulQ14(int64_t a, int64_t gain)
{
    return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(a * gain) >> 14));
}

// A sample on the scale of the delay and the reverb: shifted left by 6, wrapping.
inline int32_t ToEffectScale(int32_t sample)
{
    return static_cast<int32_t>(static_cast<uint32_t>(sample) << 6);
}

// The bytes that a DSP-ADPCM range takes up to sample `sample`, the byte of a last odd nibble included.
inline int64_t AdpcmBytes(int64_t sample)
{
    return ntdlib::wave::DspAdpcmByteCount(sample);
}

} // namespace ntdlib::nx::audio
