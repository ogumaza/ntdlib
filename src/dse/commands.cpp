// SPDX-License-Identifier: MIT

#include "ntdlib/dse/commands.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/dse/tables.h"

namespace ntdlib::dse
{
namespace
{

using commands::Command;
using commands::Kind;
using commands::kOne;
using commands::Scale;

// The bytes that start a track: the chunk's header and the preamble.
constexpr std::size_t kChunkHeader = 16;
constexpr std::size_t kTrackStart = kChunkHeader + 4;

// The chunk's label and the 8 bytes after it (formats.cpp).
constexpr std::array<uint8_t, 12> kTrackLabel = {'t', 'r', 'k', ' ', 0x00, 0x00, 0x00, 0x01, 0x04, 0xFF, 0x00, 0x00};

constexpr std::size_t kSizeField = 12;

constexpr uint8_t kLowBits = 0x3F;
constexpr uint32_t kMaxLength = 0xFFFFFF;

// What the driver keeps of a track for the bytes of its events (Driver's start of a track).
struct TrackState
{
    uint8_t octave = 4;
    uint32_t previous_length = 0;
    uint32_t previous_wait = 0;

    // Whether a track's next note takes the same octave on each pass. A note after a loop's start (0x99) or a repeat's
    // start (0x9C) takes it from the end of the pass before, unless the command that sets the octave comes first.
    bool octave_set = true;
};

// The kind of a command with a value in one byte, or nothing.
std::optional<Kind> ByteKindOf(uint8_t code)
{
    switch (code)
    {
    case kRepeatStart:
        return Kind::kLoopStart;
    case kTempo:
    case kTempoAgain:
        return Kind::kTempo;
    case kProgram:
        return Kind::kProgram;
    case kBendRange:
        return Kind::kBendRange;
    case kVolume:
        return Kind::kVolume;
    case kExpression:
        return Kind::kVolume2;
    case kPan:
        return Kind::kPan;
    default:
        return std::nullopt;
    }
}

// The code of a new command of `kind` with a value in one byte, or nothing.
std::optional<uint8_t> ByteCodeOf(Kind kind)
{
    switch (kind)
    {
    case Kind::kLoopStart:
        return kRepeatStart;
    case Kind::kTempo:
        return kTempo;
    case Kind::kProgram:
        return kProgram;
    case Kind::kBendRange:
        return kBendRange;
    case Kind::kVolume:
        return kVolume;
    case Kind::kVolume2:
        return kExpression;
    case Kind::kPan:
        return kPan;
    default:
        return std::nullopt;
    }
}

// The key that a note's low 6 bits give after `octave`. The driver's sums wrap as these do.
uint8_t KeyOf(uint8_t octave, uint8_t low_bits)
{
    const int change = ((low_bits >> 4) & 3) - 2;
    return static_cast<uint8_t>(static_cast<int8_t>(octave + change) * 12 + (low_bits & 0xF));
}

uint8_t OctaveAfter(uint8_t octave, uint8_t low_bits)
{
    return static_cast<uint8_t>(octave + ((low_bits >> 4) & 3) - 2);
}

// The low 6 bits of a new note of `key` after `octave`, or nothing where its octave lies out of reach.
std::optional<uint8_t> NewLowBits(uint8_t octave, uint8_t key)
{
    const int change = key / 12 - static_cast<int8_t>(octave);
    if (change < -2 || change > 1)
    {
        return std::nullopt;
    }

    return static_cast<uint8_t>((change + 2) << 4 | key % 12);
}

// The fewest bytes that hold a note's length.
uint8_t LengthBytes(uint32_t length)
{
    return length <= 0xFF ? 1 : length <= 0xFFFF ? 2 : 3;
}

// The code of a new wait of `ticks`, or nothing where no code holds it.
std::optional<uint8_t> NewWaitCode(uint32_t ticks)
{
    const auto* pause = std::ranges::find(tables::kPauseLengths, ticks);
    if (pause != std::end(tables::kPauseLengths))
    {
        return static_cast<uint8_t>(kFirstPause + (pause - std::begin(tables::kPauseLengths)));
    }
    if (ticks <= 0xFF)
    {
        return kWait1Byte;
    }
    if (ticks <= 0xFFFF)
    {
        return kWait2Bytes;
    }
    if (ticks <= kMaxLength)
    {
        return kWait3Bytes;
    }

    return std::nullopt;
}

// Whether a wait of `code` gives `ticks` after a wait of `previous`.
bool WaitCodeGives(uint16_t code, uint32_t ticks, uint32_t previous)
{
    if (code >= kFirstPause && code <= kLastPause)
    {
        return tables::kPauseLengths[code - kFirstPause] == ticks;
    }

    switch (code)
    {
    case kWaitAgain:
        return ticks == previous;
    case kWaitChange:
        {
            const auto change = static_cast<int32_t>(ticks - previous);
            return change >= -128 && change <= 127;
        }
    case kWait1Byte:
        return ticks <= 0xFF;
    case kWait2Bytes:
        return ticks <= 0xFFFF;
    case kWait3Bytes:
        return ticks <= kMaxLength;
    default:
        return false;
    }
}

// What a command of `code` with `arguments` does to the octave of `state`.
void FollowOctave(TrackState& state, uint16_t code, const std::vector<uint8_t>& arguments)
{
    if (code == kOctave)
    {
        state.octave = arguments.at(0);
        state.octave_set = true;
    }
    else if (code == kOctaveChange)
    {
        state.octave = static_cast<uint8_t>(state.octave + static_cast<int8_t>(arguments.at(0)));
    }
    else if (code == kLoopPoint)
    {
        state.octave_set = false;
    }
}

Command DecodeNote(const Event& event, TrackState& state)
{
    const uint8_t first = event.arguments.at(0);
    const uint8_t low_bits = first & kLowBits;
    const int count = first >> 6;

    Command command;
    command.kind = Kind::kNote;
    command.key = KeyOf(state.octave, low_bits);
    command.velocity = commands::ToUnit(event.code, Scale{});
    if (NewLowBits(state.octave, command.key) != low_bits || !state.octave_set)
    {
        command.encoding.arguments = low_bits;
    }
    state.octave_set = true;

    uint32_t length = state.previous_length;
    if (count == 0)
    {
        command.encoding.repeated = true;
    }
    else
    {
        length = 0;
        for (int i = 0; i < count; i++)
        {
            length = length << 8 | event.arguments.at(static_cast<std::size_t>(1 + i));
        }
        state.previous_length = length;
        if (count != LengthBytes(length))
        {
            command.encoding.value_bytes = static_cast<uint8_t>(count);
        }
    }
    command.value.value = commands::ToUnit(length, ScaleOf(Kind::kNote));
    state.octave = OctaveAfter(state.octave, low_bits);

    return command;
}

// The ticks of a wait of `event`, after the waits before it.
uint32_t WaitTicks(const Event& event, const TrackState& state)
{
    const std::vector<uint8_t>& a = event.arguments;
    if (event.code <= kLastPause)
    {
        return tables::kPauseLengths[event.code - kFirstPause];
    }

    switch (event.code)
    {
    case kWaitAgain:
        return state.previous_wait;
    case kWaitChange:
        return state.previous_wait + static_cast<uint32_t>(static_cast<int8_t>(a.at(0)));
    case kWait1Byte:
        return a.at(0);
    case kWait2Bytes:
        return xsflib::LoadLe16(a.data());
    default:
        return static_cast<uint32_t>(xsflib::LoadLe16(a.data())) + (uint32_t{a.at(2)} << 16);
    }
}

Command DecodeEvent(const Event& event, TrackState& state)
{
    if (event.code < kFirstPause)
    {
        return DecodeNote(event, state);
    }

    Command command;
    if (event.code <= kWait3Bytes)
    {
        const uint32_t ticks = WaitTicks(event, state);
        state.previous_wait = ticks;
        command.kind = Kind::kWait;
        command.value.value = commands::ToUnit(ticks, ScaleOf(Kind::kWait));
        if (NewWaitCode(ticks) != event.code)
        {
            command.encoding.opcode = event.code;
        }
        return command;
    }

    if (event.code == kEndTrack || ArgumentBytes(event.code, 0) < 0)
    {
        command.kind = Kind::kEnd;
        if (event.code != kEndTrack)
        {
            command.encoding.opcode = event.code;
        }
        return command;
    }

    if (event.code == kRepeatEnd)
    {
        command.kind = Kind::kLoopEnd;
        return command;
    }

    if (const std::optional<Kind> kind = ByteKindOf(event.code))
    {
        command.kind = *kind;
        state.octave_set = state.octave_set && *kind != Kind::kLoopStart;
        command.value.value = commands::ToUnit(event.arguments.at(0), ScaleOf(*kind));
        if (ByteCodeOf(*kind) != event.code)
        {
            command.encoding.opcode = event.code;
        }
        return command;
    }

    FollowOctave(state, event.code, event.arguments);
    command.kind = Kind::kOther;
    command.encoding.opcode = event.code;
    command.bytes = event.arguments;

    return command;
}

bool StartsTrack(const Command& command)
{
    return command.kind == Kind::kData && command.bytes.size() == kTrackStart &&
           std::equal(kTrackLabel.begin(), kTrackLabel.end(), command.bytes.begin());
}

// A value of a command, in a range of the driver's values.
uint32_t ValueOf(const Command& command, uint32_t max)
{
    const int64_t value = commands::FromUnit(command.value.value, ScaleOf(command.kind));
    if (value < 0 || value > int64_t{max})
    {
        throw std::invalid_argument("A DSE track can't hold the value " + std::to_string(value) + " of a command of " +
                                    std::string(commands::Name(command.kind)) + ".");
    }

    return static_cast<uint32_t>(value);
}

void AppendNote(std::vector<uint8_t>& out, const Command& command, TrackState& state)
{
    const int64_t velocity = commands::FromUnit(command.velocity, Scale{});
    if (velocity < 0 || velocity >= kFirstPause)
    {
        throw std::invalid_argument("A DSE track can't hold a note of velocity " + std::to_string(velocity) + ".");
    }

    // A new note that starts a loop or a repeat sets its octave. The tools write the same. Otherwise it would play
    // another octave on each pass.
    const std::optional<uint8_t> kept = command.encoding.arguments;
    const bool keeps = kept && KeyOf(state.octave, *kept & kLowBits) == command.key;
    std::optional<uint8_t> low_bits =
        keeps ? std::optional<uint8_t>(*kept & kLowBits) : NewLowBits(state.octave, command.key);
    if (!low_bits || (!keeps && !state.octave_set))
    {
        const uint8_t octave = command.key / 12;
        out.push_back(kOctave);
        out.push_back(octave);
        state.octave = octave;
        low_bits = NewLowBits(octave, command.key);
    }
    state.octave_set = true;

    const uint32_t length = ValueOf(command, kMaxLength);
    int count = 0;
    if (!command.encoding.repeated || length != state.previous_length)
    {
        count = std::max<int>(LengthBytes(length), std::min<int>(command.encoding.value_bytes, 3));
        state.previous_length = length;
    }

    out.push_back(static_cast<uint8_t>(velocity));
    out.push_back(static_cast<uint8_t>(count << 6 | *low_bits));
    for (int i = count - 1; i >= 0; i--)
    {
        out.push_back(static_cast<uint8_t>(length >> (8 * i)));
    }
    state.octave = OctaveAfter(state.octave, *low_bits);
}

void AppendWait(std::vector<uint8_t>& out, const Command& command, TrackState& state)
{
    const uint32_t ticks = ValueOf(command, 0xFFFFFFFF);
    const uint16_t kept = command.encoding.opcode;
    std::optional<uint8_t> code =
        WaitCodeGives(kept, ticks, state.previous_wait) ? static_cast<uint8_t>(kept) : NewWaitCode(ticks);
    if (!code)
    {
        throw std::invalid_argument("A DSE track can't hold a wait of " + std::to_string(ticks) + " ticks.");
    }

    out.push_back(*code);
    switch (*code)
    {
    case kWaitChange:
        out.push_back(static_cast<uint8_t>(ticks - state.previous_wait));
        break;
    case kWait1Byte:
        out.push_back(static_cast<uint8_t>(ticks));
        break;
    case kWait3Bytes:
        xsflib::AppendLe16(out, static_cast<uint16_t>(ticks));
        out.push_back(static_cast<uint8_t>(ticks >> 16));
        break;
    case kWait2Bytes:
        xsflib::AppendLe16(out, static_cast<uint16_t>(ticks));
        break;
    default:
        break;
    }
    state.previous_wait = ticks;
}

void AppendOther(std::vector<uint8_t>& out, const Command& command, TrackState& state)
{
    const uint16_t code = command.encoding.opcode;
    if (code < kWaitAgain || code > 0xFF ||
        ArgumentBytes(static_cast<uint8_t>(code), 0) != static_cast<int>(command.bytes.size()))
    {
        throw std::invalid_argument("A DSE track can't hold a command of code " + std::to_string(code) + " with " +
                                    std::to_string(command.bytes.size()) + " bytes of arguments.");
    }

    out.push_back(static_cast<uint8_t>(code));
    out.insert(out.end(), command.bytes.begin(), command.bytes.end());
    FollowOctave(state, code, command.bytes);
}

void AppendCommand(std::vector<uint8_t>& out, const Command& command, TrackState& state)
{
    if (command.conditional || command.time || command.value.source != commands::Source::kValue)
    {
        throw std::invalid_argument("A DSE track's commands take no conditions, times, random numbers or variables.");
    }

    switch (command.kind)
    {
    case Kind::kNote:
        AppendNote(out, command, state);
        return;
    case Kind::kWait:
        AppendWait(out, command, state);
        return;
    case Kind::kOther:
        AppendOther(out, command, state);
        return;
    case Kind::kEnd:
        out.push_back(command.encoding.opcode != 0 ? static_cast<uint8_t>(command.encoding.opcode) : kEndTrack);
        return;
    case Kind::kLoopEnd:
        out.push_back(kRepeatEnd);
        return;
    default:
        break;
    }

    const std::optional<uint8_t> code = ByteCodeOf(command.kind);
    if (!code)
    {
        throw std::invalid_argument("A DSE track can't hold a command of " + std::string(commands::Name(command.kind)) +
                                    ".");
    }
    const auto kept = command.encoding.opcode;
    out.push_back(kept != 0 && ByteKindOf(static_cast<uint8_t>(kept)) == command.kind ? static_cast<uint8_t>(kept)
                                                                                      : *code);
    out.push_back(static_cast<uint8_t>(ValueOf(command, 0xFF)));
    state.octave_set = state.octave_set && command.kind != Kind::kLoopStart;
}

} // namespace

Command TrackOpening(uint8_t number, uint8_t channel)
{
    Command start;
    start.kind = Kind::kData;
    start.bytes.assign(kTrackStart, 0);
    std::ranges::copy(kTrackLabel, start.bytes.begin());
    start.bytes[kChunkHeader] = number;
    start.bytes[kChunkHeader + 1] = channel;
    return start;
}

std::optional<TrackStart> ReadTrackOpening(const Command& command)
{
    if (!StartsTrack(command))
    {
        return std::nullopt;
    }

    return TrackStart{.number = command.bytes[kChunkHeader], .channel = command.bytes[kChunkHeader + 1]};
}

void SetTrackOpening(Command& opening, const TrackStart& start)
{
    if (!StartsTrack(opening))
    {
        throw std::invalid_argument("The item doesn't start a DSE track.");
    }

    opening.bytes[kChunkHeader] = start.number;
    opening.bytes[kChunkHeader + 1] = start.channel;
}

commands::Sequence DecodeTracks(std::span<const Track> tracks)
{
    commands::Sequence sequence;
    for (const Track& track : tracks)
    {
        Command start = TrackOpening(track.number, track.channel);
        start.bytes[kChunkHeader + 2] = track.reserved[0];
        start.bytes[kChunkHeader + 3] = track.reserved[1];
        sequence.items.push_back(std::move(start));

        TrackState state;
        for (const Event& event : track.events)
        {
            sequence.items.push_back(DecodeEvent(event, state));
        }
        if (track.end_in_padding)
        {
            sequence.items.back().encoding.value_bytes = 1;
        }
        if (!track.unread.empty())
        {
            Command unread;
            unread.kind = Kind::kData;
            unread.bytes = track.unread;
            sequence.items.push_back(std::move(unread));
        }
    }

    return sequence;
}

Encoded EncodeTracks(const commands::Sequence& sequence)
{
    Encoded encoded;
    std::vector<uint8_t>& out = encoded.bytes;
    encoded.offsets.reserve(sequence.items.size() + 1);

    std::optional<std::size_t> chunk;
    bool end_in_padding = false;
    bool unread = false;
    TrackState state;
    const auto close_chunk = [&]
    {
        if (!chunk)
        {
            return;
        }

        std::size_t size = out.size() - (*chunk + kChunkHeader);
        if (end_in_padding && !unread && size % 4 != 1)
        {
            size--;
        }
        xsflib::StoreLe32(&out[*chunk + kSizeField], static_cast<uint32_t>(size));
        while (out.size() % 4 != 0)
        {
            out.push_back(kEndTrack);
        }
    };

    for (const Command& command : sequence.items)
    {
        if (StartsTrack(command))
        {
            close_chunk();
            encoded.offsets.push_back(static_cast<uint32_t>(out.size()));
            chunk = out.size();
            end_in_padding = false;
            unread = false;
            state = {};
            out.insert(out.end(), command.bytes.begin(), command.bytes.end());
            continue;
        }

        if (!chunk)
        {
            throw std::invalid_argument("A DSE sequence starts with a track.");
        }

        encoded.offsets.push_back(static_cast<uint32_t>(out.size()));
        if (command.kind == Kind::kData)
        {
            out.insert(out.end(), command.bytes.begin(), command.bytes.end());
            unread = !command.bytes.empty();
            continue;
        }

        AppendCommand(out, command, state);
        if (command.kind == Kind::kNote && out[encoded.offsets.back()] == kOctave)
        {
            encoded.offsets.back() += 2;
        }
        end_in_padding = command.kind == Kind::kEnd && command.encoding.value_bytes == 1;
    }
    close_chunk();
    encoded.offsets.push_back(static_cast<uint32_t>(out.size()));

    return encoded;
}

Scale ScaleOf(Kind kind)
{
    switch (kind)
    {
    case Kind::kPan:
        return {kOne, -64 * kOne}; // the driver's centre is 64
    case Kind::kBendRange:
        return {100 * kOne, 0}; // semitones
    default:
        return {};
    }
}

} // namespace ntdlib::dse
