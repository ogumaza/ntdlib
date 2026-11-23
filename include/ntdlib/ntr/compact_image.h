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
//
// An image that runs only on a DSi (RunsInDsiMode) keeps its layout: a game checks each sector that it reads from the
// card against the image's hash tables. No file can therefore move. The image stays whole, with the new ARM9 binary at
// the old one's offset, followed by the old binary's bytes past the new one's end. Every file that the caller doesn't
// keep contains zeros, but for its bytes in a sector of the digest that a kept file reaches: the game checks the whole
// sector when it reads the kept file. The tables, the DSi binaries, the banner and the header stay as they are. A kept
// file's new bytes replace its old ones. A game that reads them fails its check, unless they equal the image's.

#pragma once

#include <cstdint>
#include <map>
#include <utility>
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

// The sectors that the digest of a DSi image hashes: `size` bytes each, from `start` on. The header's words at 0x1E0
// and 0x200 give them (GBATEK's "DSi Cartridge Header"). A size of 0 stands for an image without a digest.
struct DigestSectors
{
    uint32_t start = 0;
    uint32_t size = 0;

    // The range [first, end) widened to whole sectors. An empty range, a range that starts before `start` and any
    // range of an image without a digest stay as they are.
    std::pair<uint32_t, uint32_t> Around(uint32_t first, uint32_t end) const;
};

// The digest's sectors of `rom`. A size of 0 for an image for the DS alone.
DigestSectors DigestSectorsOf(const Rom& rom);

// The image of `rom` with `parts`. Throws std::invalid_argument for a Rom that Rom::ParsePartial read, for a
// DSi-enhanced image, for an ARM9 binary inside the header and for an ID past the FAT. For a DSi-only image, it also
// throws for an ARM9 binary larger than the image's and for a kept file whose size changes.
std::vector<uint8_t> WriteCompactImage(const Rom& rom, const CompactParts& parts);

} // namespace ntdlib::ntr
