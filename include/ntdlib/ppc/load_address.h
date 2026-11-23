// SPDX-License-Identifier: MIT

// The decoder of the 32-bit address that a game's code loads into a register in two instructions: a lis (addis rD, 0,
// high) and then an ori or an addi of the register to itself. The PowerPC architecture book encodes addis as opcode 15,
// addi as opcode 14 and ori as opcode 24, with rD (rS for ori) in bits 6 to 10, rA in bits 11 to 15 and the immediate
// in bits 16 to 31. Bit 0 is the word's most significant bit. ori adds the low half as it is. addi sign-extends it.

#pragma once

#include <cstdint>
#include <optional>

namespace ntdlib::ppc
{

// The address that `high`, a lis of the register `reg`, and `low`, an ori or addi of `reg` to itself, load into `reg`.
// Nothing if the words aren't those instructions. An addi with rA = 0 adds 0 in place of r0. For r0, only an ori
// counts. The two words don't have to be next to each other in the code.
constexpr std::optional<uint32_t> LoadedAddress(uint32_t high, uint32_t low, uint32_t reg)
{
    constexpr uint32_t kAddi = 14;
    constexpr uint32_t kAddis = 15;
    constexpr uint32_t kOri = 24;
    const uint32_t low_opcode = low >> 26;
    const bool lis = high >> 26 == kAddis && (high >> 21 & 31) == reg && (high >> 16 & 31) == 0;
    const bool to_itself = (low >> 21 & 31) == reg && (low >> 16 & 31) == reg;
    if (!lis || !to_itself)
    {
        return std::nullopt;
    }

    const uint32_t base = high << 16;
    if (low_opcode == kOri)
    {
        return base | (low & 0xffff);
    }

    if (low_opcode == kAddi && reg != 0)
    {
        return base + static_cast<uint32_t>(static_cast<int16_t>(low & 0xffff));
    }

    return std::nullopt;
}

} // namespace ntdlib::ppc
