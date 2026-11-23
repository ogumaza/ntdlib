// SPDX-License-Identifier: MIT

#include "ntdlib/disc/cx.h"

#include <algorithm>
#include <bit>
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
constexpr uint8_t kLzLong = 0x11;    // LZ77 with longer copies
constexpr uint8_t kHuffman4 = 0x24;  // Huffman coding of 4-bit symbols, without the header only
constexpr uint8_t kHuffman = 0x28;   // Huffman coding of 8-bit symbols
constexpr uint8_t kRunLength = 0x30; // run-length coding, without the header only
constexpr uint8_t kLh = 0x40;        // LZ77 with Huffman coding of its symbols
constexpr uint8_t kLrc = 0x50;       // LZ77 with range coding of its symbols

// The most bytes that LH and LRC data may hold after their last symbol. The game's LH decoder refuses data with more
// (see DecompressLh). Its LRC decoder doesn't check. The check here keeps data that only starts like LRC data from
// passing as it.
constexpr size_t kMaxTrailingBytes = 0x20;

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
    if (type != kLz && type != kLzLong && (magic || (type != kHuffman && type != kLh && type != kLrc)))
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

// The bytes to reserve for the output. The header's size can be up to the caller's limit, and data that only starts
// like a header fails within a few bytes. The reserve therefore stops at 64 times the input. The output grows past it
// where real data requires more.
size_t ReserveSize(std::span<const uint8_t> data, const Header& header)
{
    return std::min<size_t>(header.size, 64 * data.size());
}

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
    out.reserve(ReserveSize(data, header));
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

// Huffman coding with 8-bit symbols, or with 4-bit symbols (type 0x24), two to a byte, the first in its low half. The
// tree follows the header. Its first byte n gives the tree's size: (n + 1) * 2 bytes with that byte. The nodes follow,
// the root first. A node that isn't a symbol gives in its low 6 bits where its pair of children is: at the node's
// offset with bit 0 cleared, plus 2, plus twice that value. Bit 7 is set when the first child is a symbol. Bit 6 is set
// when the second is. The bits follow the tree, in 32-bit little-endian words. Each word is read from its top bit down.
// A bit of 0 chooses the first child. A bit of 1 chooses the second. Reaching a symbol writes it and starts again at
// the root.
std::vector<uint8_t> DecodeHuffman(std::span<const uint8_t> data, const Header& header)
{
    if (header.start >= data.size() || data.size() - header.start < (size_t{data[header.start]} + 1) * 2)
    {
        throw Error("Huffman data is truncated");
    }

    const size_t root = header.start + 1;
    const size_t bits = header.start + (size_t{data[header.start]} + 1) * 2;
    const bool nibbles = header.type == kHuffman4;
    std::vector<uint8_t> out;
    out.reserve(ReserveSize(data, header));
    Reader in(data, bits);
    size_t node = root;
    // A byte whose low half is written waits for its high half.
    bool low_written = false;
    while (out.size() < header.size || low_written)
    {
        const uint32_t word = in.Word();
        for (int bit = 31; bit >= 0 && (out.size() < header.size || low_written); --bit)
        {
            const uint32_t branch = word >> bit & 1;
            const uint8_t value = data[node];
            const size_t child = (node & ~size_t{1}) + 2 + (value & 0x3Fu) * 2 + branch;
            if (child >= bits)
            {
                throw Error("Huffman data is damaged");
            }

            if (!(value & (0x80u >> branch)))
            {
                node = child;
                continue;
            }

            node = root;
            if (!nibbles)
            {
                out.push_back(data[child]);
            }
            else if (!low_written)
            {
                out.push_back(data[child] & 0xF);
                low_written = true;
            }
            else
            {
                out.back() = static_cast<uint8_t>(out.back() | (data[child] & 0xF) << 4);
                low_written = false;
            }
        }
    }

    return out;
}

// Run-length coding: a flag byte before each run. With its top bit set, the run is the next byte (flag & 0x7F) + 3
// times. Otherwise it is the next (flag & 0x7F) + 1 bytes. A run stops at the end of the data.
std::vector<uint8_t> DecodeRunLength(std::span<const uint8_t> data, const Header& header)
{
    std::vector<uint8_t> out;
    out.reserve(ReserveSize(data, header));
    Reader in(data, header.start);
    while (out.size() < header.size)
    {
        const uint8_t flag = in.Byte();
        const size_t length = (flag & 0x80) != 0 ? (flag & 0x7Fu) + 3 : (flag & 0x7Fu) + 1;
        const size_t end = std::min(header.size, out.size() + length);
        if ((flag & 0x80) != 0)
        {
            out.resize(end, in.Byte());
            continue;
        }
        while (out.size() < end)
        {
            out.push_back(in.Byte());
        }
    }

    return out;
}

// Reads the bits of LH data: a byte at a time, each from its top bit down. Throws Error at the end of the data.
class BitReader
{
public:
    BitReader(std::span<const uint8_t> data, size_t at) : data_(data), at_(at)
    {
    }

    uint32_t Bit()
    {
        if (bits_ == 0)
        {
            if (at_ >= data_.size())
            {
                throw Error("compressed data is truncated");
            }

            byte_ = data_[at_++];
            bits_ = 8;
        }

        --bits_;
        return byte_ >> bits_ & 1u;
    }

    // `count` bits, the first one highest.
    uint32_t Bits(int count)
    {
        uint32_t value = 0;
        for (int i = 0; i < count; ++i)
        {
            value = value << 1 | Bit();
        }

        return value;
    }

    // The bytes after the last byte read.
    size_t Left() const
    {
        return data_.size() - at_;
    }

private:
    std::span<const uint8_t> data_;
    size_t at_;
    uint8_t byte_ = 0;
    int bits_ = 0;
};

// One of LH's two Huffman trees: the symbols' tree of 9-bit entries, and the tree of 5-bit entries that gives the
// number of bits of a copy's distance. The table starts with its size: a 16-bit little-endian word for 9-bit entries, a
// byte for 5-bit entries. The table takes (size + 1) * 4 bytes with the size. The entries follow, packed from the top
// bit of each byte down, from entry 1, the root. A node that isn't a symbol gives in its low bits (7 bits of 9, 3 bits
// of 5) where its pair of children is: at the node's entry with bit 0 cleared, plus 2, plus twice that value. Its top
// bit is set when the first child is a symbol. The next bit is set when the second is. A bit of 0 chooses the first
// child, a bit of 1 the second. Pokémon Rumble's code reads the table at 0x802e2b90 and walks it in its LH decoder
// (0x802e3158 to 0x802e31e8 and 0x802e3218 to 0x802e32a0, in its main program, content 1 of title 00010001-57505345).
class LhTree
{
public:
    LhTree(std::span<const uint8_t> data, size_t at, int bits) : bits_(bits)
    {
        const size_t field = bits > 8 ? 2 : 1;
        if (at > data.size() || data.size() - at < field)
        {
            throw Error("LH data is truncated");
        }

        const size_t words = bits > 8 ? xsflib::LoadLe16(&data[at]) : data[at];
        const size_t size = (words + 1) * 4;
        if (data.size() - at < size)
        {
            throw Error("LH data is truncated");
        }

        // The game keeps up to 2^(bits + 1) entries, entry 0 unused.
        const size_t count = std::min((size - field) * 8 / static_cast<size_t>(bits), (size_t{1} << (bits + 1)) - 1);
        BitReader in(data, at + field);
        entries_.push_back(0);
        for (size_t i = 0; i < count; ++i)
        {
            entries_.push_back(static_cast<uint16_t>(in.Bits(bits)));
        }

        end_ = at + size;
    }

    // Where the data after the table starts.
    size_t End() const
    {
        return end_;
    }

    uint32_t Decode(BitReader& in) const
    {
        const uint32_t offset_mask = (1u << (bits_ - 2)) - 1;
        const uint32_t first_symbol = 1u << (bits_ - 1);
        size_t node = 1;
        while (true)
        {
            const uint32_t branch = in.Bit();
            const uint32_t value = entries_[node];
            const size_t child = (node & ~size_t{1}) + ((value & offset_mask) + 1) * 2 + branch;
            if (child >= entries_.size())
            {
                throw Error("LH data is damaged");
            }
            if (value & (first_symbol >> branch))
            {
                return entries_[child];
            }

            node = child;
        }
    }

private:
    int bits_;
    std::vector<uint16_t> entries_;
    size_t end_ = 0;
};

// LZ77 with Huffman coding (LH, CXUncompressLH). The symbols' tree and the distances' tree follow the header (see
// LhTree), then the bits of the symbols. A symbol below 0x100 is a literal byte. A symbol of 0x100 or more is a copy of
// (symbol & 0xFF) + 3 bytes. The distances' tree then gives n, the number of bits of the copy's distance less 1. That
// is 0 for n of 0. Otherwise it is a 1 followed by n - 1 more bits. Pokémon Rumble's CXSecureUncompressLH (0x802e3000)
// decodes it. It refuses data with more than 0x20 bytes after the last symbol.
std::vector<uint8_t> DecompressLh(std::span<const uint8_t> data, const Header& header)
{
    const LhTree symbols(data, header.start, 9);
    const LhTree distances(data, symbols.End(), 5);
    BitReader in(data, distances.End());
    std::vector<uint8_t> out;
    out.reserve(ReserveSize(data, header));
    while (out.size() < header.size)
    {
        const uint32_t symbol = symbols.Decode(in);
        if (symbol < 0x100)
        {
            out.push_back(static_cast<uint8_t>(symbol));
            continue;
        }

        const size_t length = (symbol & 0xFF) + 3;
        const uint32_t bits = distances.Decode(in);
        uint32_t value = 0;
        if (bits > 0)
        {
            value = 1;
            for (uint32_t i = 1; i < bits; ++i)
            {
                value = (value << 1 | in.Bit()) & 0xFFFF;
            }
        }

        // The game keeps the distance in 16 bits.
        const size_t distance = (value + 1) & 0xFFFF;
        if (distance == 0 || distance > out.size() || length > header.size - out.size())
        {
            throw Error("LH data is damaged");
        }

        for (size_t i = 0; i < length; ++i)
        {
            const uint8_t byte = out[out.size() - distance];
            out.push_back(byte);
        }
    }

    if (in.Left() > kMaxTrailingBytes)
    {
        throw Error("LH data is damaged");
    }

    return out;
}

// LRC's adaptive model of `bits`-bit symbols (Pokémon Rumble's code at 0x802e3390 and 0x802e3580). Each symbol starts
// at a frequency of 1. Each decoded symbol adds 1 to its frequency. Once the total reaches 0x10000, every frequency
// above 1 is halved. A Fenwick tree keeps the sums of the frequencies.
class LrcModel
{
public:
    explicit LrcModel(int bits) : freq_(size_t{1} << bits, 1), tree_(freq_.size() + 1)
    {
        Rebuild();
    }

    uint32_t Total() const
    {
        return total_;
    }

    uint32_t Frequency(uint32_t symbol) const
    {
        return freq_[symbol];
    }

    // The sum of the frequencies of the symbols below `symbol`.
    uint32_t Low(uint32_t symbol) const
    {
        uint32_t sum = 0;
        for (size_t i = symbol; i > 0; i -= i & (~i + 1))
        {
            sum += tree_[i];
        }

        return sum;
    }

    // The last symbol whose Low is at most `value`.
    uint32_t Find(uint32_t value) const
    {
        size_t at = 0;
        size_t step = std::bit_floor(freq_.size());
        for (; step > 0; step >>= 1)
        {
            if (at + step <= freq_.size() && tree_[at + step] <= value)
            {
                at += step;
                value -= tree_[at];
            }
        }

        return static_cast<uint32_t>(std::min(at, freq_.size() - 1));
    }

    void Update(uint32_t symbol)
    {
        ++freq_[symbol];
        ++total_;
        for (size_t i = symbol + 1; i < tree_.size(); i += i & (~i + 1))
        {
            ++tree_[i];
        }

        if (total_ >= 0x10000)
        {
            for (uint32_t& freq : freq_)
            {
                freq = freq > 1 ? freq >> 1 : freq;
            }
            Rebuild();
        }
    }

private:
    void Rebuild()
    {
        std::fill(tree_.begin(), tree_.end(), 0);
        total_ = 0;
        for (size_t i = 1; i < tree_.size(); ++i)
        {
            tree_[i] += freq_[i - 1];
            total_ += freq_[i - 1];
            const size_t parent = i + (i & (~i + 1));
            if (parent < tree_.size())
            {
                tree_[parent] += tree_[i];
            }
        }
    }

    std::vector<uint32_t> freq_;
    std::vector<uint32_t> tree_;
    uint32_t total_ = 0;
};

// LZ77 with range coding (LRC, CXUncompressLRC). The symbols follow the header in a range coder's bytes: the code
// starts as the first 4 bytes, big-endian, the range as 0x80000000 and the low end as 0. A symbol's slot is the range
// divided by the model's total. The symbol is the last one whose Low is at most (code - low) / slot. The low end then
// grows by the slot times the symbol's Low, and the range becomes the slot times its frequency. While the range is
// below 2^24, the code takes the next byte, and the range and the low end move up 8 bits. The arithmetic wraps at 32
// bits. A symbol of the 9-bit model below 0x100 is a literal byte. A symbol of 0x100 or more is a copy of
// (symbol & 0xFF) + 3 bytes, from the distance that the next symbol of the 12-bit model gives, plus 1. Pokémon Rumble's
// CXSecureUncompressLRC (0x802e3620) decodes it.
class RangeDecoder
{
public:
    RangeDecoder(std::span<const uint8_t> data, size_t at) : data_(data), at_(at)
    {
        if (at > data.size() || data.size() - at < 4)
        {
            throw Error("LRC data is truncated");
        }

        code_ = xsflib::LoadBe32(&data[at]);
        at_ += 4;
    }

    uint32_t Decode(LrcModel& model)
    {
        const uint32_t slot = range_ / model.Total();
        const uint32_t symbol = model.Find((code_ - low_) / slot);
        low_ += slot * model.Low(symbol);
        range_ = slot * model.Frequency(symbol);
        model.Update(symbol);
        while (range_ < 0x1000000)
        {
            // The game reads on past the data here. Up to 4 bytes past it read as 0.
            if (at_ >= data_.size() + 4)
            {
                throw Error("LRC data is truncated");
            }

            code_ = code_ << 8 | (at_ < data_.size() ? data_[at_] : 0u);
            ++at_;
            range_ <<= 8;
            low_ <<= 8;
        }

        return symbol;
    }

    // Whether the decoder has read past the data.
    bool Past() const
    {
        return at_ > data_.size();
    }

    // The bytes after the last byte read.
    size_t Left() const
    {
        return Past() ? 0 : data_.size() - at_;
    }

private:
    std::span<const uint8_t> data_;
    size_t at_;
    uint32_t code_ = 0;
    uint32_t range_ = 0x80000000;
    uint32_t low_ = 0;
};

std::vector<uint8_t> DecompressLrc(std::span<const uint8_t> data, const Header& header)
{
    LrcModel symbols(9);
    LrcModel distances(12);
    RangeDecoder in(data, header.start);
    std::vector<uint8_t> out;
    out.reserve(ReserveSize(data, header));
    while (out.size() < header.size)
    {
        const uint32_t symbol = in.Decode(symbols);
        if (symbol < 0x100)
        {
            out.push_back(static_cast<uint8_t>(symbol));
            continue;
        }

        // The game checks a copy's length and distance, and that the data hasn't ended.
        const size_t length = (symbol & 0xFF) + 3;
        const size_t distance = in.Decode(distances) + size_t{1};
        if (length > header.size - out.size() || distance > out.size() || in.Past())
        {
            throw Error("LRC data is damaged");
        }

        for (size_t i = 0; i < length; ++i)
        {
            const uint8_t byte = out[out.size() - distance];
            out.push_back(byte);
        }
    }

    if (in.Left() > kMaxTrailingBytes)
    {
        throw Error("LRC data is damaged");
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
        throw Error("the data isn't in one of the CX library's formats: LZ77, Huffman coding, LH or LRC");
    }
    if (header->size > max_size)
    {
        throw Error("compressed data is too large");
    }

    switch (header->type)
    {
    case kHuffman:
        return DecodeHuffman(data, *header);
    case kLh:
        return DecompressLh(data, *header);
    case kLrc:
        return DecompressLrc(data, *header);
    default:
        return DecompressLz(data, *header);
    }
}

std::vector<uint8_t> DecompressBody(std::span<const uint8_t> data, BodyFormat format, size_t size)
{
    switch (format)
    {
    case BodyFormat::kHuffman4:
        return DecodeHuffman(data, Header{kHuffman4, size, 0});
    case BodyFormat::kHuffman8:
        return DecodeHuffman(data, Header{kHuffman, size, 0});
    case BodyFormat::kRunLength:
        return DecodeRunLength(data, Header{kRunLength, size, 0});
    default:
        return DecompressLz(data, Header{kLz, size, 0});
    }
}

} // namespace ntdlib::disc
