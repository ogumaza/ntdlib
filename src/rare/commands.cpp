// SPDX-License-Identifier: MIT

#include "ntdlib/rare/commands.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/rare/song.h"

namespace ntdlib::rare
{
namespace
{

using commands::FromUnit;
using commands::Kind;
using commands::kOne;
using commands::ToUnit;
using Item = commands::Command;

bool HasChannel(uint8_t command)
{
    return command >= kCmdNoteOnB && command <= kCmdBend;
}

// Reads the bytes of a command. Past the bytes, it reads 0 and sets a flag.
class Reader
{
public:
    Reader(std::span<const uint8_t> bytes, uint32_t at) : bytes_(bytes), at_(at)
    {
    }

    uint8_t Byte()
    {
        if (at_ >= bytes_.size())
        {
            past_ = true;
            at_++;
            return 0;
        }

        return bytes_[at_++];
    }

    // A little-endian value of `count` bytes.
    uint32_t Value(int count)
    {
        uint32_t value = 0;
        for (int i = 0; i < count; i++)
        {
            value |= uint32_t(Byte()) << (8 * i);
        }

        return value;
    }

    bool Past() const
    {
        return past_;
    }

    uint32_t At() const
    {
        return at_;
    }

private:
    std::span<const uint8_t> bytes_;
    uint32_t at_;
    bool past_ = false;
};

// Sets the byte that the driver ignores after a command's first argument, where it isn't 0.
void KeepIgnored(Item& item, uint8_t byte)
{
    if (byte != 0)
    {
        item.encoding.arguments = byte;
    }
}

// Makes a controller's item.
void ReadController(Item& item, uint8_t controller, uint8_t value, const Traits& traits)
{
    const auto set = [&item](Kind kind, int64_t to)
    {
        item.kind = kind;
        item.value.value = to;
    };

    switch (controller)
    {
    case kCtrlModulation:
        set(Kind::kLfoDepth, value);
        return;
    case kCtrlVolume:
        set(Kind::kVolume, value);
        return;
    case kCtrlLoopStart:
        set(Kind::kLoopStart, 0);
        KeepIgnored(item, value);
        return;
    case kCtrlLoopEnd:
        set(Kind::kLoopEnd, 0);
        KeepIgnored(item, value);
        return;
    case kCtrlMonoOn:
    case kCtrlPolyOn:
        set(Kind::kMonophonic, controller == kCtrlMonoOn ? 1 : 0);
        KeepIgnored(item, value);
        return;
    default:
        break;
    }

    if (traits.envelope_controllers && controller >= kCtrlAttack && controller <= kCtrlRelease)
    {
        constexpr Kind kKinds[] = {Kind::kAttack, Kind::kDecay, Kind::kSustain, Kind::kRelease};
        set(kKinds[controller - kCtrlAttack], value);
        return;
    }

    set(Kind::kOther, value);
    item.encoding.opcode = uint16_t(kController + controller);
}

// Reads a command in the console's values. Sets `unknown` for a command that the driver has no code for.
Item ReadItem(Reader& r, const Traits& traits, bool& unknown)
{
    Item item;
    const uint8_t first = r.Byte();
    uint8_t command = first;
    if (traits.format == Format::kChannelNibble)
    {
        command = first & 0x0F;
        item.index = first >> 4;
    }
    if (command >= kCommandCount)
    {
        unknown = true;
        return item;
    }
    if (traits.format == Format::kChannelByte && HasChannel(command))
    {
        item.index = r.Byte();
    }

    const auto set = [&item](Kind kind, int64_t value = 0)
    {
        item.kind = kind;
        item.value.value = value;
    };

    switch (command)
    {
    case kCmdTempo:
        set(Kind::kTempo, r.Value(3));
        break;
    case kCmdDelay1:
    case kCmdDelay2:
    case kCmdDelay3:
        {
            // The bytes that the delay takes, where it holds a value that fewer bytes would.
            const int count = command - kCmdDelay1 + 1;
            const uint32_t value = r.Value(count);
            const int needed = value > 0xFFFF ? 3 : value > 0xFF ? 2 : 1;
            set(Kind::kWait, value);
            item.encoding.value_bytes = uint8_t(count > needed ? count : 0);
        }
        break;
    case kCmdNoteOnB:
    case kCmdNoteOn:
        set(Kind::kNote);
        item.key = r.Byte();
        item.velocity = r.Byte();
        item.encoding.opcode = command == kCmdNoteOnB ? kCmdNoteOnB : 0;
        break;
    case kCmdNoteOff:
        set(Kind::kNoteOff);
        item.key = r.Byte();
        KeepIgnored(item, r.Byte());
        break;
    case kCmdControl:
        {
            const uint8_t controller = r.Byte();
            ReadController(item, controller, r.Byte(), traits);
        }
        break;
    case kCmdProgram:
        set(Kind::kProgram, r.Byte());
        break;
    case kCmdPressure:
        set(Kind::kOther);
        item.encoding.opcode = kCmdPressure;
        KeepIgnored(item, r.Byte());
        break;
    case kCmdBend:
        set(Kind::kPitchBend, r.Value(2));
        break;
    case kCmdEnd:
        set(Kind::kEnd);
        break;
    default:
        set(Kind::kOther);
        item.encoding.opcode = kCmdNop;
        break;
    }

    return item;
}

// Follows a sequence's tracks as the driver plays them, and records each command in the console's values.
class Flow
{
public:
    Flow(std::span<const uint8_t> data, const Traits& traits, std::span<const uint32_t> starts)
        : data_(data), traits_(traits)
    {
        for (const uint32_t start : starts)
        {
            if (problem_.empty())
            {
                Run(start);
            }
        }
    }

    const std::string& Problem() const
    {
        return problem_;
    }

    const std::map<uint32_t, Item>& Items() const
    {
        return items_;
    }

    // The offset after the last byte that a track reads.
    uint32_t End() const
    {
        return end_;
    }

    // The offset after the command at `offset`.
    uint32_t After(uint32_t offset) const
    {
        return afters_.at(offset);
    }

private:
    // Records the command at `at` and returns it, or nothing after a problem. `next` takes the place after it.
    const Item* Read(uint32_t at, uint32_t& next)
    {
        const auto found = items_.find(at);
        if (found != items_.end())
        {
            next = afters_.at(at);
            return &found->second;
        }

        Reader reader(data_, at);
        bool unknown = false;
        Item item = ReadItem(reader, traits_, unknown);
        if (unknown)
        {
            problem_ = "a track reads a command that the driver has no code for";
            return nullptr;
        }
        if (reader.Past())
        {
            problem_ = "a track runs past the bytes";
            return nullptr;
        }

        next = reader.At();
        const auto after = items_.lower_bound(at);
        if ((after != items_.end() && after->first < next) ||
            (after != items_.begin() && afters_.at(std::prev(after)->first) > at))
        {
            problem_ = "two commands overlap";
            return nullptr;
        }

        afters_[at] = next;
        end_ = std::max(end_, next);
        return &items_.emplace(at, std::move(item)).first->second;
    }

    // Reads a track from `start` to its end, or to the end of its loop after the loop's start: the driver goes back to
    // the loop's start there. A loop's end without a start goes back to a place that an earlier tune left. The track
    // reads on past it.
    void Run(uint32_t start)
    {
        uint32_t at = start;
        bool looped = false;
        while (seen_.emplace(at, looped).second)
        {
            uint32_t next = 0;
            const Item* item = Read(at, next);
            if (!item || item->kind == Kind::kEnd || (item->kind == Kind::kLoopEnd && looped))
            {
                return;
            }

            looped = looped || item->kind == Kind::kLoopStart;
            at = next;
        }
    }

    std::span<const uint8_t> data_;
    const Traits& traits_;
    std::map<uint32_t, Item> items_;
    std::map<uint32_t, uint32_t> afters_;
    std::set<std::pair<uint32_t, bool>> seen_;
    uint32_t end_ = 0;
    std::string problem_;
};

Item DataItem(std::span<const uint8_t> bytes)
{
    Item item;
    item.kind = Kind::kData;
    item.bytes.assign(bytes.begin(), bytes.end());
    return item;
}

// Keeps a sequence whose commands can't be read safely as data, split at the tracks' starts.
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

// Makes the items of the sequence in `data`, from what `flow` found.
Decoded Build(std::span<const uint8_t> data, std::span<const uint32_t> starts, const Flow& flow)
{
    if (!flow.Problem().empty())
    {
        return Opaque(data, starts, flow.Problem());
    }

    Decoded decoded;
    std::vector<Item>& items = decoded.sequence.items;
    std::map<uint32_t, uint32_t> item_at;
    uint32_t at = 0;
    for (const auto& [offset, read] : flow.Items())
    {
        if (offset > at)
        {
            items.push_back(DataItem(data.subspan(at, offset - at)));
        }

        Item item = read;
        item.velocity = ToUnit(item.velocity, commands::Scale{});
        item.value.value = ToUnit(item.value.value, ScaleOf(item.kind));
        item_at[offset] = uint32_t(items.size());
        items.push_back(std::move(item));
        at = flow.After(offset);
    }
    if (data.size() > at)
    {
        items.push_back(DataItem(data.subspan(at)));
    }

    for (const uint32_t start : starts)
    {
        decoded.entries.push_back(item_at.at(start));
    }

    return decoded;
}

// A value of `bytes` bytes. Throws std::invalid_argument outside `low` to the largest that the bytes hold.
int64_t ValueOf(int64_t value, int64_t low, int bytes, Kind kind)
{
    if (value < low || value >= int64_t(1) << (8 * bytes))
    {
        throw std::invalid_argument("a value of a " + std::string(commands::Name(kind)) +
                                    " command doesn't fit in the driver's bytes");
    }

    return value;
}

// Adds the `count` bytes of `value`, little-endian.
void AddValue(std::vector<uint8_t>& bytes, int64_t value, int count)
{
    for (int i = 0; i < count; i++)
    {
        bytes.push_back(uint8_t(value >> (8 * i)));
    }
}

// The bytes of a command, in the console's values.
std::vector<uint8_t> ItemBytes(const Item& item, const Traits& traits)
{
    if (item.conditional || item.time || item.value.source != commands::Source::kValue)
    {
        throw std::invalid_argument("the driver has no condition, time, random value or variable");
    }

    const int64_t value = FromUnit(item.value.value, ScaleOf(item.kind));
    const int64_t velocity = FromUnit(item.velocity, commands::Scale{});
    const uint8_t ignored = item.encoding.arguments.value_or(0);

    // The command's byte, and the channel's byte in the channel-byte format.
    std::vector<uint8_t> bytes;
    const auto start = [&](uint8_t command)
    {
        if (traits.format == Format::kChannelNibble)
        {
            if (item.index > 0x0F)
            {
                throw std::invalid_argument("a command's channel doesn't fit in the high nibble of its byte");
            }
            bytes.push_back(uint8_t(item.index << 4 | command));
            return;
        }

        bytes.push_back(command);
        if (HasChannel(command))
        {
            bytes.push_back(item.index);
        }
        else if (item.index != 0)
        {
            throw std::invalid_argument("a command without a channel holds one");
        }
    };
    const auto controller = [&](uint8_t number, int64_t to)
    {
        start(kCmdControl);
        bytes.push_back(number);
        bytes.push_back(uint8_t(ValueOf(to, 0, 1, item.kind)));
        return bytes;
    };
    const auto flag = [&](uint8_t number)
    {
        if (value != 0)
        {
            throw std::invalid_argument("a loop of the driver runs for ever");
        }
        return controller(number, ignored);
    };

    switch (item.kind)
    {
    case Kind::kTempo:
        start(kCmdTempo);
        AddValue(bytes, ValueOf(value, 0, 3, item.kind), 3);
        return bytes;

    case Kind::kWait:
        {
            ValueOf(value, 0, 3, item.kind);
            const int needed = value > 0xFFFF ? 3 : value > 0xFF ? 2 : 1;
            const int count = std::clamp<int>(item.encoding.value_bytes, needed, 3);
            start(uint8_t(kCmdDelay1 + count - 1));
            AddValue(bytes, value, count);
            return bytes;
        }

    case Kind::kNote:
        if (value != 0)
        {
            throw std::invalid_argument("a note of the driver holds no length");
        }
        start(item.encoding.opcode == kCmdNoteOnB ? kCmdNoteOnB : kCmdNoteOn);
        bytes.push_back(item.key);
        bytes.push_back(uint8_t(ValueOf(velocity, 0, 1, item.kind)));
        return bytes;

    case Kind::kNoteOff:
        start(kCmdNoteOff);
        bytes.push_back(item.key);
        bytes.push_back(ignored);
        return bytes;

    case Kind::kProgram:
        start(kCmdProgram);
        bytes.push_back(uint8_t(ValueOf(value, 0, 1, item.kind)));
        return bytes;

    case Kind::kPitchBend:
        start(kCmdBend);
        AddValue(bytes, ValueOf(value, 0, 2, item.kind), 2);
        return bytes;

    case Kind::kEnd:
        start(kCmdEnd);
        return bytes;

    case Kind::kLfoDepth:
        return controller(kCtrlModulation, value);
    case Kind::kVolume:
        return controller(kCtrlVolume, value);

    case Kind::kAttack:
    case Kind::kDecay:
    case Kind::kSustain:
    case Kind::kRelease:
        if (!traits.envelope_controllers)
        {
            throw std::invalid_argument("this revision of the driver ignores controllers 20 to 23");
        }
        return controller(uint8_t(kCtrlAttack + (item.kind == Kind::kAttack    ? 0
                                                 : item.kind == Kind::kDecay   ? 1
                                                 : item.kind == Kind::kSustain ? 2
                                                                               : 3)),
                          value);

    case Kind::kLoopStart:
        return flag(kCtrlLoopStart);
    case Kind::kLoopEnd:
        return flag(kCtrlLoopEnd);

    case Kind::kMonophonic:
        if (value != 0 && value != 1)
        {
            throw std::invalid_argument("a monophonic command of the driver turns the mode on or off");
        }
        return controller(value ? kCtrlMonoOn : kCtrlPolyOn, ignored);

    case Kind::kOther:
        if (item.encoding.opcode == kCmdPressure)
        {
            start(kCmdPressure);
            bytes.push_back(ignored);
            return bytes;
        }
        if (item.encoding.opcode == kCmdNop)
        {
            start(kCmdNop);
            return bytes;
        }
        if (item.encoding.opcode >= kController && item.encoding.opcode <= kController + 0xFF)
        {
            return controller(uint8_t(item.encoding.opcode - kController), value);
        }
        throw std::invalid_argument("the driver has no command with that opcode");

    default:
        throw std::invalid_argument("the driver has no " + std::string(commands::Name(item.kind)) + " command");
    }
}

} // namespace

Traits TraitsOf(const DriverInfo& info)
{
    return {info.format, info.envelope_controllers};
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
            throw std::invalid_argument("a track starts outside the sequence's bytes");
        }
    }

    const Flow flow(data, traits, starts);
    return Build(data, starts, flow);
}

int SequenceCount(const DriverInfo& info)
{
    return info.tune_count;
}

int SequenceOfSong(const DriverInfo& info, int song)
{
    if (song < 0 || song >= info.tune_count)
    {
        throw std::out_of_range("the driver holds no song " + std::to_string(song));
    }

    return song;
}

uint32_t SequenceAddress(const Rom& rom, const DriverInfo& info, int sequence)
{
    if (sequence < 0 || sequence >= SequenceCount(info))
    {
        throw std::out_of_range("the driver holds no sequence " + std::to_string(sequence));
    }

    TuneHeader header;
    if (!ReadTuneHeader(rom, TuneAddress(rom, info, sequence), header))
    {
        return 0;
    }

    return *std::min_element(header.tracks.begin(), header.tracks.end());
}

Decoded DecodeRomSequence(const Rom& rom, const DriverInfo& info, int sequence)
{
    if (sequence < 0 || sequence >= SequenceCount(info))
    {
        throw std::out_of_range("the driver holds no sequence " + std::to_string(sequence));
    }

    TuneHeader header;
    if (!ReadTuneHeader(rom, TuneAddress(rom, info, sequence), header))
    {
        throw std::invalid_argument("tune " + std::to_string(sequence) + " has no header that the driver can read");
    }

    const uint32_t address = *std::min_element(header.tracks.begin(), header.tracks.end());
    std::vector<uint32_t> starts;
    for (const uint32_t track : header.tracks)
    {
        starts.push_back(track - address);
    }

    const Traits traits = TraitsOf(info);
    const std::span<const uint8_t> data(rom.Ptr(address), rom.End() - address);
    const Flow flow(data, traits, starts);

    // The sequence ends with the last byte that a track reads, or at the next sequence where that lies close after it.
    uint32_t size = std::max<uint32_t>(flow.End(), 1);
    uint32_t next = rom.End();
    for (int s = 0; s < SequenceCount(info); s++)
    {
        const uint32_t other = SequenceAddress(rom, info, s);
        if (other >= address + size && other < next)
        {
            next = other;
        }
    }
    if (next - (address + size) < kSequenceGap && next < rom.End())
    {
        size = next - address;
    }

    return Build(data.first(size), starts, flow);
}

std::vector<uint32_t> ItemOffsets(const commands::Sequence& sequence, const Traits& traits)
{
    std::vector<uint32_t> offsets;
    offsets.reserve(sequence.items.size() + 1);
    uint32_t at = 0;
    for (const Item& item : sequence.items)
    {
        offsets.push_back(at);
        at += uint32_t(item.kind == Kind::kData ? item.bytes.size() : ItemBytes(item, traits).size());
    }
    offsets.push_back(at);

    return offsets;
}

Encoded EncodeSequence(const commands::Sequence& sequence, std::span<const uint32_t> entries, const Traits& traits)
{
    const std::vector<Item>& items = sequence.items;
    Encoded encoded;
    uint32_t at = 0;
    bool any_command = false;
    for (const Item& item : items)
    {
        const std::vector<uint8_t> bytes = item.kind == Kind::kData ? item.bytes : ItemBytes(item, traits);
        encoded.offsets.push_back(at);
        encoded.bytes.insert(encoded.bytes.end(), bytes.begin(), bytes.end());
        at += uint32_t(bytes.size());
        any_command = any_command || item.kind != Kind::kData;
    }
    encoded.offsets.push_back(at);

    std::vector<uint32_t> starts;
    for (const uint32_t entry : entries)
    {
        if (entry >= items.size())
        {
            throw std::invalid_argument("a track starts past the sequence's items");
        }
        starts.push_back(encoded.offsets[entry]);
    }

    // The driver has to read the bytes as the items say. A sequence kept as data holds no commands.
    if (!any_command)
    {
        return encoded;
    }

    const Flow flow(encoded.bytes, traits, starts);
    if (!flow.Problem().empty())
    {
        throw std::invalid_argument("the sequence's items don't read back: " + flow.Problem());
    }

    std::map<uint32_t, size_t> item_at;
    for (size_t i = 0; i < items.size(); i++)
    {
        if (encoded.offsets[i + 1] > encoded.offsets[i])
        {
            item_at[encoded.offsets[i]] = i;
        }
    }
    for (const auto& [offset, read] : flow.Items())
    {
        const auto found = item_at.find(offset);
        if (found == item_at.end() || items[found->second].kind == Kind::kData)
        {
            throw std::invalid_argument("a track reaches bytes that no item starts at");
        }
        if (items[found->second].kind != read.kind)
        {
            throw std::invalid_argument("a command's bytes read as a command of another kind");
        }
    }

    return encoded;
}

commands::Scale ScaleOf(Kind kind)
{
    switch (kind)
    {
    case Kind::kPitchBend:
        return {kOne / 0x2000, -kOne};
    default:
        return {};
    }
}

} // namespace ntdlib::rare
