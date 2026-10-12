// SPDX-License-Identifier: MIT

// A builder of DOL files for tests: a main.dol that holds AX's microcode, for the detection of the Wii's sound builds
// (ntdlib/sound_build.h).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ntdlib/sound_build.h"
#include "xsflib/bytes.h"

namespace ntdlib::testing
{

// A DOL with AX's microcode in its first data section, at 0x80004000: two NOPs and seven jumps to 0x0Fxx, then a DMA of
// the PBs by `pb_size` bytes. `banner` follows the section.
inline std::vector<uint8_t> MicrocodeDol(uint16_t pb_size, const std::string& banner)
{
    constexpr std::size_t kCode = 0x100;
    std::vector<uint8_t> dol(kCode + kMicrocodeWords * 2);
    xsflib::StoreBe32(&dol[4 * 7], static_cast<uint32_t>(kCode));
    xsflib::StoreBe32(&dol[0x48 + 4 * 7], 0x80004000);
    xsflib::StoreBe32(&dol[0x90 + 4 * 7], static_cast<uint32_t>(kMicrocodeWords * 2));
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
