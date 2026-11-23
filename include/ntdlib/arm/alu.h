// SPDX-License-Identifier: MIT

// The integer helpers that the ARM instruction sets of ARMv4T to ARMv6 share, as ARM's architecture reference manual
// gives them: the condition codes, the adder with its carry and overflow, the immediate operand of a data-processing
// instruction, a signed halfword of a register, and saturation to a signed or an unsigned width.

#pragma once

#include <array>
#include <bit>
#include <cstdint>

namespace ntdlib::arm
{
namespace detail
{

// The flag values (NZCV, N in bit 3) under which each condition passes, one bit for each value. Condition 15 never
// passes.
constexpr std::array<uint16_t, 16> ConditionTable()
{
    std::array<uint16_t, 16> table{};
    for (uint32_t cond = 0; cond < 16; ++cond)
    {
        for (uint32_t flags = 0; flags < 16; ++flags)
        {
            const bool n = (flags & 8) != 0;
            const bool z = (flags & 4) != 0;
            const bool c = (flags & 2) != 0;
            const bool v = (flags & 1) != 0;
            const bool passes[16] = {z,       !z,     c,      !c,           n,           !n,   v,    !v, c && !z,
                                     !c || z, n == v, n != v, !z && n == v, z || n != v, true, false};
            if (passes[cond])
            {
                table[cond] = static_cast<uint16_t>(table[cond] | (1u << flags));
            }
        }
    }

    return table;
}

inline constexpr std::array<uint16_t, 16> kConditions = ConditionTable();

} // namespace detail

// Whether condition `cond` (0 to 15, an instruction's bits 31 to 28) passes with the flags `nzcv` (0 to 15: N in bit
// 3, Z in bit 2, C in bit 1 and V in bit 0).
inline bool ConditionPassed(uint32_t cond, uint32_t nzcv)
{
    return ((detail::kConditions[cond] >> nzcv) & 1) != 0;
}

// a + b + carry_in, with the adder's carry and signed overflow (0 or 1). A subtraction a - b adds ~b with a carry in
// of 1.
inline uint32_t AddWithCarry(uint32_t a, uint32_t b, uint32_t carry_in, uint32_t& carry, uint32_t& overflow)
{
    const uint64_t sum = static_cast<uint64_t>(a) + b + carry_in;
    const uint32_t result = static_cast<uint32_t>(sum);
    carry = static_cast<uint32_t>(sum >> 32);
    overflow = ((a ^ result) & (b ^ result)) >> 31;

    return result;
}

// The immediate operand of an ARM data-processing instruction or of MSR, the manual's ARMExpandImm: bits 7 to 0
// rotated right by twice bits 11 to 8. assembler.h's RotatedImmediate is the inverse.
inline uint32_t ExpandImmediate(uint32_t op)
{
    return std::rotr(op & 0xFF, static_cast<int>((op >> 7) & 0x1E));
}

// The top or the bottom halfword of `value`, sign-extended, as the signed halfword multiplies read their operands.
inline int32_t SignedHalf(uint32_t value, bool top)
{
    return static_cast<int16_t>(top ? value >> 16 : value);
}

// `value` held to the range of a signed integer of `bits` bits (1 to 32). `saturated` is set when the value was out of
// range and left alone otherwise.
inline int32_t SignedSaturate(int64_t value, uint32_t bits, bool& saturated)
{
    const int64_t max = (int64_t{1} << (bits - 1)) - 1;
    const int64_t min = -(int64_t{1} << (bits - 1));
    if (value > max)
    {
        saturated = true;
        return static_cast<int32_t>(max);
    }

    if (value < min)
    {
        saturated = true;
        return static_cast<int32_t>(min);
    }

    return static_cast<int32_t>(value);
}

// `value` held to the range of an unsigned integer of `bits` bits (0 to 31). `saturated` is set when the value was out
// of range and left alone otherwise.
inline uint32_t UnsignedSaturate(int64_t value, uint32_t bits, bool& saturated)
{
    const int64_t max = (int64_t{1} << bits) - 1;
    if (value > max)
    {
        saturated = true;
        return static_cast<uint32_t>(max);
    }

    if (value < 0)
    {
        saturated = true;
        return 0;
    }

    return static_cast<uint32_t>(value);
}

} // namespace ntdlib::arm
