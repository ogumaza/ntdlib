// SPDX-License-Identifier: MIT

// A sequence file of the 3DS (CSEQ), the Wii U or the Switch (FSEQ), in parts that a writer can put back together:
// the version, the bytecode and the labels. The writer lays the file out as NintendoWare's tools do. The header and its
// table of blocks come first. DATA follows with the bytecode, then LABL with the labels. Each label holds a reference
// to its offset in the bytecode, its name's length, and the name with a NUL, padded to 4 bytes. Each part of the file
// is padded with zeros to 32 bytes.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/sar/formats.h"

namespace ntdlib::sar
{

struct SequenceFile
{
    // Reads `file`. Throws xsflib::FormatError for a file that isn't a sequence file, or one without DATA or LABL. A
    // file laid out another way reads too, and Write() then gives other bytes.
    static SequenceFile Read(std::span<const uint8_t> file);

    // The file's bytes. The writer pads `data` to the block's size.
    std::vector<uint8_t> Write() const;

    Flavor flavor = Flavor::kNx;
    uint32_t version = 0x00020000;
    std::vector<uint8_t> data;           // the bytecode: the DATA block's payload, with the zeros that pad it
    std::vector<Sequence::Label> labels; // in the file's order
};

} // namespace ntdlib::sar
