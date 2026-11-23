// SPDX-License-Identifier: MIT

// The DS's main memory: 4 MiB from 0x02000000. The memory repeats up to 0x02FFFFFF. A DSi in DSi mode has 16 MiB. A
// copy of main memory, such as Machine::MainMemory, is a span whose first byte is the byte at kMainMemory.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ntdlib::ntr
{

// The address of main memory's first byte, and the memory's size.
inline constexpr uint32_t kMainMemory = 0x02000000;
inline constexpr uint32_t kMainMemorySize = 0x400000;
inline constexpr uint32_t kDsiMainMemorySize = 0x1000000;

// The end of the area of the address space in which main memory repeats.
inline constexpr uint32_t kMainMemoryAreaEnd = 0x03000000;

// Whether `memory`, a copy of main memory, has the byte at `address`.
inline bool InMainMemory(std::span<const uint8_t> memory, uint32_t address)
{
    return address >= kMainMemory && address - kMainMemory < memory.size();
}

// Whether `memory`, a copy of main memory, has the byte at `address` and the `bytes` bytes from it.
inline bool InMainMemory(std::span<const uint8_t> memory, uint32_t address, std::size_t bytes)
{
    return InMainMemory(memory, address) && memory.size() - (address - kMainMemory) >= bytes;
}

// Whether `memory`, a copy of main memory, contains `data` at `address`.
inline bool MainMemoryContains(std::span<const uint8_t> memory, uint32_t address, std::span<const uint8_t> data)
{
    if (!InMainMemory(memory, address, data.size()))
    {
        return false;
    }

    return std::ranges::equal(memory.subspan(address - kMainMemory, data.size()), data);
}

} // namespace ntdlib::ntr
