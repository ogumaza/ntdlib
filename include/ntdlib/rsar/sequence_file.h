// SPDX-License-Identifier: MIT

// A sequence file of the Wii (RSEQ), in parts that a writer can put back together: the version, the bytecode and the
// labels. The writer lays the file out as NintendoWare for Revolution's tools do. A header of 0x20 bytes comes first,
// with the offset and the size of each of the two blocks. DATA follows with the bytecode's offset in the block, 0xC,
// and the bytecode. LABL holds the number of labels and an offset to each, then the labels. Each label holds its offset
// in the bytecode, its name's length, and the name with a NUL, padded to 4 bytes. Each block is padded with zeros to 32
// bytes.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/rsar/formats.h"

namespace ntdlib::rsar
{

struct SequenceFile
{
    // Reads `file`. Throws xsflib::FormatError for a file that isn't an RSEQ file, or one without DATA or LABL. A file
    // laid out another way reads too, and Write() then gives other bytes.
    static SequenceFile Read(std::span<const uint8_t> file);

    // The file's bytes. The writer pads `data` to the block's size.
    std::vector<uint8_t> Write() const;

    uint16_t version = 0x0100;
    std::vector<uint8_t> data;           // the bytecode: from its offset in DATA to the block's end, padding included
    std::vector<Sequence::Label> labels; // in the file's order
};

} // namespace ntdlib::rsar
