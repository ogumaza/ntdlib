// SPDX-License-Identifier: MIT

// FSYS archives, and LZSS, the compression of their files. Pokémon Colosseum keeps most of its files in FSYS archives.
//
// An archive starts with the magic "FSYS". Its words are big-endian. The word at 0xC gives the number of files, and the
// word at 0x18 gives the offset of the archive's information block. The block's first word gives the offset of a table
// of the entries' offsets. An entry gives the file's ID and type in its first word, the file's offset at +4, its size
// at +8, its flags at +0xC, the size that the archive stores at +0x14 and the offset of its name at +0x24. Flag bit 31
// marks a file stored with LZSS. A name ends with a zero byte. Every offset counts from the archive's start.
//
// LZSS data starts with a header of four big-endian words: the magic "LZSS", the size of the data once decompressed,
// the size of the compressed data with the header, and a word that the reader skips. The compressed stream follows the
// header. Each flag byte of the stream gives eight items, from its lowest bit up: a 1 for a byte stored as it is, and a
// 0 for a copy in two bytes. A copy names a place in a window of the last 4,096 bytes of output, and a length. The
// place's low 8 bits are the first byte. Its high 4 bits are the second byte's high 4 bits. The length is the second
// byte's low 4 bits plus 3. The window starts as zeros, and the first byte of output goes to its place 0xFEE. A copy
// takes one byte at a time. A copy can therefore repeat bytes that it has just written.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/disc/error.h"

namespace ntdlib::disc
{

// A file of an FSYS archive. `id` is the first word of the file's entry. The word holds the file's ID and type.
struct FsysFile
{
    uint32_t id = 0;
    std::string name;
    std::vector<uint8_t> data;
};

// Decompresses LZSS data. Throws Error if the data is damaged or truncated, or if its header gives a size that its
// stream can't hold.
std::vector<uint8_t> DecompressLzss(std::span<const uint8_t> data);

// The files of the FSYS archive `archive`, in the order of its table, decompressed. Throws Error if the archive is
// damaged, or if a file decompresses to a size other than its entry's.
std::vector<FsysFile> ReadFsys(std::span<const uint8_t> archive);

} // namespace ntdlib::disc
