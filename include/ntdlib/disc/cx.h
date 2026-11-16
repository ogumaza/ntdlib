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

// The formats of the DS's BIOS that DecompressBody takes. GBATEK documents them as types 0x10, 0x24, 0x28 and 0x30.
enum class BodyFormat
{
    kLz77,      // LZ77 (type 0x10)
    kHuffman4,  // Huffman coding of 4-bit symbols (type 0x24)
    kHuffman8,  // Huffman coding of 8-bit symbols (type 0x28)
    kRunLength, // run-length coding (type 0x30)
};

// Decompresses `size` bytes of data in `format` that comes without the 4-byte header, as in packs with a header that
// gives the format and the size. Two 4-bit Huffman symbols make a byte, the first in its low half. Run-length data is a
// flag byte before each run: with its top bit set, the next byte (flag & 0x7F) + 3 times, else the next (flag & 0x7F)
// + 1 bytes. Throws Error if the data is damaged.
std::vector<uint8_t> DecompressBody(std::span<const uint8_t> data, BodyFormat format, std::size_t size);

} // namespace ntdlib::disc
