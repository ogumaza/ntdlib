// SPDX-License-Identifier: MIT

// Writes a DS cartridge image (an NDS ROM) again with new bytes for some of its files. The new image takes the layout
// of the SDK's tools. rom.h reads the image.
//
// The tools put each file on a multiple of 0x200 bytes from the image's start, or right after the file before it, and
// fill the bytes between with 0xFF. The cartridge's data ends at the last file's end, the header's used size. Some
// images hold a signature for download play there: 0x88 bytes that start with "ac". The rest of the image, up to the
// chip's capacity, holds 0xFF.
//
// A DSi-enhanced or DSi-only image holds a digest after the files: a hash of each sector of the NTR region (from the
// ARM9 binary to the files' end, on a whole sector) and of the TWL region (from the ARM9i binary to the ARM7i binary's
// end, on a whole sector). A hash of each block of sectors follows. The header holds a hash of the blocks' hashes, the
// digest's master hash. Each one is an HMAC-SHA1 with a key of 64 bytes from the DSi's system. The table of the
// sectors' hashes starts at the NTR region's end. The table of the blocks' hashes follows on a multiple of 0x200 bytes,
// and the used size ends on the next. The DSi's part of the image starts on the next multiple of 0x80000 bytes, with
// 0x3000 bytes before the ARM9i binary.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xsflib/sha1.h"

namespace ntdlib::ntr
{

// The DSi's key of the HMACs of a cartridge image's digest. ntdlib holds no key: the caller gives it.
struct DsiKeys
{
    std::array<uint8_t, 64> digest{};
};

// Hashes that a change leaves stale: what they are, where the first one lies in the new image and how many there are.
struct StaleHash
{
    friend bool operator==(const StaleHash&, const StaleHash&) = default;

    std::string what;
    uint32_t offset = 0;
    uint32_t count = 0;
};

struct RomWrite
{
    std::vector<uint8_t> image;
    std::vector<StaleHash> stale;
};

// The image with new bytes for the files in `files`, by ID. Each file keeps its start. The items after a file that
// changes size move by a multiple of 0x200 bytes, and keep their places within 0x200 bytes. The last item moves by the
// change in size alone. The FAT, the header's offsets, its used size and its CRC-16 follow. The chip's capacity grows
// when the data no longer fits. An image cut short at its data stays cut short.
//
// In a DSi image, the digest's tables move to the NTR region's new end, and the DSi's part moves to the next multiple
// of 0x80000 bytes. The header's offsets of the DSi's part follow. With `keys`, the hashes of the sectors and the
// blocks that the change touches, and the master hash, are written again. Without them, those hashes hold zeros, and
// the result lists them as stale.
//
// The result also lists the hashes that no key here can redo. The header's RSA signature at 0xF80 covers the header's
// first 0xE00 bytes. Only Nintendo can sign it. A DS image for the DSi's whitelist holds two more HMACs: one of the
// header's first 0x160 bytes with the ARM9 and ARM7 binaries (0x378), and one of the ARM9 overlays with the FAT
// (0x38C). The signature for download play covers the header's first 0x160 bytes too. Each one is listed whenever its
// bytes change.
//
// Throws std::invalid_argument for an ID past the FAT, for an overlay's file (the overlay tables describe it), for a
// file whose bytes another item shares, and for a DSi image whose digest isn't laid out as above. Throws
// xsflib::FormatError for an image that rom.h can't read.
RomWrite WithFiles(std::span<const uint8_t> image, const std::map<uint32_t, std::vector<uint8_t>>& files,
                   const std::optional<DsiKeys>& keys = std::nullopt);

// The HMAC-SHA1 of `data` with `key` (RFC 2104).
xsflib::Sha1::Digest HmacSha1(std::span<const uint8_t> key, std::span<const uint8_t> data);

// The CRC-16 of the DS's BIOS (GetCRC16), from 0xFFFF: the header's CRC at 0x15E covers its first 0x15E bytes.
uint16_t Crc16(std::span<const uint8_t> data);

} // namespace ntdlib::ntr
