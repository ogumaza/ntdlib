// SPDX-License-Identifier: MIT

// Builders of DOL files for tests: a section in a DOL's header, and a main.dol that holds AX's microcode, for the
// detection of the Wii's sound builds (ntdlib/sound_build.h).

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/sound_build.h"

namespace ntdlib::testing
{

// Sets section `index` in the header of `dol`: its file offset, its load address and its size. Sections 0 to 6 hold
// text, and 7 to 17 data. The header holds three tables with a word for each section: the offsets at 0x00, the
// addresses at 0x48 and the sizes at 0x90. `dol` must hold the whole header (0x100 bytes).
inline void SetDolSection(std::span<uint8_t> dol, int index, uint32_t offset, uint32_t address, uint32_t size)
{
    const std::size_t at = static_cast<std::size_t>(index) * 4;
    xsflib::StoreBe32(&dol[at], offset);
    xsflib::StoreBe32(&dol[0x48 + at], address);
    xsflib::StoreBe32(&dol[0x90 + at], size);
}

// A DOL with AX's microcode in its first data section, at 0x80004000: two NOPs and seven jumps to 0x0Fxx, then a DMA of
// the PBs by `pb_size` bytes. `banner` follows the section.
inline std::vector<uint8_t> MicrocodeDol(uint16_t pb_size, const std::string& banner)
{
    constexpr std::size_t kCode = 0x100;
    std::vector<uint8_t> dol(kCode + kMicrocodeWords * 2);
    SetDolSection(dol, 7, static_cast<uint32_t>(kCode), 0x80004000, static_cast<uint32_t>(kMicrocodeWords * 2));
    for (std::size_t i = 0; i < 7; i++)
    {
        xsflib::StoreBe16(&dol[kCode + 4 + 4 * i], 0x029f);
        xsflib::StoreBe16(&dol[kCode + 6 + 4 * i], static_cast<uint16_t>(0x0f00 + 0x10 * i));
    }

    xsflib::StoreBe16(&dol[kCode + 0x100], 0x16cb);
    xsflib::StoreBe16(&dol[kCode + 0x102], pb_size);
    dol.insert(dol.end(), banner.begin(), banner.end());

    return dol;
}

} // namespace ntdlib::testing
