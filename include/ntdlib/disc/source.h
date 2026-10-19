// SPDX-License-Identifier: MIT

// Random access to the bytes of a file, or of a file split into parts that are read as one, and the reads of data that
// a format stores in blocks of one size.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "ntdlib/disc/error.h"

namespace ntdlib::disc
{

// Random access to a file, or to the parts of a split file, read as one.
class Source
{
public:
    // Opens the files in `parts` and joins them in order. `noun` names the whole in messages, such as "image" or
    // "file". Throws Error ("can't open" and the part's file name) if a part can't be opened.
    Source(const std::vector<std::filesystem::path>& parts, std::string noun);

    uint64_t Size() const;

    // Reads exactly `size` bytes at `offset`. A format gives the positions of its data. A read past the end is then an
    // error. Throws Error ("the <noun> is truncated") if the bytes aren't all there, or ("reading the <noun> failed")
    // if the read fails.
    void Read(uint64_t offset, uint8_t* out, size_t size);

    std::vector<uint8_t> Read(uint64_t offset, size_t size);

    // Reads `size` bytes at `offset`, with zeros for the bytes past the end. A dump that ends early reads that way.
    // Throws Error if the read fails.
    void ReadPadded(uint64_t offset, uint8_t* out, size_t size);

private:
    struct Part
    {
        std::ifstream file;
        uint64_t start = 0;
        uint64_t size = 0;
    };

    std::vector<Part> parts_;
    uint64_t size_ = 0;
    std::string noun_;
};

// Splits a read of `size` bytes at `offset`, from data stored in blocks of `block_size` bytes, into pieces that each
// lie in one block. Calls `read(index, within, out, n)` for each piece: `n` bytes at `within` in block `index`, into
// `out`.
template <typename ReadPiece>
void ReadByBlocks(uint64_t offset, uint8_t* out, size_t size, uint64_t block_size, ReadPiece read)
{
    while (size)
    {
        const uint64_t index = offset / block_size;
        const size_t within = static_cast<size_t>(offset % block_size);
        const size_t n = static_cast<size_t>(std::min<uint64_t>(size, block_size - within));
        read(index, within, out, n);
        out += n;
        offset += n;
        size -= n;
    }
}

} // namespace ntdlib::disc
