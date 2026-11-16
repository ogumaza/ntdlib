// SPDX-License-Identifier: MIT

// The compression formats of the SDK's CX library that the Wii's titles use: LZ77 (type 0x10), LZ77 with longer copies
// (type 0x11), Huffman coding of bytes (type 0x28), LZ77 with Huffman coding of its symbols (LH, type 0x40) and LZ77
// with range coding of its symbols (LRC, type 0x50). GBATEK documents types 0x10 and 0x28. The GBA's and the DS's BIOS
// decompress them too. LH and LRC follow the SDK's decoders in Pokémon Rumble's main program (WiiWare, title
// 00010001-57505345, content 1): CXSecureUncompressLH at 0x802e3000 and CXSecureUncompressLRC at 0x802e3620. Data in
// these formats starts with a 4-byte header: the type, then the size of the decompressed data in 24 bits,
// little-endian. A size of 0 means that a 32-bit size follows. LZ77 data may have the magic "LZ77" before its header.
// The game's LH and LRC decoders read only the type's top 4 bits. The functions here take only the types above.

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
// than `max_size` bytes. LH or LRC data with more than 0x20 bytes after its last symbol counts as damaged.
std::vector<uint8_t> DecompressCx(std::span<const uint8_t> data, std::size_t max_size);

} // namespace ntdlib::disc
