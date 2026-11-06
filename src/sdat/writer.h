// SPDX-License-Identifier: MIT

// The bytes of a file that the sdat module's writers build. Every file of the DS's sound archive is little-endian.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace ntdlib::sdat::detail
{

// `value` rounded up to a multiple of `alignment`.
inline std::size_t Align(std::size_t value, std::size_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

// Bytes that grow as they're written, with fields to fill in later.
class Writer
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

    void PadTo(std::size_t size)
    {
        bytes_.resize(std::max(bytes_.size(), size), 0);
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

} // namespace ntdlib::sdat::detail
