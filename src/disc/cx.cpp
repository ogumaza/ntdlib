// SPDX-License-Identifier: MIT

#include "ntdlib/disc/cx.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/disc/error.h"

namespace ntdlib::disc
{
namespace
{

constexpr uint8_t kLz = 0x10;
constexpr uint8_t kLzLong = 0x11;  // LZ77 with longer copies
constexpr uint8_t kHuffman = 0x28; // Huffman coding of 8-bit symbols

// A header: the format, the size of the decompressed data, and where the compressed data starts.
struct Header
{
    uint8_t type;
    size_t size;
    size_t start;
};

std::optional<Header> ReadHeader(std::span<const uint8_t> data)
{
    const bool magic = data.size() >= 4 && data[0] == 'L' && data[1] == 'Z' && data[2] == '7' && data[3] == '7';
    size_t at = magic ? 4 : 0;
    if (data.size() < at + 4)
    {
        return std::nullopt;
    }

    const uint8_t type = data[at];
    if (type != kLz && type != kLzLong && (type != kHuffman || magic))
    {
        return std::nullopt;
    }

    size_t size = xsflib::LoadLe32(&data[at]) >> 8;
    at += 4;
    if (size == 0)
    {
        if (data.size() < at + 4)
        {
            return std::nullopt;
        }

        size = xsflib::LoadLe32(&data[at]);
        at += 4;
    }

    return Header{type, size, at};
}

// Reads compressed data in order. Throws Error at the end of the data.
class Reader
{
public:
    Reader(std::span<const uint8_t> data, size_t at) : data_(data), at_(at)
    {
    }

    uint8_t Byte()
    {
        if (at_ >= data_.size())
        {
            throw Error("compressed data is truncated");
        }

        return data_[at_++];
    }

    // A 32-bit little-endian word.
    uint32_t Word()
    {
        if (at_ > data_.size() || data_.size() - at_ < 4)
        {
            throw Error("compressed data is truncated");
        }

        const uint32_t word = xsflib::LoadLe32(&data_[at_]);
        at_ += 4;

        return word;
    }

private:
    std::span<const uint8_t> data_;
    size_t at_;
};

// A copy of earlier output: how many bytes, from how far back.
struct Copy
{
    size_t length;
    size_t distance;
};

// Reads an LZ77 copy. Type 0x10 packs a copy in 2 bytes: the length minus 3 in the top 4 bits, and the distance minus 1
// in the other 12. In type 0x11, the top 4 bits choose the copy's form. A value of 2 or more is the length minus 1,
// again with 12 bits of distance. A value of 0 adds a byte, for 8 bits of length minus 17. A value of 1 adds two bytes,
// for 16 bits of length minus 273. Each of these is followed by 12 bits of distance.
Copy ReadCopy(Reader& in, bool long_copies)
{
    const uint32_t first = in.Byte();
    const uint32_t form = first >> 4;
    if (!long_copies || form >= 2)
    {
        return {form + (long_copies ? 1 : 3), ((first & 0xF) << 8 | in.Byte()) + 1u};
    }

    const uint32_t second = in.Byte();
    const uint32_t third = in.Byte();
    if (form == 0)
    {
        return {((first & 0xF) << 4 | second >> 4) + 17u, ((second & 0xF) << 8 | third) + 1u};
    }

    const uint32_t fourth = in.Byte();

    return {((first & 0xF) << 12 | second << 4 | third >> 4) + 273u, ((third & 0xF) << 8 | fourth) + 1u};
}

// LZ77. A flag byte comes before every 8 blocks. Its top bit is for the first block. A clear bit marks a literal byte.
// A set bit marks a copy of earlier output (see ReadCopy).
std::vector<uint8_t> DecompressLz(std::span<const uint8_t> data, const Header& header)
{
    std::vector<uint8_t> out;
    out.reserve(header.size);
    Reader in(data, header.start);
    while (out.size() < header.size)
    {
        const uint8_t flags = in.Byte();
        for (int block = 0; block < 8 && out.size() < header.size; ++block)
        {
            if (!(flags & (0x80 >> block)))
            {
                out.push_back(in.Byte());
                continue;
            }

            const Copy copy = ReadCopy(in, header.type == kLzLong);
            if (copy.distance > out.size())
            {
                throw Error("LZ77 data is damaged");
            }

            // A copy can overlap the bytes that it writes. It stops at the end of the data.
            const size_t end = std::min(header.size, out.size() + copy.length);
            while (out.size() < end)
            {
                const uint8_t byte = out[out.size() - copy.distance];
                out.push_back(byte);
            }
        }
    }

    return out;
}

// Huffman coding with 8-bit symbols. The tree follows the header. Its first byte n gives the tree's size: (n + 1) * 2
// bytes with that byte. The nodes follow, the root first. A node that isn't a symbol gives in its low 6 bits where its
// pair of children is: at the node's offset with bit 0 cleared, plus 2, plus twice that value. Bit 7 is set when the
// first child is a symbol. Bit 6 is set when the second is. The bits follow the tree, in 32-bit little-endian words.
// Each word is read from its top bit down. A bit of 0 chooses the first child. A bit of 1 chooses the second. Reaching
// a symbol writes it and starts again at the root.
std::vector<uint8_t> DecodeHuffman(std::span<const uint8_t> data, const Header& header)
{
    if (header.start >= data.size() || data.size() - header.start < (size_t{data[header.start]} + 1) * 2)
    {
        throw Error("Huffman data is truncated");
    }

    const size_t root = header.start + 1;
    const size_t bits = header.start + (size_t{data[header.start]} + 1) * 2;
    std::vector<uint8_t> out;
    out.reserve(header.size);
    Reader in(data, bits);
    size_t node = root;
    while (out.size() < header.size)
    {
        const uint32_t word = in.Word();
        for (int bit = 31; bit >= 0 && out.size() < header.size; --bit)
        {
            const uint32_t branch = word >> bit & 1;
            const uint8_t value = data[node];
            const size_t child = (node & ~size_t{1}) + 2 + (value & 0x3Fu) * 2 + branch;
            if (child >= bits)
            {
                throw Error("Huffman data is damaged");
            }

            if (value & (0x80u >> branch))
            {
                out.push_back(data[child]);
                node = root;
            }
            else
            {
                node = child;
            }
        }
    }

    return out;
}

} // namespace

bool IsCx(std::span<const uint8_t> data)
{
    return ReadHeader(data).has_value();
}

std::vector<uint8_t> DecompressCx(std::span<const uint8_t> data, size_t max_size)
{
    const std::optional<Header> header = ReadHeader(data);
    if (!header)
    {
        throw Error("the data isn't in one of the CX library's formats: LZ77 or Huffman coding");
    }
    if (header->size > max_size)
    {
        throw Error("compressed data is too large");
    }

    return header->type == kHuffman ? DecodeHuffman(data, *header) : DecompressLz(data, *header);
}

} // namespace ntdlib::disc
