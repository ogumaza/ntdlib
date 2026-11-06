// SPDX-License-Identifier: MIT

// The fixed-point steps that the renderer's reverb and the I3DL2 reverb share, each worked out the way Nintendo's code
// works it out: counts of samples, powers of ten, sines, quotients and square roots in Q14. A table holds Q30 values,
// and a step between two entries is interpolated by a Q14 fraction. A quotient takes as many bits as its operands give
// it, and rounds its last bit half up.

#pragma once

#include <cstdint>

#include "nx/audio/arithmetic.h"
#include "nx/audio/tables.h"

namespace ntdlib::nx::audio
{

// The least exponent of the powers of ten, in Q14 (a little above -5.3), and the factor that turns an exponent in Q14
// into a position in the table, from bit 28 of the product.
inline constexpr int32_t kPowerFloor = -86834;
inline constexpr int64_t kPowerScale = -791378;

// A quarter turn in the sine table's Q14 angles. The sine of an angle a quarter turn on is the angle's cosine.
inline constexpr int32_t kQuarterTurn = 64 << 14;

// A time in ms in Q14 as a count of samples at `k` kHz in Q14, rounded half up: the Q28 product taken to Q14 in 32
// bits, then to an integer, plus its half bit.
inline int32_t Samples(int64_t time, int64_t k)
{
    const auto product = static_cast<uint64_t>(time * k);
    const auto q14 = static_cast<int32_t>(static_cast<uint32_t>(product >> 14));

    return (q14 >> 14) + static_cast<int32_t>((product >> 27) & 1);
}

// Entry `index` of a Q30 table, interpolated towards the next by a Q14 fraction, in Q14.
inline int32_t Interpolate(const int32_t* table, uint32_t index, int32_t fraction)
{
    const int32_t base = table[index];
    const int32_t step = static_cast<int32_t>(static_cast<uint32_t>(table[index + 1]) - static_cast<uint32_t>(base));
    const uint32_t low = static_cast<uint32_t>(step & 0x3FFF) * static_cast<uint32_t>(fraction);
    const int32_t sum = WrapAdd(WrapAdd(WrapMul(step >> 14, fraction), base), static_cast<int32_t>(low >> 14));

    return sum >> 16;
}

// 10 to the power of an exponent in Q14, in Q14: 1 from 0 up, 0 below the table's end.
inline int32_t PowerQ14(int32_t exponent)
{
    if (exponent >= 0)
    {
        return 0x4000;
    }
    if (exponent < kPowerFloor)
    {
        return 0;
    }

    const auto position = static_cast<uint64_t>(static_cast<int64_t>(exponent) * kPowerScale);
    const auto index = static_cast<uint32_t>(position >> 28) & 0xFF;
    const auto fraction = static_cast<int32_t>((position >> 14) & 0x3FFF);

    return Interpolate(tables::kI3dl2Power, index, fraction);
}

// The sine of an angle in 256ths of a turn, in Q14, as a Q14 value.
inline int32_t Sine(int32_t angle)
{
    const auto index = static_cast<uint32_t>(angle >> 14) & 0xFF;

    return Interpolate(tables::kI3dl2Sine, index, angle & 0x3FFF);
}

// The bits of `x` shifted up until bit 30 is set, with the count of shifts and the count of quotient bits a division by
// it starts from. A value with bit 30 set already takes none.
struct Normalized
{
    uint32_t value;
    uint32_t total;
};

inline Normalized NormalizeDivisor(uint32_t divisor)
{
    if ((divisor & 0x40000000u) != 0)
    {
        return {divisor, 15};
    }

    uint32_t count = 15;
    uint32_t total = 16;
    for (;;)
    {
        const uint32_t before = divisor;
        divisor <<= 1;
        total = count + 1;
        if ((before & 0x20000000u) != 0)
        {
            break;
        }

        const bool more = count - 14 < 31;
        count = total;
        if (!more)
        {
            break;
        }
    }

    return {divisor, total};
}

// A quotient in Q14: `dividend`, the magnitude of `v`, over `divisor`, with as many bits as the two take, rounded half
// up, signed as divisor ^ v. The dividend has bit 30 clear, and is shifted up to it first.
inline int32_t DivideShifted(uint32_t dividend, int32_t divisor, int32_t v)
{
    if (divisor == 0)
    {
        return 0;
    }

    uint32_t remainder = dividend;
    uint32_t shifts = 0;
    for (;;)
    {
        const uint32_t before = remainder;
        remainder <<= 1;
        ++shifts;
        if ((before & 0x20000000u) != 0 || shifts >= 31)
        {
            break;
        }
    }

    const uint32_t magnitude = divisor < 0 ? 0u - static_cast<uint32_t>(divisor) : static_cast<uint32_t>(divisor);
    const Normalized normalized = NormalizeDivisor(magnitude);
    if (normalized.total <= shifts - 1)
    {
        return 0;
    }

    uint32_t bits = normalized.total - shifts;
    uint32_t quotient = 0;
    if (remainder != 0 && normalized.total != shifts)
    {
        for (;;)
        {
            const bool fits = remainder >= normalized.value;
            --bits;
            quotient = (quotient << 1) | (fits ? 1u : 0u);
            remainder = (remainder - (fits ? normalized.value : 0u)) << 1;
            if (bits == 0 || remainder == 0)
            {
                break;
            }
        }
    }
    quotient += remainder >= normalized.value ? 1u : 0u;
    quotient <<= bits & 31;

    return (divisor ^ v) < 0 ? static_cast<int32_t>(0u - quotient) : static_cast<int32_t>(quotient);
}

// The same for a dividend with bit 30 set. That dividend takes no shift.
inline int32_t DivideWide(uint32_t dividend, int32_t divisor, int32_t v)
{
    if (divisor == 0)
    {
        return 0;
    }

    const uint32_t magnitude = divisor < 0 ? 0u - static_cast<uint32_t>(divisor) : static_cast<uint32_t>(divisor);
    const Normalized normalized = NormalizeDivisor(magnitude);
    uint32_t bits = normalized.total;
    uint32_t previous = 0;
    uint32_t quotient = 0;
    uint32_t remainder = dividend;
    for (;;)
    {
        const bool fits = remainder >= normalized.value;
        --bits;
        remainder -= fits ? normalized.value : 0u;
        quotient = (previous << 1) | (fits ? 1u : 0u);
        remainder <<= 1;
        if (bits == 0)
        {
            break;
        }

        previous = quotient;
        if (remainder == 0)
        {
            break;
        }
    }
    quotient += remainder >= normalized.value ? 1u : 0u;
    quotient <<= bits & 31;

    return (divisor ^ v) < 0 ? static_cast<int32_t>(0u - quotient) : static_cast<int32_t>(quotient);
}

// The Q14 quotient of two Q14 values, rounded half up in magnitude. A zero dividend or divisor gives 0, and so does a
// quotient too small for the bits.
inline int32_t Divide(int32_t dividend, int32_t divisor)
{
    if (dividend == 0)
    {
        return 0;
    }

    const uint32_t magnitude = dividend < 0 ? 0u - static_cast<uint32_t>(dividend) : static_cast<uint32_t>(dividend);
    if ((magnitude & 0x40000000u) != 0)
    {
        return DivideWide(magnitude, divisor, dividend);
    }

    return DivideShifted(magnitude, divisor, dividend);
}

// The square root of a Q14 value: Newton's method from a guess by the value's size, for at most seven steps.
inline int32_t SquareRoot(int32_t v)
{
    // The first guess, and the dividend. A value of 0x40 or less, negative ones included, divides its magnitude.
    auto guess = static_cast<int32_t>(static_cast<uint32_t>(v));
    uint32_t dividend = static_cast<uint32_t>(v);
    if (v > 0x40003FFF)
    {
        guess = static_cast<int32_t>(static_cast<uint32_t>(v) >> 9);
    }
    else if (v > 0x10003FFF)
    {
        guess = static_cast<int32_t>(static_cast<uint32_t>(v) >> 8);
    }
    else if (v > 0x04003FFF)
    {
        guess = static_cast<int32_t>(static_cast<uint32_t>(v) >> 7);
    }
    else if (v > 0x01003FFF)
    {
        guess = static_cast<int32_t>(static_cast<uint32_t>(v) >> 6);
    }
    else if (v > 0x00403FFF)
    {
        guess = static_cast<int32_t>(static_cast<uint32_t>(v) >> 5);
    }
    else if (v > 0x00103FFF)
    {
        guess = static_cast<int32_t>(static_cast<uint32_t>(v) >> 4);
    }
    else if (v > 0x00043FFF)
    {
        guess = static_cast<int32_t>(static_cast<uint32_t>(v) >> 3);
    }
    else if (v > 0x00013FFF)
    {
        guess = static_cast<int32_t>(static_cast<uint32_t>(v) >> 2);
    }
    else if (v >= 0x8000)
    {
        guess = static_cast<int32_t>(static_cast<uint32_t>(v) >> 1);
    }
    else if (v > 0x2000)
    {
        guess = v;
    }
    else if (v > 0x1000)
    {
        guess = static_cast<int32_t>(static_cast<uint32_t>(v) << 1);
    }
    else if (v > 0x400)
    {
        guess = static_cast<int32_t>(static_cast<uint32_t>(v) << 2);
    }
    else if (v > 0x100)
    {
        guess = static_cast<int32_t>(static_cast<uint32_t>(v) << 3);
    }
    else if (v > 0x40)
    {
        guess = static_cast<int32_t>(static_cast<uint32_t>(v) << 4);
    }
    else
    {
        if (v == 0)
        {
            return 0;
        }

        guess = static_cast<int32_t>(static_cast<uint32_t>(v) << (v <= 0x10 ? 6 : 5));
        dividend = v < 0 ? 0u - static_cast<uint32_t>(v) : static_cast<uint32_t>(v);
    }

    // Newton's steps, each (guess + v / guess) / 2, until one changes nothing.
    const bool wide = (dividend & 0x40000000u) != 0;
    for (int step = 1;; ++step)
    {
        const int32_t quotient = wide ? DivideWide(dividend, guess, v) : DivideShifted(dividend, guess, v);
        const int32_t next = WrapAdd(quotient, guess) >> 1;
        if (step > 6 || next == guess)
        {
            return next;
        }

        guess = next;
    }
}

} // namespace ntdlib::nx::audio
