// SPDX-License-Identifier: MIT

#include "ntdlib/brownie/commands.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/agb/rom.h"
#include "ntdlib/brownie/driver.h"
#include "ntdlib/brownie/song.h"

namespace ntdlib::brownie
{
namespace
{

using Item = commands::Command;
using commands::Kind;

constexpr uint32_t kEndOfList = 0xFF;
constexpr uint32_t kListEntryWords = 3;

// What a sound's channels read.
struct SoundWalk
{
    bool ok = true;
    uint32_t low = 0xFFFFFFFF;
    uint32_t high = 0;
    std::map<uint32_t, uint32_t> commands; // the address of each command that the channels reach, and its size
    std::map<uint32_t, uint32_t> targets;  // the address of each call and jump, and the address that its word gives
    std::set<uint32_t> table_words;        // the words of the tables that the commands read
    std::set<uint32_t> tables;             // the start of each table that a command reads
    bool targets_differ = false;           // two channels' tables give a call or a jump other targets

    void Keep(uint32_t address, uint32_t size)
    {
        low = std::min(low, address);
        high = std::max(high, address + size);
    }
};

// Follows channel `channel` of a sound as the driver reads it, up to every place that it can reach. A channel runs on
// over a note, as it does in a later frame.
void WalkChannel(const Rom& rom, const DriverInfo& info, const SongChannel& channel, SoundWalk& walk,
                 std::set<std::pair<uint32_t, uint32_t>>& seen)
{
    std::vector<uint32_t> places = {channel.data};
    while (!places.empty())
    {
        uint32_t at = places.back();
        places.pop_back();
        while (seen.insert({at, channel.subroutines}).second)
        {
            if (!rom.Contains(at))
            {
                walk.ok = false;
                return;
            }

            const uint8_t raw = rom.U8(at);
            const uint8_t b = CommandOf(info.revision, raw);
            const uint32_t size = CommandSize(info.revision, raw);
            if (!rom.Contains(at, size))
            {
                walk.ok = false;
                return;
            }

            walk.commands[at] = size;
            walk.Keep(at, size);
            if (b == 0xF6 || b == 0xF8)
            {
                if (channel.subroutines & 3)
                {
                    walk.ok = false;
                    return;
                }

                const uint32_t word = channel.subroutines + 4 * uint32_t(rom.U8(at + 1));
                if (!rom.Contains(word, 4))
                {
                    walk.ok = false;
                    return;
                }

                const uint32_t target = rom.U32(word);
                walk.table_words.insert(word);
                walk.tables.insert(channel.subroutines);
                walk.Keep(word, 4);
                const auto [place, fresh] = walk.targets.insert({at, target});
                walk.targets_differ = walk.targets_differ || place->second != target;
                places.push_back(target);
                if (b == 0xF8)
                {
                    break;
                }
            }
            else if (b == 0xF7 || b == 0xFF)
            {
                break;
            }

            at += size;
        }
    }
}

// Follows the channels of sound `song`. A sound that ReadSong doesn't read isn't ok.
SoundWalk WalkSound(const Rom& rom, const DriverInfo& info, int song, std::vector<SongChannel>& channels)
{
    SoundWalk walk;
    if (!ReadSong(rom, info, song, channels))
    {
        walk.ok = false;
        return walk;
    }

    const uint32_t list = SongAddress(rom, info, song);
    walk.Keep(list, uint32_t(channels.size()) * kListEntryWords * 4 + 4);
    std::set<std::pair<uint32_t, uint32_t>> seen;
    for (const SongChannel& channel : channels)
    {
        WalkChannel(rom, info, channel, walk, seen);
        if (!walk.ok)
        {
            break;
        }
    }

    return walk;
}

// The usual byte of `kind` and `index` in the driver's commands, or nothing for a kind that has none.
std::optional<uint8_t> UsualByte(const Item& item)
{
    switch (item.kind)
    {
    case Kind::kWait:
        return 0xFE;
    case Kind::kNoteOff:
        return item.index == 0 ? 0xEC : 0xED;
    case Kind::kRelease:
        return 0xE0;
    case Kind::kProgram:
        return item.index == 0 ? 0xE1 : item.index == 1 ? 0xF0 : item.index == 2 ? 0xF1 : 0xFC;
    case Kind::kPan:
        return item.index == 0 ? 0xEA : 0xFD;
    case Kind::kVolume:
        return 0xF2;
    case Kind::kTranspose:
        return 0xF3;
    case Kind::kTune:
        return 0xF9;
    case Kind::kSweep:
        return item.index == 0 ? 0xFA : 0xFB;
    case Kind::kLoopStart:
        return 0xF4;
    case Kind::kLoopEnd:
        return 0xF5;
    case Kind::kCall:
        return 0xF6;
    case Kind::kReturn:
        return 0xF7;
    case Kind::kJump:
        return 0xF8;
    case Kind::kEnd:
        return 0xFF;
    default:
        return std::nullopt;
    }
}

// The item that the command at `at` makes.
Item ItemOf(const Rom& rom, const DriverInfo& info, uint32_t at)
{
    const uint8_t raw = rom.U8(at);
    const uint8_t b = CommandOf(info.revision, raw);
    const uint8_t argument = rom.U8(at + 1);
    Item item;
    if (b < 0x80)
    {
        item.kind = Kind::kNote;
        item.key = b;
        return item;
    }

    if (b < 0xE0 || IsNoOp(info.revision, b))
    {
        item.kind = Kind::kOther;
        item.encoding.opcode = b;
        return item;
    }

    const auto with_value = [&](Kind kind, int index, int64_t value)
    {
        item.kind = kind;
        item.index = uint8_t(index);
        item.value.value = commands::ToUnit(value, ScaleOf(kind, info.revision));
    };

    switch (b)
    {
    case 0xE0:
        with_value(Kind::kRelease, 0, argument);
        break;
    case 0xE1:
        with_value(Kind::kProgram, 0, argument);
        break;
    case 0xE2:
    case 0xEE:
        item.kind = Kind::kOther;
        item.encoding.opcode = b;
        item.bytes = {argument};
        break;
    case 0xE3:
    case 0xE8:
    case 0xE9:
        item.kind = Kind::kOther;
        item.encoding.opcode = b;
        break;
    case 0xEA:
        with_value(Kind::kPan, 0, argument);
        break;
    case 0xEC:
        item.kind = Kind::kNoteOff;
        break;
    case 0xED:
        item.kind = Kind::kNoteOff;
        item.index = 1;
        break;
    case 0xF0:
        with_value(Kind::kProgram, 1, argument);
        break;
    case 0xF1:
        with_value(Kind::kProgram, 2, argument);
        break;
    case 0xF2:
        with_value(Kind::kVolume, 0, argument);
        break;
    case 0xF3:
        with_value(Kind::kTranspose, 0, int8_t(argument));
        break;
    case 0xF4:
        with_value(Kind::kLoopStart, 0, argument);
        break;
    case 0xF5:
        item.kind = Kind::kLoopEnd;
        break;
    case 0xF6:
        item.kind = Kind::kCall;
        item.index = argument;
        break;
    case 0xF7:
        item.kind = Kind::kReturn;
        break;
    case 0xF8:
        item.kind = Kind::kJump;
        item.index = argument;
        break;
    case 0xF9:
        with_value(Kind::kTune, 0, int8_t(argument));
        break;
    case 0xFA:
        with_value(Kind::kSweep, 0, argument);
        break;
    case 0xFB:
        item.kind = Kind::kSweep;
        item.index = 1;
        break;
    case 0xFC:
        with_value(Kind::kProgram, 3, argument);
        break;
    case 0xFD:
        with_value(Kind::kPan, 1, argument);
        break;
    case 0xFE:
        item.kind = Kind::kWait;
        break;
    case 0xFF:
        item.kind = Kind::kEnd;
        break;
    default:
        item.kind = Kind::kOther;
        item.encoding.opcode = b;
        break;
    }

    if (raw != b)
    {
        item.encoding.opcode = uint16_t(kKeptBytes | raw);
    }

    return item;
}

bool IsWordItem(const Item& item)
{
    return item.kind == Kind::kOpenTrack || (item.kind == Kind::kJump && item.encoding.opcode == kAddressWord) ||
           (item.kind == Kind::kEnd && item.index == 1);
}

// The bytes of an item that holds no address.
uint32_t ItemSize(const Item& item)
{
    switch (item.kind)
    {
    case Kind::kOpenTrack:
        return 8;
    case Kind::kData:
        return uint32_t(item.bytes.size());
    case Kind::kNote:
    case Kind::kWait:
    case Kind::kNoteOff:
    case Kind::kLoopEnd:
    case Kind::kReturn:
        return 1;
    case Kind::kOther:
        return 1 + uint32_t(item.bytes.size());
    case Kind::kSweep:
        return item.index == 0 ? 2 : 1;
    case Kind::kEnd:
        return item.index == 1 ? 4 : 1;
    case Kind::kJump:
        return item.encoding.opcode == kAddressWord ? 4 : 2;
    default:
        return 2;
    }
}

// The byte of a value. Throws std::invalid_argument past `low` to `high`.
uint8_t ByteOf(const Item& item, Revision revision, int64_t low, int64_t high)
{
    const int64_t value = commands::FromUnit(item.value.value, ScaleOf(item.kind, revision));
    if (value < low || value > high)
    {
        throw std::invalid_argument("a value of a " + std::string(commands::Name(item.kind)) +
                                    " command doesn't fit in the driver's bytes");
    }

    return uint8_t(value);
}

// The first byte of a command, with the revision's rules. Throws std::invalid_argument for a command that the revision
// doesn't have.
uint8_t OpcodeOf(const Item& item, Revision revision)
{
    if (item.encoding.opcode & kKeptBytes)
    {
        const uint8_t kept = uint8_t(item.encoding.opcode);
        const std::optional<uint8_t> usual = UsualByte(item);
        if (!usual || CommandOf(revision, kept) != *usual)
        {
            throw std::invalid_argument("a " + std::string(commands::Name(item.kind)) +
                                        " command keeps a byte that the driver reads otherwise");
        }

        return kept;
    }

    const std::optional<uint8_t> usual = item.kind == Kind::kOther ? uint8_t(item.encoding.opcode) : UsualByte(item);
    if (!usual)
    {
        throw std::invalid_argument("the driver has no " + std::string(commands::Name(item.kind)) + " command");
    }

    if (*usual >= 0xE0 && IsNoOp(revision, *usual) && item.kind != Kind::kOther)
    {
        throw std::invalid_argument("this revision of the driver does nothing with a " +
                                    std::string(commands::Name(item.kind)) + " command");
    }

    return *usual;
}

// Returns true if the channel that starts at item `entry` reads its table: a call or a jump comes before its end.
bool ReadsTable(const commands::Sequence& sequence, uint32_t entry)
{
    for (uint32_t at = entry; at < sequence.items.size(); at++)
    {
        const Item& item = sequence.items[at];
        if (item.kind == Kind::kCall || item.kind == Kind::kJump)
        {
            return item.encoding.opcode != kAddressWord;
        }
        if (item.kind == Kind::kData || item.kind == Kind::kReturn || item.kind == Kind::kEnd ||
            item.kind == Kind::kOpenTrack)
        {
            return false;
        }
    }

    return false;
}

// Checks that the driver reads channel `track` as the items say, from its data, with the table `table`.
void CheckChannel(const commands::Sequence& sequence, const std::vector<uint32_t>& offsets,
                  const std::map<uint32_t, uint32_t>& item_at, uint32_t entry, uint32_t table,
                  std::set<std::pair<uint32_t, uint32_t>>& seen)
{
    std::vector<uint32_t> places = {entry};
    while (!places.empty())
    {
        uint32_t at = places.back();
        places.pop_back();
        while (seen.insert({at, table}).second)
        {
            if (at >= sequence.items.size())
            {
                throw std::invalid_argument("a channel runs past the sequence's items");
            }

            // A channel's FF can be the first byte of a list's word of 0xFF.
            const Item& item = sequence.items[at];
            if (item.kind == Kind::kData || (IsWordItem(item) && item.kind != Kind::kEnd))
            {
                throw std::invalid_argument("a channel reaches bytes that aren't commands");
            }

            if (item.kind == Kind::kCall || item.kind == Kind::kJump)
            {
                if (item.target >= sequence.items.size() || table >= sequence.items.size())
                {
                    throw std::invalid_argument("a call or a jump has no table to read");
                }

                const auto word = item_at.find(offsets[table] + 4 * uint32_t(item.index));
                const Item* words = word == item_at.end() ? nullptr : &sequence.items[word->second];
                if (!words || words->kind != Kind::kJump || words->encoding.opcode != kAddressWord ||
                    words->target_outside || words->target != item.target)
                {
                    throw std::invalid_argument("a call or a jump finds another target in its channel's table");
                }

                places.push_back(item.target);
                if (item.kind == Kind::kJump)
                {
                    break;
                }
            }
            else if (item.kind == Kind::kReturn || item.kind == Kind::kEnd)
            {
                break;
            }

            at++;
        }
    }
}

// Returns the tables that a channel reads. Each starts on a multiple of 4, where the driver reads its words, even when
// its first word is no call's or jump's.
std::set<uint32_t> ReadTables(const commands::Sequence& sequence)
{
    std::set<uint32_t> tables;
    for (size_t i = 0; i + 1 < sequence.items.size(); i++)
    {
        const Item& open = sequence.items[i];
        const Item& table = sequence.items[i + 1];
        if (open.kind == Kind::kOpenTrack && table.kind == Kind::kJump && table.encoding.opcode == kAddressWord &&
            !table.target_outside && !open.target_outside && ReadsTable(sequence, open.target))
        {
            tables.insert(table.target);
        }
    }

    return tables;
}

// Writes the bytes of `item`, a note or a command, at `out`.
void WriteCommand(const Item& item, Revision revision, uint8_t* out)
{
    if (item.kind == Kind::kNote)
    {
        if (item.key >= 0x80)
        {
            throw std::invalid_argument("a note's byte lies past the driver's notes");
        }

        *out = item.key;
        return;
    }

    out[0] = OpcodeOf(item, revision);
    switch (item.kind)
    {
    case Kind::kCall:
    case Kind::kJump:
        out[1] = item.index;
        break;
    case Kind::kRelease:
    case Kind::kProgram:
    case Kind::kPan:
    case Kind::kVolume:
    case Kind::kLoopStart:
        out[1] = ByteOf(item, revision, 0, 255);
        break;
    case Kind::kSweep:
        if (item.index == 0)
        {
            out[1] = ByteOf(item, revision, 0, 255);
        }
        break;
    case Kind::kTranspose:
    case Kind::kTune:
        out[1] = ByteOf(item, revision, -128, 127);
        break;
    case Kind::kOther:
        if (item.bytes.size() != CommandSize(revision, out[0]) - 1)
        {
            throw std::invalid_argument("a command holds another number of bytes than the driver reads");
        }
        std::copy(item.bytes.begin(), item.bytes.end(), out + 1);
        break;
    default:
        break;
    }
}

// Checks that each list's channels read their commands as the items say. Throws std::invalid_argument otherwise.
void CheckLists(const commands::Sequence& sequence, const Encoded& encoded, const std::map<uint32_t, uint32_t>& item_at)
{
    std::set<std::pair<uint32_t, uint32_t>> seen;
    for (uint32_t i = 0; i < sequence.items.size(); i++)
    {
        const Item& item = sequence.items[i];
        if (item.kind != Kind::kOpenTrack)
        {
            continue;
        }

        if (i + 1 >= sequence.items.size() || sequence.items[i + 1].kind != Kind::kJump ||
            sequence.items[i + 1].encoding.opcode != kAddressWord)
        {
            throw std::invalid_argument("a list's channel has no word with its table's address");
        }

        const Item& table = sequence.items[i + 1];
        CheckChannel(sequence, encoded.offsets, item_at, item.target,
                     table.target_outside ? uint32_t(sequence.items.size()) : table.target, seen);
    }
}

// Decodes a region's sounds into one sequence. A region that can't be read safely stays opaque: one item of data.
class RegionDecoder
{
public:
    RegionDecoder(const Rom& rom, const DriverInfo& info, const Region& region)
        : rom_(rom), info_(info), region_(region)
    {
        decoded_.sounds = region.sounds;
    }

    // Each item of each sound, by its address: the list of its channels, the commands that they reach, and the words
    // of the tables that the commands read. Returns false if the region stays opaque.
    bool ReadSounds()
    {
        for (const int sound : region_.sounds)
        {
            const SoundWalk walk = WalkSound(rom_, info_, sound, channels_);
            if (walk.targets_differ)
            {
                Opaque("a call or a jump that two channels of sound " + std::to_string(sound) +
                       " read with other targets");
                return false;
            }

            const uint32_t list = SongAddress(rom_, info_, sound);
            list_addresses_.push_back(list);
            uint32_t at = list;
            bool same = true;
            for (const SongChannel& channel : channels_)
            {
                Item open;
                open.kind = Kind::kOpenTrack;
                open.index = uint8_t(channel.channel);
                same = Add(at, {open, 8, channel.data}) && same;
                Item table;
                table.kind = Kind::kJump;
                table.encoding.opcode = kAddressWord;
                same = Add(at + 8, {table, 4, channel.subroutines}) && same;
                boundaries_.insert(channel.subroutines);
                at += kListEntryWords * 4;
            }

            Item end;
            end.kind = Kind::kEnd;
            end.index = 1;
            same = Add(at, {end, 4, std::nullopt}) && same;
            for (const auto& [address, size] : walk.commands)
            {
                const auto target = walk.targets.find(address);
                std::optional<uint32_t> names;
                if (target != walk.targets.end())
                {
                    names = target->second;
                    const auto [known, fresh] = call_targets_.insert({address, target->second});
                    if (known->second != target->second)
                    {
                        Opaque("a call or a jump that two sounds read with other targets");
                        return false;
                    }
                }

                same = Add(address, {ItemOf(rom_, info_, address), size, names}) && same;
            }

            for (const uint32_t word : walk.table_words)
            {
                Item table;
                table.kind = Kind::kJump;
                table.encoding.opcode = kAddressWord;
                same = Add(word, {table, 4, rom_.U32(word)}) && same;
            }

            used_tables_.insert(walk.tables.begin(), walk.tables.end());
            if (!same)
            {
                Opaque("the bytes at " + agb::Hex(clash_) + " read as two different items");
                return false;
            }
        }

        return true;
    }

    // No item may overlap the next. Returns false if one does.
    bool CheckOverlaps()
    {
        uint32_t reached = region_.start;
        for (const auto& [address, placed] : items_)
        {
            if (address < reached)
            {
                Opaque("the bytes at " + agb::Hex(address) + " read as two different items");
                return false;
            }

            reached = address + placed.size;
        }

        return true;
    }

    // The items in the order of their addresses, with the bytes between them as data, split where a table starts.
    void NumberItems()
    {
        const uint32_t end = region_.start + region_.size;
        uint32_t at = region_.start;
        const auto add_data = [&](uint32_t to)
        {
            while (at < to)
            {
                const auto boundary = boundaries_.upper_bound(at);
                const uint32_t stop = boundary != boundaries_.end() && *boundary < to ? *boundary : to;
                Item data;
                data.kind = Kind::kData;
                data.bytes.assign(rom_.Ptr(at), rom_.Ptr(at) + (stop - at));
                number_[at] = uint32_t(decoded_.sequence.items.size());
                decoded_.sequence.items.push_back(std::move(data));
                at = stop;
            }
        };

        for (const auto& [address, placed] : items_)
        {
            add_data(address);
            number_[address] = uint32_t(decoded_.sequence.items.size());
            decoded_.sequence.items.push_back(placed.item);
            at = address + placed.size;
        }
        add_data(end);
    }

    // A table that a command reads has to move with its words. Each item that names a place gets the number of the
    // item there. Returns false if a table's start or a target falls inside another item.
    bool SetTargets()
    {
        for (const uint32_t table : used_tables_)
        {
            if (!number_.count(table))
            {
                Opaque("a table's start that falls inside another item");
                return false;
            }
        }

        for (const auto& [address, placed] : items_)
        {
            if (!placed.names)
            {
                continue;
            }

            Item& item = decoded_.sequence.items[number_[address]];
            const auto target = number_.find(*placed.names);
            if (target != number_.end())
            {
                item.target = target->second;
            }
            else if (item.kind == Kind::kJump && item.encoding.opcode == kAddressWord)
            {
                item.target = *placed.names;
                item.target_outside = true;
            }
            else
            {
                Opaque("a command's target that falls inside another item");
                return false;
            }
        }

        return true;
    }

    // The item where each sound starts its list, with its label.
    void AddLists()
    {
        for (size_t s = 0; s < region_.sounds.size(); s++)
        {
            decoded_.lists.push_back(number_[list_addresses_[s]]);
            decoded_.sequence.labels.push_back({"SOUND_" + std::to_string(region_.sounds[s]), decoded_.lists.back()});
        }
    }

    Decoded Result()
    {
        return std::move(decoded_);
    }

private:
    // An item at its address. An item that names a place holds the address until every item has its number.
    struct Placed
    {
        Item item;
        uint32_t size = 0;
        std::optional<uint32_t> names;
    };

    // Keeps the whole region as data, for `reason`.
    void Opaque(const std::string& reason)
    {
        Item data;
        data.kind = Kind::kData;
        const uint8_t* bytes = rom_.Ptr(region_.start);
        data.bytes.assign(bytes, bytes + region_.size);
        decoded_.sequence = {};
        decoded_.sequence.items.push_back(std::move(data));
        decoded_.lists.clear();
        decoded_.opaque = true;
        decoded_.reason = reason;
    }

    // Adds the item at `address`. Returns false if another item that starts there differs from it.
    bool Add(uint32_t address, Placed placed)
    {
        const auto [place, fresh] = items_.insert({address, placed});
        if (!fresh && place->second.item.kind == Kind::kEnd && placed.item.kind == Kind::kEnd)
        {
            // A channel's FF can be the first byte of a list's word of 0xFF.
            if (placed.item.index == 1)
            {
                place->second = placed;
            }
            return true;
        }

        const bool same = fresh || (place->second.size == placed.size && place->second.names == placed.names &&
                                    place->second.item == placed.item);
        clash_ = same ? clash_ : address;
        return same;
    }

    const Rom& rom_;
    const DriverInfo& info_;
    const Region& region_;
    Decoded decoded_;
    std::map<uint32_t, Placed> items_;
    std::set<uint32_t> boundaries_; // the addresses that have to start an item: the tables' starts
    std::set<uint32_t> used_tables_;
    std::map<uint32_t, uint32_t> call_targets_;
    std::vector<SongChannel> channels_;
    std::vector<uint32_t> list_addresses_;
    uint32_t clash_ = 0;
    std::map<uint32_t, uint32_t> number_; // the number of the item at each address
};

} // namespace

std::vector<Region> FindRegions(const Rom& rom, const DriverInfo& info)
{
    struct Span
    {
        uint32_t low = 0;
        uint32_t high = 0;
        int sound = 0;
    };

    std::vector<Span> spans;
    std::vector<SongChannel> channels;
    for (int s = 0; s < info.song_count; s++)
    {
        const SoundWalk walk = WalkSound(rom, info, s, channels);
        if (walk.ok)
        {
            spans.push_back({walk.low, walk.high, s});
        }
    }

    std::stable_sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.low < b.low; });
    std::vector<Region> regions;
    uint32_t end = 0;
    for (const Span& span : spans)
    {
        if (regions.empty() || span.low >= end)
        {
            regions.push_back({span.low, 0, {}});
            end = span.high;
        }

        end = std::max(end, span.high);
        regions.back().size = end - regions.back().start;
        regions.back().sounds.push_back(span.sound);
    }

    for (Region& region : regions)
    {
        std::sort(region.sounds.begin(), region.sounds.end());
    }

    return regions;
}

Decoded DecodeRegion(const Rom& rom, const DriverInfo& info, const Region& region)
{
    RegionDecoder decoder(rom, info, region);
    if (decoder.ReadSounds() && decoder.CheckOverlaps())
    {
        decoder.NumberItems();
        if (decoder.SetTargets())
        {
            decoder.AddLists();
        }
    }

    return decoder.Result();
}

Decoded DecodeSong(const Rom& rom, const DriverInfo& info, int song)
{
    for (const Region& region : FindRegions(rom, info))
    {
        if (std::binary_search(region.sounds.begin(), region.sounds.end(), song))
        {
            return DecodeRegion(rom, info, region);
        }
    }

    throw std::invalid_argument("sound " + std::to_string(song) + " lies in no region of the ROM");
}

std::vector<ListTrack> TracksOfList(const commands::Sequence& sequence, uint32_t list)
{
    std::vector<ListTrack> tracks;
    uint32_t at = list;
    while (at < sequence.items.size())
    {
        const Item& item = sequence.items[at];
        if (item.kind == Kind::kEnd && item.index == 1)
        {
            return tracks;
        }

        if (item.kind != Kind::kOpenTrack || at + 1 >= sequence.items.size() ||
            sequence.items[at + 1].kind != Kind::kJump || sequence.items[at + 1].encoding.opcode != kAddressWord)
        {
            break;
        }

        tracks.push_back({int(item.index), item.target, at + 1});
        at += 2;
    }

    throw std::invalid_argument("item " + std::to_string(list) + " starts no list of channels");
}

Encoded EncodeSequence(const commands::Sequence& sequence, Revision revision, uint32_t base)
{
    Encoded encoded;
    encoded.offsets.reserve(sequence.items.size() + 1);

    // The offsets first: the words name items after them. A table that a channel reads starts on a multiple of 4.
    const std::set<uint32_t> tables = ReadTables(sequence);

    uint32_t offset = 0;
    std::map<uint32_t, uint32_t> item_at;
    for (size_t i = 0; i < sequence.items.size(); i++)
    {
        const Item& item = sequence.items[i];
        if (IsWordItem(item) || tables.contains(uint32_t(i)))
        {
            offset += (4 - (base + offset) % 4) % 4;
        }

        encoded.offsets.push_back(offset);
        item_at[offset] = uint32_t(i);
        offset += ItemSize(item);
    }
    encoded.offsets.push_back(offset);

    const auto address_of = [&](const Item& item)
    {
        if (item.target_outside)
        {
            return item.target;
        }
        if (item.target >= sequence.items.size())
        {
            throw std::invalid_argument("a word names an item that the sequence lacks");
        }

        return base + encoded.offsets[item.target];
    };

    encoded.bytes.assign(offset, 0);
    for (size_t i = 0; i < sequence.items.size(); i++)
    {
        const Item& item = sequence.items[i];
        uint8_t* out = &encoded.bytes[0] + encoded.offsets[i];
        if (item.kind == Kind::kData)
        {
            std::copy(item.bytes.begin(), item.bytes.end(), out);
            continue;
        }

        if (item.kind == Kind::kOpenTrack)
        {
            if (item.target_outside)
            {
                throw std::invalid_argument("a channel's data lies outside its sequence");
            }

            xsflib::StoreLe32(out, item.index);
            xsflib::StoreLe32(out + 4, address_of(item));
            continue;
        }

        if (item.kind == Kind::kJump && item.encoding.opcode == kAddressWord)
        {
            xsflib::StoreLe32(out, address_of(item));
            continue;
        }

        if (item.kind == Kind::kEnd && item.index == 1)
        {
            xsflib::StoreLe32(out, kEndOfList);
            continue;
        }

        WriteCommand(item, revision, out);
    }

    CheckLists(sequence, encoded, item_at);

    return encoded;
}

commands::Scale ScaleOf(Kind kind, Revision)
{
    if (kind == Kind::kTranspose)
    {
        return {100 * commands::kOne, 0};
    }

    return {};
}

} // namespace ntdlib::brownie
