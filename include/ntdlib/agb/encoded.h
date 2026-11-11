// SPDX-License-Identifier: MIT

// What the encoders of the GBA drivers' commands give (each driver's EncodeSequence).

#pragma once

#include <cstdint>
#include <vector>

namespace ntdlib::agb
{

// The bytes of a sequence, and each item's offset in them. `offsets` holds one more entry than the sequence has items:
// the end.
struct Encoded
{
    std::vector<uint8_t> bytes;
    std::vector<uint32_t> offsets;
};

} // namespace ntdlib::agb
