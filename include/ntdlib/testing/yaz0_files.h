// SPDX-License-Identifier: MIT

// Builds Yaz0 blocks for tests: a header and a given body, or data compressed with literals only.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ntdlib/testing/byte_writer.h"

namespace ntdlib::testing::yaz0_files
{

// A Yaz0 block: the header with the decoded size `size`, then `body`.
inline std::vector<uint8_t> Yaz0Block(uint32_t size, const std::vector<uint8_t>& body)
{
    ByteWriter block;
    block.Tag("Yaz0").U32(size).U32(0).U32(0).Append(body);

    return block.Take();
}

// `data` compressed with Yaz0, every byte as a literal: groups of eight chunks after a flag byte of 0xff.
inline std::vector<uint8_t> LiteralYaz0(const std::vector<uint8_t>& data)
{
    std::vector<uint8_t> body;
    for (std::size_t i = 0; i < data.size(); i++)
    {
        if (i % 8 == 0)
        {
            body.push_back(0xff);
        }
        body.push_back(data[i]);
    }

    return Yaz0Block(static_cast<uint32_t>(data.size()), body);
}

} // namespace ntdlib::testing::yaz0_files
