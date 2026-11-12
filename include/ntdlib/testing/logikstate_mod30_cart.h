// SPDX-License-Identifier: MIT

// Synthetic cartridges for the tests of the GBAMOD30 modules of Logik State's later drivers: modules in each of the
// three layouts with packed patterns, a bank of samples after them, and a table of linear frequencies.

#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/logikstate/mod30.h"
#include "ntdlib/logikstate/mod30_player.h"
#include "ntdlib/testing/agb_cart.h"

namespace ntdlib::testing::logikstate_mod30_cart
{

using ntdlib::agb::kRomBase;
using ntdlib::logikstate::HeaderLayout;
using ntdlib::logikstate::kMod30BlockHeader;
using ntdlib::logikstate::kMod30BlockSize;
using ntdlib::logikstate::kMod30BlockStreamB;
using ntdlib::logikstate::kMod30BlockStreamC;
using ntdlib::logikstate::kMod30BlockStreamEnd;
using ntdlib::logikstate::kMod30Channels;
using ntdlib::logikstate::kMod30EndOfOrders;
using ntdlib::logikstate::kMod30LongHeader;
using ntdlib::logikstate::kMod30NoNote;
using ntdlib::logikstate::kMod30NoteShift;
using ntdlib::logikstate::kMod30Rate0;
using ntdlib::logikstate::kMod30RateWord;
using ntdlib::logikstate::kMod30Samples;
using ntdlib::logikstate::kMod30ShortHeader;
using ntdlib::logikstate::kMod30Signature1;
using ntdlib::logikstate::kMod30Signature2;
using ntdlib::logikstate::Mod30HeaderLayout;
using ntdlib::logikstate::Mod30Layout;
using ntdlib::testing::agb_files::CartImage;

// A cell of a test pattern.
struct TestCell
{
    uint16_t note = kMod30NoNote;
    uint8_t sample = 0; // from 1, or 0 for none
    uint8_t volume = 0; // the volume plus 1, or 0 for none
    uint8_t effect = 0;
    uint8_t operand = 0;
};

// A test module: each pattern is a list of rows, and each row a cell for each channel.
struct TestModule
{
    Mod30Layout layout = Mod30Layout::kB;
    uint32_t channels = 1;
    uint32_t rows = 4;
    uint32_t speed = 6;
    uint32_t tempo = 125;
    bool amiga = false;
    std::vector<uint8_t> orders = {0};
    std::vector<std::vector<std::vector<TestCell>>> patterns;
};

// A sample of the test bank, with points that rise from -100 by 1 a point.
struct TestSample
{
    uint32_t length = 0;
    int16_t fine_tune = 0;
    uint16_t volume = 64;
    uint16_t loop_start = 0;
    uint16_t loop_length = 0;
    int16_t relative_note = 1;
};

// Where the test cart keeps its parts.
constexpr uint32_t kLinearTable = kRomBase + 0x200; // 768 halfwords
constexpr uint32_t kFirstModule = kRomBase + 0x1000;

// The linear table's entries: twice 8363 at place 0, rising by an octave over the 768 places. The drivers shift an
// entry left by 2 and then right by 7 less the octave. A sample with relative note 1 then plays note 48 at 8363 Hz.
inline uint16_t LinearEntry(int place)
{
    return uint16_t(std::lround(2 * 8363.0 * std::exp2(place / 768.0)));
}

// Appends a stream of runs to `out`: a count of rows and the value of each run, then a byte of 0.
inline void AppendStream(std::vector<uint8_t>& out, const std::vector<uint16_t>& rows, bool wide)
{
    const auto put = [&](uint8_t count, uint16_t value)
    {
        out.push_back(count);
        if (wide)
        {
            out.push_back(uint8_t(value >> 8));
        }
        out.push_back(uint8_t(value));
    };
    for (size_t i = 0; i < rows.size();)
    {
        size_t n = 1;
        while (i + n < rows.size() && rows[i + n] == rows[i] && n < 255)
        {
            n++;
        }
        put(uint8_t(n), rows[i]);
        i += n;
    }
    out.push_back(0);
}

// Returns a channel's block of a pattern: its five words, then its three streams.
inline std::vector<uint8_t> MakeBlock(const std::vector<std::vector<TestCell>>& pattern, uint32_t channel)
{
    std::vector<uint16_t> a, b, c;
    for (const std::vector<TestCell>& row : pattern)
    {
        const TestCell cell = channel < row.size() ? row[channel] : TestCell{};
        a.push_back(uint16_t(cell.note << kMod30NoteShift | cell.sample));
        b.push_back(cell.volume);
        c.push_back(uint16_t(cell.effect << 8 | cell.operand));
    }
    std::vector<uint8_t> data;
    AppendStream(data, a, true);
    const uint32_t stream_b = uint32_t(data.size());
    AppendStream(data, b, false);
    const uint32_t stream_c = uint32_t(data.size());
    AppendStream(data, c, true);
    const uint32_t stream_end = uint32_t(data.size());
    while (data.size() % 4 != 0)
    {
        data.push_back(0);
    }

    std::vector<uint8_t> block(kMod30BlockHeader, 0);
    const auto word = [&](uint32_t at, uint32_t v)
    {
        for (uint32_t i = 0; i < 4; i++)
        {
            block[at + i] = uint8_t(v >> (8 * i));
        }
    };
    word(kMod30BlockStreamB, stream_b);
    word(kMod30BlockStreamC, stream_c);
    word(kMod30BlockStreamEnd, stream_end);
    word(kMod30BlockSize, uint32_t(data.size()));
    block.insert(block.end(), data.begin(), data.end());

    return block;
}

// Writes `module` at `address` and returns the address after it. Layouts A and C get a table of sizes, and layout B one
// of offsets.
inline uint32_t PutModule(CartImage& cart, uint32_t address, const TestModule& module)
{
    const Mod30HeaderLayout& layout = HeaderLayout(module.layout);
    cart.Put32(address, kMod30Signature1);
    cart.Put32(address + 4, kMod30Signature2);
    cart.Put32(address + kMod30Channels, module.channels);
    if (module.layout != Mod30Layout::kB)
    {
        cart.Put32(address + kMod30RateWord, kMod30Rate0);
    }
    cart.Put32(address + layout.order_count, uint32_t(module.orders.size()));
    cart.Put32(address + layout.pattern_count, uint32_t(module.patterns.size()));
    cart.Put32(address + layout.rows, module.rows);
    cart.Put32(address + layout.amiga, module.amiga ? 1 : 0);
    cart.Put32(address + layout.tempo, module.tempo);
    cart.Put32(address + layout.speed, module.speed);
    for (uint32_t i = 0; i < 256; i++)
    {
        const bool listed = i < module.orders.size();
        const uint8_t end = module.layout == Mod30Layout::kA ? 0 : kMod30EndOfOrders;
        cart.Put8(address + layout.orders + i, listed ? module.orders[i] : end);
    }

    uint32_t offset = 0;
    for (size_t p = 0; p < module.patterns.size(); p++)
    {
        std::vector<uint8_t> bytes;
        for (uint32_t c = 0; c < module.channels; c++)
        {
            const std::vector<uint8_t> block = MakeBlock(module.patterns[p], c);
            bytes.insert(bytes.end(), block.begin(), block.end());
        }
        cart.Put32(address + layout.pattern_table + 4 * uint32_t(p),
                   module.layout == Mod30Layout::kB ? offset : uint32_t(bytes.size()));
        cart.PutBytes(address + layout.patterns + offset, bytes);
        offset += uint32_t(bytes.size());
    }

    return address + layout.patterns + offset;
}

// Writes a bank at `address` with headers of `header_size` bytes, and returns the address after its points.
inline uint32_t PutBank(CartImage& cart, uint32_t address, const std::vector<TestSample>& samples, uint32_t header_size)
{
    const bool long_headers = header_size == kMod30LongHeader;
    uint32_t points = address + uint32_t(kMod30Samples) * header_size;
    for (size_t i = 0; i < samples.size(); i++)
    {
        const TestSample& s = samples[i];
        const uint32_t h = address + uint32_t(i) * header_size;
        const uint32_t fields = h + (long_headers ? 4 : 2);
        if (long_headers)
        {
            cart.Put32(h, s.length);
        }
        else
        {
            cart.Put16(h, uint16_t(s.length));
        }
        cart.Put16(fields, uint16_t(s.fine_tune));
        cart.Put16(fields + 2, s.volume);
        cart.Put16(fields + 4, s.loop_start);
        cart.Put16(fields + 6, s.loop_length);
        cart.Put16(fields + 8, uint16_t(s.relative_note));
        for (uint32_t k = 0; k < s.length; k++)
        {
            cart.Put8(points + k, uint8_t(int8_t(int(k % 200) - 100)));
        }
        points += s.length;
    }

    return points;
}

// A cart with the linear table, `modules` from kFirstModule on, each at a word's start, and a bank of `samples` after
// the last of them. The modules' addresses go to `addresses`, and the bank's to `bank`.
inline CartImage MakeCart(const std::vector<TestModule>& modules, const std::vector<TestSample>& samples,
                          uint32_t header_size, std::vector<uint32_t>& addresses, uint32_t& bank)
{
    CartImage cart(0x100000);
    for (int i = 0; i < 768; i++)
    {
        cart.Put16(kLinearTable + uint32_t(i) * 2, LinearEntry(i));
    }
    uint32_t at = kFirstModule;
    addresses.clear();
    for (const TestModule& m : modules)
    {
        addresses.push_back(at);
        at = (PutModule(cart, at, m) + 3) & ~3u;
    }
    bank = at;
    PutBank(cart, bank, samples, header_size);

    return cart;
}

} // namespace ntdlib::testing::logikstate_mod30_cart
