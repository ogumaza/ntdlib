// SPDX-License-Identifier: MIT

// A builder of binary files for tests, in either byte order: values appended in turn, and stores over the bytes
// already added. The testing headers round sizes and offsets up with ntdlib::AlignUp.

#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/rounding.h"

namespace ntdlib::testing
{

using ntdlib::AlignUp;

// Bytes of a file under construction. They're big-endian unless the constructor is given false. A little-endian file
// stores each value of more than a byte in reverse order, and each tag as a 32-bit word, its characters reversed.
class ByteWriter
{
public:
    explicit ByteWriter(bool big_endian = true) : big_endian_(big_endian)
    {
    }

    std::size_t Size() const
    {
        return data_.size();
    }

    ByteWriter& U8(uint32_t v)
    {
        data_.push_back(static_cast<uint8_t>(v));

        return *this;
    }

    ByteWriter& U16(uint32_t v)
    {
        Put16(Grow(2), v);

        return *this;
    }

    ByteWriter& U24(uint32_t v)
    {
        Put24(Grow(3), v);

        return *this;
    }

    ByteWriter& U32(uint32_t v)
    {
        Put32(Grow(4), v);

        return *this;
    }

    ByteWriter& F32(float v)
    {
        return U32(std::bit_cast<uint32_t>(v));
    }

    // An offset in `width` bytes: 4, or 8 in a file whose offsets are widened.
    ByteWriter& Offset(std::size_t v, std::size_t width)
    {
        const auto low = static_cast<uint32_t>(v);
        if (width == 8)
        {
            return big_endian_ ? U32(0).U32(low) : U32(low).U32(0);
        }

        return U32(low);
    }

    // Little-endian values, for the few tables that hold them, such as MusyX's envelopes.
    ByteWriter& Le16(uint32_t v)
    {
        xsflib::AppendLe16(data_, static_cast<uint16_t>(v));

        return *this;
    }

    ByteWriter& Le32(uint32_t v)
    {
        xsflib::AppendLe32(data_, v);

        return *this;
    }

    ByteWriter& Tag(const char* tag)
    {
        for (int i = 0; i < 4; i++)
        {
            U8(static_cast<uint8_t>(tag[big_endian_ ? i : 3 - i]));
        }

        return *this;
    }

    // The characters of `text`, in their order, without the terminating zero.
    ByteWriter& Text(const char* text)
    {
        for (const char* c = text; *c; c++)
        {
            U8(static_cast<uint8_t>(*c));
        }

        return *this;
    }

    ByteWriter& Append(const std::vector<uint8_t>& more)
    {
        data_.insert(data_.end(), more.begin(), more.end());

        return *this;
    }

    ByteWriter& Fill(std::size_t count, uint8_t value)
    {
        data_.insert(data_.end(), count, value);

        return *this;
    }

    ByteWriter& PadTo(std::size_t size)
    {
        data_.resize(std::max(data_.size(), size));

        return *this;
    }

    // Pads with `value` to a multiple of `alignment` bytes.
    ByteWriter& Align(std::size_t alignment, uint8_t value = 0)
    {
        data_.resize(AlignUp(data_.size(), alignment), value);

        return *this;
    }

    // Each stores `v` at `at`, over bytes already added.
    void Put8(std::size_t at, uint32_t v)
    {
        data_[at] = static_cast<uint8_t>(v);
    }

    void Put16(std::size_t at, uint32_t v)
    {
        big_endian_ ? xsflib::StoreBe16(data_.data() + at, static_cast<uint16_t>(v))
                    : xsflib::StoreLe16(data_.data() + at, static_cast<uint16_t>(v));
    }

    void Put24(std::size_t at, uint32_t v)
    {
        Put8(at + (big_endian_ ? 0 : 2), v >> 16);
        Put8(at + 1, v >> 8);
        Put8(at + (big_endian_ ? 2 : 0), v);
    }

    void Put32(std::size_t at, uint32_t v)
    {
        big_endian_ ? xsflib::StoreBe32(data_.data() + at, v) : xsflib::StoreLe32(data_.data() + at, v);
    }

    // Stores an offset over one that Offset added, `width` bytes wide.
    void PutOffset(std::size_t at, std::size_t v, std::size_t width)
    {
        Put32(width == 8 && big_endian_ ? at + 4 : at, static_cast<uint32_t>(v));
    }

    // Stores `tag` and `size` at the start, in the big-endian order whatever the file's order. A file of the Switch's
    // release of Pikmin keeps its first tag and its size that way.
    void PutBigEndianHeader(const char* tag, uint32_t size)
    {
        std::copy_n(tag, 4, data_.begin());
        xsflib::StoreBe32(data_.data() + 4, size);
    }

    const std::vector<uint8_t>& Data() const
    {
        return data_;
    }

    // Moves the bytes out. The writer is empty afterwards.
    std::vector<uint8_t> Take()
    {
        return std::exchange(data_, {});
    }

private:
    // Appends `size` zero bytes, and returns where they start.
    std::size_t Grow(std::size_t size)
    {
        const std::size_t at = data_.size();
        data_.resize(at + size);

        return at;
    }

    bool big_endian_;
    std::vector<uint8_t> data_;
};

} // namespace ntdlib::testing
