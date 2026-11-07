// SPDX-License-Identifier: MIT

#include "ntdlib/krawall/commands.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/krawall/driver.h"

namespace ntdlib::krawall
{
namespace
{

using commands::FromUnit;
using commands::Kind;
using commands::ToUnit;
using Item = commands::Command;

// The bytes of a pattern's index.
constexpr uint32_t kIndexBytes = 2 * kPatternIndexEntries;

// The rows that the index gives the data of.
constexpr uint32_t kIndexedRows = 4 * kPatternIndexEntries;

// The bytes of a pattern before its rows.
uint32_t HeaderBytes(const Traits& traits)
{
    return kPatternRows + (traits.wide ? 2 : 1);
}

// The index entries that a pattern of `rows` rows uses: one for every 4th row, up to the index's 16.
uint32_t UsedEntries(uint32_t rows)
{
    return std::min((rows + 3) / 4, kPatternIndexEntries);
}

uint32_t Aligned(uint32_t at)
{
    return (at + 3) & ~3u;
}

uint32_t Le16(std::span<const uint8_t> bytes, uint32_t at)
{
    return uint32_t(bytes[at]) | uint32_t(bytes[at + 1]) << 8;
}

Item DataItem(std::span<const uint8_t> bytes)
{
    Item item;
    item.kind = Kind::kData;
    item.bytes.assign(bytes.begin(), bytes.end());
    return item;
}

Item Wait(uint32_t rows)
{
    Item item;
    item.kind = Kind::kWait;
    item.value.value = ToUnit(rows, {});
    return item;
}

// Keeps a sequence whose patterns can't be read as the encoder writes them as data, split at the patterns' starts.
Decoded Opaque(std::span<const uint8_t> data, std::span<const uint32_t> starts, std::string reason)
{
    Decoded decoded;
    decoded.opaque = true;
    decoded.reason = std::move(reason);

    std::set<uint32_t> splits = {0};
    splits.insert(starts.begin(), starts.end());
    std::map<uint32_t, uint32_t> items;
    for (auto it = splits.begin(); it != splits.end(); ++it)
    {
        const auto next = std::next(it);
        const uint32_t end = next == splits.end() ? uint32_t(data.size()) : *next;
        items[*it] = uint32_t(decoded.sequence.items.size());
        decoded.sequence.items.push_back(DataItem(data.subspan(*it, end - *it)));
    }
    for (const uint32_t start : starts)
    {
        decoded.entries.push_back(items.at(start));
    }

    return decoded;
}

// Reads the pattern at `start` into `items`, up to `end`. Returns the first byte past its rows, or sets `problem`.
uint32_t ReadPattern(std::span<const uint8_t> data, uint32_t start, uint32_t end, const Traits& traits,
                     std::vector<Item>& items, std::string& problem)
{
    const uint32_t first = start + HeaderBytes(traits);
    if (first > end)
    {
        problem = "a pattern's header runs past its bytes";
        return 0;
    }

    const uint32_t rows = traits.wide ? Le16(data, start + kPatternRows) : data[start + kPatternRows];
    Item header;
    header.kind = Kind::kOther;
    header.encoding.opcode = kPatternOpcode;
    items.push_back(std::move(header));
    const size_t header_item = items.size() - 1;

    std::vector<uint32_t> indexed;
    uint32_t at = first;
    uint32_t empty_rows = 0;
    const auto byte = [&]() -> int
    {
        if (at >= end)
        {
            problem = "a pattern's rows run past its bytes";
            return -1;
        }
        return data[at++];
    };

    for (uint32_t row = 0; row < rows; row++)
    {
        if (row % 4 == 0 && row < kIndexedRows)
        {
            indexed.push_back(at - first);
        }

        bool any = false;
        for (;;)
        {
            const int flags = byte();
            if (flags < 0)
            {
                return 0;
            }
            if (flags == 0)
            {
                break;
            }
            if (!(flags & (kEntryNote | kEntryVolume | kEntryEffect)))
            {
                problem = "a row has an entry without fields";
                return 0;
            }

            if (!any && empty_rows)
            {
                items.push_back(Wait(empty_rows));
                empty_rows = 0;
            }
            any = true;

            const uint8_t channel = uint8_t(flags & kEntryChannel);
            if (flags & kEntryNote)
            {
                const int b0 = byte();
                const int b1 = byte();
                if (b0 < 0 || b1 < 0)
                {
                    return 0;
                }
                Item item;
                item.index = channel;
                uint32_t instrument = 0;
                uint8_t note = 0;
                if (traits.wide)
                {
                    note = uint8_t(b0 & 0x7F);
                    instrument = uint32_t(b1);
                    if (b0 & 0x80)
                    {
                        const int b2 = byte();
                        if (b2 < 0)
                        {
                            return 0;
                        }
                        instrument |= uint32_t(b2) << 8;
                        if (instrument < 0x100)
                        {
                            item.encoding.value_bytes = 3;
                        }
                    }
                }
                else
                {
                    note = uint8_t(b0 >> 1);
                    instrument = uint32_t(b1) | uint32_t(b0 & 1) << 8;
                }

                if (note == 0)
                {
                    item.kind = Kind::kProgram;
                    item.value.value = ToUnit(instrument, {});
                }
                else
                {
                    item.kind = note == kNoteOff ? Kind::kNoteOff : Kind::kNote;
                    item.key = note == kNoteOff ? uint8_t(0) : note;
                    item.sample = uint16_t(instrument);
                }
                items.push_back(std::move(item));
            }
            if (flags & kEntryVolume)
            {
                const int volume = byte();
                if (volume < 0)
                {
                    return 0;
                }
                Item item;
                item.kind = Kind::kVolume;
                item.index = channel;
                item.value.value = ToUnit(volume, {});
                items.push_back(std::move(item));
            }
            if (flags & kEntryEffect)
            {
                const int effect = byte();
                const int operand = byte();
                if (effect < 0 || operand < 0)
                {
                    return 0;
                }
                Item item;
                item.kind = Kind::kOther;
                item.index = channel;
                item.encoding.opcode = uint16_t(kEffectOpcode + effect);
                item.bytes = {uint8_t(operand)};
                items.push_back(std::move(item));
            }
        }

        if (any)
        {
            empty_rows = 1;
        }
        else
        {
            empty_rows++;
        }
    }
    if (empty_rows)
    {
        items.push_back(Wait(empty_rows));
    }

    // The index has to give the data of every 4th row. Its spare entries stay with the pattern's start.
    for (uint32_t i = 0; i < indexed.size(); i++)
    {
        if (Le16(data, start + 2 * i) != indexed[i])
        {
            problem = "a pattern's index doesn't give its rows' data";
            return 0;
        }
    }
    const uint32_t used = 2 * uint32_t(indexed.size());
    const std::span<const uint8_t> spare = data.subspan(start + used, kIndexBytes - used);
    if (std::any_of(spare.begin(), spare.end(), [](uint8_t b) { return b != 0; }))
    {
        items[header_item].bytes.assign(spare.begin(), spare.end());
    }

    return at;
}

// Writes the items of a sequence of patterns. Each item's offset goes in `offsets`, and the patterns' starts in
// `pattern_items`.
class Writer
{
public:
    explicit Writer(const Traits& traits) : traits_(traits)
    {
    }

    void Add(const Item& item, size_t number)
    {
        if (item.kind == Kind::kOther && item.encoding.opcode == kPatternOpcode)
        {
            EndPattern();
            pattern_ = bytes_.size();
            spare_ = item.bytes;
            rows_ = 0;
            row_offsets_.clear();
            offsets_.push_back(uint32_t(bytes_.size()));
            bytes_.resize(bytes_.size() + HeaderBytes(traits_));
            in_pattern_ = true;
            row_open_ = false;
            return;
        }
        if (!in_pattern_)
        {
            throw std::invalid_argument("item " + std::to_string(number) + " comes before the first pattern");
        }

        if (item.kind == Kind::kWait)
        {
            const int64_t rows = FromUnit(item.value.value, {});
            if (rows < 1)
            {
                throw std::invalid_argument("a wait of item " + std::to_string(number) + " ends no row");
            }
            offsets_.push_back(uint32_t(bytes_.size()));
            for (int64_t r = 0; r < rows; r++)
            {
                MarkRow();
                bytes_.push_back(0);
                rows_++;
                if (rows_ > (traits_.wide ? 0xFFFFu : 0xFFu))
                {
                    throw std::invalid_argument("a pattern has more rows than the build's count holds");
                }
            }
            row_open_ = false;
            entry_ = -1;
            return;
        }

        const int field = FieldOf(item, number);
        if (item.index > kEntryChannel)
        {
            throw std::invalid_argument("item " + std::to_string(number) + " names a channel past 31");
        }
        if (!row_open_)
        {
            MarkRow();
            row_open_ = true;
        }
        offsets_.push_back(uint32_t(bytes_.size()));
        if (entry_ < 0 || item.index != channel_ || field <= field_)
        {
            entry_ = int64_t(bytes_.size());
            channel_ = item.index;
            bytes_.push_back(item.index);
        }
        field_ = field;
        bytes_[size_t(entry_)] = uint8_t(bytes_[size_t(entry_)] | (field == 0   ? kEntryNote
                                                                   : field == 1 ? kEntryVolume
                                                                                : kEntryEffect));
        WriteField(item, field, number);
    }

    // Ends the last pattern, and returns the bytes.
    std::vector<uint8_t> Finish()
    {
        EndPattern();
        return bytes_;
    }

    const std::vector<uint32_t>& Offsets() const
    {
        return offsets_;
    }

private:
    // The field of a row's entry that an item fills: the note, the volume or the effect.
    static int FieldOf(const Item& item, size_t number)
    {
        switch (item.kind)
        {
        case Kind::kNote:
        case Kind::kProgram:
        case Kind::kNoteOff:
            return 0;
        case Kind::kVolume:
            return 1;
        case Kind::kOther:
            if (item.encoding.opcode >= kEffectOpcode && item.encoding.opcode < kEffectOpcode + 0x100)
            {
                return 2;
            }
            break;
        default:
            break;
        }

        throw std::invalid_argument("item " + std::to_string(number) + " is a " +
                                    std::string(commands::Name(item.kind)) + ", which a pattern can't hold");
    }

    void WriteField(const Item& item, int field, size_t number)
    {
        if (field == 1)
        {
            bytes_.push_back(Byte(FromUnit(item.value.value, {}), number));
            return;
        }
        if (field == 2)
        {
            if (item.bytes.size() != 1)
            {
                throw std::invalid_argument("the effect of item " + std::to_string(number) + " has no operand byte");
            }
            bytes_.push_back(uint8_t(item.encoding.opcode - kEffectOpcode));
            bytes_.push_back(item.bytes[0]);
            return;
        }

        uint32_t note = 0;
        int64_t instrument = 0;
        if (item.kind == Kind::kProgram)
        {
            instrument = FromUnit(item.value.value, {});
        }
        else
        {
            note = item.kind == Kind::kNoteOff ? kNoteOff : item.key;
            instrument = item.sample.value_or(0);
            if (note == 0 || note > 0x7F || (item.kind == Kind::kNote && note == kNoteOff))
            {
                throw std::invalid_argument("the note of item " + std::to_string(number) + " can't be stored");
            }
        }

        if (traits_.wide)
        {
            if (instrument < 0 || instrument > 0xFFFF)
            {
                throw std::invalid_argument("the instrument of item " + std::to_string(number) + " can't be stored");
            }
            const bool third = instrument > 0xFF || item.encoding.value_bytes == 3;
            bytes_.push_back(uint8_t(note | (third ? 0x80 : 0)));
            bytes_.push_back(uint8_t(instrument));
            if (third)
            {
                bytes_.push_back(uint8_t(instrument >> 8));
            }
            return;
        }

        if (instrument < 0 || instrument > 0x1FF)
        {
            throw std::invalid_argument("the instrument of item " + std::to_string(number) + " can't be stored");
        }
        bytes_.push_back(uint8_t(note << 1 | uint32_t(instrument >> 8)));
        bytes_.push_back(uint8_t(instrument));
    }

    static uint8_t Byte(int64_t value, size_t number)
    {
        if (value < 0 || value > 0xFF)
        {
            throw std::invalid_argument("the value of item " + std::to_string(number) + " doesn't fit in a byte");
        }
        return uint8_t(value);
    }

    // Notes where a row starts, for the index.
    void MarkRow()
    {
        if (rows_ % 4 == 0 && rows_ < kIndexedRows && row_offsets_.size() == rows_ / 4)
        {
            row_offsets_.push_back(uint32_t(bytes_.size()));
        }
    }

    void EndPattern()
    {
        if (!in_pattern_)
        {
            return;
        }
        if (row_open_)
        {
            throw std::invalid_argument("a pattern's last row has no end");
        }

        const uint32_t first = uint32_t(pattern_) + HeaderBytes(traits_);
        uint8_t* const header = &bytes_[pattern_];
        for (uint32_t i = 0; i < row_offsets_.size(); i++)
        {
            const uint32_t offset = row_offsets_[i] - first;
            if (offset > 0xFFFF)
            {
                throw std::invalid_argument("a pattern's rows run past what its index can give");
            }
            header[2 * i] = uint8_t(offset);
            header[2 * i + 1] = uint8_t(offset >> 8);
        }
        const uint32_t used = 2 * UsedEntries(rows_);
        if (spare_.size() == kIndexBytes - used)
        {
            std::copy(spare_.begin(), spare_.end(), header + used);
        }
        header[kPatternRows] = uint8_t(rows_);
        if (traits_.wide)
        {
            header[kPatternRows + 1] = uint8_t(rows_ >> 8);
        }

        bytes_.resize(Aligned(uint32_t(bytes_.size())));
        in_pattern_ = false;
    }

    Traits traits_;
    std::vector<uint8_t> bytes_;
    std::vector<uint32_t> offsets_;
    size_t pattern_ = 0;
    std::vector<uint8_t> spare_;
    uint32_t rows_ = 0;
    std::vector<uint32_t> row_offsets_;
    bool in_pattern_ = false;
    bool row_open_ = false;
    int64_t entry_ = -1;
    uint8_t channel_ = 0;
    int field_ = 0;
};

// Writes `sequence`. A sequence of data alone, as the decoder keeps one that it can't read, keeps its bytes.
Encoded Write(const commands::Sequence& sequence, const Traits& traits)
{
    Encoded encoded;
    const bool data = std::all_of(sequence.items.begin(), sequence.items.end(),
                                  [](const Item& item) { return item.kind == Kind::kData; });
    if (data)
    {
        for (const Item& item : sequence.items)
        {
            encoded.offsets.push_back(uint32_t(encoded.bytes.size()));
            encoded.bytes.insert(encoded.bytes.end(), item.bytes.begin(), item.bytes.end());
        }
        encoded.offsets.push_back(uint32_t(encoded.bytes.size()));
        return encoded;
    }

    Writer writer(traits);
    for (size_t i = 0; i < sequence.items.size(); i++)
    {
        if (sequence.items[i].kind == Kind::kData)
        {
            throw std::invalid_argument("item " + std::to_string(i) + " is data among patterns");
        }
        writer.Add(sequence.items[i], i);
    }
    encoded.bytes = writer.Finish();
    encoded.offsets = writer.Offsets();
    encoded.offsets.push_back(uint32_t(encoded.bytes.size()));
    return encoded;
}

} // namespace

Traits TraitsOf(const DriverInfo& info)
{
    return {info.build >= Build::k2004_09_17};
}

Decoded DecodeSequence(std::span<const uint8_t> data, std::span<const uint32_t> starts, const Traits& traits)
{
    if (data.empty())
    {
        throw std::invalid_argument("a sequence holds no bytes");
    }
    for (const uint32_t start : starts)
    {
        if (start >= data.size())
        {
            throw std::invalid_argument("a pattern starts outside the sequence's bytes");
        }
    }

    // The patterns follow one another in the order of the module's table, from the sequence's start.
    if (starts.empty() || starts[0] != 0)
    {
        return Opaque(data, starts, "the sequence doesn't start with the module's first pattern");
    }

    Decoded decoded;
    std::vector<Item>& items = decoded.sequence.items;
    for (size_t p = 0; p < starts.size(); p++)
    {
        const uint32_t end = p + 1 < starts.size() ? starts[p + 1] : uint32_t(data.size());
        if (end <= starts[p])
        {
            return Opaque(data, starts, "the module's patterns don't lie in the order of its table");
        }

        decoded.entries.push_back(uint32_t(items.size()));
        std::string problem;
        const uint32_t after = ReadPattern(data, starts[p], end, traits, items, problem);
        if (!problem.empty())
        {
            return Opaque(data, starts, problem);
        }

        // Bytes of 0 pad the pattern to a multiple of 4 bytes.
        if (Aligned(after) != end ||
            std::any_of(data.begin() + after, data.begin() + end, [](uint8_t b) { return b != 0; }))
        {
            return Opaque(data, starts, "a pattern isn't followed by the padding to a multiple of 4 bytes");
        }
    }

    // The encoder has to give the same bytes.
    const Encoded encoded = Write(decoded.sequence, traits);
    if (!std::equal(encoded.bytes.begin(), encoded.bytes.end(), data.begin(), data.end()))
    {
        return Opaque(data, starts, "the patterns' bytes don't read back");
    }

    return decoded;
}

int SequenceCount(const DriverInfo& info)
{
    return int(info.modules.size());
}

int SequenceOfSong(const DriverInfo& info, int song)
{
    if (song < 0 || size_t(song) >= info.songs.size())
    {
        throw std::out_of_range("the game holds no piece " + std::to_string(song));
    }

    const auto at = std::find(info.modules.begin(), info.modules.end(), info.songs[size_t(song)].module);
    if (at == info.modules.end())
    {
        throw std::out_of_range("piece " + std::to_string(song) + " plays a module that the game's list lacks");
    }

    return int(at - info.modules.begin());
}

uint32_t SequenceAddress(const Rom& rom, const DriverInfo& info, int sequence)
{
    if (sequence < 0 || sequence >= SequenceCount(info))
    {
        throw std::out_of_range("the game holds no sequence " + std::to_string(sequence));
    }

    const Rom& modules = info.unpacked ? *info.unpacked : rom;
    ModuleInfo module;
    if (!ReadModule(modules, info.build, info.modules[size_t(sequence)], module) || module.patterns.empty())
    {
        return 0;
    }

    return module.patterns[0];
}

Decoded DecodeRomSequence(const Rom& rom, const DriverInfo& info, int sequence)
{
    if (sequence < 0 || sequence >= SequenceCount(info))
    {
        throw std::out_of_range("the game holds no sequence " + std::to_string(sequence));
    }

    const Rom& modules = info.unpacked ? *info.unpacked : rom;
    ModuleInfo module;
    if (!ReadModule(modules, info.build, info.modules[size_t(sequence)], module) || module.patterns.empty())
    {
        throw std::invalid_argument("module " + std::to_string(sequence) + " has no header that the player can read");
    }

    const uint32_t address = module.patterns[0];
    const uint32_t last = *std::max_element(module.patterns.begin(), module.patterns.end());
    if (*std::min_element(module.patterns.begin(), module.patterns.end()) < address || !modules.Contains(last))
    {
        throw std::invalid_argument("a pattern of module " + std::to_string(sequence) +
                                    " lies before its first pattern or outside the ROM");
    }

    // The sequence ends with the padding of the pattern that starts last. Where that pattern's rows don't read, it
    // ends at the module's header, as the patterns of the games' modules do.
    const Traits traits = TraitsOf(info);
    const std::span<const uint8_t> rest(modules.Ptr(address), modules.End() - address);
    std::vector<Item> items;
    std::string problem;
    const uint32_t after = ReadPattern(rest, last - address, uint32_t(rest.size()), traits, items, problem);
    uint32_t end = 0;
    if (problem.empty() && Aligned(after) <= rest.size())
    {
        end = Aligned(after);
    }
    else if (module.address > last && modules.Contains(address, module.address - address))
    {
        end = module.address - address;
    }
    else
    {
        throw std::invalid_argument("the last pattern of module " + std::to_string(sequence) + " doesn't read");
    }

    std::vector<uint32_t> starts;
    for (const uint32_t pattern : module.patterns)
    {
        starts.push_back(pattern - address);
    }

    return DecodeSequence(rest.first(end), starts, traits);
}

std::vector<uint32_t> ItemOffsets(const commands::Sequence& sequence, const Traits& traits)
{
    return Write(sequence, traits).offsets;
}

Encoded EncodeSequence(const commands::Sequence& sequence, std::span<const uint32_t> entries, const Traits& traits)
{
    Encoded encoded = Write(sequence, traits);
    for (const uint32_t entry : entries)
    {
        if (entry >= sequence.items.size())
        {
            throw std::invalid_argument("a pattern starts past the sequence's items");
        }
        const Item& item = sequence.items[entry];
        if (item.kind != Kind::kData && !(item.kind == Kind::kOther && item.encoding.opcode == kPatternOpcode))
        {
            throw std::invalid_argument("entry item " + std::to_string(entry) + " isn't a pattern's start");
        }
    }

    return encoded;
}

commands::Scale ScaleOf(Kind)
{
    return {};
}

} // namespace ntdlib::krawall
