// SPDX-License-Identifier: MIT

// The GFSA archives of Level-5's games, and their compression, for the finder of DSE files (files.cpp). The archive's
// file and its tables are described in gfsa.cpp.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ntdlib::dse
{

// Data in Level-5's compression: a 32-bit little-endian word with the method in bits 0 to 2 and the size of the
// decompressed data from bit 3, then the data. Method 0 stores the data. Methods 1 to 4 compress it in a format of the
// DS's BIOS without its header: LZ77, Huffman coding of 4-bit and of 8-bit symbols, and run-length coding. Nothing for
// another method, for damaged data, or for data that would decompress to more than `max_size` bytes.
std::optional<std::vector<uint8_t>> DecompressLevel5(std::span<const uint8_t> data, std::size_t max_size);

// A file of a GFSA archive: its path in the archive and its bytes, as the archive stores them (DecompressLevel5).
struct GfsaMember
{
    std::string path;
    std::span<const uint8_t> bytes;
};

// The files of the GFSA archive `archive`, in the order of its folders' names and of the files' names in each. Nothing
// when `archive` isn't one, or when its tables are damaged.
std::optional<std::vector<GfsaMember>> GfsaMembers(std::span<const uint8_t> archive);

} // namespace ntdlib::dse
