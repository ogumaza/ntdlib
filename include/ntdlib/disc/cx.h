// SPDX-License-Identifier: MIT

// The compression formats of the SDK's CX library that the Wii's titles use: LZ77 (type 0x10), LZ77 with longer copies
// (type 0x11) and Huffman coding of bytes (type 0x28). GBATEK documents types 0x10 and 0x28. The GBA's and the DS's
// BIOS decompress them too. Data in these formats starts with a 4-byte header: the type, then the size of the
// decompressed data in 24 bits, little-endian. A size of 0 means that a 32-bit size follows. LZ77 data may have the
// magic "LZ77" before its header.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/disc/error.h"

namespace ntdlib::disc
{

// True if `data` starts with the header of one of the formats above.
bool IsCx(std::span<const uint8_t> data);

// Decompresses data in one of the formats above. Throws Error if the data is damaged, or if it would decompress to more
// than `max_size` bytes.
std::vector<uint8_t> DecompressCx(std::span<const uint8_t> data, std::size_t max_size);

} // namespace ntdlib::disc
