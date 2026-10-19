// SPDX-License-Identifier: MIT

// The bytes of a file that the sar module's writers build, in the file's byte order.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

namespace ntdlib::sar::detail
{

// A file's bytes in its byte order.
class Out
{
public:
    explicit Out(bool big_endian) : big_endian_(big_endian)
    {
    }

    // Appends `bytes`, and returns where they start.
    std::size_t Append(std::span<const uint8_t> bytes)
    {
        const std::size_t at = bytes_.size();
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
        return at;
    }

    // Appends `count` zero bytes, and returns where they start.
    std::size_t Zeros(std::size_t count)
    {
        const std::size_t at = bytes_.size();
        bytes_.resize(at + count);
        return at;
    }

    void Align(std::size_t alignment)
    {
        Zeros((alignment - bytes_.size() % alignment) % alignment);
    }

    void U8(std::size_t at, uint8_t value)
    {
        bytes_[at] = value;
    }

    void U16(std::size_t at, uint16_t value)
    {
        big_endian_ ? xsflib::StoreBe16(&bytes_[at], value) : xsflib::StoreLe16(&bytes_[at], value);
    }

    void U32(std::size_t at, uint32_t value)
    {
        big_endian_ ? xsflib::StoreBe32(&bytes_[at], value) : xsflib::StoreLe32(&bytes_[at], value);
    }

    // A reference: its type, two bytes of padding and an offset.
    void Reference(std::size_t at, uint16_t type, uint32_t offset)
    {
        U16(at, type);
        U32(at + 4, offset);
    }

    std::size_t Size() const
    {
        return bytes_.size();
    }

    std::vector<uint8_t> Take()
    {
        return std::move(bytes_);
    }

private:
    std::vector<uint8_t> bytes_;
    bool big_endian_ = false;
};

} // namespace ntdlib::sar::detail
