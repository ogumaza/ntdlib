// SPDX-License-Identifier: MIT

#include "ntdlib/alphadream/commands.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ntdlib::alphadream
{
namespace
{

using commands::Command;
using commands::FromUnit;
using commands::Kind;
using commands::Scale;
using commands::ToUnit;

// The tracks that a header's mask can name.
constexpr uint16_t kTrackBits = (1u << kTracks) - 1;

// The farthest that a track reads from its start: its place in its data is 16 bits.
constexpr uint32_t kTrackReach = 0x10000;

constexpr uint8_t kPitchLfo = 0xE0;
constexpr uint8_t kPitchLfoOn = 0xE1;
constexpr uint8_t kPitchLfoOff = 0xE2;
constexpr uint8_t kLevelLfo = 0xE3;
constexpr uint8_t kLevelLfoOn = 0xE4;
constexpr uint8_t kLevelLfoOff = 0xE5;
constexpr uint8_t kCountedJump = 0xE8; // E8 to EB
constexpr uint8_t kCountedLoop = 0xEC; // EC to EF
constexpr uint8_t kInstrument = 0xF0;
constexpr uint8_t kTrackVolume = 0xF1;
constexpr uint8_t kTrackPan = 0xF2;
constexpr uint8_t kFineTune = 0xF3;
constexpr uint8_t kRange = 0xF4;
constexpr uint8_t kBend = 0xF5;
constexpr uint8_t kRest = 0xF6;
constexpr uint8_t kEndNote = 0xF7;
constexpr uint8_t kGoto = 0xF8;
constexpr uint8_t kTrackTempo = 0xF9;
constexpr uint8_t kChannelPriority = 0xFA;
constexpr uint8_t kTrackEnd = 0xFF;

// Whether byte `b` starts a note in `version`: every byte that isn't one of the version's commands.
bool IsNoteByte(uint8_t b, Version version)
{
    if (b < 0xE0 || b == 0xE6 || b == 0xE7 || (b >= 0xFB && b <= 0xFE))
    {
        return true;
    }
    if (b == kTrackVolume)
    {
        return version != Version::kSuperstarSaga;
    }
    if (b == kChannelPriority)
    {
        return version != Version::kHamSports;
    }

    return false;
}

// The bytes of a note in `version`: its length, its key and, in B85A's and A84J's versions, the track's volume.
uint32_t NoteBytes(Version version)
{
    return version == Version::kSuperstarSaga ? 2 : 3;
}

Command Item(Kind kind, int64_t value = 0)
{
    Command item;
    item.kind = kind;
    item.value.value = ToUnit(value, Scale{});

    return item;
}

// A command that a track reached.
struct Read
{
    uint32_t size = 0;
    Command item;
    std::optional<uint32_t> target; // the address that a jump goes to
    bool rest_free = false;         // a note that isn't tied and has no bit 7: a rest follows it in the rests' state
    std::set<bool> states;          // the rests' states in which a track reached it
};

// Follows a song's tracks as the driver does.
class Reader
{
public:
    Reader(const Rom& rom, Version version) : rom_(rom), version_(version)
    {
    }

    // Reads the track that starts at `start`. Returns false, with `reason` set, for bytes that can't be read safely.
    bool Track(uint32_t start)
    {
        // Each track keeps its places: a jump's target depends on the start of the track that reads it.
        std::set<std::pair<uint32_t, bool>> visited;
        std::vector<std::pair<uint32_t, bool>> work = {{start, false}};
        while (!work.empty())
        {
            const auto [at, rests] = work.back();
            work.pop_back();
            if (!visited.insert({at, rests}).second)
            {
                continue;
            }
            if (!Step(start, at, rests, work))
            {
                return false;
            }
        }

        return true;
    }

    const std::map<uint32_t, Read>& Reads() const
    {
        return reads_;
    }

    const std::string& Reason() const
    {
        return reason_;
    }

    // The end of the last byte that a track reads.
    uint32_t End() const
    {
        return end_;
    }

private:
    bool Fail(const std::string& reason)
    {
        reason_ = reason;
        return false;
    }

    // Adds the item `read` at `at`, reached in state `rests`. Returns false where it overlaps another item's bytes.
    bool Add(uint32_t at, Read read, bool rests)
    {
        const auto same = reads_.find(at);
        if (same != reads_.end())
        {
            if (same->second.size != read.size || same->second.item != read.item || same->second.target != read.target)
            {
                return Fail("bytes at " + Hex(at) + " read as two commands");
            }
            same->second.states.insert(rests);
            if (same->second.rest_free && same->second.states.size() > 1)
            {
                return Fail("the note at " + Hex(at) + " takes a rest after it on one path and not on another");
            }
            return true;
        }

        const auto next = reads_.lower_bound(at);
        if ((next != reads_.end() && next->first < at + read.size) ||
            (next != reads_.begin() && std::prev(next)->first + std::prev(next)->second.size > at))
        {
            return Fail("bytes at " + Hex(at) + " read as part of two commands");
        }

        read.states.insert(rests);
        end_ = std::max(end_, at + read.size);
        reads_.emplace(at, std::move(read));

        return true;
    }

    // Reads the command at `at` of the track that starts at `start`, and queues the places that the track goes to.
    bool Step(uint32_t start, uint32_t at, bool rests, std::vector<std::pair<uint32_t, bool>>& work)
    {
        if (at - start >= kTrackReach)
        {
            return Fail("a track reads more than 64 KiB past its start");
        }
        if (!rom_.Contains(at))
        {
            return Fail("a track reads past the ROM at " + Hex(at));
        }

        const uint8_t b = rom_.U8(at);
        const auto need = [&](uint32_t size)
        {
            return rom_.Contains(at, size);
        };
        Read read;
        if (IsNoteByte(b, version_))
        {
            const uint32_t bytes = NoteBytes(version_);
            if (!need(bytes))
            {
                return Fail("a note runs past the ROM at " + Hex(at));
            }

            const uint8_t key = rom_.U8(at + 1);
            read.size = bytes;
            read.item = Item(Kind::kNote, b);
            read.item.key = key & 0x7F;
            read.item.encoding.opcode = key & kRestFlag;
            if (version_ != Version::kSuperstarSaga)
            {
                read.item.velocity = ToUnit(rom_.U8(at + 2), Scale{});
            }
            read.rest_free = b != 0 && !(key & kRestFlag);
            const bool rest = b == 0 ? (key & kRestFlag) != 0 : (rests || (key & kRestFlag));
            const bool after = b == 0 ? rests : rest;
            if (!Add(at, std::move(read), rests))
            {
                return false;
            }
            if (!rest)
            {
                work.push_back({at + bytes, after});
                return true;
            }
            if (!need(bytes + 1))
            {
                return Fail("a note's wait runs past the ROM at " + Hex(at));
            }

            Read wait;
            wait.size = 1;
            wait.item = Item(Kind::kWait, rom_.U8(at + bytes));
            if (!Add(at + bytes, std::move(wait), after))
            {
                return false;
            }
            work.push_back({at + bytes + 1, after});
            return true;
        }

        const auto value = [&](uint32_t offset)
        {
            return int64_t(rom_.U8(at + offset));
        };
        const auto signed_value = [&](uint32_t offset)
        {
            return int64_t(int8_t(rom_.U8(at + offset)));
        };
        const auto target = [&](uint32_t offset, uint32_t end)
        {
            const uint32_t jump = rom_.U16(at + offset);
            return start + ((end - start + jump) & 0xFFFF);
        };
        bool goes_on = true;
        switch (b)
        {
        case kPitchLfo:
        case kLevelLfo:
            read.size = 4;
            read.item = Item(Kind::kOther);
            read.item.encoding.opcode = b;
            if (need(4))
            {
                read.item.bytes = {rom_.U8(at + 1), rom_.U8(at + 2), rom_.U8(at + 3)};
            }
            break;
        case kPitchLfoOn:
        case kPitchLfoOff:
        case kLevelLfoOn:
        case kLevelLfoOff:
            read.size = 1;
            read.item = Item(Kind::kOther);
            read.item.encoding.opcode = b;
            break;
        case kInstrument:
            read.size = 2;
            read.item = Item(Kind::kProgram, need(2) ? value(1) : 0);
            break;
        case kTrackVolume:
            read.size = 2;
            read.item = Item(Kind::kVolume, need(2) ? value(1) : 0);
            break;
        case kTrackPan:
            read.size = 2;
            read.item = Item(Kind::kPan, need(2) ? value(1) : 0);
            break;
        case kFineTune:
            read.size = 2;
            read.item = Item(Kind::kTune, need(2) ? signed_value(1) : 0);
            break;
        case kRange:
            read.size = 2;
            read.item = Item(Kind::kBendRange, need(2) ? value(1) : 0);
            break;
        case kBend:
            read.size = 2;
            read.item = Item(Kind::kPitchBend, need(2) ? signed_value(1) : 0);
            break;
        case kRest:
            read.size = 2;
            read.item = Item(Kind::kWait, need(2) ? value(1) : 0);
            read.item.encoding.opcode = kRest;
            break;
        case kEndNote:
            read.size = 1;
            read.item = Item(Kind::kNoteOff);
            break;
        case kGoto:
            read.size = 3;
            read.item = Item(Kind::kJump);
            if (need(3))
            {
                read.target = target(1, at + 3);
            }
            goes_on = false;
            break;
        case kTrackTempo:
            read.size = 2;
            read.item = Item(Kind::kTempo, need(2) ? value(1) : 0);
            break;
        case kChannelPriority:
            read.size = 2;
            read.item = Item(Kind::kPriority, need(2) ? value(1) : 0);
            break;
        case kTrackEnd:
            read.size = 1;
            read.item = Item(Kind::kEnd);
            goes_on = false;
            break;
        default:
            // E8 to EF.
            read.size = 4;
            read.item =
                Item(b < kCountedLoop ? Kind::kJumpIfGreater : Kind::kJumpIfLessOrEqual, need(4) ? value(1) : 0);
            read.item.index = b & 3;
            if (need(4))
            {
                read.target = target(2, at + 4);
            }
            break;
        }
        if (!need(read.size))
        {
            return Fail("a command runs past the ROM at " + Hex(at));
        }

        const uint32_t size = read.size;
        const std::optional<uint32_t> to = read.target;
        if (!Add(at, std::move(read), rests))
        {
            return false;
        }
        if (to)
        {
            work.push_back({*to, rests});
        }
        if (goes_on)
        {
            work.push_back({at + size, rests});
        }

        return true;
    }

    static std::string Hex(uint32_t value)
    {
        char text[16];
        std::snprintf(text, sizeof(text), "0x%08X", value);
        return text;
    }

    const Rom& rom_;
    Version version_;
    std::map<uint32_t, Read> reads_;
    uint32_t end_ = 0;
    std::string reason_;
};

// The bytes from `from` to `to` as one item of data.
Command DataItem(const Rom& rom, uint32_t from, uint32_t to)
{
    Command item;
    item.kind = Kind::kData;
    item.bytes.assign(rom.Ptr(from), rom.Ptr(from) + (to - from));

    return item;
}

// The song's bytes up to `end` as one item, with `reason`.
Decoded KeptAsData(const Rom& rom, uint32_t address, uint32_t end, const std::string& reason)
{
    Decoded decoded;
    const auto last = uint32_t(std::min<uint64_t>(std::max(end, address + 2), rom.End()));
    decoded.sequence.items.push_back(DataItem(rom, address, last));
    decoded.size = last - address;
    decoded.opaque = true;
    decoded.reason = reason;

    return decoded;
}

// The value of a field in the driver's units, from `low` to `high`. Throws when the bytes can't hold it.
int64_t DriverValue(commands::Fixed unit, int64_t low, int64_t high, const char* what)
{
    const int64_t value = FromUnit(unit, Scale{});
    if (value < low || value > high)
    {
        throw std::invalid_argument(std::string("AlphaDream's driver can't hold this ") + what);
    }

    return value;
}

// The bytes that `item` takes in `version`. Throws for a command that the version's bytes can't hold.
uint32_t ItemSize(const Command& item, Version version)
{
    switch (item.kind)
    {
    case Kind::kAllocateTracks:
    case Kind::kOpenTrack:
        return 2;
    case Kind::kNote:
        return NoteBytes(version);
    case Kind::kWait:
        if (item.encoding.opcode != 0 && item.encoding.opcode != kRest)
        {
            throw std::invalid_argument("AlphaDream's driver has no wait with this opcode");
        }
        return item.encoding.opcode == kRest ? 2 : 1;
    case Kind::kOther:
        if (item.encoding.opcode == kPitchLfo || item.encoding.opcode == kLevelLfo)
        {
            return 4;
        }
        if (item.encoding.opcode == kPitchLfoOn || item.encoding.opcode == kPitchLfoOff ||
            item.encoding.opcode == kLevelLfoOn || item.encoding.opcode == kLevelLfoOff)
        {
            return 1;
        }
        throw std::invalid_argument("AlphaDream's driver has no other command with this opcode");
    case Kind::kJumpIfGreater:
    case Kind::kJumpIfLessOrEqual:
        return 4;
    case Kind::kVolume:
        if (version != Version::kSuperstarSaga)
        {
            throw std::invalid_argument("this version of AlphaDream's driver has no volume command");
        }
        return 2;
    case Kind::kPriority:
        if (version != Version::kHamSports)
        {
            throw std::invalid_argument("this version of AlphaDream's driver has no priority command");
        }
        return 2;
    case Kind::kProgram:
    case Kind::kPan:
    case Kind::kTune:
    case Kind::kBendRange:
    case Kind::kPitchBend:
    case Kind::kTempo:
        return 2;
    case Kind::kNoteOff:
    case Kind::kEnd:
        return 1;
    case Kind::kJump:
        return 3;
    case Kind::kData:
        return uint32_t(item.bytes.size());
    default:
        throw std::invalid_argument("AlphaDream's driver has no command of this kind");
    }
}

// Whether `item` is a byte of ticks that a note's bytes lead the driver to.
bool IsBareWait(const Command& item)
{
    return item.kind == Kind::kWait && item.encoding.opcode == 0;
}

// Whether item `target` is where the driver reaches it from a track that starts at offset `start`: at most 16 bits
// on.
bool InReach(const std::vector<uint32_t>& offsets, uint32_t start, uint32_t target)
{
    return offsets[target] >= start && offsets[target] - start < kTrackReach;
}

// Follows each of the first `tracks` tracks of `items` as the driver does, with the items at `offsets`. Throws
// std::invalid_argument where the driver wouldn't read the items as they say.
void CheckTracks(const std::vector<Command>& items, const std::vector<uint32_t>& offsets, size_t tracks)
{
    // The rests' states in which a track reaches each note that isn't tied and has no bit 7.
    std::map<uint32_t, std::set<bool>> note_states;
    for (size_t t = 1; t <= tracks; t++)
    {
        if (items[t].target >= items.size())
        {
            throw std::invalid_argument("an AlphaDream track's start is missing from the song");
        }

        const uint32_t start = offsets[items[t].target];
        std::set<std::pair<uint32_t, bool>> visited;
        std::vector<std::pair<uint32_t, bool>> work = {{items[t].target, false}};
        while (!work.empty())
        {
            const auto [i, rests] = work.back();
            work.pop_back();
            if (!visited.insert({i, rests}).second)
            {
                continue;
            }
            if (i >= items.size())
            {
                throw std::invalid_argument("an AlphaDream track runs past its song's items");
            }
            if (!InReach(offsets, start, i))
            {
                throw std::invalid_argument("an AlphaDream track reads more than 64 KiB past its start");
            }

            const Command& item = items[i];
            const auto go = [&](uint32_t target)
            {
                if (target >= items.size())
                {
                    throw std::invalid_argument("an AlphaDream command's target is missing from the song");
                }
                if (!InReach(offsets, start, target))
                {
                    throw std::invalid_argument("an AlphaDream jump goes outside the 64 KiB from its track's start");
                }
                work.push_back({target, rests});
            };
            switch (item.kind)
            {
            case Kind::kAllocateTracks:
            case Kind::kOpenTrack:
            case Kind::kData:
                throw std::invalid_argument("an AlphaDream track reaches bytes that aren't commands");
            case Kind::kWait:
                if (IsBareWait(item))
                {
                    throw std::invalid_argument("an AlphaDream track reaches a byte of ticks without its note");
                }
                work.push_back({i + 1, rests});
                break;
            case Kind::kNote:
                {
                    const bool flag = (item.encoding.opcode & kRestFlag) != 0;
                    const bool tie = FromUnit(item.value.value, Scale{}) == 0;
                    const bool rest = tie ? flag : (rests || flag);
                    const bool after = tie ? rests : rest;
                    if (!tie && !flag)
                    {
                        std::set<bool>& states = note_states[i];
                        states.insert(rests);
                        if (states.size() > 1)
                        {
                            throw std::invalid_argument(
                                "an AlphaDream note takes a rest after it on one path and not on another");
                        }
                    }
                    if (!rest)
                    {
                        work.push_back({i + 1, after});
                        break;
                    }
                    if (i + 1 >= items.size() || !IsBareWait(items[i + 1]))
                    {
                        throw std::invalid_argument("an AlphaDream note takes a byte of ticks after it");
                    }
                    work.push_back({i + 2, after});
                    break;
                }
            case Kind::kJump:
                go(item.target);
                break;
            case Kind::kJumpIfGreater:
            case Kind::kJumpIfLessOrEqual:
                go(item.target);
                work.push_back({i + 1, rests});
                break;
            case Kind::kEnd:
                break;
            default:
                work.push_back({i + 1, rests});
                break;
            }
        }
    }
}

} // namespace

Decoded DecodeSequence(const Rom& rom, Version version, uint32_t address)
{
    if (!rom.Contains(address))
    {
        Decoded outside;
        outside.opaque = true;
        outside.reason = "the song starts outside the ROM";
        return outside;
    }
    if (!rom.Contains(address, 2))
    {
        return KeptAsData(rom, address, rom.End(), "the song's header is past the ROM");
    }

    const uint16_t mask = rom.U16(address);
    std::vector<std::pair<int, uint32_t>> starts; // each track of the header and the address where it starts
    uint32_t header = address + 2;
    for (int t = 0; t < kTracks; t++)
    {
        if (!(mask & (1u << t)))
        {
            continue;
        }
        if (!rom.Contains(header, 2))
        {
            return KeptAsData(rom, address, rom.End(), "the song's header runs past the ROM");
        }
        starts.push_back({t, address + rom.U16(header)});
        header += 2;
    }
    if (!(mask & kTrackBits))
    {
        return KeptAsData(rom, address, header, "the song plays no track");
    }

    Reader reader(rom, version);
    for (const auto& [track, start] : starts)
    {
        if (start < header)
        {
            return KeptAsData(rom, address, header, "track " + std::to_string(track) + " starts inside the header");
        }
        if (!reader.Track(start))
        {
            return KeptAsData(rom, address, std::max(header, reader.End()), reader.Reason());
        }
    }

    // The header, then the items in the order of their places, with the bytes between them as data.
    Decoded decoded;
    std::vector<Command>& items = decoded.sequence.items;
    Command allocate = Item(Kind::kAllocateTracks, mask);
    items.push_back(allocate);
    const size_t first_track = items.size();
    for (const auto& [track, start] : starts)
    {
        Command open = Item(Kind::kOpenTrack);
        open.index = uint8_t(track);
        items.push_back(open);
    }

    std::map<uint32_t, uint32_t> item_at; // the item at each address
    std::vector<std::optional<uint32_t>> targets(items.size());
    uint32_t at = header;
    for (const auto& [address_of, read] : reader.Reads())
    {
        if (address_of > at)
        {
            items.push_back(DataItem(rom, at, address_of));
            targets.emplace_back();
        }
        item_at[address_of] = uint32_t(items.size());
        items.push_back(read.item);
        targets.push_back(read.target);
        at = address_of + read.size;
    }

    for (size_t i = 0; i < starts.size(); i++)
    {
        const uint32_t item = item_at.at(starts[i].second);
        items[first_track + i].target = item;
        decoded.tracks.push_back(item);
    }
    for (size_t i = 0; i < items.size(); i++)
    {
        if (targets[i])
        {
            items[i].target = item_at.at(*targets[i]);
        }
    }
    decoded.size = at - address;

    return decoded;
}

std::vector<uint32_t> SequenceAddresses(const Rom& rom, const DriverInfo& info)
{
    std::vector<uint32_t> addresses;
    std::set<uint32_t> seen;
    for (int song = 0; song < info.song_count; song++)
    {
        const uint32_t address = rom.U32(info.song_table + 4 * uint32_t(song));
        if (seen.insert(address).second)
        {
            addresses.push_back(address);
        }
    }

    return addresses;
}

int SequenceCount(const Rom& rom, const DriverInfo& info)
{
    return int(SequenceAddresses(rom, info).size());
}

uint32_t SequenceAddress(const Rom& rom, const DriverInfo& info, int sequence)
{
    const std::vector<uint32_t> addresses = SequenceAddresses(rom, info);
    if (sequence < 0 || size_t(sequence) >= addresses.size())
    {
        throw std::out_of_range("the song table names no such AlphaDream song");
    }

    return addresses[size_t(sequence)];
}

int SequenceOfSong(const Rom& rom, const DriverInfo& info, int song)
{
    if (song < 0 || song >= info.song_count)
    {
        throw std::out_of_range("AlphaDream's song table has no such song");
    }

    const uint32_t address = rom.U32(info.song_table + 4 * uint32_t(song));
    const std::vector<uint32_t> addresses = SequenceAddresses(rom, info);

    return int(std::find(addresses.begin(), addresses.end(), address) - addresses.begin());
}

Decoded DecodeRomSequence(const Rom& rom, const DriverInfo& info, int sequence)
{
    return DecodeSequence(rom, info.version, SequenceAddress(rom, info, sequence));
}

std::vector<uint32_t> ItemOffsets(const commands::Sequence& sequence, Version version)
{
    std::vector<uint32_t> offsets;
    uint64_t at = 0;
    for (const Command& item : sequence.items)
    {
        offsets.push_back(uint32_t(at));
        at += ItemSize(item, version);
        if (at > UINT32_MAX)
        {
            throw std::invalid_argument("an AlphaDream song can't be this long");
        }
    }
    offsets.push_back(uint32_t(at));

    return offsets;
}

Encoded EncodeSequence(const commands::Sequence& sequence, Version version)
{
    const std::vector<Command>& items = sequence.items;
    if (items.empty() || items[0].kind != Kind::kAllocateTracks)
    {
        throw std::invalid_argument("an AlphaDream song starts with its tracks' mask");
    }

    const auto mask = uint16_t(DriverValue(items[0].value.value, 0, 0xFFFF, "mask"));
    std::vector<int> tracks;
    for (int t = 0; t < kTracks; t++)
    {
        if (mask & (1u << t))
        {
            tracks.push_back(t);
        }
    }
    if (tracks.empty())
    {
        throw std::invalid_argument("an AlphaDream song plays at least one track");
    }
    for (size_t i = 0; i < tracks.size(); i++)
    {
        if (items.size() <= i + 1 || items[i + 1].kind != Kind::kOpenTrack || items[i + 1].index != tracks[i])
        {
            throw std::invalid_argument(
                "an AlphaDream song's header gives an offset for each track of its mask, in "
                "order");
        }
    }

    Encoded encoded;
    encoded.offsets = ItemOffsets(sequence, version);
    CheckTracks(items, encoded.offsets, tracks.size());
    const auto offset_of = [&](uint32_t target)
    {
        if (target >= items.size())
        {
            throw std::invalid_argument("an AlphaDream command's target is missing from the song");
        }
        return encoded.offsets[target];
    };
    std::vector<uint8_t>& out = encoded.bytes;
    out.reserve(encoded.offsets.back());
    const auto put16 = [&out](uint32_t value)
    {
        out.push_back(uint8_t(value));
        out.push_back(uint8_t(value >> 8));
    };
    const auto jump = [&](const Command& item, uint32_t end)
    {
        const uint32_t to = offset_of(item.target);
        if (item.target_outside || to > end + 0xFFFF || end > to + 0xFFFF)
        {
            throw std::invalid_argument("an AlphaDream jump goes 16 bits at most");
        }
        put16((to - end) & 0xFFFF);
    };

    for (size_t i = 0; i < items.size(); i++)
    {
        const Command& item = items[i];
        const uint32_t end = encoded.offsets[i + 1];
        const auto byte = [&](int64_t low, int64_t high, const char* what)
        {
            out.push_back(uint8_t(DriverValue(item.value.value, low, high, what)));
        };
        switch (item.kind)
        {
        case Kind::kAllocateTracks:
            if (i != 0)
            {
                throw std::invalid_argument("an AlphaDream song has one mask, at its start");
            }
            put16(mask);
            break;
        case Kind::kOpenTrack:
            if (i > tracks.size())
            {
                throw std::invalid_argument("an AlphaDream track's offset is outside the song's header");
            }
            if (offset_of(item.target) > 0xFFFF)
            {
                throw std::invalid_argument("an AlphaDream track starts 16 bits at most from its song's start");
            }
            put16(offset_of(item.target));
            break;
        case Kind::kNote:
            {
                const auto length = uint8_t(DriverValue(item.value.value, 0, 0xFF, "note's length"));
                if (!IsNoteByte(length, version))
                {
                    throw std::invalid_argument("this length of a note reads as a command of AlphaDream's driver");
                }
                if (item.key > 0x7F || (item.encoding.opcode & ~kRestFlag))
                {
                    throw std::invalid_argument("AlphaDream's driver can't hold this key");
                }
                out.push_back(length);
                out.push_back(uint8_t(item.key | (item.encoding.opcode & kRestFlag)));
                if (version != Version::kSuperstarSaga)
                {
                    out.push_back(uint8_t(DriverValue(item.velocity, 0, 0xFF, "note's volume")));
                }
                else if (item.velocity != 0)
                {
                    throw std::invalid_argument("this version of AlphaDream's driver holds no volume in a note");
                }
                break;
            }
        case Kind::kWait:
            if (item.encoding.opcode == kRest)
            {
                out.push_back(kRest);
            }
            byte(0, 0xFF, "wait");
            break;
        case Kind::kOther:
            out.push_back(uint8_t(item.encoding.opcode));
            if (ItemSize(item, version) == 4)
            {
                if (item.bytes.size() != 3)
                {
                    throw std::invalid_argument("an AlphaDream LFO's settings take 3 bytes");
                }
                out.insert(out.end(), item.bytes.begin(), item.bytes.end());
            }
            else if (!item.bytes.empty())
            {
                throw std::invalid_argument("this AlphaDream command takes no bytes");
            }
            break;
        case Kind::kJumpIfGreater:
        case Kind::kJumpIfLessOrEqual:
            if (item.index > 3)
            {
                throw std::invalid_argument("AlphaDream's driver has 4 counters");
            }
            out.push_back(uint8_t((item.kind == Kind::kJumpIfGreater ? kCountedJump : kCountedLoop) + item.index));
            byte(0, 0xFF, "count");
            jump(item, end);
            break;
        case Kind::kProgram:
            out.push_back(kInstrument);
            byte(0, 0xFF, "instrument");
            break;
        case Kind::kVolume:
            out.push_back(kTrackVolume);
            byte(0, 0xFF, "volume");
            break;
        case Kind::kPan:
            out.push_back(kTrackPan);
            byte(0, 0xFF, "pan");
            break;
        case Kind::kTune:
            out.push_back(kFineTune);
            byte(-128, 127, "fine tune");
            break;
        case Kind::kBendRange:
            out.push_back(kRange);
            byte(0, 0xFF, "bend range");
            break;
        case Kind::kPitchBend:
            out.push_back(kBend);
            byte(-128, 127, "bend");
            break;
        case Kind::kNoteOff:
            out.push_back(kEndNote);
            break;
        case Kind::kJump:
            out.push_back(kGoto);
            jump(item, end);
            break;
        case Kind::kTempo:
            out.push_back(kTrackTempo);
            byte(0, 0xFF, "tempo");
            break;
        case Kind::kPriority:
            out.push_back(kChannelPriority);
            byte(0, 0xFF, "priority");
            break;
        case Kind::kEnd:
            out.push_back(kTrackEnd);
            break;
        case Kind::kData:
            out.insert(out.end(), item.bytes.begin(), item.bytes.end());
            break;
        default:
            throw std::invalid_argument("AlphaDream's driver has no command of this kind");
        }
        if (out.size() != end)
        {
            throw std::logic_error("an AlphaDream item took other bytes than its size");
        }
    }

    return encoded;
}

commands::Scale ScaleOf(commands::Kind)
{
    return {};
}

bool StartsNote(uint8_t first, Version version)
{
    return IsNoteByte(first, version);
}

uint32_t CommandSize(uint8_t first, Version version)
{
    if (IsNoteByte(first, version))
    {
        return NoteBytes(version);
    }

    switch (first)
    {
    case kPitchLfo:
    case kLevelLfo:
        return 4;
    case kPitchLfoOn:
    case kPitchLfoOff:
    case kLevelLfoOn:
    case kLevelLfoOff:
    case kEndNote:
    case kTrackEnd:
        return 1;
    case kGoto:
        return 3;
    default:
        // E8 to EF take 4 bytes. The others take a byte after them.
        return first >= kCountedJump && first < kInstrument ? 4 : 2;
    }
}

} // namespace ntdlib::alphadream
