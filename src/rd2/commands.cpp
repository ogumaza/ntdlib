// SPDX-License-Identifier: MIT

#include "ntdlib/rd2/commands.h"

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

#include "xsflib/bytes.h"

#include "ntdlib/agb/data_items.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/rd2/sequencer.h"

namespace ntdlib::rd2
{
namespace
{

using commands::FromUnit;
using commands::HasTarget;
using commands::Kind;
using commands::kOne;
using commands::ToUnit;
using Item = commands::Command;
using agb::DataItem;

// A track's settings for the notes and waits that give none, as the driver starts a track.
struct Running
{
    uint16_t length = 0x7F;
    uint8_t velocity = 0x7F;
    uint16_t wait = 0;
};

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

    uint16_t Halfword()
    {
        const uint32_t low = Byte();
        return uint16_t(low | uint32_t(Byte()) << 8);
    }

    uint16_t Length(uint8_t& value_bytes)
    {
        const uint32_t first = Byte();
        if (!(first & 0x80))
        {
            return uint16_t(first);
        }

        const uint16_t value = uint16_t((first & 0x7F) << 8 | Byte());
        value_bytes = value < 0x80 ? 2 : 0;
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

// Reads a command in the console's values. A target stays a byte offset, with `target_outside` set.
// `running` gives the values of a note or a wait that leaves them out, and takes those that a command gives.
Item ReadItem(Reader& r, const RevisionTraits& traits, Running& running)
{
    Item item;
    const uint8_t op = r.Byte();
    const auto set = [&item](Kind kind, int64_t value = 0, uint8_t index = 0)
    {
        item.kind = kind;
        item.value.value = value;
        item.index = index;
    };
    const auto target = [&item, &r]
    {
        item.target = r.Halfword();
        item.target_outside = true;
    };

    if (op <= 0x5F)
    {
        set(Kind::kNote, running.length);
        item.key = op;
        item.velocity = running.velocity;
        item.encoding.repeated = true;
        return item;
    }
    if (op <= 0xBF)
    {
        running.length = r.Length(item.encoding.value_bytes);
        running.velocity = r.Byte();
        set(Kind::kNote, running.length);
        item.key = uint8_t(op - 0x60);
        item.velocity = running.velocity;
        return item;
    }
    if ((op & 0xF0) == 0xD0)
    {
        item.kind = Kind::kPortamentoFrom;
        item.index = op & 0x0F;
        item.key = r.Byte();
        item.value.value = r.Byte();
        item.velocity = op & 1 ? r.Byte() : 0;
        return item;
    }

    switch (op)
    {
    case 0xC0:
        set(Kind::kWait, running.wait);
        item.encoding.repeated = true;
        break;

    case 0xC1:
        running.wait = r.Length(item.encoding.value_bytes);
        set(Kind::kWait, running.wait);
        break;

    case 0xC2:
        set(Kind::kProgram, r.Byte());
        break;

    case 0xC3:
        set(Kind::kPan, r.Byte());
        break;

    case 0xC4:
        set(Kind::kPriority, r.Byte());
        break;

    case 0xC5:
        set(Kind::kTie, 1);
        break;

    case 0xC6:
        set(Kind::kTie, 0);
        break;

    case 0xC7:
        set(Kind::kBankSelect, r.Byte());
        break;

    case 0xC8:
        set(Kind::kNoteWait, 1);
        break;

    case 0xC9:
        set(Kind::kNoteWait, 0);
        break;

    case 0xCA:
        set(Kind::kUserProcedure, r.Byte());
        break;

    case 0xE0:
        set(Kind::kVolume, r.Byte());
        break;

    case 0xE1:
        set(Kind::kPitchBend, int8_t(r.Byte()));
        break;

    case 0xE2:
        set(Kind::kBendRange, r.Byte());
        break;

    case 0xE3:
        set(Kind::kSend, r.Byte());
        break;

    case 0xE4:
        if (traits.byte_tempo)
        {
            set(Kind::kTempo, r.Byte());
        }
        else
        {
            set(Kind::kTempo, r.Length(item.encoding.value_bytes));
        }
        break;

    case 0xE5:
        set(Kind::kLfoDelay, r.Byte());
        break;

    case 0xE6:
        set(Kind::kLfoSpeed, r.Byte());
        break;

    case 0xE7:
        set(Kind::kLfoDepth, r.Byte());
        break;

    case 0xE8:
        set(Kind::kPortamento);
        break;

    case 0xE9:
        set(Kind::kTranspose, int8_t(r.Byte()));
        break;

    case 0xEA:
        if (traits.player_volume)
        {
            set(Kind::kMainVolume, r.Byte());
        }
        else
        {
            item.kind = Kind::kOther;
            item.encoding.opcode = op;
        }
        break;

    case 0xF0:
        item.kind = Kind::kJump;
        target();
        break;

    case 0xF4:
        item.kind = Kind::kCall;
        target();
        break;

    case 0xF8:
        item.kind = Kind::kOpenTrack;
        item.index = r.Byte();
        target();
        break;

    case 0xFF:
        item.kind = Kind::kEnd;
        break;

    default:
        item.kind = Kind::kOther;
        item.encoding.opcode = op;
        break;
    }

    return item;
}

// The tracks that a header names.
struct Header
{
    uint32_t bytes = 0;            // the header's size
    std::vector<uint16_t> offsets; // each track's offset, or 0 for none
    std::string problem;
};

Header ReadHeader(std::span<const uint8_t> data, bool effects)
{
    Header header;
    const auto halfword = [&data](uint32_t at)
    {
        return xsflib::LoadLe16(&data[at]);
    };

    if (effects)
    {
        // The table ends where its first track starts, as the game's request for an effect reads it.
        uint32_t end = 0x10000;
        for (uint32_t t = 0; 2 * t < end && 2 * t + 1 < data.size(); t++)
        {
            const uint16_t offset = halfword(2 * t);
            header.offsets.push_back(offset);
            if (offset != 0)
            {
                end = std::min<uint32_t>(end, offset);
            }
        }
        if (end == 0x10000)
        {
            header.offsets.clear();
            header.problem = "the table names no track";
        }
        else if (end % 2 != 0)
        {
            header.problem = "a track starts inside the table";
        }

        header.bytes = 2 * uint32_t(header.offsets.size());
        return header;
    }

    if (data.size() < 2)
    {
        header.problem = "the sequence holds no header";
        return header;
    }

    const int count = int8_t(data[0]);
    if (count < 0 || count > kPlayerTracks)
    {
        header.problem = "the header's count of tracks lies outside a player's tracks";
        return header;
    }

    header.bytes = 2 + 2 * uint32_t(count);
    if (header.bytes > data.size())
    {
        header.bytes = 2;
        header.problem = "the header runs past the bytes";
        return header;
    }

    for (int t = 0; t < count; t++)
    {
        header.offsets.push_back(halfword(2 + 2 * uint32_t(t)));
    }

    return header;
}

// Follows a sequence's tracks as the driver plays them, and records each command. A command's values are the first
// reading's, in the console's values. A target stays a byte offset.
class Flow
{
public:
    Flow(std::span<const uint8_t> data, const RevisionTraits& traits, const Header& header)
        : data_(data), traits_(traits), header_bytes_(header.bytes), problem_(header.problem)
    {
        if (problem_.empty())
        {
            for (const uint16_t offset : header.offsets)
            {
                if (offset != 0)
                {
                    Run(offset);
                }
            }
        }

        for (auto& [offset, item] : items_)
        {
            if (item.kind == Kind::kEnd && !ends_.contains(offset))
            {
                item.kind = Kind::kReturn;
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

    // The offset after the last byte that a track reads, or after the header.
    uint32_t End() const
    {
        return std::max(end_, header_bytes_);
    }

    // The offset after the command at `offset`.
    uint32_t After(uint32_t offset) const
    {
        return afters_.at(offset);
    }

private:
    // A track's place, with the places that its calls return to.
    struct Walk
    {
        uint32_t at = 0;
        std::vector<uint32_t> returns;
        Running running;
    };

    // Records the command at `at` and returns it, or nothing after a problem. `next` takes the place after it.
    const Item* Read(uint32_t at, Running& running, uint32_t& next)
    {
        const auto found = items_.find(at);
        if (found != items_.end())
        {
            // Another path has read the command. The command still gives this track its values.
            Reader again(data_, at);
            ReadItem(again, traits_, running);
            next = again.At();
            return &found->second;
        }

        Reader reader(data_, at);
        Item item = ReadItem(reader, traits_, running);
        if (reader.Past())
        {
            problem_ = "a track runs past the bytes";
            return nullptr;
        }

        next = reader.At();
        if (at < header_bytes_)
        {
            problem_ = "a track reaches the header";
            return nullptr;
        }

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

    void Run(uint32_t start)
    {
        std::vector<Walk> walks;
        walks.push_back({start, {}, {}});
        while (!walks.empty() && problem_.empty())
        {
            Walk walk = std::move(walks.back());
            walks.pop_back();
            while (seen_.emplace(walk.at, walk.returns).second)
            {
                uint32_t next = 0;
                const Item* item = Read(walk.at, walk.running, next);
                if (!item)
                {
                    return;
                }

                if (item->kind == Kind::kJump)
                {
                    next = item->target;
                }
                else if (item->kind == Kind::kCall)
                {
                    // The driver keeps kMaxDepth places, and frees a track that nests deeper.
                    if (walk.returns.size() == size_t(kMaxDepth))
                    {
                        break;
                    }

                    walk.returns.push_back(next);
                    next = item->target;
                }
                else if (item->kind == Kind::kOpenTrack)
                {
                    // The driver starts no track past a player's tracks.
                    if (item->index < kPlayerTracks)
                    {
                        walks.push_back({item->target, {}, {}});
                    }
                }
                else if (item->kind == Kind::kEnd)
                {
                    if (walk.returns.empty())
                    {
                        ends_.insert(walk.at);
                        break;
                    }

                    next = walk.returns.back();
                    walk.returns.pop_back();
                }

                walk.at = next;
            }
        }
    }

    std::span<const uint8_t> data_;
    const RevisionTraits& traits_;
    uint32_t header_bytes_;
    std::map<uint32_t, Item> items_;
    std::map<uint32_t, uint32_t> afters_;
    std::set<std::pair<uint32_t, std::vector<uint32_t>>> seen_;
    std::set<uint32_t> ends_; // the FF commands that a track reaches outside every call
    uint32_t end_ = 0;
    std::string problem_;
};

bool IsHeaderEntry(const Item& item)
{
    return item.kind == Kind::kOpenTrack && item.encoding.opcode == kHeaderEntry;
}

// Keeps a sequence whose commands can't be read safely as data, split at the tracks' starts.
Decoded Opaque(std::span<const uint8_t> data, const Header& header, std::string reason)
{
    Decoded decoded;
    decoded.opaque = true;
    decoded.reason = std::move(reason);

    std::set<uint32_t> splits;
    for (const uint16_t offset : header.offsets)
    {
        if (offset != 0 && offset < data.size())
        {
            splits.insert(offset);
        }
    }

    const std::map<uint32_t, uint32_t> items = agb::AppendDataItems(data, splits, decoded.sequence.items);
    for (const uint16_t offset : header.offsets)
    {
        const auto found = items.find(offset);
        decoded.entries.push_back(offset != 0 && found != items.end() ? found->second : kNoEntry);
    }

    return decoded;
}

// Makes the items of the sequence in `data`, from what `flow` found.
Decoded Build(std::span<const uint8_t> data, const Header& header, bool effects, const Flow& flow)
{
    if (!flow.Problem().empty())
    {
        return Opaque(data, header, flow.Problem());
    }

    Decoded decoded;
    std::vector<Item>& items = decoded.sequence.items;
    if (!effects)
    {
        items.push_back(DataItem(data.first(2)));
    }

    const size_t first_entry = items.size();
    for (size_t t = 0; t < header.offsets.size(); t++)
    {
        Item item;
        item.kind = Kind::kOpenTrack;
        item.index = uint8_t(t);
        item.target = header.offsets[t];
        item.target_outside = true;
        item.encoding.opcode = kHeaderEntry;
        items.push_back(std::move(item));
    }

    std::map<uint32_t, uint32_t> item_at;
    uint32_t at = header.bytes;
    for (const auto& [offset, read] : flow.Items())
    {
        if (offset > at)
        {
            items.push_back(DataItem(data.subspan(at, offset - at)));
        }

        Item item = read;
        item.velocity = ToUnit(item.velocity, commands::Scale{});
        item.value.value = ToUnit(item.value.value, ScaleOf(item.kind));
        if (HasTarget(item.kind))
        {
            item.target_outside = true; // until the items are known
        }

        item_at[offset] = uint32_t(items.size());
        items.push_back(std::move(item));
        at = flow.After(offset);
    }
    if (data.size() > at)
    {
        items.push_back(DataItem(data.subspan(at)));
    }

    for (Item& item : items)
    {
        if (HasTarget(item.kind) && !(IsHeaderEntry(item) && item.target == 0))
        {
            const auto found = item_at.find(item.target);
            if (found != item_at.end())
            {
                item.target = found->second;
                item.target_outside = false;
            }
        }
    }

    for (size_t t = 0; t < header.offsets.size(); t++)
    {
        const Item& entry = items[first_entry + t];
        decoded.entries.push_back(entry.target_outside ? kNoEntry : entry.target);
    }

    return decoded;
}

// A value's byte. Throws std::invalid_argument outside `low` to `high`.
uint8_t ByteOf(int64_t value, int64_t low, int64_t high, Kind kind)
{
    if (value < low || value > high)
    {
        throw std::invalid_argument("a value of a " + std::string(commands::Name(kind)) +
                                    " command doesn't fit in the driver's bytes");
    }

    return uint8_t(value);
}

// Adds a length's byte, or its two bytes.
void AddLength(std::vector<uint8_t>& bytes, int64_t value, uint8_t value_bytes, Kind kind)
{
    if (value < 0 || value > 0x7FFF)
    {
        throw std::invalid_argument("a length of a " + std::string(commands::Name(kind)) +
                                    " command doesn't fit in the driver's bytes");
    }

    if (value < 0x80 && value_bytes != 2)
    {
        bytes.push_back(uint8_t(value));
    }
    else
    {
        bytes.push_back(uint8_t(0x80 | value >> 8));
        bytes.push_back(uint8_t(value));
    }
}

// The bytes of a command, in the console's values, with zeros for its target.
std::vector<uint8_t> ItemBytes(const Item& item, const RevisionTraits& traits)
{
    if (item.conditional || item.time || item.value.source != commands::Source::kValue)
    {
        throw std::invalid_argument("the driver has no condition, time, random value or variable");
    }

    const int64_t value = FromUnit(item.value.value, ScaleOf(item.kind));
    const int64_t velocity = FromUnit(item.velocity, commands::Scale{});
    const auto byte_command = [&](uint8_t opcode) -> std::vector<uint8_t>
    {
        return {opcode, ByteOf(value, 0, 0xFF, item.kind)};
    };
    const auto signed_command = [&](uint8_t opcode) -> std::vector<uint8_t>
    {
        return {opcode, ByteOf(value, -0x80, 0x7F, item.kind)};
    };
    const auto flag_command = [&](uint8_t on, uint8_t off) -> std::vector<uint8_t>
    {
        return {ByteOf(value, 0, 1, item.kind) ? on : off};
    };

    std::vector<uint8_t> bytes;
    switch (item.kind)
    {
    case Kind::kNote:
        if (item.key > 0x5F)
        {
            throw std::invalid_argument("a note's key doesn't fit in the driver's bytes");
        }
        if (item.encoding.repeated)
        {
            return {item.key};
        }
        bytes.push_back(uint8_t(0x60 + item.key));
        AddLength(bytes, value, item.encoding.value_bytes, item.kind);
        bytes.push_back(ByteOf(velocity, 0, 0xFF, item.kind));
        return bytes;

    case Kind::kWait:
        if (item.encoding.repeated)
        {
            return {0xC0};
        }
        bytes.push_back(0xC1);
        AddLength(bytes, value, item.encoding.value_bytes, item.kind);
        return bytes;

    case Kind::kProgram:
        return byte_command(0xC2);

    case Kind::kPan:
        return byte_command(0xC3);

    case Kind::kPriority:
        return byte_command(0xC4);

    case Kind::kTie:
        return flag_command(0xC5, 0xC6);

    case Kind::kBankSelect:
        return byte_command(0xC7);

    case Kind::kNoteWait:
        return flag_command(0xC8, 0xC9);

    case Kind::kUserProcedure:
        return byte_command(0xCA);

    case Kind::kPortamentoFrom:
        if (item.index > 0x0F)
        {
            throw std::invalid_argument("a slide's flags don't fit in the driver's bytes");
        }
        bytes = {uint8_t(0xD0 | item.index), item.key, ByteOf(value, 0, 0xFF, item.kind)};
        if (item.index & 1)
        {
            bytes.push_back(ByteOf(velocity, 0, 0xFF, item.kind));
        }
        else if (velocity != 0)
        {
            throw std::invalid_argument("a slide with even flags reads no delay");
        }
        return bytes;

    case Kind::kVolume:
        return byte_command(0xE0);

    case Kind::kPitchBend:
        return signed_command(0xE1);

    case Kind::kBendRange:
        return byte_command(0xE2);

    case Kind::kSend:
        if (item.index != 0)
        {
            throw std::invalid_argument("the driver has one send");
        }
        return byte_command(0xE3);

    case Kind::kTempo:
        if (traits.byte_tempo)
        {
            return byte_command(0xE4);
        }
        bytes.push_back(0xE4);
        AddLength(bytes, value, item.encoding.value_bytes, item.kind);
        return bytes;

    case Kind::kLfoDelay:
        return byte_command(0xE5);

    case Kind::kLfoSpeed:
        return byte_command(0xE6);

    case Kind::kLfoDepth:
        return byte_command(0xE7);

    case Kind::kPortamento:
        return {0xE8};

    case Kind::kTranspose:
        return signed_command(0xE9);

    case Kind::kMainVolume:
        if (!traits.player_volume)
        {
            throw std::invalid_argument("this revision of the driver has no player's volume");
        }
        return byte_command(0xEA);

    case Kind::kJump:
        return {0xF0, 0, 0};

    case Kind::kCall:
        return {0xF4, 0, 0};

    case Kind::kOpenTrack:
        if (IsHeaderEntry(item))
        {
            return {0, 0};
        }
        return {0xF8, item.index, 0, 0};

    case Kind::kEnd:
    case Kind::kReturn:
        return {0xFF};

    case Kind::kOther:
        if (item.encoding.opcode > 0xFF)
        {
            throw std::invalid_argument("an opcode doesn't fit in the driver's bytes");
        }
        return {uint8_t(item.encoding.opcode)};

    default:
        throw std::invalid_argument("the driver has no " + std::string(commands::Name(item.kind)) + " command");
    }
}

// Checks that the driver reads `encoded` as `items` say: its header names the tracks at `starts`, and each command that
// a track reaches reads as its item. Throws std::invalid_argument otherwise.
void CheckReadBack(const std::vector<Item>& items, const Encoded& encoded, const RevisionTraits& traits,
                   const std::vector<uint16_t>& starts)
{
    const bool effects = IsHeaderEntry(items.front());
    const Header header = ReadHeader(encoded.bytes, effects);
    if (!header.problem.empty())
    {
        throw std::invalid_argument("the sequence's header doesn't read back: " + header.problem);
    }
    if (header.offsets != starts)
    {
        throw std::invalid_argument("the sequence's header doesn't read back as its items");
    }

    const Flow flow(encoded.bytes, traits, header);
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

        const Kind kind = items[found->second].kind;
        const bool both_end =
            (kind == Kind::kEnd || kind == Kind::kReturn) && (read.kind == Kind::kEnd || read.kind == Kind::kReturn);
        if (kind != read.kind && !both_end)
        {
            throw std::invalid_argument("a command's opcode reads as a command of another kind");
        }
    }
}

} // namespace

Decoded DecodeSequence(std::span<const uint8_t> data, Revision revision, bool effects)
{
    if (data.empty())
    {
        throw std::invalid_argument("a sequence holds no bytes");
    }

    const Header header = ReadHeader(data, effects);
    const Flow flow(data, TraitsOf(revision), header);
    return Build(data, header, effects, flow);
}

int SequenceCount(const DriverInfo& info)
{
    return int(info.sequence_addresses.size() + info.effect_sequence_addresses.size());
}

uint32_t SequenceAddress(const DriverInfo& info, int sequence)
{
    if (sequence < 0 || sequence >= SequenceCount(info))
    {
        throw std::out_of_range("the driver holds no sequence " + std::to_string(sequence));
    }

    const size_t index = size_t(sequence);
    return IsEffectSequence(info, sequence) ? info.effect_sequence_addresses[index - info.sequence_addresses.size()]
                                            : info.sequence_addresses[index];
}

bool IsEffectSequence(const DriverInfo& info, int sequence)
{
    return size_t(sequence) >= info.sequence_addresses.size();
}

int SequenceOfSong(const DriverInfo& info, int song)
{
    const size_t music = info.sequence_addresses.size();
    if (song < 0 || size_t(song) >= music + info.sound_effects.size())
    {
        throw std::out_of_range("the driver holds no song " + std::to_string(song));
    }

    return size_t(song) < music ? song : int(music) + info.sound_effects[size_t(song) - music].sequence;
}

Decoded DecodeRomSequence(const Rom& rom, const DriverInfo& info, int sequence)
{
    const uint32_t address = SequenceAddress(info, sequence);
    if (!rom.Contains(address))
    {
        throw std::invalid_argument("sequence " + std::to_string(sequence) + " has no bytes in the ROM");
    }

    const bool effects = IsEffectSequence(info, sequence);
    const std::span<const uint8_t> data(rom.Ptr(address), rom.End() - address);
    const Header header = ReadHeader(data, effects);
    const Flow flow(data, TraitsOf(info.revision), header);

    // The sequence ends with the last byte that a track reads, or at the next sequence where that lies close after it.
    uint32_t size = std::max<uint32_t>(flow.End(), 1);
    uint32_t next = rom.End();
    for (int s = 0; s < SequenceCount(info); s++)
    {
        const uint32_t other = SequenceAddress(info, s);
        if (other >= address + size && other < next)
        {
            next = other;
        }
    }
    if (next - (address + size) < kSequenceGap && next < rom.End())
    {
        size = next - address;
    }

    return Build(data.first(size), header, effects, flow);
}

std::vector<uint32_t> ItemOffsets(const commands::Sequence& sequence, Revision revision)
{
    const RevisionTraits& traits = TraitsOf(revision);
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

Encoded EncodeSequence(const commands::Sequence& sequence, Revision revision)
{
    const RevisionTraits& traits = TraitsOf(revision);
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

    std::vector<uint16_t> starts;
    for (size_t i = 0; i < items.size(); i++)
    {
        const Item& item = items[i];
        if (!HasTarget(item.kind))
        {
            continue;
        }
        if (!item.target_outside && item.target >= items.size())
        {
            throw std::invalid_argument("a command's target lies past the sequence's items");
        }

        const uint32_t target = item.target_outside ? item.target : encoded.offsets[item.target];
        if (target > 0xFFFF)
        {
            throw std::invalid_argument("a command's target lies past 0xFFFF bytes from the sequence's start");
        }

        const uint32_t end = encoded.offsets[i + 1];
        encoded.bytes[end - 2] = uint8_t(target);
        encoded.bytes[end - 1] = uint8_t(target >> 8);
        if (IsHeaderEntry(item))
        {
            starts.push_back(uint16_t(target));
        }
    }

    // The driver has to read the bytes as the items say. A sequence kept as data holds no commands.
    if (any_command)
    {
        CheckReadBack(items, encoded, traits, starts);
    }

    return encoded;
}

commands::Scale ScaleOf(Kind kind)
{
    switch (kind)
    {
    case Kind::kPan:
        return {kOne, -64 * kOne};
    case Kind::kPitchBend:
        return {kOne / 128, 0};
    case Kind::kTranspose:
    case Kind::kBendRange:
        return {100 * kOne, 0};
    default:
        return {};
    }
}

} // namespace ntdlib::rd2
