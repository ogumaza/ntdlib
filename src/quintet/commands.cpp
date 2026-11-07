// SPDX-License-Identifier: MIT

#include "ntdlib/quintet/commands.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/quintet/song.h"

namespace ntdlib::quintet
{
namespace
{

using commands::FromUnit;
using commands::Kind;
using commands::ToUnit;
using Item = commands::Command;

// The size of each command with arguments, in the A revision's numbering. The others take a byte.
uint32_t CommandSize(uint8_t command)
{
    switch (command)
    {
    case 0xD0:
    case 0xE6:
        return 3;
    case 0xDE:
    case 0xDF:
    case 0xE7:
        return 4;
    case 0xD2:
    case 0xE2:
        return 6;
    case 0x0E:
    case 0x1F:
    case 0x2E:
    case 0x2F:
    case 0x3E:
    case 0x4F:
    case 0x5E:
    case 0x7F:
    case 0x8E:
    case 0x8F:
    case 0x9E:
    case 0x9F:
    case 0xAE:
    case 0xAF:
    case 0xBE:
    case 0xCE:
    case 0xCF:
    case 0xD5:
    case 0xD7:
    case 0xDA:
    case 0xDB:
    case 0xDC:
    case 0xDD:
    case 0xE4:
    case 0xE5:
    case 0xE8:
    case 0xE9:
    case 0xEA:
    case 0xEC:
    case 0xED:
    case 0xF1:
        return 2;
    default:
        return 1;
    }
}

// The S revision's byte for the A revision's command `command`, or 0 if the S revision has no such command.
uint8_t ByteOfS(uint8_t command)
{
    if (command == 0xFF)
    {
        return 0xFF;
    }
    if (command == 0xF2)
    {
        return 0;
    }

    for (int b = kFirstCommandS; b < 0xFF; b++)
    {
        if (b != kSkipByteS && CommandOfS(uint8_t(b)) == command)
        {
            return uint8_t(b);
        }
    }

    return 0;
}

// The ticks of a note value, as the driver works them out.
int64_t TicksOf(uint8_t value)
{
    return value == 0 ? 0 : kWholeNote / value;
}

// The code of a note's length: the length table's index, or in the S revision the note value.
uint8_t LengthCode(const Item& item, int64_t ticks, const Traits& traits)
{
    if (item.encoding.length_code)
    {
        const uint8_t code = *item.encoding.length_code;
        const int64_t given = traits.byte_notes              ? TicksOf(code)
                              : code < traits.lengths.size() ? TicksOf(traits.lengths[code])
                                                             : -1;
        if (given != ticks)
        {
            throw std::invalid_argument("a note's length code gives another length");
        }
        return code;
    }

    if (traits.byte_notes)
    {
        for (int value = 1; value <= 0xFF; value++)
        {
            if (TicksOf(uint8_t(value)) == ticks)
            {
                return uint8_t(value);
            }
        }
    }
    else
    {
        for (size_t index = 0; index < traits.lengths.size(); index++)
        {
            if (TicksOf(traits.lengths[index]) == ticks)
            {
                return uint8_t(index);
            }
        }
    }

    throw std::invalid_argument("no note value of the driver gives a length of " + std::to_string(ticks) + " ticks");
}

// The usual code of a note's length, or nothing where it has none.
std::optional<uint8_t> UsualCode(int64_t ticks, const Traits& traits)
{
    try
    {
        Item item;
        return LengthCode(item, ticks, traits);
    }
    catch (const std::invalid_argument&)
    {
        return std::nullopt;
    }
}

// Reads the command at `at` in the console's values. Sets `past` if it runs past the bytes.
Item ReadItem(std::span<const uint8_t> data, uint32_t at, const Traits& traits, uint32_t& size, bool& past)
{
    const auto byte = [&](uint32_t i) -> uint8_t
    {
        if (at + i >= data.size())
        {
            past = true;
            return 0;
        }
        return data[at + i];
    };

    Item item;
    const uint8_t first = byte(0);

    // A note, a play again, a rest or a wait.
    int pitch = first & 15;
    const int length = first >> 4;
    if (traits.byte_notes ? first < kFirstCommandS : pitch <= 13 && length <= 12)
    {
        uint8_t code = uint8_t(length);
        uint8_t value = 0;
        if (traits.byte_notes)
        {
            code = byte(1);
            value = code;
            size = 2;
            pitch = first <= 11 ? first : first == kPlayAgainS ? 12 : first == kRestS ? 13 : -1;
        }
        else
        {
            value = traits.lengths[size_t(length)];
            size = 1;
        }

        const int64_t ticks = TicksOf(value);
        if (pitch < 0)
        {
            item.kind = Kind::kWait;
            item.key = first;
        }
        else if (pitch == 13)
        {
            item.kind = Kind::kNoteOff;
        }
        else
        {
            item.kind = Kind::kNote;
            item.key = uint8_t(pitch == 12 ? 0 : pitch);
            item.index = pitch == 12 ? 1 : 0;
        }
        item.value.value = ticks;
        if (UsualCode(ticks, traits) != code)
        {
            item.encoding.length_code = code;
        }
        return item;
    }

    // The S revision's command that skips a byte, and its bytes without a command.
    if (traits.byte_notes && (first == kSkipByteS || (first != 0xFF && CommandOfS(first) == 0xF2)))
    {
        item.kind = Kind::kOther;
        item.encoding.opcode = uint16_t(kKeptByte + first);
        size = first == kSkipByteS ? 2 : 1;
        if (first == kSkipByteS)
        {
            item.bytes.push_back(byte(1));
        }
        return item;
    }

    // A command of one byte reads no argument. Its byte can be the last.
    const uint8_t command = traits.byte_notes ? CommandOfS(first) : first;
    size = CommandSize(command);
    const uint8_t arg = size > 1 ? byte(1) : 0;
    const auto set = [&](Kind kind, uint8_t index, int64_t value)
    {
        item.kind = kind;
        item.index = index;
        item.value.value = value;
    };

    switch (command)
    {
    case 0x7F:
        set(Kind::kTempo, 0, arg);
        break;
    case 0x5E:
        set(Kind::kVolume, 0, arg);
        break;
    case 0xF1:
        set(Kind::kVolume, 1, int8_t(arg));
        break;
    case 0xE4:
        set(Kind::kTranspose, 0, int8_t(arg));
        break;
    case 0xE9:
        set(Kind::kTranspose, 1, int8_t(arg));
        break;
    case 0xD5:
        set(Kind::kTune, 0, int8_t(arg));
        break;
    case 0xE6:
        set(Kind::kTune, 1, int16_t(arg | byte(2) << 8));
        break;
    case 0xDC:
    case 0xDD:
        set(Kind::kProgram, 0, arg);
        item.encoding.opcode = command == 0xDD ? 0xDD : 0;
        break;
    case 0xDB:
        set(Kind::kProgram, 1, arg);
        break;
    case 0xDA:
        set(Kind::kProgram, 2, arg);
        break;
    case 0xD4:
        item.kind = Kind::kTie;
        break;
    case 0xD6:
        item.kind = Kind::kLoopStart;
        break;
    case 0xD7:
        set(Kind::kLoopEnd, 0, arg ? arg : 256);
        break;
    case 0xFF:
        item.kind = Kind::kEnd;
        break;
    default:
        item.kind = Kind::kOther;
        item.encoding.opcode = command;
        for (uint32_t i = 1; i < size; i++)
        {
            item.bytes.push_back(byte(i));
        }
        break;
    }

    return item;
}

// Follows each channel from its start to its FF, and records each command that it reads.
class Flow
{
public:
    Flow(std::span<const uint8_t> data, const Traits& traits, std::span<const uint32_t> starts)
        : data_(data), traits_(traits)
    {
        for (const uint32_t start : starts)
        {
            Run(start);
            if (!problem_.empty())
            {
                return;
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

    // The offset after the command at `offset`.
    uint32_t After(uint32_t offset) const
    {
        return afters_.at(offset);
    }

private:
    void Run(uint32_t start)
    {
        uint32_t at = start;
        while (true)
        {
            const auto found = items_.find(at);
            if (found != items_.end())
            {
                // Another channel read on from here already.
                return;
            }

            uint32_t size = 1;
            bool past = false;
            Item item = ReadItem(data_, at, traits_, size, past);
            if (past)
            {
                problem_ = "a channel runs past the song's bytes";
                return;
            }

            const uint32_t next = at + size;
            const auto after = items_.lower_bound(at);
            if ((after != items_.end() && after->first < next) ||
                (after != items_.begin() && afters_.at(std::prev(after)->first) > at))
            {
                problem_ = "two commands overlap";
                return;
            }

            const bool end = item.kind == Kind::kEnd;
            afters_[at] = next;
            items_.emplace(at, std::move(item));
            if (end)
            {
                return;
            }
            at = next;
        }
    }

    std::span<const uint8_t> data_;
    const Traits& traits_;
    std::map<uint32_t, Item> items_;
    std::map<uint32_t, uint32_t> afters_;
    std::string problem_;
};

Item DataItem(std::span<const uint8_t> bytes)
{
    Item item;
    item.kind = Kind::kData;
    item.bytes.assign(bytes.begin(), bytes.end());
    return item;
}

// Keeps a song whose commands can't be read safely as data, split at the channels' starts.
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

// Makes the items of the song in `data`, from what `flow` found.
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

// A value from `low` to `high`. Throws std::invalid_argument outside them.
int64_t ValueOf(int64_t value, int64_t low, int64_t high, Kind kind)
{
    if (value < low || value > high)
    {
        throw std::invalid_argument("a value of a " + std::string(commands::Name(kind)) +
                                    " command doesn't fit in the driver's bytes");
    }

    return value;
}

// The bytes of a command, in the console's values.
std::vector<uint8_t> ItemBytes(const Item& item, const Traits& traits)
{
    if (item.conditional || item.time || item.value.source != commands::Source::kValue)
    {
        throw std::invalid_argument("the driver has no condition, time, random value or variable");
    }

    const int64_t value = FromUnit(item.value.value, ScaleOf(item.kind));
    std::vector<uint8_t> bytes;

    // A command in the A revision's numbering, with its arguments.
    const auto command = [&](uint8_t number, std::vector<uint8_t> arguments)
    {
        uint8_t first = number;
        if (traits.byte_notes)
        {
            first = ByteOfS(number);
            if (first == 0)
            {
                throw std::invalid_argument("the S revision of the driver has no command " + std::to_string(number));
            }
        }
        if (arguments.size() + 1 != CommandSize(number))
        {
            throw std::invalid_argument("a command of the driver holds another number of arguments");
        }

        bytes.push_back(first);
        bytes.insert(bytes.end(), arguments.begin(), arguments.end());
        return bytes;
    };
    const auto only = [&](uint8_t index)
    {
        if (item.index != index)
        {
            throw std::invalid_argument("the driver has no " + std::string(commands::Name(item.kind)) +
                                        " command with that index");
        }
    };

    // A note, a rest or a wait: the pitch, and the note value or its index.
    const auto note = [&](int pitch)
    {
        const uint8_t code = LengthCode(item, value, traits);
        if (traits.byte_notes)
        {
            bytes.push_back(uint8_t(pitch));
            bytes.push_back(code);
            return bytes;
        }

        bytes.push_back(uint8_t(code << 4 | pitch));
        return bytes;
    };

    switch (item.kind)
    {
    case Kind::kNote:
        if (item.index > 1 || item.key > 11 || (item.index == 1 && item.key != 0))
        {
            throw std::invalid_argument("a note of the driver has a pitch from 0 to 11, or plays the last note again");
        }
        if (item.index == 1)
        {
            return note(traits.byte_notes ? kPlayAgainS : 12);
        }
        return note(item.key);

    case Kind::kNoteOff:
        only(0);
        return note(traits.byte_notes ? kRestS : 13);

    case Kind::kWait:
        if (!traits.byte_notes || item.key < 12 || item.key >= kFirstCommandS || item.key == kPlayAgainS ||
            item.key == kRestS)
        {
            throw std::invalid_argument("only the S revision's notes of a pitch from 12 to 7F wait");
        }
        return note(item.key);

    case Kind::kTempo:
        only(0);
        return command(0x7F, {uint8_t(ValueOf(value, 0, 0xFF, item.kind))});

    case Kind::kVolume:
        if (item.index == 0)
        {
            return command(0x5E, {uint8_t(ValueOf(value, 0, 0xFF, item.kind))});
        }
        only(1);
        return command(0xF1, {uint8_t(ValueOf(value, -0x80, 0x7F, item.kind))});

    case Kind::kTranspose:
        if (item.index == 0)
        {
            return command(0xE4, {uint8_t(ValueOf(value, -0x80, 0x7F, item.kind))});
        }
        only(1);
        return command(0xE9, {uint8_t(ValueOf(value, -0x80, 0x7F, item.kind))});

    case Kind::kTune:
        if (item.index == 0)
        {
            return command(0xD5, {uint8_t(ValueOf(value, -0x80, 0x7F, item.kind))});
        }
        {
            only(1);
            const int64_t tune = ValueOf(value, -0x8000, 0x7FFF, item.kind);
            return command(0xE6, {uint8_t(tune), uint8_t(tune >> 8)});
        }

    case Kind::kProgram:
        {
            const uint8_t program = uint8_t(ValueOf(value, 0, 0xFF, item.kind));
            if (item.index == 0)
            {
                return command(item.encoding.opcode == 0xDD ? 0xDD : 0xDC, {program});
            }
            if (item.index == 1)
            {
                return command(0xDB, {program});
            }
            only(2);
            return command(0xDA, {program});
        }

    case Kind::kTie:
        only(0);
        return command(0xD4, {});

    case Kind::kLoopStart:
        only(0);
        if (value != 0)
        {
            throw std::invalid_argument("a repeat's start holds no passes");
        }
        return command(0xD6, {});

    case Kind::kLoopEnd:
        only(0);
        return command(0xD7, {uint8_t(ValueOf(value, 1, 256, item.kind))});

    case Kind::kEnd:
        only(0);
        return command(0xFF, {});

    case Kind::kOther:
        if (item.encoding.opcode >= kKeptByte)
        {
            const uint16_t byte = uint16_t(item.encoding.opcode - kKeptByte);
            const bool skip = byte == kSkipByteS;
            if (!traits.byte_notes || byte < kFirstCommandS || byte > 0xFE ||
                (!skip && CommandOfS(uint8_t(byte)) != 0xF2) || item.bytes.size() != (skip ? 1u : 0u))
            {
                throw std::invalid_argument("the driver has no such byte");
            }
            bytes.push_back(uint8_t(byte));
            bytes.insert(bytes.end(), item.bytes.begin(), item.bytes.end());
            return bytes;
        }
        {
            const uint16_t number = item.encoding.opcode;
            if (number > 0xFE)
            {
                throw std::invalid_argument("the driver has no command with that number");
            }

            // A note's byte isn't a command.
            const int pitch = number & 15;
            if (pitch <= 13 && (number >> 4) <= 12)
            {
                throw std::invalid_argument("the byte of a command reads as a note");
            }
            switch (number)
            {
            case 0x7F:
            case 0x5E:
            case 0xF1:
            case 0xE4:
            case 0xE9:
            case 0xD5:
            case 0xE6:
            case 0xDC:
            case 0xDD:
            case 0xDB:
            case 0xDA:
            case 0xD4:
            case 0xD6:
            case 0xD7:
                throw std::invalid_argument("a command of the driver with a kind is kept as another");
            default:
                break;
            }
            return command(uint8_t(number), item.bytes);
        }

    default:
        throw std::invalid_argument("the driver has no " + std::string(commands::Name(item.kind)) + " command");
    }
}

} // namespace

Traits TraitsOf(const Rom& rom, const DriverInfo& info)
{
    Traits traits;
    traits.byte_notes = quintet::TraitsOf(info.revision).byte_notes;
    if (!traits.byte_notes && info.length_table)
    {
        for (size_t i = 0; i < traits.lengths.size(); i++)
        {
            traits.lengths[i] = rom.U8(info.length_table + uint32_t(i));
        }
    }

    return traits;
}

Decoded DecodeSequence(std::span<const uint8_t> data, std::span<const uint32_t> starts, const Traits& traits)
{
    for (const uint32_t start : starts)
    {
        if (start >= data.size())
        {
            throw std::invalid_argument("a channel starts outside the song's bytes");
        }
    }

    const Flow flow(data, traits, starts);
    return Build(data, starts, flow);
}

int SequenceCount(const DriverInfo& info)
{
    return SongCount(info);
}

int SequenceOfSong(const DriverInfo& info, int song)
{
    if (song < 0 || song >= SongCount(info))
    {
        throw std::out_of_range("the music file holds no song " + std::to_string(song));
    }

    return song;
}

Decoded DecodeRomSequence(const Rom& rom, const DriverInfo& info, int sequence)
{
    if (sequence < 0 || sequence >= SequenceCount(info))
    {
        throw std::out_of_range("the music file holds no sequence " + std::to_string(sequence));
    }

    SongHeader header;
    if (!ReadSongHeader(rom, SongAddress(info, sequence), header))
    {
        throw std::invalid_argument("song " + std::to_string(sequence) + " has no header that the driver can read");
    }

    std::vector<uint32_t> starts;
    for (const uint32_t channel : header.channels)
    {
        starts.push_back(channel - header.address - kHeaderBytes);
    }

    const std::span<const uint8_t> data(rom.Ptr(header.address + kHeaderBytes), header.length - kHeaderBytes);
    return DecodeSequence(data, starts, TraitsOf(rom, info));
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
            throw std::invalid_argument("a channel starts past the song's items");
        }
        starts.push_back(encoded.offsets[entry]);
    }

    // The driver has to read the bytes as the items say. A song kept as data holds no commands.
    if (!any_command)
    {
        return encoded;
    }

    const Flow flow(encoded.bytes, traits, starts);
    if (!flow.Problem().empty())
    {
        throw std::invalid_argument("the song's items don't read back: " + flow.Problem());
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
            throw std::invalid_argument("a channel reaches bytes that no item starts at");
        }
        if (items[found->second].kind != read.kind)
        {
            throw std::invalid_argument("a command's bytes read as a command of another kind");
        }
    }

    return encoded;
}

commands::Scale ScaleOf(Kind)
{
    return {};
}

} // namespace ntdlib::quintet
