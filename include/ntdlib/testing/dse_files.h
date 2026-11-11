// SPDX-License-Identifier: MIT

// The helpers that the DSE tests share: a builder of little-endian files with the chunk headers of DSE's files, the
// start of a file's header, and a song's settings.

#pragma once

#include <cstdint>
#include <vector>

#include "ntdlib/testing/byte_writer.h"

namespace ntdlib::testing::dse_files
{

using Bytes = std::vector<uint8_t>;

// Bytes of a file, with the little-endian fields of formats.h.
class Builder : public ByteWriter
{
public:
    Builder() : ByteWriter(false)
    {
    }
};

// A chunk's header: the label, the 8 bytes of a bank's chunks and the size.
inline ByteWriter& BankChunk(ByteWriter& b, const char* label, uint32_t size)
{
    return b.Text(label).Append({0x00, 0x00, 0x15, 0x04, 0x10, 0x00, 0x00, 0x00}).U32(size);
}

// A track chunk's header, or an eoc chunk's with `label` "eoc ".
inline ByteWriter& TrackChunk(ByteWriter& b, const char* label, uint32_t size)
{
    return b.Text(label).Append({0x00, 0x00, 0x00, 0x01, 0x04, 0xFF, 0x00, 0x00}).U32(size);
}

// The common start of a header: the magic word, a size that the caller fills, the version, two other bytes, 8 zeros,
// a date and a name with the tools' bytes after its zero.
inline void HeaderStart(ByteWriter& b, const char* magic, uint16_t other)
{
    b.Text(magic).U32(0).U32(0).U16(0x0415).U16(other).Fill(8, 0);
    b.Append({0xD8, 0x07, 0x0B, 0x12, 0x15, 0x1E, 0x06, 0x05});
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
