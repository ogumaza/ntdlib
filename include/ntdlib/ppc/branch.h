// SPDX-License-Identifier: MIT

// The encoder and the decoder of PowerPC's relative branches, b and bl, as the PowerPC architecture book encodes them:
// opcode 18 in bits 0 to 5, a signed offset in bytes in bits 6 to 29 (a multiple of 4), the absolute-address bit AA
// (bit 30) and the link bit LK (bit 31). Bit 0 is the word's most significant bit.

#pragma once

#include <cstdint>
#include <optional>

namespace ntdlib::ppc
{

// The bits of a word that tell b and bl from the other instructions: the opcode, AA and LK.
inline constexpr uint32_t kBranchBits = 0xfc000003;

// A relative b and a relative bl, with an offset of 0.
inline constexpr uint32_t kB = 0x48000000;
inline constexpr uint32_t kBl = 0x48000001;

// A relative b at `from` to `to`. The two addresses must be within 32 MiB of each other.
constexpr uint32_t Branch(uint32_t from, uint32_t to)
{
    return kB | ((to - from) & 0x03fffffc);
}

// Whether `word` is a relative bl: a call.
constexpr bool IsCall(uint32_t word)
{
    return (word & kBranchBits) == kBl;
}

// The offset of the relative branch `word` in bytes, from the branch's address.
constexpr int32_t BranchOffset(uint32_t word)
{
    return static_cast<int32_t>((word & 0x03fffffc) << 6) >> 6;
}

// The function that the word `word` at `address` calls, or nothing if `word` isn't a relative bl.
constexpr std::optional<uint32_t> CallTarget(uint32_t address, uint32_t word)
{
    if (!IsCall(word))
    {
        return std::nullopt;
    }

    return address + static_cast<uint32_t>(BranchOffset(word));
}

} // namespace ntdlib::ppc
