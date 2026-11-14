// SPDX-License-Identifier: MIT

// The text of a float for a tag or a message: the shortest decimal that reads back as the same float.

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ntdlib
{

namespace detail
{

// An unsigned integer of up to 224 bits. A float's text has at most 45 digits after the point, and the bounds of a
// float's interval times 10^45 take less than 180 bits.
class WideInteger
{
public:
    explicit WideInteger(uint64_t value)
    {
        limbs_[0] = static_cast<uint32_t>(value);
        limbs_[1] = static_cast<uint32_t>(value >> 32);
    }

    void Multiply(uint32_t factor)
    {
        uint64_t carry = 0;
        for (uint32_t& limb : limbs_)
        {
            const uint64_t product = uint64_t{limb} * factor + carry;
            limb = static_cast<uint32_t>(product);
            carry = product >> 32;
        }
    }

    // Shifts the integer `bits` places up. A float's mantissa takes 24 bits, and its exponent shifts it up to 104.
    void ShiftLeft(int bits)
    {
        const int whole = bits / 32;
        const int part = bits % 32;
        for (int i = kLimbs - 1; i >= 0; --i)
        {
            const uint64_t pair = (uint64_t{Limb(i - whole)} << 32) | Limb(i - whole - 1);
            limbs_[i] = static_cast<uint32_t>(pair >> (32 - part));
        }
    }

    // Divides the integer by `divisor` and returns the remainder.
    uint32_t Divide(uint32_t divisor)
    {
        uint64_t remainder = 0;
        for (int i = kLimbs - 1; i >= 0; --i)
        {
            const uint64_t part = (remainder << 32) | limbs_[i];
            limbs_[i] = static_cast<uint32_t>(part / divisor);
            remainder = part % divisor;
        }

        return static_cast<uint32_t>(remainder);
    }

    bool IsZero() const
    {
        for (uint32_t limb : limbs_)
        {
            if (limb != 0)
            {
                return false;
            }
        }

        return true;
    }

    // The integer divided by 2^shift, rounded down. The quotient has to fit in 64 bits.
    uint64_t ShiftedDown(int shift) const
    {
        const int whole = shift / 32;
        const int part = shift % 32;
        const uint64_t low = Limb(whole) | (uint64_t{Limb(whole + 1)} << 32);
        if (part == 0)
        {
            return low;
        }

        return (low >> part) | (uint64_t{Limb(whole + 2)} << (64 - part));
    }

    // Whether the bits below bit `shift` are all 0: whether 2^shift divides the integer.
    bool LowBitsAreZero(int shift) const
    {
        const int whole = shift / 32;
        for (int i = 0; i < whole; ++i)
        {
            if (limbs_[i] != 0)
            {
                return false;
            }
        }

        const uint32_t mask = (uint32_t{1} << (shift % 32)) - 1;

        return (Limb(whole) & mask) == 0;
    }

    bool Bit(int index) const
    {
        return ((Limb(index / 32) >> (index % 32)) & 1) != 0;
    }

private:
    static constexpr int kLimbs = 7;

    // Limb `index`, or 0 outside the integer.
    uint32_t Limb(int index) const
    {
        return index >= 0 && index < kLimbs ? limbs_[index] : 0;
    }

    std::array<uint32_t, kLimbs> limbs_{}; // The lowest 32 bits first.
};

// The decimal digits of `number`.
inline std::string DecimalDigits(WideInteger number)
{
    if (number.IsZero())
    {
        return "0";
    }

    // Nine digits at a time, from the lowest.
    std::string digits;
    while (!number.IsZero())
    {
        uint32_t group = number.Divide(1'000'000'000);
        for (int i = 0; i < 9; ++i)
        {
            digits.insert(digits.begin(), static_cast<char>('0' + group % 10));
            group /= 10;
        }
    }

    return digits.substr(digits.find_first_not_of('0'));
}

// `number` divided by 10^places, with `places` digits after the point. A zero comes before the point when no other
// digit does.
inline std::string WithPoint(uint64_t number, int places)
{
    std::string text = std::to_string(number);
    if (places == 0)
    {
        return text;
    }

    const auto fraction_size = static_cast<std::size_t>(places);
    if (text.size() <= fraction_size)
    {
        text.insert(0, fraction_size + 1 - text.size(), '0');
    }
    text.insert(text.size() - fraction_size, 1, '.');

    return text;
}

// The shortest decimal in the interval of numbers that read back as the float mantissa * 2^exponent, for a float
// that isn't a whole number (exponent < 0). Of the decimals with the fewest digits after the point, it's the one
// nearest the float, and the even one of two that are equally near.
inline std::string ShortestFraction(uint32_t mantissa, int exponent, bool power_of_two)
{
    // Scaled by 2^scale, the float is 4 * mantissa, and its interval runs halfway to the next float on each side.
    // Below a power of two, the next float is half as far away. A decimal on a bound reads back as the float when the
    // mantissa is even (ties go to even).
    const int scale = 2 - exponent;
    WideInteger low(4 * uint64_t{mantissa} - (power_of_two ? 1 : 2));
    WideInteger value(4 * uint64_t{mantissa});
    WideInteger high(4 * uint64_t{mantissa} + 2);
    const bool bounds_read_back = mantissa % 2 == 0;

    for (int places = 0;; ++places)
    {
        // The decimals with `places` digits after the point that read back, times 10^places: first to last.
        const uint64_t first = low.ShiftedDown(scale) + (low.LowBitsAreZero(scale) && bounds_read_back ? 0 : 1);
        const uint64_t last = high.ShiftedDown(scale) - (high.LowBitsAreZero(scale) && !bounds_read_back ? 1 : 0);
        if (first <= last)
        {
            // The nearest such decimal to the float, rounding half to even.
            uint64_t nearest = value.ShiftedDown(scale);
            const bool above_half = value.Bit(scale - 1) && !value.LowBitsAreZero(scale - 1);
            const bool half = value.Bit(scale - 1) && value.LowBitsAreZero(scale - 1);
            if (above_half || (half && nearest % 2 == 1))
            {
                ++nearest;
            }

            return WithPoint(std::clamp(nearest, first, last), places);
        }

        low.Multiply(10);
        value.Multiply(10);
        high.Multiply(10);
    }
}

} // namespace detail

// The shortest decimal that reads back as the same float, in every locale: 1.3f gives "1.3". Of the shortest, it's the
// one nearest the float. A whole number gives all its digits. The text has no exponent. Infinities give "inf", a
// signalling NaN "nan(snan)" and a quiet NaN "nan", each with a minus sign when the sign bit is set. The default NaN
// of the x86 instructions (only the sign and the quiet bit set) gives "-nan(ind)". The text is std::to_chars's with
// std::chars_format::fixed, which macOS has only from 13.3 on. It's 48 characters at most, and never empty.
inline std::string ShortestDecimal(float value)
{
    const auto bits = std::bit_cast<uint32_t>(value);
    const bool negative = (bits >> 31) != 0;
    const uint32_t biased_exponent = (bits >> 23) & 0xFF;
    const uint32_t fraction = bits & 0x7FFFFF;
    const std::string sign = negative ? "-" : "";

    if (biased_exponent == 0xFF)
    {
        if (fraction == 0)
        {
            return sign + "inf";
        }
        if ((fraction & 0x400000) == 0)
        {
            return sign + "nan(snan)";
        }

        return negative && fraction == 0x400000 ? "-nan(ind)" : sign + "nan";
    }

    // The float is mantissa * 2^exponent.
    const uint32_t mantissa = biased_exponent == 0 ? fraction : fraction | 0x800000;
    const int exponent = (biased_exponent == 0 ? 1 : static_cast<int>(biased_exponent)) - 150;

    if (mantissa == 0)
    {
        return sign + "0";
    }
    if (exponent >= 0)
    {
        detail::WideInteger whole(mantissa);
        whole.ShiftLeft(exponent);

        return sign + detail::DecimalDigits(whole);
    }
    if (std::countr_zero(mantissa) >= -exponent)
    {
        return sign + std::to_string(mantissa >> -exponent);
    }

    const bool power_of_two = fraction == 0 && biased_exponent > 1;

    return sign + detail::ShortestFraction(mantissa, exponent, power_of_two);
}

} // namespace ntdlib
