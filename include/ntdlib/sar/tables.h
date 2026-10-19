// SPDX-License-Identifier: MIT

// The tables of a sound archive of the 3DS (CSAR), the Wii U or the Switch (FSAR): its STRG and INFO blocks
// (ntdlib/sar/string_block.h and ntdlib/sar/info_block.h). TablesOf finds them in an archive. WithTables puts new ones
// in their place.
//
// NintendoWare's tools lay an archive out as its header, then its STRG, INFO and FILE blocks, in that order and
// without gaps. The header's table of blocks lists the three. An archive without names lists its STRG block at offset
// 0xFFFFFFFF. The FILE block holds the archive's files and ends the archive. Its magic and size come first. Zeros
// follow, up to the first file. Each file starts on a multiple of 64 bytes from the archive's start on the Switch, and
// of 32 bytes on the 3DS and the Wii U. The first file starts at the first such multiple after the block's size. The
// INFO block's file table gives each file's offset from the end of the FILE block's size.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/info_block.h"
#include "ntdlib/sar/string_block.h"

namespace ntdlib::sar
{

// The tables of an archive: bytes of the archive.
struct Tables
{
    std::optional<std::span<const uint8_t>> strings; // the STRG block, if the archive has one
    std::span<const uint8_t> info;                   // the INFO block
};

// The tables of `archive`, an archive of `flavor`, from the header's table of blocks. Throws xsflib::FormatError for
// an archive without an INFO block, and for a block that runs past the archive's end.
Tables TablesOf(std::span<const uint8_t> archive, Flavor flavor);

// `archive`, an archive of `flavor`, with the blocks of `strings` and `info` in the place of its STRG and INFO blocks.
// The FILE block follows them, with the same files in the same order. The zeros before its first file change to keep
// each file's offset from the archive's start a multiple of 64 or 32 bytes. The offsets in the file table of `info`
// change with them. The header's table of blocks and the archive's size follow the new blocks. Throws
// xsflib::FormatError for an archive laid out another way, for an archive cut short, and for a file of `info` whose
// location lies outside its entry or before the first file. Throws std::invalid_argument for `strings` with an archive
// without a STRG block, for no `strings` with an archive with one, and for tables of another flavor.
std::vector<uint8_t> WithTables(std::span<const uint8_t> archive, Flavor flavor,
                                const std::optional<StringBlock>& strings, InfoBlock info);

} // namespace ntdlib::sar
