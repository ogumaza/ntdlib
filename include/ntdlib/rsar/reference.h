// SPDX-License-Identifier: MIT

// NW4R's data references. The NintendoWare for Revolution sound formats use them to point from one structure to
// another.

#pragma once

#include <cstddef>
#include <cstdint>

#include "xsflib/binary.h"

namespace ntdlib::rsar
{

// An NW4R data reference: whether it's an offset (1) or an address, the type of the data it points to, and the offset.
// Only data loaded into memory has addresses. The type matters for references that can point to several types. The
// structure holding the reference defines the offset's base. A reference to nothing is all zeros.
struct Reference
{
    bool IsOffset() const
    {
        return ref_type == 1;
    }

    uint8_t ref_type = 0;
    uint8_t data_type = 0;
    uint32_t offset = 0;
};

inline Reference ReadRef(const xsflib::Reader& r, std::size_t off)
{
    return Reference{r.U8(off), r.U8(off + 1), r.U32(off + 4)};
}

} // namespace ntdlib::rsar
