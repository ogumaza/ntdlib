// SPDX-License-Identifier: MIT

// Registers that only store what's written to them. They cover the blocks of a GameCube or a Wii that a machine doesn't
// model further.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ntdlib::flipper
{

// `N` bytes of big-endian registers. Bytes past the end aren't stored, and read as 0.
template <std::size_t N>
struct RegisterFile
{
    // The `size`-byte value at `offset`.
    uint32_t Read(uint32_t offset, int size) const
    {
        uint32_t value = 0;
        for (int i = 0; i < size; i++)
        {
            value = value << 8 | (offset + i < N ? bytes[offset + i] : 0);
        }

        return value;
    }

    // Stores the `size`-byte `value` at `offset`.
    void Write(uint32_t offset, int size, uint32_t value)
    {
        for (int i = size - 1; i >= 0; i--, value >>= 8)
        {
            if (offset + i < N)
            {
                bytes[offset + i] = static_cast<uint8_t>(value);
            }
        }
    }

    std::array<uint8_t, N> bytes{};
};

} // namespace ntdlib::flipper
