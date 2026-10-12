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

} // namespace ntdlib::sar
