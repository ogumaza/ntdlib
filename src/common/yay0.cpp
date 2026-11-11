// SPDX-License-Identifier: MIT

#include "ntdlib/yay0.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

namespace ntdlib
{

namespace
{

constexpr std::size_t kHeaderSize = 16;
constexpr std::size_t kMaxDecodedSize = 0x4000000;

// The places of the decoder's three streams, and the byte of bits that it reads.
struct ThreeStreams
{
    std::size_t bits = kHeaderSize;
    std::size_t references = 0;
    std::size_t literals = 0;
    uint8_t current = 0;
    int bits_left = 0;
};

bool NextBit(std::span<const uint8_t> data, ThreeStreams& streams)
{
    if (streams.bits_left == 0)
    {
        if (streams.bits >= data.size())
        {
            throw xsflib::FormatError("a compressed block's bits leave the data");
        }

        streams.current = data[streams.bits++];
        streams.bits_left = 8;
    }

    streams.bits_left--;

    return ((streams.current >> streams.bits_left) & 1) != 0;
}

uint8_t ByteAt(std::span<const uint8_t> data, std::size_t offset)
{
    if (offset >= data.size())
    {
        throw xsflib::FormatError("a compressed block leaves the data");
    }

    return data[offset];
}

void CopyBack(std::vector<uint8_t>& out, std::size_t distance, std::size_t count)
{
    if (distance > out.size())
    {
        throw xsflib::FormatError("a compressed block repeats bytes before its start");
    }

    for (std::size_t i = 0; i < count; i++)
    {
        out.push_back(out[out.size() - distance]);
    }
}

} // namespace

std::vector<uint8_t> DecodeYay0(std::span<const uint8_t> data)
{
    if (data.size() < kHeaderSize || std::memcmp(data.data(), "Yay0", 4) != 0)
    {
        throw xsflib::FormatError("not Yay0 data");
    }

    const std::size_t size = xsflib::LoadBe32(data.data() + 4);
    if (size > kMaxDecodedSize)
    {
        throw xsflib::FormatError("Yay0 data decodes to more than 64 MiB");
    }

    ThreeStreams streams;
    streams.references = xsflib::LoadBe32(data.data() + 8);
    streams.literals = xsflib::LoadBe32(data.data() + 12);
    std::vector<uint8_t> out;
    out.reserve(size);
    while (out.size() < size)
    {
        if (NextBit(data, streams))
        {
            out.push_back(ByteAt(data, streams.literals++));
            continue;
        }

        const uint8_t high = ByteAt(data, streams.references++);
        const uint8_t low = ByteAt(data, streams.references++);
        std::size_t count = high >> 4;
        count = count == 0 ? ByteAt(data, streams.literals++) + 18U : count + 2;
        CopyBack(out, (static_cast<std::size_t>(high & 0x0F) << 8 | low) + 1, count);
    }

    out.resize(size);

    return out;
}

} // namespace ntdlib
