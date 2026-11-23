// SPDX-License-Identifier: Zlib

// Minimal zlib decoder, adapted from puff, Mark Adler's reference inflate in the zlib sources (contrib/puff). This is
// an altered version: the DEFLATE decoder is rewritten as a C++ class, and ZlibDecompress adds the zlib header and
// Adler-32 checks. DecodeDeflate decodes a raw DEFLATE stream. puff's copyright notice follows.

/*
  Copyright (C) 2002-2013 Mark Adler, all rights reserved
  version 2.3, 21 Jan 2013

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the author be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Mark Adler    madler@alumni.caltech.edu
 */

#include "ntdlib/inflate.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ntdlib
{
namespace
{

constexpr int kMaxBits = 15;

// A canonical Huffman code, stored as puff stores it: the number of codes of each length, and the symbols in code
// order.
struct Huffman
{
    uint16_t count[kMaxBits + 1] = {};
    uint16_t symbol[288] = {};
};

// Decodes a DEFLATE stream (RFC 1951) into `out`, one block at a time.
class Inflater
{
public:
    Inflater(const uint8_t* in, size_t size, std::vector<uint8_t>& out) : in_(in), size_(size), out_(out)
    {
    }

    // Decodes blocks up to the last one. Returns false and sets `error` if the stream is corrupt or cut short.
    bool Run(std::string& error)
    {
        int last = 0;
        do
        {
            last = Bits(1);
            const int type = Bits(2);
            bool ok = false;
            switch (type)
            {
            case 0:
                ok = Stored();
                break;

            case 1:
                ok = Fixed();
                break;

            case 2:
                ok = Dynamic();
                break;

            default:
                error = "invalid DEFLATE block type";
                return false;
            }

            if (!ok || overrun_)
            {
                error = overrun_ ? "truncated DEFLATE stream" : "corrupt DEFLATE stream";
                return false;
            }
        } while (last == 0);

        return true;
    }

    // Returns the number of bytes of input the stream took up, once Run() has finished.
    size_t BytePosition() const
    {
        return pos_;
    }

private:
    // The codes RFC 1951 fixes for blocks of type 1: 8 or 9 bits a literal, 7 or 8 a length, and 5 a distance.
    struct FixedCodes
    {
        Huffman length_code;
        Huffman distance_code;
    };

    // Reads the next `count` bits of input and returns them, the first bit lowest.
    int Bits(int count)
    {
        uint32_t value = bit_buf_;
        while (bit_count_ < count)
        {
            if (pos_ >= size_)
            {
                overrun_ = true;
                return 0;
            }

            value |= static_cast<uint32_t>(in_[pos_++]) << bit_count_;
            bit_count_ += 8;
        }

        bit_buf_ = value >> count;
        bit_count_ -= count;

        return static_cast<int>(value & ((1u << count) - 1));
    }

    // Decodes a stored block: its length, the length inverted, and that many bytes as they are.
    bool Stored()
    {
        bit_buf_ = 0;
        bit_count_ = 0;

        if (pos_ + 4 > size_)
        {
            overrun_ = true;
            return false;
        }

        const unsigned length = in_[pos_] | (in_[pos_ + 1] << 8);
        const unsigned inverted = in_[pos_ + 2] | (in_[pos_ + 3] << 8);
        pos_ += 4;

        if (length != (~inverted & 0xFFFFu))
        {
            return false;
        }
        if (pos_ + length > size_)
        {
            overrun_ = true;
            return false;
        }

        out_.insert(out_.end(), in_ + pos_, in_ + pos_ + length);
        pos_ += length;

        return true;
    }

    // Decodes a symbol of `code`, a bit at a time. Returns -1 for an invalid code or the end of the input.
    int Decode(const Huffman& code)
    {
        int bits = 0;
        int first = 0;
        int index = 0;
        for (int length = 1; length <= kMaxBits; length++)
        {
            bits |= Bits(1);
            if (overrun_)
            {
                return -1;
            }

            const int count = code.count[length];
            if (bits - count < first)
            {
                return code.symbol[index + (bits - first)];
            }

            index += count;
            first += count;
            first <<= 1;
            bits <<= 1;
        }

        return -1;
    }

    // Builds `code` from the code lengths of `symbols` symbols. Returns <0 for an over-subscribed set, 0 for complete,
    // >0 for incomplete.
    static int Construct(Huffman& code, const uint8_t* lengths, int symbols)
    {
        for (uint16_t& count : code.count)
        {
            count = 0;
        }

        for (int symbol = 0; symbol < symbols; symbol++)
        {
            code.count[lengths[symbol]]++;
        }
        if (code.count[0] == symbols)
        {
            return 0;
        }

        int left = 1;
        for (int length = 1; length <= kMaxBits; length++)
        {
            left <<= 1;
            left -= code.count[length];
            if (left < 0)
            {
                return left;
            }
        }

        uint16_t offsets[kMaxBits + 1];
        offsets[1] = 0;
        for (int length = 1; length < kMaxBits; length++)
        {
            offsets[length + 1] = static_cast<uint16_t>(offsets[length] + code.count[length]);
        }

        for (int symbol = 0; symbol < symbols; symbol++)
        {
            if (lengths[symbol] != 0)
            {
                code.symbol[offsets[lengths[symbol]]++] = static_cast<uint16_t>(symbol);
            }
        }

        return left;
    }

    // Decodes literals and length/distance pairs with the given codes, up to the end of the block.
    bool Codes(const Huffman& length_code, const Huffman& distance_code)
    {
        static const uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const uint16_t kDistanceBase[30] = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                                   33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                                   1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const uint8_t kDistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                   6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

        for (;;)
        {
            int symbol = Decode(length_code);
            if (symbol < 0)
            {
                return false;
            }

            if (symbol < 256)
            {
                out_.push_back(static_cast<uint8_t>(symbol));
            }
            else if (symbol == 256)
            {
                return true;
            }
            else
            {
                symbol -= 257;
                if (symbol >= 29)
                {
                    return false;
                }

                const size_t length = kLengthBase[symbol] + Bits(kLengthExtra[symbol]);
                const int distance_symbol = Decode(distance_code);
                if (distance_symbol < 0 || distance_symbol >= 30)
                {
                    return false;
                }

                const size_t distance = kDistanceBase[distance_symbol] + Bits(kDistanceExtra[distance_symbol]);
                if (overrun_ || distance > out_.size())
                {
                    return false;
                }

                const size_t from = out_.size() - distance;
                for (size_t i = 0; i < length; i++)
                {
                    out_.push_back(out_[from + i]);
                }
            }
        }
    }

    // Returns the fixed codes, built from the code lengths that RFC 1951 gives them.
    static FixedCodes BuildFixedCodes()
    {
        FixedCodes codes;
        uint8_t lengths[288];
        int symbol = 0;
        for (; symbol < 144; symbol++)
        {
            lengths[symbol] = 8;
        }
        for (; symbol < 256; symbol++)
        {
            lengths[symbol] = 9;
        }
        for (; symbol < 280; symbol++)
        {
            lengths[symbol] = 7;
        }
        for (; symbol < 288; symbol++)
        {
            lengths[symbol] = 8;
        }
        Construct(codes.length_code, lengths, 288);

        for (symbol = 0; symbol < 30; symbol++)
        {
            lengths[symbol] = 5;
        }
        Construct(codes.distance_code, lengths, 30);

        return codes;
    }

    // Decodes a block coded with the fixed codes of RFC 1951.
    bool Fixed()
    {
        // Built on first use. The initialisation of a local static is thread-safe.
        static const FixedCodes kFixed = BuildFixedCodes();
        return Codes(kFixed.length_code, kFixed.distance_code);
    }

    // Decodes a block with dynamic codes. The block starts with the code lengths, Huffman coded themselves.
    bool Dynamic()
    {
        static const uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

        uint8_t lengths[320] = {};
        const int literals = Bits(5) + 257;
        const int distances = Bits(5) + 1;
        const int code_lengths = Bits(4) + 4;
        if (overrun_ || literals > 286 || distances > 30)
        {
            return false;
        }

        // The lengths of the code-length code.
        int index = 0;
        for (; index < code_lengths; index++)
        {
            lengths[kOrder[index]] = static_cast<uint8_t>(Bits(3));
        }
        for (; index < 19; index++)
        {
            lengths[kOrder[index]] = 0;
        }

        Huffman length_code;
        Huffman distance_code;
        if (Construct(length_code, lengths, 19) != 0)
        {
            return false;
        }

        // The code lengths of the literal/length code and the distance code. A symbol under 16 is a length. 16 repeats
        // the last length 3 to 6 times, and 17 and 18 repeat a 0.
        index = 0;
        while (index < literals + distances)
        {
            int symbol = Decode(length_code);
            if (symbol < 0)
            {
                return false;
            }

            if (symbol < 16)
            {
                lengths[index++] = static_cast<uint8_t>(symbol);
            }
            else
            {
                int length = 0;
                if (symbol == 16)
                {
                    if (index == 0)
                    {
                        return false;
                    }

                    length = lengths[index - 1];
                    symbol = 3 + Bits(2);
                }
                else if (symbol == 17)
                {
                    symbol = 3 + Bits(3);
                }
                else
                {
                    symbol = 11 + Bits(7);
                }
                if (index + symbol > literals + distances)
                {
                    return false;
                }

                while (symbol--)
                {
                    lengths[index++] = static_cast<uint8_t>(length);
                }
            }
        }

        if (lengths[256] == 0)
        {
            return false;
        }

        // Build the literal/length code and the distance code. Either one can only be incomplete if it's a single code
        // of one bit.
        int result = Construct(length_code, lengths, literals);
        if (result < 0 || (result > 0 && literals != length_code.count[0] + length_code.count[1]))
        {
            return false;
        }

        result = Construct(distance_code, lengths + literals, distances);
        if (result < 0 || (result > 0 && distances != distance_code.count[0] + distance_code.count[1]))
        {
            return false;
        }

        return Codes(length_code, distance_code);
    }

    const uint8_t* in_;
    size_t size_;
    size_t pos_ = 0;
    uint32_t bit_buf_ = 0;
    int bit_count_ = 0;
    bool overrun_ = false;
    std::vector<uint8_t>& out_;
};

// Returns the zlib checksum of `data`. The sums are reduced every 5552 bytes, the most that can't overflow 32 bits.
uint32_t Adler32(const std::vector<uint8_t>& data)
{
    uint32_t sum = 1;
    uint32_t sum_of_sums = 0;
    size_t start = 0;
    while (start < data.size())
    {
        const size_t chunk = std::min<size_t>(data.size() - start, 5552);
        for (size_t i = 0; i < chunk; i++)
        {
            sum += data[start + i];
            sum_of_sums += sum;
        }

        sum %= 65521;
        sum_of_sums %= 65521;
        start += chunk;
    }

    return (sum_of_sums << 16) | sum;
}

} // namespace

bool DecodeDeflate(std::span<const uint8_t> in, std::vector<uint8_t>& out, std::size_t& used, std::string& error)
{
    out.clear();
    Inflater inflater(in.data(), in.size(), out);
    if (!inflater.Run(error))
    {
        return false;
    }

    used = inflater.BytePosition();

    return true;
}

bool ZlibDecompress(const uint8_t* data, size_t size, std::vector<uint8_t>& out, std::string& error)
{
    out.clear();

    if (size < 6)
    {
        error = "zlib stream too short";
        return false;
    }

    const unsigned method = data[0];
    const unsigned flags = data[1];
    if ((method & 0x0F) != 8 || ((method << 8) | flags) % 31 != 0)
    {
        error = "not a zlib stream";
        return false;
    }
    if ((flags & 0x20) != 0)
    {
        error = "zlib preset dictionaries aren't supported";
        return false;
    }

    std::size_t used = 0;
    if (!DecodeDeflate({data + 2, size - 2}, out, used, error))
    {
        return false;
    }

    const size_t end = 2 + used;
    if (end + 4 > size)
    {
        error = "zlib stream ends before its checksum";
        return false;
    }

    const uint32_t expected = (static_cast<uint32_t>(data[end]) << 24) | (static_cast<uint32_t>(data[end + 1]) << 16) |
                              (static_cast<uint32_t>(data[end + 2]) << 8) | data[end + 3];
    if (expected != Adler32(out))
    {
        error = "zlib checksum mismatch";
        return false;
    }

    return true;
}

} // namespace ntdlib
