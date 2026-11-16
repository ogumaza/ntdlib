// SPDX-License-Identifier: MIT

// A cut-down DS cartridge image: the header, a new ARM9 binary, the ARM7 binary, the overlay tables, the FNT, the FAT
// and the files that the caller keeps, at new offsets. Every other file stays in the tables and has no bytes. A
// program that boots the image and reads only the kept files runs as on the whole image.
//
// The layout follows the SDK's tools where it can. The bytes before the ARM9 binary stay as they are: the header and
// the space up to the binary. The ARM9 binary keeps its offset, followed by its footer where the image has one: the
// words 0xDEC00621, the module parameters' offset and 0. The other items follow, each on a multiple of 0x200 bytes:
// the ARM9 overlay table, the ARM7 binary, the ARM7 overlay table, the FNT, the FAT and the kept files in the order of
// their offsets in the image. The bytes between them hold 0xFF. The image ends at the last item's end, the header's
// used size. The banner is left out: the console's menu reads it, and a game's code doesn't. The chip's capacity is
// the smallest that holds the image. The header's CRC-16 is written again.
//
// The header's checksum of the secure area covers the ARM9 binary's first 0x4000 bytes on the card, encrypted. It
// stays as it was, and a new ARM9 binary leaves it stale.

#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "ntdlib/ntr/rom.h"

namespace ntdlib::ntr
{

struct CompactParts
{
    // The ARM9 binary as the image stores it, such as UncompressedArm9 (code.h) with a stub.
    std::vector<uint8_t> arm9;

    // The kept files by ID: each file's bytes as the image holds them, or new ones.
    std::map<uint32_t, std::vector<uint8_t>> files;
};

// The image of `rom` with `parts`. Throws std::invalid_argument for a Rom that Rom::ParsePartial read, for an image for
// the DSi, for an ARM9 binary inside the header and for an ID past the FAT.
std::vector<uint8_t> WriteCompactImage(const Rom& rom, const CompactParts& parts);

} // namespace ntdlib::ntr
