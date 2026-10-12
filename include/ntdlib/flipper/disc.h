// SPDX-License-Identifier: MIT

// The data that the disc drive reads: a GameCube disc, or a Wii disc's game partition.

#pragma once

#include <cstddef>
#include <cstdint>

namespace ntdlib::flipper
{

// The disc's data in the form that the disc drive reads.
class Disc
{
public:
    virtual ~Disc() = default;

    // Reads `size` bytes at byte `offset` of the data. Returns false if any of them is missing.
    virtual bool Read(uint64_t offset, void* out, std::size_t size) = 0;
};

} // namespace ntdlib::flipper
