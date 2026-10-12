// SPDX-License-Identifier: MIT

// The references in NintendoWare's sound files for the 3DS, the Wii U and the Switch.

#pragma once

#include <cstddef>
#include <cstdint>

#include "xsflib/binary.h"

namespace ntdlib::sar
{

// A NintendoWare reference: 16-bit type ID, 2 padding bytes and a signed 32-bit offset. The offset's base depends on
// the containing structure.
struct Reference
{
    uint16_t type = 0;
    int32_t offset = -1;
};

inline Reference ReadRef(const xsflib::Reader& r, std::size_t off)
{
    return Reference{r.U16(off), r.S32(off + 4)};
}

} // namespace ntdlib::sar
