// SPDX-License-Identifier: MIT

// The bytes of a little-endian file as a writer builds them. The sdat module writes the DS's sound archives with it,
// and the dse module writes DSE's files.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/rounding.h"

namespace ntdlib::detail
{

// Bytes that grow as they're written, with fields to fill in later.
class LeWriter
{
public:
    std::size_t Size() const
    {
        return bytes_.size();
    }

    void U8(uint8_t value)
    {
        bytes_.push_back(value);
    }

    void U16(uint16_t value)
    {
        U8(static_cast<uint8_t>(value));
        U8(static_cast<uint8_t>(value >> 8));
    }

    void U32(uint32_t value)
    {
        U16(static_cast<uint16_t>(value));
        U16(static_cast<uint16_t>(value >> 16));
    }

    // Writes a block's four-character label.
    void Magic(const char* magic)
    {
        for (int i = 0; i < 4; i++)
        {
            U8(static_cast<uint8_t>(magic[i]));
        }
    }

    void Bytes(std::span<const uint8_t> data)
    {
        bytes_.insert(bytes_.end(), data.begin(), data.end());
    }

    // Adds zeros up to `size` bytes.
    void PadTo(std::size_t size)
    {
        bytes_.resize(std::max(bytes_.size(), size), 0);
    }

    // Pads to a multiple of `alignment` with `filler` where it fills the gap exactly, and otherwise with `value`.
    void Pad(std::size_t alignment, std::span<const uint8_t> filler, uint8_t value)
    {
        const std::size_t count = AlignUp(bytes_.size(), alignment) - bytes_.size();
        for (std::size_t i = 0; i < count; i++)
        {
            U8(i < filler.size() && filler.size() == count ? filler[i] : value);
        }
    }

    void Put16(std::size_t at, uint32_t value)
    {
        xsflib::StoreLe16(&bytes_[at], static_cast<uint16_t>(value));
    }

    void Put32(std::size_t at, std::size_t value)
    {
        for (std::size_t i = 0; i < 4; i++)
        {
            bytes_[at + i] = static_cast<uint8_t>(value >> (8 * i));
        }
    }

    std::vector<uint8_t> Take()
    {
        return std::move(bytes_);
    }

private:
    std::vector<uint8_t> bytes_;
};

} // namespace ntdlib::detail
