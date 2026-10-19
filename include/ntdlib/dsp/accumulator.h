// SPDX-License-Identifier: MIT

// The DSP's 40-bit accumulators: the wrap of a result to 40 bits, the rounding to the middle word, and the middle word
// as a read in 40-bit mode gives it. The interpreter uses them, and so can a model of a DSP program's arithmetic that
// runs outside the interpreter.

#pragma once

#include <cstdint>

namespace ntdlib::dsp
{

// The range of a signed 32-bit value. 40-bit results set flags and saturate against this range.
inline constexpr int64_t kS32Min = -(int64_t{1} << 31);
inline constexpr int64_t kS32Max = (int64_t{1} << 31) - 1;

// A value wrapped to 40 bits and sign-extended from bit 39.
inline int64_t Wrap40(int64_t value)
{
    return static_cast<int64_t>(static_cast<uint64_t>(value) << 24) >> 24;
}

// A value rounded to the nearest multiple of 0x10000, ties to even. clrl, movpz, addpaxz, mulmvz, mulxmvz and mulcmvz
// round that way. The result isn't wrapped to 40 bits.
inline int64_t Round(int64_t value)
{
    const int64_t biased = (value & 0x10000) ? value + 0x8000 : value + 0x7fff;

    return static_cast<int64_t>(static_cast<uint64_t>(biased) & ~uint64_t{0xffff});
}

// An accumulator's middle word as a read of $ac.m in 40-bit mode gives it: saturated to 16 bits when the accumulator is
// outside 32 bits' range.
inline int16_t MiddleWord(int64_t accumulator)
{
    if (accumulator > kS32Max)
    {
        return 0x7fff;
    }
    if (accumulator < kS32Min)
    {
        return -0x8000;
    }

    return static_cast<int16_t>(accumulator >> 16);
}

} // namespace ntdlib::dsp
