// SPDX-License-Identifier: MIT

// Tests of the DS's main memory (ntdlib/ntr/main_memory.h): its place in the ARM9's address map, and the range checks
// on a copy of 16 bytes.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/ntr/main_memory.h"

namespace
{

using ntdlib::ntr::InMainMemory;
using ntdlib::ntr::kMainMemory;
using ntdlib::ntr::kMainMemoryAreaEnd;
using ntdlib::ntr::kMainMemorySize;
using ntdlib::ntr::MainMemoryContains;

// 32 bytes from 0x10 up. The copy of main memory is the first 16: a check that reads past them sees the next bytes.
std::vector<uint8_t> Memory()
{
    std::vector<uint8_t> memory(32);
    for (std::size_t i = 0; i < memory.size(); i++)
    {
        memory[i] = static_cast<uint8_t>(0x10 + i);
    }

    return memory;
}

// 4 MiB from 0x02000000, repeated up to 0x02FFFFFF.
void TestMap()
{
    XSFLIB_CHECK(kMainMemory == 0x02000000);
    XSFLIB_CHECK(kMainMemorySize == 0x400000);
    XSFLIB_CHECK(kMainMemoryAreaEnd == 0x03000000);
}

void TestRange()
{
    const std::vector<uint8_t> buffer = Memory();
    const std::span<const uint8_t> memory = std::span(buffer).first(16);

    XSFLIB_CHECK(!InMainMemory(memory, kMainMemory - 1));
    XSFLIB_CHECK(InMainMemory(memory, kMainMemory));
    XSFLIB_CHECK(InMainMemory(memory, kMainMemory + 15));
    XSFLIB_CHECK(!InMainMemory(memory, kMainMemory + 16));
    XSFLIB_CHECK(InMainMemory(memory, kMainMemory + 12, 4));
    XSFLIB_CHECK(!InMainMemory(memory, kMainMemory + 13, 4));
    XSFLIB_CHECK(InMainMemory(memory, kMainMemory + 15, 0));
    XSFLIB_CHECK(!InMainMemory(memory, kMainMemory + 16, 0));
    XSFLIB_CHECK(!InMainMemory(memory, kMainMemory - 1, 2));
}

void TestContains()
{
    const std::vector<uint8_t> buffer = Memory();
    const std::span<const uint8_t> memory = std::span(buffer).first(16);
    constexpr std::array<uint8_t, 3> kAtTwo = {0x12, 0x13, 0x14};
    constexpr std::array<uint8_t, 2> kAtFourteen = {0x1E, 0x1F};
    constexpr std::array<uint8_t, 3> kPastTheEnd = {0x1E, 0x1F, 0x20};

    XSFLIB_CHECK(MainMemoryContains(memory, kMainMemory + 2, kAtTwo));
    XSFLIB_CHECK(!MainMemoryContains(memory, kMainMemory + 3, kAtTwo));
    XSFLIB_CHECK(MainMemoryContains(memory, kMainMemory + 14, kAtFourteen));
    XSFLIB_CHECK(!MainMemoryContains(memory, kMainMemory + 14, kPastTheEnd));
    XSFLIB_CHECK(!MainMemoryContains(memory, kMainMemory - 1, std::span(kAtTwo).first(0)));
    XSFLIB_CHECK(!MainMemoryContains(memory, kMainMemory + 16, std::span(kAtTwo).first(0)));
}

} // namespace

int main()
{
    TestMap();
    TestRange();
    TestContains();

    return xsflib::testing::Summary("DS main memory");
}
