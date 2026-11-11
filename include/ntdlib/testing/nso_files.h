// SPDX-License-Identifier: MIT

// Builders of the Switch's executables (NSO, ntdlib/nx/nso.h) for tests. A segment is stored as an LZ4 block
// (ntdlib/nx/lz4.h) or as it is.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/sha256.h"

#include "ntdlib/testing/byte_writer.h"

namespace ntdlib::testing
{

// An LZ4 block that holds `bytes` as the literals of one sequence. The token holds their number up to 15. Bytes of 255,
// then one below 255, give the rest.
inline std::vector<uint8_t> Lz4Literals(const std::vector<uint8_t>& bytes)
{
    std::vector<uint8_t> block = {static_cast<uint8_t>(std::min<std::size_t>(bytes.size(), 15) << 4)};
    if (bytes.size() >= 15)
    {
        std::size_t rest = bytes.size() - 15;
        for (; rest >= 255; rest -= 255)
        {
            block.push_back(255);
        }

        block.push_back(static_cast<uint8_t>(rest));
    }

    block.insert(block.end(), bytes.begin(), bytes.end());

    return block;
}

// A segment of NsoFile: its bytes, whether the file stores them as an LZ4 block (Lz4Literals), and whether the header
// holds their hash.
struct NsoSegmentSpec
{
    std::vector<uint8_t> bytes;
    bool compressed = true;
    bool hashed = true;
};

// An NSO of the segments `text`, `rodata` and `data`, in that order in memory from address 0. Each starts on a
// 0x1000-byte boundary. The file stores them after the header, in the same order.
inline std::vector<uint8_t> NsoFile(const NsoSegmentSpec& text, const NsoSegmentSpec& rodata,
                                    const NsoSegmentSpec& data)
{
    std::vector<uint8_t> file(0x100);
    std::copy_n("NSO0", 4, file.begin());
    uint32_t flags = 0;
    uint32_t address = 0;
    const std::array<const NsoSegmentSpec*, 3> segments = {&text, &rodata, &data};
    for (std::size_t i = 0; i < segments.size(); i++)
    {
        // The segment's entry: its offset in the file, its address and its size. Then its size in the file.
        const NsoSegmentSpec& segment = *segments[i];
        const std::vector<uint8_t> stored = segment.compressed ? Lz4Literals(segment.bytes) : segment.bytes;
        const auto size = static_cast<uint32_t>(segment.bytes.size());
        xsflib::StoreLe32(&file[0x10 + 0x10 * i], static_cast<uint32_t>(file.size()));
        xsflib::StoreLe32(&file[0x14 + 0x10 * i], address);
        xsflib::StoreLe32(&file[0x18 + 0x10 * i], size);
        xsflib::StoreLe32(&file[0x60 + 4 * i], static_cast<uint32_t>(stored.size()));
        if (segment.compressed)
        {
            flags |= 1u << i;
        }

        if (segment.hashed)
        {
            const xsflib::Sha256::Digest hash = xsflib::Sha256::Of(segment.bytes);
            std::copy(hash.begin(), hash.end(), file.begin() + static_cast<std::ptrdiff_t>(0xa0 + 0x20 * i));
            flags |= 8u << i;
        }

        file.insert(file.end(), stored.begin(), stored.end());
        address += AlignUp(size, 0x1000);
    }

    xsflib::StoreLe32(&file[0xc], flags);

    return file;
}

} // namespace ntdlib::testing
