// SPDX-License-Identifier: MIT

// The helpers that the DSE tests share: a builder of little-endian files with the chunk headers of DSE's files, the
// start of a file's header, and a song's settings.

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ntdlib::testing::dse_files
{

using Bytes = std::vector<uint8_t>;

// Bytes of a file, with the little-endian fields of formats.h.
class Builder
{
public:
    std::size_t Size() const
    {
        return bytes_.size();
    }

    Builder& Raw(const Bytes& bytes)
    {
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
        return *this;
    }

    Builder& Text(const char* text)
    {
        for (const char* c = text; *c; c++)
        {
            bytes_.push_back(static_cast<uint8_t>(*c));
        }
        return *this;
    }

    Builder& U16(uint16_t value)
    {
        return Raw({static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8)});
    }

    Builder& U32(uint32_t value)
    {
        return U16(static_cast<uint16_t>(value)).U16(static_cast<uint16_t>(value >> 16));
    }

    Builder& Fill(std::size_t count, uint8_t value)
    {
        bytes_.insert(bytes_.end(), count, value);
        return *this;
    }

    // A chunk's header: the label, the 8 bytes of a bank's chunks and the size.
    Builder& BankChunk(const char* label, uint32_t size)
    {
        return Text(label).Raw({0x00, 0x00, 0x15, 0x04, 0x10, 0x00, 0x00, 0x00}).U32(size);
    }

    // A track chunk's header, or an eoc chunk's with `label` "eoc ".
    Builder& TrackChunk(const char* label, uint32_t size)
    {
        return Text(label).Raw({0x00, 0x00, 0x00, 0x01, 0x04, 0xFF, 0x00, 0x00}).U32(size);
    }

    void Put32(std::size_t at, uint32_t value)
    {
        for (std::size_t i = 0; i < 4; i++)
        {
            bytes_[at + i] = static_cast<uint8_t>(value >> (8 * i));
        }
    }

    Bytes Take()
    {
        return std::move(bytes_);
    }

private:
    Bytes bytes_;
};

// The common start of a header: the magic word, a size that the caller fills, the version, two other bytes, 8 zeros,
// a date and a name with the tools' bytes after its zero.
inline void HeaderStart(Builder& b, const char* magic, uint16_t other)
{
    b.Text(magic).U32(0).U32(0).U16(0x0415).U16(other).Fill(8, 0);
    b.Raw({0xD8, 0x07, 0x0B, 0x12, 0x15, 0x1E, 0x06, 0x05});
    b.Text("B_TEST").Fill(1, 0).Fill(9, 0xAA);
}

// A song's settings: 48 ticks to a quarter note, `tracks` tracks and `channels` channels.
inline Bytes SequenceInfoBytes(uint8_t tracks, uint8_t channels)
{
    Bytes info = {0x01, 0x00, 0x30, 0x00, 0x01, 0xFF, tracks, channels, 0x00, 0x00, 0x00, 0x0F, 0xFF, 0xFF, 0xFF, 0xFF,
                  0x00, 0x00, 0x00, 0x40, 0x00, 0x40, 0x40,   0x00,     0x00, 0x02, 0x00, 0x08, 0x00, 0xFF, 0xFF, 0xFF};
    info.insert(info.end(), 16, 0xFF);
    return info;
}

} // namespace ntdlib::testing::dse_files
