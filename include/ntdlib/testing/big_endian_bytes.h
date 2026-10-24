// SPDX-License-Identifier: MIT

// A builder of big-endian files for tests, such as the GameCube's: values appended in turn, and stores over the bytes
// already added.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "xsflib/bytes.h"

namespace ntdlib::testing
{

// Big-endian data under construction.
class BigEndianBytes
{
public:
    std::size_t Size() const
    {
        return data_.size();
    }

    BigEndianBytes& U8(uint32_t v)
    {
        data_.push_back(static_cast<uint8_t>(v));

        return *this;
    }

    BigEndianBytes& U16(uint32_t v)
    {
        return U8(v >> 8).U8(v);
    }

    BigEndianBytes& U24(uint32_t v)
    {
        return U8(v >> 16).U16(v);
    }

    BigEndianBytes& U32(uint32_t v)
    {
        return U16(v >> 16).U16(v);
    }

    BigEndianBytes& F32(float v)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, 4);
        return U32(bits);
    }

    // Little-endian values, for the few tables that hold them, such as MusyX's envelopes.
    BigEndianBytes& Le16(uint32_t v)
    {
        return U8(v).U8(v >> 8);
    }

    BigEndianBytes& Le32(uint32_t v)
    {
        return Le16(v).Le16(v >> 16);
    }

    BigEndianBytes& Tag(const char* tag)
    {
        for (int i = 0; i < 4; i++)
        {
            U8(static_cast<uint8_t>(tag[i]));
        }

        return *this;
    }

    BigEndianBytes& Append(const std::vector<uint8_t>& more)
    {
        data_.insert(data_.end(), more.begin(), more.end());

        return *this;
    }

    BigEndianBytes& PadTo(std::size_t size)
    {
        data_.resize(std::max(data_.size(), size));

        return *this;
    }

    // Each stores `v` at `at`, over bytes already added.
    void Put8(std::size_t at, uint32_t v)
    {
        data_[at] = static_cast<uint8_t>(v);
    }

    void Put16(std::size_t at, uint32_t v)
    {
        xsflib::StoreBe16(data_.data() + at, static_cast<uint16_t>(v));
    }

    void Put32(std::size_t at, uint32_t v)
    {
        xsflib::StoreBe32(data_.data() + at, v);
    }

    const std::vector<uint8_t>& Data() const
    {
        return data_;
    }

private:
    std::vector<uint8_t> data_;
};

} // namespace ntdlib::testing
