// SPDX-License-Identifier: MIT

// The ARM barrel shifter, as ARM's architecture reference manual gives it for ARMv4T to ARMv6: LSL, LSR, ASR and ROR,
// with an immediate amount or an amount from a register. `type` is the instruction's shift field: 0 for LSL, 1 for
// LSR, 2 for ASR and 3 for ROR. `carry` holds the C flag (0 or 1) and gets the shifter's carry out.

#pragma once

#include <bit>
#include <cstdint>

namespace ntdlib::arm
{

// The barrel shifter with an immediate amount (0 to 31). An ARM register operand or a Thumb shift encodes the amount.
// An amount of 0 means LSL #0 (no shift), LSR #32, ASR #32 or RRX.
inline uint32_t ShiftByImmediate(uint32_t value, uint32_t type, uint32_t amount, uint32_t& carry)
{
    switch (type)
    {
    case 0:
        if (amount == 0)
        {
            return value;
        }

        carry = (value >> (32 - amount)) & 1;
        return value << amount;

    case 1:
        if (amount == 0)
        {
            carry = value >> 31;
            return 0;
        }

        carry = (value >> (amount - 1)) & 1;
        return value >> amount;

    case 2:
        if (amount == 0)
        {
            carry = value >> 31;
            return static_cast<uint32_t>(static_cast<int32_t>(value) >> 31);
        }

        carry = (value >> (amount - 1)) & 1;
        return static_cast<uint32_t>(static_cast<int32_t>(value) >> amount);

    default:
        if (amount == 0)
        {
            const uint32_t result = (carry << 31) | (value >> 1);
            carry = value & 1;
            return result;
        }

        carry = (value >> (amount - 1)) & 1;
        return std::rotr(value, static_cast<int>(amount));
    }
}

// The barrel shifter with an amount from a register's low byte (0 to 255). An amount of 0 leaves the value and the
// carry as they are.
inline uint32_t ShiftByRegister(uint32_t value, uint32_t type, uint32_t amount, uint32_t& carry)
{
    if (amount == 0)
    {
        return value;
    }

    switch (type)
    {
    case 0:
        if (amount < 32)
        {
            carry = (value >> (32 - amount)) & 1;
            return value << amount;
        }

        carry = amount == 32 ? value & 1 : 0;
        return 0;

    case 1:
        if (amount < 32)
        {
            carry = (value >> (amount - 1)) & 1;
            return value >> amount;
        }

        carry = amount == 32 ? value >> 31 : 0;
        return 0;

    case 2:
        if (amount < 32)
        {
            carry = (value >> (amount - 1)) & 1;
            return static_cast<uint32_t>(static_cast<int32_t>(value) >> amount);
        }

        carry = value >> 31;
        return static_cast<uint32_t>(static_cast<int32_t>(value) >> 31);

    default:
        amount &= 31;
        if (amount == 0)
        {
            carry = value >> 31;
            return value;
        }

        carry = (value >> (amount - 1)) & 1;
        return std::rotr(value, static_cast<int>(amount));
    }
}

} // namespace ntdlib::arm
