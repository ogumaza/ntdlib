// SPDX-License-Identifier: MIT

#include "ntdlib/rd1/commands.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/data_items.h"

namespace ntdlib::rd1
{
namespace
{

using agb::DataItem;
using commands::Command;
using commands::Kind;
using commands::kOne;
using commands::Scale;

// The bytes of a header chunk with 6 bytes of data, and of a track chunk's header.
constexpr uint32_t kHeaderBytes = 14;
constexpr uint32_t kChunkHeaderBytes = 8;

// The header chunk's ID: "MThd".
constexpr uint32_t kHeaderId = 0x4D546864;

// The microseconds of a minute, from which the driver works out the beats a minute.
constexpr uint32_t kMicrosPerMinute = 60000000;

// The most microseconds a quarter note that a tempo's 3 bytes hold.
constexpr uint32_t kLargestMicros = 0xFFFFFF;

// The meta events that the driver reads.
constexpr uint8_t kMarker = 0x06;
constexpr uint8_t kEndOfTrack = 0x2F;
constexpr uint8_t kSetTempo = 0x51;

// The markers that start and end the loop.
constexpr uint8_t kLoopStartMarker = '[';
constexpr uint8_t kLoopEndMarker = ']';

// The controllers that have a kind.
struct ControllerKind
{
    uint8_t controller = 0;
    Kind kind = Kind::kOther;
};

constexpr std::array<ControllerKind, 9> kControllers = {{
    {1, Kind::kLfoDepth},
    {7, Kind::kVolume},
    {10, Kind::kPan},
    {11, Kind::kVolume2},
    {20, Kind::kBendRange},
    {21, Kind::kLfoSpeed},
    {22, Kind::kLfoType},
    {26, Kind::kLfoDelay},
    {33, Kind::kPriority},
}};

std::optional<Kind> ControllerKindOf(uint8_t controller)
{
    for (const ControllerKind& c : kControllers)
    {
        if (c.controller == controller)
        {
            return c.kind;
        }
    }

    return std::nullopt;
}

std::optional<uint8_t> ControllerOf(Kind kind)
{
    for (const ControllerKind& c : kControllers)
    {
        if (c.kind == kind)
        {
            return c.controller;
        }
    }

    return std::nullopt;
}

uint32_t BigEndian32(std::span<const uint8_t> data, size_t at)
{
    return uint32_t(data[at]) << 24 | uint32_t(data[at + 1]) << 16 | uint32_t(data[at + 2]) << 8 | data[at + 3];
}

uint16_t BigEndian16(std::span<const uint8_t> data, size_t at)
{
    return uint16_t(data[at] << 8 | data[at + 1]);
}

uint32_t BigEndian24(std::span<const uint8_t> data)
{
    return uint32_t(data[0]) << 16 | uint32_t(data[1]) << 8 | data[2];
}

void StoreBigEndian32(std::vector<uint8_t>& bytes, size_t at, uint32_t value)
{
    bytes[at] = uint8_t(value >> 24);
    bytes[at + 1] = uint8_t(value >> 16);
    bytes[at + 2] = uint8_t(value >> 8);
    bytes[at + 3] = uint8_t(value);
}

void StoreBigEndian16(std::vector<uint8_t>& bytes, size_t at, uint16_t value)
{
    bytes[at] = uint8_t(value >> 8);
    bytes[at + 1] = uint8_t(value);
}

// The beats a minute that the driver takes from a tempo of `micros` microseconds a quarter note.
uint32_t BeatsOf(uint32_t micros)
{
    return micros ? kMicrosPerMinute / micros : 0;
}

// A variable-length number as the driver reads it: up to a byte below 0x80, however many bytes that takes.
struct Variable
{
    uint32_t value = 0;
    uint32_t bytes = 0;
    bool complete = false; // a byte below 0x80 ended the number before the end
};

Variable ReadVariable(std::span<const uint8_t> data, size_t at, size_t end)
{
    Variable v;
    while (at + v.bytes < end)
    {
        const uint8_t b = data[at + v.bytes];
        v.bytes++;
        v.value = v.value << 7 | (b & 0x7F);
        if (!(b & 0x80))
        {
            v.complete = true;
            break;
        }
    }

    return v;
}

// The fewest bytes that hold `value` as a variable-length number.
uint32_t VariableBytes(uint32_t value)
{
    uint32_t bytes = 1;
    while (value >>= 7)
    {
        bytes++;
    }

    return bytes;
}

// Adds `value` as a variable-length number of `value_bytes` bytes, or of the fewest that hold it.
void AddVariable(std::vector<uint8_t>& bytes, uint32_t value, uint8_t value_bytes)
{
    const uint32_t count = std::max<uint32_t>(VariableBytes(value), value_bytes);
    for (uint32_t i = count; i-- > 0;)
    {
        const uint8_t group = i < 5 ? uint8_t((uint64_t(value) >> (7 * i)) & 0x7F) : 0;
        bytes.push_back(uint8_t(group | (i ? 0x80 : 0)));
    }
}

// The value's byte. Throws std::invalid_argument outside `low` to `high`.
int64_t Checked(int64_t value, int64_t low, int64_t high, Kind kind)
{
    if (value < low || value > high)
    {
        throw std::invalid_argument("a value of a " + std::string(commands::Name(kind)) +
                                    " command doesn't fit in a MIDI file's bytes");
    }

    return value;
}

// The bytes of a MIDI file's header chunk, its tracks, and where they end.
struct FileShape
{
    uint32_t header_end = 0;
    std::vector<std::pair<uint32_t, uint32_t>> chunks; // each track chunk's start and end
    uint32_t end = 0;                                  // the end of the last chunk that is whole in the bytes
};

// Finds a file's chunks the way the driver's start routine does, up to the end of `data`. Returns the reason
// where the driver reads the file otherwise than its chunks' lengths say, or where they aren't whole in `data`.
std::string ShapeOf(std::span<const uint8_t> data, FileShape& shape)
{
    if (data.size() < kHeaderBytes || BigEndian32(data, 0) != kHeaderId)
    {
        shape.end = uint32_t(data.size());
        return "the bytes don't start with a header chunk";
    }

    const uint32_t length = BigEndian32(data, 4);
    if (length < kHeaderBytes - kChunkHeaderBytes || length > data.size() - kChunkHeaderBytes)
    {
        shape.end = uint32_t(data.size());
        return length < kHeaderBytes - kChunkHeaderBytes ? "the header chunk is shorter than the fields it holds"
                                                         : "the header chunk runs past the bytes";
    }

    shape.header_end = kChunkHeaderBytes + length;
    shape.end = shape.header_end;
    const uint16_t tracks = BigEndian16(data, 10);
    for (int t = 0; t < (tracks & kMaxTracks); t++)
    {
        const uint32_t at = shape.end;
        if (data.size() - at < kChunkHeaderBytes || BigEndian32(data, at + 4) > data.size() - at - kChunkHeaderBytes)
        {
            return "a track chunk runs past the bytes";
        }

        shape.end = at + kChunkHeaderBytes + BigEndian32(data, at + 4);
        shape.chunks.emplace_back(at, shape.end);
    }

    if (tracks > kMaxTracks)
    {
        return "the header names more tracks than the driver reads";
    }

    const uint16_t division = BigEndian16(data, 12);
    if (division == 0 || division & 0x8000)
    {
        return "the header gives a division that the driver doesn't play";
    }

    return {};
}

// An event as the driver reads it.
struct Event
{
    Command item;
    size_t next = 0;    // the byte after the event
    bool ended = false; // the event ends the track
};

// Reads the event at `at` of a track whose chunk ends at `end`, with the running status `status`. Returns nothing and
// sets `reason` where the driver reads otherwise.
std::optional<Event> ReadEvent(std::span<const uint8_t> data, size_t at, size_t end, uint8_t& status,
                               std::string& reason)
{
    Event event;
    Command& item = event.item;
    if (data[at] & 0x80)
    {
        status = data[at];
        at++;
    }
    else if (!status)
    {
        reason = "a track reads a data byte before any status byte";
        return std::nullopt;
    }
    else
    {
        item.encoding.repeated = true;
    }

    reason = "a track runs past its chunk's end";
    if (status >= 0xF0)
    {
        uint8_t type = 0;
        if (status == 0xFF)
        {
            if (at >= end)
            {
                return std::nullopt;
            }

            type = data[at++];
        }

        const Variable length = ReadVariable(data, at, end);
        if (!length.complete || length.value > end - at - length.bytes)
        {
            return std::nullopt;
        }

        at += length.bytes;
        if (length.bytes > VariableBytes(length.value))
        {
            item.encoding.value_bytes = uint8_t(std::min<uint32_t>(length.bytes, 0xFF));
        }
        item.bytes.assign(data.begin() + std::ptrdiff_t(at), data.begin() + std::ptrdiff_t(at + length.value));
        event.next = at + length.value;
        if (status != 0xFF)
        {
            item.kind = Kind::kOther;
            item.encoding.opcode = status;
        }
        else if (type == kEndOfTrack)
        {
            item.kind = Kind::kEnd;
            event.ended = true;
        }
        else if (type == kSetTempo && length.value == 3)
        {
            item.kind = Kind::kTempo;
            item.value.value = ToUnit(BeatsOf(BigEndian24(item.bytes)), ScaleOf(Kind::kTempo));
        }
        else if (type == kMarker && length.value == 1 &&
                 (item.bytes[0] == kLoopStartMarker || item.bytes[0] == kLoopEndMarker))
        {
            item.kind = item.bytes[0] == kLoopStartMarker ? Kind::kLoopStart : Kind::kLoopEnd;
            item.bytes.clear();
        }
        else
        {
            item.kind = Kind::kOther;
            item.encoding.opcode = uint16_t(kMetaEvent | type);
        }
        reason.clear();

        return event;
    }

    const uint8_t high = status & 0xF0;
    const size_t size = high == 0xC0 || high == 0xD0 ? 1 : 2;
    if (end - at < size)
    {
        return std::nullopt;
    }

    reason.clear();
    item.encoding.opcode = status;
    event.next = at + size;
    const uint8_t first = data[at];
    const uint8_t second = size == 2 ? data[at + 1] : 0;
    if (first >= 0x80 || second >= 0x80 || high == 0xA0 || high == 0xD0)
    {
        item.kind = Kind::kOther;
        item.bytes.assign(data.begin() + std::ptrdiff_t(at), data.begin() + std::ptrdiff_t(at + size));
        return event;
    }

    switch (high)
    {
    case 0x80:
    case 0x90:
        item.kind = high == 0x90 && second ? Kind::kNote : Kind::kNoteOff;
        item.key = first;
        item.velocity = ToUnit(second, Scale{});
        break;

    case 0xB0:
        {
            const std::optional<Kind> kind = ControllerKindOf(first);
            if (kind)
            {
                item.kind = *kind;
                item.value.value = ToUnit(second, ScaleOf(*kind));
            }
            else
            {
                item.kind = Kind::kOther;
                item.bytes = {first, second};
            }
            break;
        }

    case 0xC0:
        item.kind = Kind::kProgram;
        item.value.value = ToUnit(first, ScaleOf(Kind::kProgram));
        break;

    default:
        item.kind = Kind::kPitchBend;
        item.value.value = ToUnit(first | second << 7, ScaleOf(Kind::kPitchBend));
        break;
    }

    return event;
}

// Reads the track whose events run from `start` to `end` the way the driver does. Returns the reason where the driver
// reads them otherwise.
std::string ReadTrack(std::span<const uint8_t> data, size_t start, size_t end, std::vector<Command>& items)
{
    uint8_t status = 0;
    size_t at = start;
    for (;;)
    {
        const Variable delta = ReadVariable(data, at, end);
        if (!delta.complete || at + delta.bytes >= end)
        {
            return "a track runs past its chunk's end";
        }

        Command wait;
        wait.kind = Kind::kWait;
        wait.value.value = ToUnit(delta.value, Scale{});
        if (delta.bytes > VariableBytes(delta.value))
        {
            wait.encoding.value_bytes = uint8_t(std::min<uint32_t>(delta.bytes, 0xFF));
        }
        items.push_back(std::move(wait));
        at += delta.bytes;

        std::string reason;
        std::optional<Event> event = ReadEvent(data, at, end, status, reason);
        if (!event)
        {
            return reason;
        }

        items.push_back(std::move(event->item));
        at = event->next;
        if (event->ended)
        {
            break;
        }
    }

    if (at < end)
    {
        items.push_back(DataItem(data.subspan(at, end - at)));
    }

    return {};
}

// The status byte of an event and the bytes after it.
struct EventBytes
{
    uint8_t status = 0;
    std::vector<uint8_t> rest;
};

// The status byte of a channel event of `high`: the item's status byte, or one on `channel`.
uint8_t ChannelStatus(const Command& item, uint8_t high, uint8_t channel)
{
    const uint16_t opcode = item.encoding.opcode;
    if (opcode >= 0x80 && opcode < 0xF0 && (opcode & 0xF0) == high)
    {
        return uint8_t(opcode);
    }

    return uint8_t(high | channel);
}

// Adds a meta event's or a system exclusive event's length and bytes.
void AddLengthAndBytes(std::vector<uint8_t>& rest, const Command& item, std::span<const uint8_t> bytes)
{
    AddVariable(rest, uint32_t(bytes.size()), item.encoding.value_bytes);
    rest.insert(rest.end(), bytes.begin(), bytes.end());
}

// The tempo's 3 bytes: the item's bytes where they give its beats a minute, or the longest quarter note that gives
// them.
std::array<uint8_t, 3> TempoBytes(const Command& item)
{
    const int64_t beats = Checked(FromUnit(item.value.value, ScaleOf(Kind::kTempo)), 0, kMicrosPerMinute, item.kind);
    if (item.bytes.size() == 3 && BeatsOf(BigEndian24(item.bytes)) == beats)
    {
        return {item.bytes[0], item.bytes[1], item.bytes[2]};
    }

    const uint32_t micros = beats ? std::min<uint32_t>(kMicrosPerMinute / uint32_t(beats), kLargestMicros) : 0;
    if (BeatsOf(micros) != beats)
    {
        throw std::invalid_argument("no tempo in a MIDI file's 3 bytes gives " + std::to_string(beats) +
                                    " beats a minute");
    }

    return {uint8_t(micros >> 16), uint8_t(micros >> 8), uint8_t(micros)};
}

// The bytes of an event. A new channel event plays on `channel`. Throws std::invalid_argument for a command that the
// bytes can't hold.
EventBytes BytesOf(const Command& item, uint8_t channel)
{
    const int64_t value = FromUnit(item.value.value, ScaleOf(item.kind));
    const int64_t velocity = FromUnit(item.velocity, Scale{});
    const auto data_byte = [&item](int64_t v)
    {
        return uint8_t(Checked(v, 0, 0x7F, item.kind));
    };

    EventBytes out;
    switch (item.kind)
    {
    case Kind::kNote:
        out.status = ChannelStatus(item, 0x90, channel);
        out.rest = {data_byte(item.key), uint8_t(Checked(velocity, 1, 0x7F, item.kind))};
        break;

    case Kind::kNoteOff:
        {
            const uint16_t opcode = item.encoding.opcode;
            const bool channel_event = opcode >= 0x80 && opcode < 0xF0;
            if (channel_event && (opcode & 0xF0) == 0x90 && velocity == 0)
            {
                out.status = uint8_t(opcode);
            }
            else
            {
                out.status = uint8_t(0x80 | (channel_event ? opcode & 0xF : channel));
            }
            out.rest = {data_byte(item.key), data_byte(velocity)};
            break;
        }

    case Kind::kProgram:
        out.status = ChannelStatus(item, 0xC0, channel);
        out.rest = {data_byte(value)};
        break;

    case Kind::kPitchBend:
        {
            const auto bend = uint32_t(Checked(value, 0, 0x3FFF, item.kind));
            out.status = ChannelStatus(item, 0xE0, channel);
            out.rest = {uint8_t(bend & 0x7F), uint8_t(bend >> 7)};
            break;
        }

    case Kind::kTempo:
        {
            const std::array<uint8_t, 3> micros = TempoBytes(item);
            out.status = 0xFF;
            out.rest = {kSetTempo};
            AddLengthAndBytes(out.rest, item, micros);
            break;
        }

    case Kind::kLoopStart:
    case Kind::kLoopEnd:
        {
            const uint8_t marker = item.kind == Kind::kLoopStart ? kLoopStartMarker : kLoopEndMarker;
            out.status = 0xFF;
            out.rest = {kMarker};
            AddLengthAndBytes(out.rest, item, std::span<const uint8_t>(&marker, 1));
            break;
        }

    case Kind::kEnd:
        out.status = 0xFF;
        out.rest = {kEndOfTrack};
        AddLengthAndBytes(out.rest, item, item.bytes);
        break;

    case Kind::kOther:
        {
            const uint16_t opcode = item.encoding.opcode;
            if ((opcode & 0xFF00) == kMetaEvent)
            {
                out.status = 0xFF;
                out.rest = {uint8_t(opcode)};
                AddLengthAndBytes(out.rest, item, item.bytes);
            }
            else if (opcode >= 0xF0 && opcode < 0xFF)
            {
                out.status = uint8_t(opcode);
                AddLengthAndBytes(out.rest, item, item.bytes);
            }
            else if (opcode >= 0x80 && opcode < 0xF0)
            {
                const size_t size = (opcode & 0xE0) == 0xC0 ? 1 : 2;
                if (item.bytes.size() != size)
                {
                    throw std::invalid_argument("a channel event of kOther holds " + std::to_string(item.bytes.size()) +
                                                " bytes in place of " + std::to_string(size));
                }
                out.status = uint8_t(opcode);
                out.rest = item.bytes;
            }
            else
            {
                throw std::invalid_argument("a command of kOther without a status byte that the driver reads");
            }
            break;
        }

    default:
        {
            const std::optional<uint8_t> controller = ControllerOf(item.kind);
            if (!controller)
            {
                throw std::invalid_argument("a " + std::string(commands::Name(item.kind)) +
                                            " command that a MIDI file can't hold");
            }
            out.status = ChannelStatus(item, 0xB0, channel);
            out.rest = {*controller, data_byte(value)};
            break;
        }
    }

    return out;
}

// What the driver reads kOther item `item` as, where it reads more than an item without a kind: a track's end, a tempo,
// a loop marker or a note. Nothing where the driver reads the item as it says.
const char* StructureOf(const Command& item)
{
    const uint16_t opcode = item.encoding.opcode;
    const std::vector<uint8_t>& bytes = item.bytes;
    if ((opcode & 0xFF00) == kMetaEvent)
    {
        const auto type = uint8_t(opcode);
        if (type == kEndOfTrack)
        {
            return "a kOther item that the driver reads as a track's end";
        }
        if (type == kSetTempo && bytes.size() == 3)
        {
            return "a kOther item that the driver reads as a tempo";
        }
        if (type == kMarker && bytes.size() == 1 && (bytes[0] == kLoopStartMarker || bytes[0] == kLoopEndMarker))
        {
            return "a kOther item that the driver reads as a loop marker";
        }
    }
    else if ((opcode & 0xFFF0) == 0x90 && bytes.size() == 2 && (bytes[1] & 0x7F) != 0)
    {
        return "a kOther item that the driver reads as a note";
    }

    return nullptr;
}

bool IsTrackChunk(const Command& item)
{
    return item.kind == Kind::kOpenTrack && item.encoding.opcode == kTrackChunk;
}

// The ticks that each run of waits in `items` writes, at the run's first wait. Sets `writes` at each run's first wait.
std::vector<uint64_t> WaitRuns(const std::vector<Command>& items, std::vector<bool>& writes)
{
    std::vector<uint64_t> waits(items.size(), 0);
    writes.assign(items.size(), false);
    size_t run = 0;
    for (size_t i = 0; i < items.size(); i++)
    {
        if (items[i].kind != Kind::kWait)
        {
            continue;
        }

        const auto ticks = uint64_t(Checked(FromUnit(items[i].value.value, Scale{}), 0, 0xFFFFFFFF, Kind::kWait));
        if (i == 0 || items[i - 1].kind != Kind::kWait)
        {
            run = i;
            writes[i] = true;
        }
        waits[run] += ticks;
        if (waits[run] > 0xFFFFFFFF)
        {
            throw std::invalid_argument("a run of waits takes more ticks than a delta time holds");
        }
    }

    return waits;
}

// Appends the header chunk of `item`, of kind kTimebase, to `bytes`: its bytes or the usual header, with its size and
// division.
void AddHeaderChunk(std::vector<uint8_t>& bytes, const Command& item)
{
    const size_t header = bytes.size();
    if (item.bytes.empty())
    {
        bytes.insert(bytes.end(), {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 0, 0, 0});
    }
    else
    {
        bytes.insert(bytes.end(), item.bytes.begin(), item.bytes.end());
    }
    StoreBigEndian32(bytes, header + 4, uint32_t(bytes.size() - header - kChunkHeaderBytes));
    const int64_t division = FromUnit(item.value.value, ScaleOf(Kind::kTimebase));
    StoreBigEndian16(bytes, header + 12, uint16_t(Checked(division, 1, 0x7FFF, item.kind)));
}

// Lays `sequence` out. With `check`, throws std::invalid_argument for items that the driver wouldn't read as they say.
// Throws std::invalid_argument for a command that the bytes can't hold.
Encoded Write(const commands::Sequence& sequence, bool check)
{
    const std::vector<Command>& items = sequence.items;
    const auto fail = [check](const char* what)
    {
        if (check)
        {
            throw std::invalid_argument(what);
        }
    };

    std::vector<bool> writes;
    const std::vector<uint64_t> waits = WaitRuns(items, writes);

    enum class State
    {
        kOutside,   // before the first track
        kNeedDelta, // a delta time comes next
        kNeedEvent, // an event comes next
        kEnded,     // the track has ended
    };

    Encoded out;
    std::vector<uint8_t>& bytes = out.bytes;
    std::optional<size_t> header;
    std::optional<size_t> chunk;
    int tracks = 0;
    State state = State::kOutside;
    uint8_t status = 0;
    uint8_t channel = 0;
    const auto close_chunk = [&]()
    {
        if (!chunk)
        {
            return;
        }

        if (state != State::kEnded)
        {
            fail("a track without an end runs into the next chunk");
        }
        StoreBigEndian32(bytes, *chunk + 4, uint32_t(bytes.size() - *chunk - kChunkHeaderBytes));
    };

    for (size_t i = 0; i < items.size(); i++)
    {
        const Command& item = items[i];
        out.offsets.push_back(uint32_t(bytes.size()));
        if (item.kind == Kind::kTimebase)
        {
            if (i != 0)
            {
                fail("a MIDI file's header comes after other items");
            }
            if (!item.bytes.empty() && item.bytes.size() < kHeaderBytes)
            {
                throw std::invalid_argument("a header chunk's bytes are fewer than 14");
            }

            header = bytes.size();
            AddHeaderChunk(bytes, item);
            continue;
        }

        if (IsTrackChunk(item))
        {
            if (!header)
            {
                fail("a track chunk comes before the header");
            }
            if (!item.bytes.empty() && item.bytes.size() != kChunkHeaderBytes)
            {
                throw std::invalid_argument("a track chunk's header takes 8 bytes");
            }

            close_chunk();
            chunk = bytes.size();
            if (item.bytes.empty())
            {
                bytes.insert(bytes.end(), {'M', 'T', 'r', 'k', 0, 0, 0, 0});
            }
            else
            {
                bytes.insert(bytes.end(), item.bytes.begin(), item.bytes.end());
            }
            state = State::kNeedDelta;
            status = 0;
            channel = uint8_t(tracks & 0xF);
            tracks++;
            continue;
        }

        if (item.kind == Kind::kData)
        {
            if (state != State::kEnded)
            {
                fail(state == State::kOutside ? "data before a MIDI file's first track" : "a track reaches data");
            }
            bytes.insert(bytes.end(), item.bytes.begin(), item.bytes.end());
            continue;
        }

        if (state == State::kOutside || state == State::kEnded)
        {
            fail(state == State::kOutside ? "an event outside a track" : "an event after a track's end");
        }
        if (item.kind == Kind::kWait)
        {
            if (writes[i])
            {
                AddVariable(bytes, uint32_t(waits[i]), item.encoding.value_bytes);
            }
            state = State::kNeedEvent;
            continue;
        }

        // An event without a wait before it takes a delta time of 0.
        if (state == State::kNeedDelta)
        {
            bytes.push_back(0);
            out.offsets.back() = uint32_t(bytes.size());
        }

        if (item.kind == Kind::kOther)
        {
            if (const char* structure = StructureOf(item))
            {
                fail(structure);
            }
        }

        const EventBytes event = BytesOf(item, channel);
        const bool omit =
            item.encoding.repeated && event.status == status && !event.rest.empty() && event.rest.front() < 0x80;
        if (!omit)
        {
            bytes.push_back(event.status);
        }
        bytes.insert(bytes.end(), event.rest.begin(), event.rest.end());
        status = event.status;
        if (status < 0xF0)
        {
            channel = status & 0xF;
        }
        state = item.kind == Kind::kEnd ? State::kEnded : State::kNeedDelta;
    }

    close_chunk();
    if (check && !header)
    {
        throw std::invalid_argument("a MIDI file without a header chunk");
    }
    if (check && tracks > kMaxTracks)
    {
        throw std::invalid_argument("a MIDI file with more tracks than the driver reads");
    }
    if (header)
    {
        StoreBigEndian16(bytes, *header + 10, uint16_t(tracks));
    }
    out.offsets.push_back(uint32_t(bytes.size()));

    return out;
}

} // namespace

Decoded DecodeSequence(std::span<const uint8_t> data)
{
    if (data.empty())
    {
        throw std::invalid_argument("a MIDI file of no bytes");
    }

    Decoded decoded;
    FileShape shape;
    std::string reason = ShapeOf(data, shape);
    decoded.size = shape.end;
    const std::span<const uint8_t> file = data.first(shape.end);
    std::vector<Command>& items = decoded.sequence.items;
    if (reason.empty())
    {
        Command header;
        header.kind = Kind::kTimebase;
        header.value.value = ToUnit(BigEndian16(file, 12), ScaleOf(Kind::kTimebase));
        header.bytes.assign(file.begin(), file.begin() + shape.header_end);
        std::fill(header.bytes.begin() + 4, header.bytes.begin() + 8, 0);
        std::fill(header.bytes.begin() + 10, header.bytes.begin() + kHeaderBytes, 0);
        items.push_back(std::move(header));
        for (size_t t = 0; t < shape.chunks.size() && reason.empty(); t++)
        {
            const auto [start, end] = shape.chunks[t];
            Command chunk;
            chunk.kind = Kind::kOpenTrack;
            chunk.encoding.opcode = kTrackChunk;
            chunk.index = uint8_t(t);
            chunk.target = uint32_t(items.size() + 1);
            chunk.bytes.assign(file.begin() + start, file.begin() + start + kChunkHeaderBytes);
            std::fill(chunk.bytes.begin() + 4, chunk.bytes.end(), 0);
            decoded.tracks.push_back(uint32_t(items.size()));
            items.push_back(std::move(chunk));
            reason = ReadTrack(file, start + kChunkHeaderBytes, end, items);
        }
    }

    // The items have to give the bytes back.
    if (reason.empty())
    {
        try
        {
            const Encoded encoded = Write(decoded.sequence, true);
            if (!std::equal(encoded.bytes.begin(), encoded.bytes.end(), file.begin(), file.end()))
            {
                reason = "its items don't give its bytes back";
            }
        }
        catch (const std::invalid_argument& e)
        {
            reason = std::string("its items don't encode: ") + e.what();
        }
    }

    if (!reason.empty())
    {
        decoded.opaque = true;
        decoded.reason = reason;
        decoded.tracks.clear();
        items.clear();
        items.push_back(DataItem(file));
    }

    return decoded;
}

std::vector<uint32_t> SequenceAddresses(const Rom& rom, const DriverInfo& info)
{
    std::vector<uint32_t> addresses;
    std::set<uint32_t> seen;
    for (int song = 0; song < info.song_count; song++)
    {
        SongRecord record;
        if (ReadSongRecord(rom, info, song, record) && seen.insert(record.midi).second)
        {
            addresses.push_back(record.midi);
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
        throw std::out_of_range("R&D1's driver has no sequence " + std::to_string(sequence));
    }

    return addresses[size_t(sequence)];
}

int SequenceOfSong(const Rom& rom, const DriverInfo& info, int song)
{
    SongRecord record;
    if (!ReadSongRecord(rom, info, song, record))
    {
        throw std::out_of_range("R&D1's driver has no song " + std::to_string(song));
    }

    const std::vector<uint32_t> addresses = SequenceAddresses(rom, info);
    return int(std::find(addresses.begin(), addresses.end(), record.midi) - addresses.begin());
}

Decoded DecodeRomSequence(const Rom& rom, const DriverInfo& info, int sequence)
{
    const uint32_t address = SequenceAddress(rom, info, sequence);
    const std::span<const uint8_t> rest(rom.Ptr(address), rom.End() - address);
    FileShape shape;
    ShapeOf(rest, shape);

    return DecodeSequence(rest.first(shape.end));
}

Encoded EncodeSequence(const commands::Sequence& sequence)
{
    return Write(sequence, true);
}

std::vector<uint32_t> ItemOffsets(const commands::Sequence& sequence)
{
    return Write(sequence, false).offsets;
}

Scale ScaleOf(Kind kind)
{
    switch (kind)
    {
    case Kind::kPan:
        return {kOne, -64 * kOne};
    case Kind::kPitchBend:
        return {kOne / 8192, -kOne};
    case Kind::kBendRange:
        return {100 * kOne, 0};
    default:
        return {};
    }
}

} // namespace ntdlib::rd1
