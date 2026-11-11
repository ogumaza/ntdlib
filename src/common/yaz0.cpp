// SPDX-License-Identifier: MIT

#include "ntdlib/yaz0.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "xsflib/binary.h"

namespace ntdlib
{

bool IsYaz0(std::span<const uint8_t> data)
{
    return data.size() >= 4 && data[0] == 'Y' && data[1] == 'a' && data[2] == 'z' && data[3] == '0';
}

std::vector<uint8_t> DecodeYaz0(std::span<const uint8_t> data)
{
    // A 16-byte header: "Yaz0", the decoded size and 8 bytes that are unused. The data then comes in groups of eight
    // chunks. A group's first byte has a bit for each chunk, from the top. A set bit marks a byte to copy to the
    // output. A clear bit marks a repeat of earlier output: two bytes, with the distance back less 1 in the low 12 bits
    // and the length less 2 in the top 4. A length of 0 there means that a third byte gives the length less 0x12.
    constexpr uint32_t kMaxSize = 64 << 20;
    const xsflib::Reader r(data, true);
    if (!IsYaz0(data) || r.Size() < 16)
    {
        throw xsflib::FormatError("not Yaz0 data");
    }

    const uint32_t size = r.U32(4);
    if (size > kMaxSize)
    {
        throw xsflib::FormatError("Yaz0 data decodes to more than 64 MiB");
    }

    std::vector<uint8_t> out;
    out.reserve(size);
    std::size_t at = 16;
    while (out.size() < size)
    {
        const uint8_t chunks = r.U8(at++);
        for (int bit = 7; bit >= 0 && out.size() < size; bit--)
        {
            if (chunks >> bit & 1)
            {
                out.push_back(r.U8(at++));
                continue;
            }

            const uint16_t repeat = r.U16(at);
            at += 2;
            const std::size_t distance = (repeat & 0xfffu) + std::size_t{1};
            const std::size_t length = repeat >> 12 ? (repeat >> 12) + std::size_t{2} : r.U8(at++) + std::size_t{0x12};
            if (distance > out.size())
            {
                throw xsflib::FormatError("Yaz0 data repeats bytes before its start");
            }

            for (std::size_t i = 0; i < length && out.size() < size; i++)
            {
                out.push_back(out[out.size() - distance]);
            }
        }
    }

    return out;
}

std::span<const uint8_t> DecodeIfYaz0(std::span<const uint8_t> data, std::vector<uint8_t>& decoded)
{
    if (!IsYaz0(data))
    {
        return data;
    }

    decoded = DecodeYaz0(data);

    return decoded;
}

} // namespace ntdlib
