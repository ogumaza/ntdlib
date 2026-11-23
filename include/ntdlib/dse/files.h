// SPDX-License-Identifier: MIT

// The DSE files among a game's files, loose or inside other files. Many games keep their songs, wave banks and banks of
// sound effects in packs that the game's developer made: files end to end, each behind a small header, or a whole
// cartridge image inside the game's file system. Each DSE file in such a pack keeps its header: the magic word (smdl,
// swdl or sedl), 4 zero bytes, its size at 0x08, its version at 0x0C and its name at 0x20, in 16 bytes. A scan of the
// pack's bytes for those headers finds each file without the pack's index.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/dse/sets.h"
#include "ntdlib/ntr/rom.h"

namespace ntdlib::dse
{

// The DSE files of `files`, in their order. A file that starts with a DSE header and holds no other DSE file after the
// size that the header gives is one DSE file, as it is. Any other file is scanned for headers of version 0x415 or 0x402
// whose size fits in the file. A header of version 0x402 that gives a size of 0 ends with its end chunk. The scan goes
// on after each file that it finds. A file found by the scan points into the pack's bytes. Its path is the pack's path,
// a '/', and the name at 0x20: the bytes up to a zero or 0xFF, with '_' in place of each byte that a file name can't
// hold. A name without an extension, or with one cut short, such as "BGM_01.S", gets the kind's: .smd, .swd or .sed. An
// empty name gives the file's offset in the pack, in hexadecimal. A name that comes again in the same pack (the names
// are often cut short) gets _2, _3 and so on before its extension: the first name that the pack hasn't given yet.
std::vector<NamedFile> FindFiles(std::span<const NamedFile> files);

// `files`, with the decompressed bytes in place of each file that is compressed with the SDK's LZ77 (type 0x10) and
// holds a DSE header of version 0x415 or 0x402 once decompressed. The decompressed bytes keep the file's path. Some
// games compress each song with its bank, in a small pack of their making. A GFSA archive of Level-5's games gives its
// files that hold such a header once decompressed in its place, under its path, a '/' and the file's path in the
// archive. FindFiles then finds the DSE files in the decompressed bytes. `storage` keeps the decompressed bytes for as
// long as the list is in use. A file that doesn't decompress, or that would decompress to more than 16 MiB, stays as it
// is.
std::vector<NamedFile> Decompressed(std::span<const NamedFile> files, std::vector<std::vector<uint8_t>>& storage);

// The DSE files of `rom`'s files: FindFiles of their Decompressed bytes. A file without a path in the file system, such
// as an overlay or any file of a partial image, goes by "file <ID>". The files point into the image and into `storage`.
std::vector<NamedFile> FindRomFiles(const ntr::Rom& rom, std::vector<std::vector<uint8_t>>& storage);

} // namespace ntdlib::dse
