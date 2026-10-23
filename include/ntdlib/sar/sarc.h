// SPDX-License-Identifier: MIT

// SARC archives: Nintendo's archive format on the Wii U, the 3DS and the Switch. A game can keep a sound archive's
// group files in them, under other names.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ntdlib::sar
{

// A file in a SARC: its name, empty when the SARC gives it none, and its data.
struct SarcFile
{
    std::string name;
    std::span<const uint8_t> data;
};

// The files in a SARC that isn't compressed. The header gives "SARC", its size, a byte order mark, the archive's size
// and where the data starts. The SFAT block follows, with its size and its count of nodes. A node of 16 bytes follows
// for each file. The node's last two words give where the file starts and ends in the data. A node's second word gives
// a nonzero top byte for a file with a name, and the name's offset in words in its low 24 bits. The SFNT block follows
// the nodes. Its names start after its 8-byte header, and each ends with a NUL. The spans point into `sarc`. A file
// that doesn't fit is left out. A name that doesn't end before the data is left empty. Data that isn't a SARC has no
// files.
std::vector<SarcFile> SarcFiles(std::span<const uint8_t> sarc);

// The SARC `sarc` with `data` in place of the data of `old`, a file that SarcFiles(sarc) gave. Every node that gives
// the old data's place gives the new place. The other files keep their bytes and their places. The blocks before the
// data stay, apart from the archive's size in the header and those nodes' offsets. The new data takes the old place
// when it fits before the next file's data. Zeros then fill the rest of the old place. When no file's data follows,
// the new data takes the old place whatever its size, and the bytes after the old data follow it. Otherwise the new
// data goes after the last file's data, at an offset with the old offset's alignment, up to 0x2000 bytes. Zeros then
// fill the old place. Throws std::invalid_argument when `old` isn't the data of one of the SARC's files, and when the
// new SARC passes 4 GB.
std::vector<uint8_t> WithSarcFile(std::span<const uint8_t> sarc, std::span<const uint8_t> old,
                                  std::span<const uint8_t> data);

} // namespace ntdlib::sar
