// SPDX-License-Identifier: MIT

// The decoder of ARM's branches with an offset, B, BL and BLX, as ARM's architecture reference manual for ARMv5TE
// encodes them: a condition, bits 27 to 25 of 0b101, the link bit 24, and a signed offset of 24 bits in words.
// Condition 0xF gives BLX with an offset. BLX switches to Thumb code, and its bit 24 adds a halfword to the offset.

#pragma once

#include <cstdint>

namespace ntdlib::arm
{

// Whether `instruction` is B or BL, under any condition.
constexpr bool IsBranch(uint32_t instruction)
{
    return (instruction & 0x0E000000) == 0x0A000000 && instruction >> 28 != 0xF;
}

// Whether the branch `instruction` is BL rather than B. BL sets lr.
constexpr bool IsLink(uint32_t instruction)
{
    return (instruction & 0x01000000) != 0;
}

// The offset of the branch `instruction` in bytes, from its address plus 8.
constexpr int32_t BranchOffset(uint32_t instruction)
{
    return static_cast<int32_t>(instruction << 8) >> 6;
}

// Where the branch `instruction` at `address` goes.
constexpr uint32_t BranchTarget(uint32_t address, uint32_t instruction)
{
    return address + 8 + static_cast<uint32_t>(BranchOffset(instruction));
}

// Whether `instruction` is BLX with an offset: condition 0xF and bits 27 to 25 of 0b101.
constexpr bool IsBlxImmediate(uint32_t instruction)
{
    return (instruction & 0xFE000000) == 0xFA000000;
}

// The offset of BLX with an offset `instruction` in bytes, from its address plus 8.
constexpr int32_t BlxOffset(uint32_t instruction)
{
    return BranchOffset(instruction) | static_cast<int32_t>((instruction >> 23) & 2);
}

// Where BLX with an offset `instruction` at `address` goes. The target is Thumb code, and bit 0 of the result is clear.
constexpr uint32_t BlxTarget(uint32_t address, uint32_t instruction)
{
    return address + 8 + static_cast<uint32_t>(BlxOffset(instruction));
}

} // namespace ntdlib::arm
