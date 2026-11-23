// SPDX-License-Identifier: MIT

// Compressors of the CX library's LH (type 0x40) and LRC (type 0x50) formats, for tests of the decompressors in
// ntdlib/disc/cx.h. That header describes the formats. Each compressor parses the data greedily into literal bytes and
// copies of 3 to 258 bytes. LH codes the parse with Huffman trees that it lays out level by level. A tree that doesn't
// fit the format's offsets throws std::length_error. A test should therefore use data with few distinct symbols. LRC
// codes the parse with the format's adaptive models and a range coder that carries into the bytes it has written.

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <queue>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ntdlib/rounding.h"

namespace ntdlib::testing::cx_encoders
{

// A literal byte, or a copy of `length` bytes from `distance` back.
struct Token
{
    bool copy = false;
    uint8_t literal = 0;
    uint32_t length = 0;
    uint32_t distance = 0;
};

// The greedy parse of `data`: at each byte, the longest copy of 3 to 258 bytes from up to `max_distance` back, among
// the last 64 places that start with the same 3 bytes, or a literal byte.
inline std::vector<Token> ParseLz(std::span<const uint8_t> data, uint32_t max_distance)
{
    constexpr size_t kMinLength = 3;
    constexpr size_t kMaxLength = 258;
    constexpr size_t kCandidates = 64;
    std::map<uint32_t, std::deque<size_t>> places;
    const auto key = [&data](size_t at)
    {
        return uint32_t{data[at]} << 16 | uint32_t{data[at + 1]} << 8 | data[at + 2];
    };
    const auto remember = [&](size_t at)
    {
        if (at + kMinLength <= data.size())
        {
            std::deque<size_t>& list = places[key(at)];
            list.push_back(at);
            if (list.size() > kCandidates)
            {
                list.pop_front();
            }
        }
    };

    std::vector<Token> tokens;
    size_t at = 0;
    while (at < data.size())
    {
        size_t best_length = 0;
        size_t best_from = 0;
        const auto found = at + kMinLength <= data.size() ? places.find(key(at)) : places.end();
        if (found != places.end())
        {
            for (const size_t from : found->second)
            {
                if (at - from > max_distance)
                {
                    continue;
                }

                size_t length = 0;
                while (length < kMaxLength && at + length < data.size() && data[from + length] == data[at + length])
                {
                    ++length;
                }
                if (length >= best_length)
                {
                    best_length = length;
                    best_from = from;
                }
            }
        }

        if (best_length >= kMinLength)
        {
            tokens.push_back({true, 0, static_cast<uint32_t>(best_length), static_cast<uint32_t>(at - best_from)});
            for (size_t i = 0; i < best_length; ++i)
            {
                remember(at + i);
            }
            at += best_length;
        }
        else
        {
            tokens.push_back({false, data[at], 0, 0});
            remember(at);
            ++at;
        }
    }

    return tokens;
}

// The header: the type, then the size in 24 bits, or 0 and the size in 32 bits.
inline std::vector<uint8_t> CxHeader(uint8_t type, size_t size)
{
    std::vector<uint8_t> out = {type};
    if (size > 0 && size < 0x1000000)
    {
        out.insert(out.end(),
                   {static_cast<uint8_t>(size), static_cast<uint8_t>(size >> 8), static_cast<uint8_t>(size >> 16)});
    }
    else
    {
        out.insert(out.end(), {0, 0, 0, static_cast<uint8_t>(size), static_cast<uint8_t>(size >> 8),
                               static_cast<uint8_t>(size >> 16), static_cast<uint8_t>(size >> 24)});
    }

    return out;
}

// Writes bits from the top bit of each byte down.
class BitWriter
{
public:
    void Put(uint32_t value, int count)
    {
        for (int i = count - 1; i >= 0; --i)
        {
            if (used_ == 0)
            {
                bytes_.push_back(0);
            }

            bytes_.back() |= static_cast<uint8_t>((value >> i & 1u) << (7 - used_));
            used_ = (used_ + 1) % 8;
        }
    }

    const std::vector<uint8_t>& Bytes() const
    {
        return bytes_;
    }

private:
    std::vector<uint8_t> bytes_;
    int used_ = 0;
};

// One of LH's Huffman trees for the symbols that `counts` gives, by symbol: the table as the format stores it, and the
// code of each symbol, its bits from the root.
struct LhTreeCode
{
    std::vector<uint8_t> table;
    std::map<uint32_t, std::vector<bool>> codes;
};

// A node of a Huffman tree: a symbol, or a node with two children.
struct LhNode
{
    uint64_t count;
    uint32_t symbol;
    int children[2];
};

// The Huffman tree of the symbols that `counts` gives: the nodes, and the index of the root.
inline std::pair<std::vector<LhNode>, int> LhHuffman(const std::map<uint32_t, uint64_t>& counts)
{
    std::vector<LhNode> nodes;
    using Entry = std::pair<uint64_t, int>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> queue;
    for (const auto& [symbol, count] : counts)
    {
        nodes.push_back({count, symbol, {-1, -1}});
        queue.push({count, static_cast<int>(nodes.size()) - 1});
    }
    while (queue.size() > 1)
    {
        const Entry first = queue.top();
        queue.pop();
        const Entry second = queue.top();
        queue.pop();
        nodes.push_back({first.first + second.first, 0, {first.second, second.second}});
        queue.push({nodes.back().count, static_cast<int>(nodes.size()) - 1});
    }

    return {nodes, queue.top().second};
}

// LH's table of a tree's `entries` of `bits` bits: the size field, then the entries from entry 1, padded to a multiple
// of 4 bytes.
inline std::vector<uint8_t> LhTable(const std::vector<uint32_t>& entries, int bits)
{
    const size_t field = bits > 8 ? 2 : 1;
    BitWriter packed;
    for (size_t i = 1; i < entries.size(); ++i)
    {
        packed.Put(entries[i], bits);
    }

    const size_t size = ntdlib::AlignUp(field + packed.Bytes().size(), 4);
    const size_t words = size / 4 - 1;
    if (words >= (size_t{1} << (field * 8)))
    {
        throw std::length_error("the tree doesn't fit LH's table");
    }

    std::vector<uint8_t> table = {static_cast<uint8_t>(words)};
    if (field == 2)
    {
        table.push_back(static_cast<uint8_t>(words >> 8));
    }
    table.insert(table.end(), packed.Bytes().begin(), packed.Bytes().end());
    table.resize(size, 0);

    return table;
}

// The tree of the symbols that `counts` gives, with table entries of `bits` bits.
inline LhTreeCode BuildLhTree(std::map<uint32_t, uint64_t> counts, int bits)
{
    // A tree has at least two symbols.
    while (counts.size() < 2)
    {
        counts.emplace(counts.empty() || counts.begin()->first != 0 ? 0 : 1, 1);
    }

    const auto [nodes, root] = LhHuffman(counts);

    // Level by level: the root is entry 1. Each node that isn't a symbol gets the next free pair of entries for its
    // children.
    const uint32_t max_offset = (1u << (bits - 2)) - 1;
    const uint32_t first_symbol = 1u << (bits - 1);
    std::vector<uint32_t> entries(2, 0);
    LhTreeCode tree;
    std::deque<std::pair<int, size_t>> pending = {{root, 1}}; // a node and its entry
    std::map<int, std::vector<bool>> paths = {{root, {}}};
    size_t next_pair = 1;
    while (!pending.empty())
    {
        const auto [index, entry] = pending.front();
        pending.pop_front();
        const LhNode& node = nodes[static_cast<size_t>(index)];
        const size_t pair = next_pair++;
        const size_t offset = pair - entry / 2 - 1;
        if (offset > max_offset)
        {
            throw std::length_error("the tree doesn't fit LH's offsets");
        }

        entries.resize(pair * 2 + 2, 0);
        uint32_t value = static_cast<uint32_t>(offset);
        for (int branch = 0; branch < 2; ++branch)
        {
            const int child = node.children[branch];
            const LhNode& child_node = nodes[static_cast<size_t>(child)];
            std::vector<bool> path = paths[index];
            path.push_back(branch == 1);
            if (child_node.children[0] < 0)
            {
                value |= first_symbol >> branch;
                entries[pair * 2 + static_cast<size_t>(branch)] = child_node.symbol;
                tree.codes[child_node.symbol] = path;
            }
            else
            {
                paths[child] = path;
                pending.push_back({child, pair * 2 + static_cast<size_t>(branch)});
            }
        }
        entries[entry] = value;
    }

    tree.table = LhTable(entries, bits);

    return tree;
}

// Codes `tokens` in LH, for data of `size` bytes. The tokens' copies may be any that the format supports: 3 to 258
// bytes, from 1 to 0xFFFF bytes back.
inline std::vector<uint8_t> EncodeLh(const std::vector<Token>& tokens, size_t size)
{
    std::map<uint32_t, uint64_t> symbol_counts;
    std::map<uint32_t, uint64_t> bit_counts;
    for (const Token& token : tokens)
    {
        if (token.copy)
        {
            ++symbol_counts[0x100 + token.length - 3];
            ++bit_counts[static_cast<uint32_t>(std::bit_width(token.distance - 1))];
        }
        else
        {
            ++symbol_counts[token.literal];
        }
    }

    const LhTreeCode symbols = BuildLhTree(symbol_counts, 9);
    const LhTreeCode distances = BuildLhTree(bit_counts, 5);
    BitWriter out;
    const auto put_code = [&out](const std::vector<bool>& code)
    {
        for (const bool bit : code)
        {
            out.Put(bit ? 1 : 0, 1);
        }
    };
    for (const Token& token : tokens)
    {
        if (!token.copy)
        {
            put_code(symbols.codes.at(token.literal));
            continue;
        }

        put_code(symbols.codes.at(0x100 + token.length - 3));
        const uint32_t value = token.distance - 1;
        const int bits = std::bit_width(value);
        put_code(distances.codes.at(static_cast<uint32_t>(bits)));
        if (bits > 1)
        {
            out.Put(value, bits - 1);
        }
    }

    std::vector<uint8_t> result = CxHeader(0x40, size);
    result.insert(result.end(), symbols.table.begin(), symbols.table.end());
    result.insert(result.end(), distances.table.begin(), distances.table.end());
    result.insert(result.end(), out.Bytes().begin(), out.Bytes().end());
    result.resize(ntdlib::AlignUp(result.size(), 4), 0);

    return result;
}

// Compresses `data` in LH: copies from up to 0xFFFF bytes back.
inline std::vector<uint8_t> CompressLh(std::span<const uint8_t> data)
{
    return EncodeLh(ParseLz(data, 0xFFFF), data.size());
}

// LRC's adaptive model of `bits`-bit symbols, as the format keeps it: each symbol's frequency, the sum of the
// frequencies below it, and the total.
class LrcEncoderModel
{
public:
    explicit LrcEncoderModel(int bits) : freq_(size_t{1} << bits, 1), low_(freq_.size())
    {
        for (size_t i = 0; i < low_.size(); ++i)
        {
            low_[i] = static_cast<uint32_t>(i);
        }
        total_ = static_cast<uint32_t>(freq_.size());
    }

    uint32_t Total() const
    {
        return total_;
    }

    uint32_t Low(uint32_t symbol) const
    {
        return low_[symbol];
    }

    uint32_t Frequency(uint32_t symbol) const
    {
        return freq_[symbol];
    }

    void Update(uint32_t symbol)
    {
        ++freq_[symbol];
        ++total_;
        for (size_t i = symbol + 1; i < low_.size(); ++i)
        {
            ++low_[i];
        }

        if (total_ >= 0x10000)
        {
            total_ = 0;
            for (size_t i = 0; i < freq_.size(); ++i)
            {
                freq_[i] = freq_[i] > 1 ? freq_[i] >> 1 : freq_[i];
                low_[i] = i == 0 ? 0 : low_[i - 1] + freq_[i - 1];
                total_ += freq_[i];
            }
        }
    }

private:
    std::vector<uint32_t> freq_;
    std::vector<uint32_t> low_;
    uint32_t total_ = 0;
};

// Codes `tokens` in LRC, for data of `size` bytes. The tokens' copies may be any that the format holds: 3 to 258
// bytes, from 1 to 4,096 bytes back.
inline std::vector<uint8_t> EncodeLrc(const std::vector<Token>& tokens, size_t size)
{
    std::vector<uint8_t> out = CxHeader(0x50, size);
    const size_t start = out.size();
    uint64_t low = 0;
    uint32_t range = 0x80000000;
    const auto encode = [&](LrcEncoderModel& model, uint32_t symbol)
    {
        const uint32_t slot = range / model.Total();
        low += uint64_t{slot} * model.Low(symbol);
        range = slot * model.Frequency(symbol);
        model.Update(symbol);
        if (low >= (uint64_t{1} << 32))
        {
            low -= uint64_t{1} << 32;
            size_t at = out.size();
            while (at > start && out[at - 1] == 0xFF)
            {
                out[--at] = 0;
            }
            if (at == start)
            {
                throw std::logic_error("the range coder carried past its first byte");
            }
            ++out[at - 1];
        }
        while (range < 0x1000000)
        {
            out.push_back(static_cast<uint8_t>(low >> 24));
            low = (low << 8) & 0xFFFFFFFF;
            range <<= 8;
        }
    };

    LrcEncoderModel symbols(9);
    LrcEncoderModel distances(12);
    for (const Token& token : tokens)
    {
        if (token.copy)
        {
            encode(symbols, 0x100 + token.length - 3);
            encode(distances, token.distance - 1);
        }
        else
        {
            encode(symbols, token.literal);
        }
    }

    for (int shift = 24; shift >= 0; shift -= 8)
    {
        out.push_back(static_cast<uint8_t>(low >> shift));
    }
    out.resize(ntdlib::AlignUp(out.size(), 4), 0);

    return out;
}

// Compresses `data` in LRC: copies from up to 4,096 bytes back.
inline std::vector<uint8_t> CompressLrc(std::span<const uint8_t> data)
{
    return EncodeLrc(ParseLz(data, 0x1000), data.size());
}

} // namespace ntdlib::testing::cx_encoders
