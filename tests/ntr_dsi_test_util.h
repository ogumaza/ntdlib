// SPDX-License-Identifier: MIT

// Helpers that the tests of DSi mode share (ntr_dsi_test.cpp and ntr_dsi_burst_test.cpp): images that run only on a
// DSi, and reads of the main memory.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/ntr/machine.h"
#include "ntdlib/rounding.h"
#include "ntdlib/testing/nds_files.h"

namespace ntr_dsi_tests
{

using Bytes = std::vector<uint8_t>;

constexpr uint32_t kMainMemory = 0x02000000;
constexpr uint32_t kIo = 0x04000000;

// The DSi binaries: in the image from kArm9iOffset, and in memory where the launcher puts a game's.
constexpr uint32_t kArm9iOffset = 0x8000;
constexpr uint32_t kArm9iAddress = 0x02400000;
constexpr uint32_t kArm7iAddress = 0x02E80000;

// A DSi game's new shared WRAM settings in the header, from 0x180: MBK1 to MBK5, the ARM9's MBK6 to MBK8, the ARM7's
// MBK6 to MBK8. WRAM-A belongs to the ARM7 at 0x037C0000, and WRAM-B and WRAM-C to the ARM9 at 0x03740000 and
// 0x03700000. Both CPUs' windows of WRAM-B and WRAM-C cover the same addresses.
constexpr std::array<uint32_t, 11> kWramSettings = {
    0x8D898581, 0x8C888480, 0x9C989490, 0x8C888480, 0x9C989490, // MBK1 to MBK5
    0x00000000, 0x07C03740, 0x07403700,                         // the ARM9's MBK6 to MBK8
    0x080037C0, 0x07C03740, 0x07403700,                         // the ARM7's MBK6 to MBK8
};

// The header's SCFG_EXT7 bits, as a DSi game's header gives them.
constexpr uint32_t kHeaderExtended = 0x00040406;

// A CPU that halts for good.
inline const std::vector<uint32_t> kHalts = {
    0xEF060000, // 0x0 halt: swi 0x060000: Halt
    0xEAFFFFFD, // 0x4 b halt
};

inline Bytes Words(const std::vector<uint32_t>& words)
{
    Bytes bytes;
    for (const uint32_t word : words)
    {
        xsflib::AppendLe32(bytes, word);
    }

    return bytes;
}

// An image with the binaries, the DSi binaries at kArm9iOffset and after, and the header's DSi fields.
inline Bytes DsiImage(const Bytes& arm9, const Bytes& arm7, const Bytes& arm9i, const Bytes& arm7i,
                      uint8_t unit_code = 3)
{
    ntdlib::testing::nds_files::Image spec;
    spec.unit_code = unit_code;
    spec.arm9 = arm9;
    spec.arm7 = arm7;
    Bytes image = ntdlib::testing::nds_files::BuildImage(spec).first;
    if (image.size() > kArm9iOffset)
    {
        throw std::logic_error("the test image runs into the ARM9i binary");
    }
    image.resize(kArm9iOffset, 0);
    image.insert(image.end(), arm9i.begin(), arm9i.end());
    image.resize(ntdlib::AlignUp(image.size(), 4), 0);
    const auto arm7i_offset = static_cast<uint32_t>(image.size());
    image.insert(image.end(), arm7i.begin(), arm7i.end());

    for (std::size_t i = 0; i < kWramSettings.size(); i++)
    {
        xsflib::StoreLe32(&image[0x180 + 4 * i], kWramSettings[i]);
    }
    image[0x1AC] = 0x0F; // MBK9: WRAM-A's slots writable by the ARM7
    image[0x1AF] = 0x03; // WRAMCNT
    xsflib::StoreLe32(&image[0x1B8], kHeaderExtended);
    xsflib::StoreLe32(&image[0x1C0], kArm9iOffset);
    xsflib::StoreLe32(&image[0x1C8], kArm9iAddress);
    xsflib::StoreLe32(&image[0x1CC], static_cast<uint32_t>(arm9i.size()));
    xsflib::StoreLe32(&image[0x1D0], arm7i_offset);
    xsflib::StoreLe32(&image[0x1D8], kArm7iAddress);
    xsflib::StoreLe32(&image[0x1DC], static_cast<uint32_t>(arm7i.size()));

    return image;
}

inline Bytes Pattern(std::size_t size, uint8_t seed)
{
    Bytes bytes(size);
    for (std::size_t i = 0; i < size; i++)
    {
        bytes[i] = static_cast<uint8_t>(seed + i * 7);
    }

    return bytes;
}

inline std::span<uint8_t> MemoryAt(std::span<uint8_t> memory, uint32_t address, std::size_t size)
{
    return memory.subspan((address - kMainMemory) & (memory.size() - 1), size);
}

inline uint32_t Word(ntdlib::ntr::Machine& machine, uint32_t address)
{
    return xsflib::LoadLe32(MemoryAt(machine.MainMemory(), address, 4).data());
}

} // namespace ntr_dsi_tests
