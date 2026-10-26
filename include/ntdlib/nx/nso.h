// SPDX-License-Identifier: MIT

// The Switch's executable format, NSO, as SwitchBrew's wiki describes it. A header of 0x100 bytes starts with "NSO0".
// From 0x10, it holds an entry for each of a module's three segments: text, read-only data (rodata) and data. An
// entry gives the segment's offset in the file, its address and its size. The address counts from the module's start.
// The flags at 0xC mark the segments that the file stores compressed (bits 0 to 2) and those with a SHA-256 hash in the
// header (bits 3 to 5). The header gives segment i's size in the file at 0x60 + 4 * i, and its hash at 0xA0 + 0x20 * i.
// A compressed segment is one LZ4 block (lz4.h). Each field is little-endian.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace ntdlib::nx
{

// A segment of an NSO: its address, counted from the module's start, and its bytes.
struct NsoSegment
{
    uint32_t address = 0;
    std::vector<uint8_t> bytes;
};

// The segments of an NSO.
struct Nso
{
    NsoSegment text;
    NsoSegment rodata;
    NsoSegment data;
};

// The segments of the NSO `file`, each decompressed. A segment whose flag of a hash is set must match that hash. Throws
// FormatError if `file` isn't an NSO, if it's damaged, or if a segment doesn't match its hash.
Nso ReadNso(std::span<const uint8_t> file);

} // namespace ntdlib::nx
