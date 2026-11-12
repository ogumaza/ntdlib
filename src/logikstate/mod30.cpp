// SPDX-License-Identifier: MIT

#include "ntdlib/logikstate/mod30.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "ntdlib/agb/rom.h"

namespace ntdlib::logikstate
{

namespace
{

// The most channels that a module's header may give.
constexpr uint32_t kMaxChannels = 16;

// The most rows of a pattern.
constexpr uint32_t kMaxRows = 256;

// The fields of a sample's header: the length is a halfword in a short header and a word in a long one, and the other
// fields follow it.
constexpr uint32_t kShortFields = 2;
constexpr uint32_t kLongFields = 4;
constexpr uint32_t kFineTune = 0;
constexpr uint32_t kVolume = 2;
constexpr uint32_t kLoopStart = 4;
constexpr uint32_t kLoopLength = 6;
constexpr uint32_t kRelativeNote = 8;
constexpr uint32_t kLongPadding = 14; // a halfword of 0 at the end of a long header

// The most that a sample's volume may be.
constexpr uint16_t kMaxVolume = 64;

constexpr Mod30HeaderLayout kLayoutA = {
    .order_count = 0x38,
    .sample_map = 0x171C,
    .pattern_count = 0x1B1C,
    .rows = 0x1B20,
    .amiga = 0x1B24,
    .tempo = 0x1B28,
    .speed = 0x1B2C,
    .orders = 0x1B30,
    .pattern_table = 0x1C30,
    .patterns = 0x2034,
};

constexpr Mod30HeaderLayout kLayoutB = {
    .order_count = 0x34,
    .sample_map = 0x38,
    .pattern_count = 0x138,
    .rows = 0x13C,
    .amiga = 0x140,
    .tempo = 0x144,
    .speed = 0x148,
    .orders = 0x14C,
    .pattern_table = 0x24C,
    .patterns = 0x650,
};

constexpr Mod30HeaderLayout kLayoutC = {
    .order_count = 0x38,
    .sample_map = 0x3C,
    .pattern_count = 0x13C,
    .rows = 0x140,
    .amiga = 0x144,
    .tempo = 0x148,
    .speed = 0x14C,
    .orders = 0x150,
    .pattern_table = 0x250,
    .patterns = 0x654,
};

// Returns the address after the blocks of a pattern's channels, or 0 if a block leaves the image.
uint32_t PatternEnd(const Rom& rom, uint32_t block, uint32_t channels)
{
    for (uint32_t c = 0; c < channels; c++)
    {
        if (!rom.Contains(block, kMod30BlockHeader))
        {
            return 0;
        }

        const uint32_t size = rom.U32(block + kMod30BlockSize);
        if (size > rom.Size())
        {
            return 0;
        }
        block += kMod30BlockHeader + size;
    }

    return rom.Contains(block - 1) ? block : 0;
}

// A stream of runs, as the drivers' readers keep it.
struct Stream
{
    uint32_t address = 0;
    int32_t count = 0;
    uint16_t value = 0;
};

// Reads a stream's next row as the drivers' readers do: a new run when the count is used up, then one row of it.
uint16_t NextRow(const Rom& rom, Stream& stream, bool wide)
{
    if (stream.count <= 0)
    {
        stream.count = rom.U8(stream.address);
        stream.value =
            wide ? uint16_t(rom.U8(stream.address + 1) << 8 | rom.U8(stream.address + 2)) : rom.U8(stream.address + 1);
        stream.address += wide ? 3 : 2;
    }
    stream.count--;

    return stream.value;
}

} // namespace

const Mod30HeaderLayout& HeaderLayout(Mod30Layout layout)
{
    switch (layout)
    {
    case Mod30Layout::kA:
        return kLayoutA;

    case Mod30Layout::kC:
        return kLayoutC;

    case Mod30Layout::kB:
    default:
        return kLayoutB;
    }
}

Mod30Layout DetectLayout(const Rom& rom, uint32_t address)
{
    if (rom.U32(address + kMod30RateWord) != kMod30Rate0)
    {
        return Mod30Layout::kB;
    }

    const uint32_t patterns = rom.U32(address + kLayoutC.pattern_count);
    const uint32_t rows = rom.U32(address + kLayoutC.rows);
    const bool compact = patterns >= 1 && patterns <= uint32_t(kMod30Patterns) && rows >= 1 && rows <= kMaxRows;

    return compact ? Mod30Layout::kC : Mod30Layout::kA;
}

bool ReadMod30Module(const Rom& rom, uint32_t address, Mod30Module& module)
{
    if (address % 4 != 0 || !rom.Contains(address, kLayoutA.patterns) || rom.U32(address) != kMod30Signature1 ||
        rom.U32(address + 4) != kMod30Signature2)
    {
        return false;
    }

    // The header's counts.
    Mod30Module m;
    m.address = address;
    m.layout = DetectLayout(rom, address);
    const Mod30HeaderLayout& layout = HeaderLayout(m.layout);
    m.channels = rom.U32(address + kMod30Channels);
    m.order_count = rom.U32(address + layout.order_count);
    m.pattern_count = rom.U32(address + layout.pattern_count);
    m.rows = rom.U32(address + layout.rows);
    m.amiga = rom.U32(address + layout.amiga);
    m.tempo = rom.U32(address + layout.tempo);
    m.speed = rom.U32(address + layout.speed);
    if (m.channels < 1 || m.channels > kMaxChannels || m.order_count < 1 || m.order_count > uint32_t(kMod30Orders) ||
        m.pattern_count < 1 || m.pattern_count > uint32_t(kMod30Patterns) || m.rows < 1 || m.rows > kMaxRows)
    {
        return false;
    }

    // The orders, up to the first end of the list in the layouts that have one.
    for (int i = 0; i < kMod30Orders; i++)
    {
        m.orders[size_t(i)] = rom.U8(address + layout.orders + uint32_t(i));
    }
    m.real_orders = m.order_count;
    if (m.layout != Mod30Layout::kA)
    {
        const auto end = std::find(m.orders.begin(), m.orders.begin() + m.order_count, kMod30EndOfOrders);
        m.real_orders = uint32_t(end - m.orders.begin());
    }

    // The patterns' first blocks. Layout B gives offsets. So does a table of layout C whose first word is 0. No
    // pattern has a size of 0.
    const uint32_t data = address + layout.patterns;
    const uint32_t table = address + layout.pattern_table;
    m.pattern_offsets = m.layout == Mod30Layout::kB || (m.layout == Mod30Layout::kC && rom.U32(table) == 0);
    uint32_t sum = 0;
    for (uint32_t i = 0; i < uint32_t(kMod30Patterns); i++)
    {
        const uint32_t word = rom.U32(table + 4 * i);
        if (m.pattern_offsets)
        {
            m.patterns[i] = data + word;
        }
        else if (i < m.pattern_count)
        {
            m.patterns[i] = data + sum;
            sum += word;
        }
    }

    // The module's end: after the last pattern's data, or after the blocks of the pattern that starts last.
    m.end = data + sum;
    for (uint32_t i = 0; i < m.pattern_count; i++)
    {
        const uint32_t end = PatternEnd(rom, m.patterns[i], m.channels);
        if (end == 0)
        {
            return false;
        }
        m.end = std::max(m.end, end);
    }

    module = m;

    return true;
}

std::vector<uint32_t> FindMod30Modules(const Rom& rom)
{
    std::vector<uint32_t> found;
    Mod30Module module;
    for (uint32_t a = agb::kRomBase; a + kLayoutA.patterns <= rom.End(); a += 4)
    {
        if (rom.U32(a) == kMod30Signature1 && rom.U32(a + 4) == kMod30Signature2 && ReadMod30Module(rom, a, module))
        {
            found.push_back(a);
        }
    }

    return found;
}

bool ReadMod30Bank(const Rom& rom, uint32_t address, uint32_t header_size, Mod30Bank& bank)
{
    const bool long_headers = header_size == kMod30LongHeader;
    const uint32_t points = address + uint32_t(kMod30Samples) * header_size;
    if (!rom.Contains(address, points - address))
    {
        return false;
    }

    // The headers, then the points of each sample in turn.
    Mod30Bank b;
    b.address = address;
    b.header_size = header_size;
    uint32_t next = points;
    for (uint32_t i = 0; i < uint32_t(kMod30Samples); i++)
    {
        const uint32_t h = address + i * header_size;
        const uint32_t fields = h + (long_headers ? kLongFields : kShortFields);
        Mod30Sample& s = b.samples[i];
        s.length = long_headers ? rom.U32(h) : rom.U16(h);
        s.fine_tune = int16_t(rom.U16(fields + kFineTune));
        s.volume = rom.U16(fields + kVolume);
        s.loop_start = rom.U16(fields + kLoopStart);
        s.loop_length = rom.U16(fields + kLoopLength);
        s.relative_note = int16_t(rom.U16(fields + kRelativeNote));
        if (s.length != 0)
        {
            b.points[i] = next;
            next += s.length;
        }
    }
    if (next < points || !rom.Contains(points, next - points))
    {
        return false;
    }

    b.end = next;
    bank = b;

    return true;
}

bool FindMod30Bank(const Rom& rom, const std::vector<uint32_t>& modules, Mod30Bank& bank)
{
    if (modules.empty())
    {
        return false;
    }

    Mod30Module last;
    if (!ReadMod30Module(rom, modules.back(), last))
    {
        return false;
    }

    // A long header ends with a halfword of 0. Every sample's volume is at most 64, and its loop ends inside it.
    const auto fits = [&rom](const Mod30Bank& b)
    {
        bool any = false;
        for (uint32_t i = 0; i < uint32_t(kMod30Samples); i++)
        {
            const Mod30Sample& s = b.samples[i];
            const uint32_t h = b.address + i * b.header_size;
            if ((b.header_size == kMod30LongHeader && rom.U16(h + kLongPadding) != 0) ||
                (s.length != 0 && (s.volume > kMaxVolume || uint32_t(s.loop_start) + s.loop_length > s.length)))
            {
                return false;
            }
            any = any || s.length != 0;
        }

        return any;
    };

    const auto count = [](const Mod30Bank& b)
    {
        return std::count_if(b.samples.begin(), b.samples.end(), [](const Mod30Sample& s) { return s.length != 0; });
    };

    // A bank read a word early takes each header's last word for the next one's length, and finds fewer samples.
    const uint32_t start = (last.end + 3) & ~3u;
    for (uint32_t header_size : {kMod30LongHeader, kMod30ShortHeader})
    {
        bool found = false;
        for (uint32_t address : {start, start + 4})
        {
            Mod30Bank b;
            if (ReadMod30Bank(rom, address, header_size, b) && fits(b) && (!found || count(b) > count(bank)))
            {
                bank = b;
                found = true;
            }
        }
        if (found)
        {
            return true;
        }
    }

    return false;
}

bool DecodeMod30Pattern(const Rom& rom, uint32_t block, uint32_t channels, uint32_t rows,
                        std::vector<std::vector<Mod30Cell>>& cells, std::string& error)
{
    cells.assign(channels, std::vector<Mod30Cell>(rows));
    for (uint32_t c = 0; c < channels; c++)
    {
        if (!rom.Contains(block, kMod30BlockHeader))
        {
            error = "the block of channel " + std::to_string(c) + " at " + agb::Hex(block) + " leaves the image";
            return false;
        }

        // The three streams, each up to the next one's start.
        const uint32_t data = block + kMod30BlockHeader;
        const uint32_t size = rom.U32(block + kMod30BlockSize);
        const std::array<uint32_t, 4> bounds = {data, data + rom.U32(block + kMod30BlockStreamB),
                                                data + rom.U32(block + kMod30BlockStreamC),
                                                data + rom.U32(block + kMod30BlockStreamEnd)};
        std::array<Stream, 3> streams;
        for (size_t s = 0; s < streams.size(); s++)
        {
            streams[s].address = bounds[s];
        }
        for (uint32_t r = 0; r < rows; r++)
        {
            Mod30Cell& cell = cells[c][r];
            const uint16_t a = NextRow(rom, streams[0], true);
            cell.note = uint16_t(a >> kMod30NoteShift);
            cell.sample = uint8_t(a & kMod30CellSample);
            cell.volume = uint8_t(NextRow(rom, streams[1], false));
            const uint16_t e = NextRow(rom, streams[2], true);
            cell.effect = uint8_t(e >> 8);
            cell.operand = uint8_t(e);
        }

        // Each stream ends with a count of 0 right after its rows' runs.
        for (size_t s = 0; s < streams.size(); s++)
        {
            if (streams[s].count != 0 || rom.U8(streams[s].address) != 0 || streams[s].address + 1 != bounds[s + 1] ||
                bounds[s + 1] > data + size)
            {
                error = "stream " + std::to_string(s) + " of channel " + std::to_string(c) + " at " +
                        agb::Hex(bounds[s]) + " doesn't end after its rows";
                return false;
            }
        }
        block = data + size;
    }

    return true;
}

} // namespace ntdlib::logikstate
