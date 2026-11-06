// SPDX-License-Identifier: MIT

#include "ntdlib/ntr/snd/commands.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"

namespace ntdlib::ntr::snd
{

namespace
{

using commands::Argument;
using commands::Command;
using commands::Kind;
using commands::Source;
using detail::Width;

constexpr uint8_t kAllocateTracks = 0xFE;

// The largest offset that a jump, a call or a track start can hold: 24 bits.
constexpr uint32_t kMaxOffset = 0xFFFFFF;

// The kinds of the opcodes from 0xC0 to 0xD7.
constexpr Kind kByteKinds[] = {
    Kind::kPan,      Kind::kVolume,   Kind::kMainVolume, Kind::kTranspose,      Kind::kPitchBend,     Kind::kBendRange,
    Kind::kPriority, Kind::kNoteWait, Kind::kTie,        Kind::kPortamentoFrom, Kind::kLfoDepth,      Kind::kLfoSpeed,
    Kind::kLfoType,  Kind::kLfoRange, Kind::kPortamento, Kind::kPortamentoTime, Kind::kAttack,        Kind::kDecay,
    Kind::kSustain,  Kind::kRelease,  Kind::kLoopStart,  Kind::kVolume2,        Kind::kPrintVariable, Kind::kMute,
};

// The opcode of `kind` with `index`, or nothing if the bytecode has none. Notes, kOther and kData have no single
// opcode.
std::optional<uint8_t> OpcodeOf(Kind kind, uint8_t index)
{
    for (std::size_t i = 0; i < std::size(kByteKinds); i++)
    {
        if (kByteKinds[i] == kind)
        {
            const bool lfo =
                kind == Kind::kLfoDepth || kind == Kind::kLfoSpeed || kind == Kind::kLfoType || kind == Kind::kLfoRange;
            if (lfo && index != 0)
            {
                return std::nullopt; // the DS has one LFO
            }
            return static_cast<uint8_t>(0xC0 + i);
        }
    }
    if (kind >= Kind::kSetVariable && kind <= Kind::kRandomVariable)
    {
        return static_cast<uint8_t>(0xB0 + (static_cast<int>(kind) - static_cast<int>(Kind::kSetVariable)));
    }
    if (kind >= Kind::kCompareEqual && kind <= Kind::kCompareNotEqual)
    {
        return static_cast<uint8_t>(0xB8 + (static_cast<int>(kind) - static_cast<int>(Kind::kCompareEqual)));
    }

    switch (kind)
    {
    case Kind::kWait:
        return 0x80;
    case Kind::kProgram:
        return 0x81;
    case Kind::kOpenTrack:
        return 0x93;
    case Kind::kJump:
        return 0x94;
    case Kind::kCall:
        return 0x95;
    case Kind::kLfoDelay:
        return index == 0 ? std::optional<uint8_t>(0xE0) : std::nullopt;
    case Kind::kTempo:
        return 0xE1;
    case Kind::kSweepPitch:
        return 0xE3;
    case Kind::kLoopEnd:
        return 0xFC;
    case Kind::kReturn:
        return 0xFD;
    case Kind::kAllocateTracks:
        return kAllocateTracks;
    case Kind::kEnd:
        return 0xFF;
    default:
        return std::nullopt;
    }
}

uint8_t VariableLengthBytes(uint32_t value)
{
    uint8_t bytes = 1;
    while (value >= 0x80)
    {
        value >>= 7;
        bytes++;
    }

    return bytes;
}

// The command at `at` of `data` as the driver reads it in a track. The bytes past the end of `data` read as 0, and
// `next` then lies past the end.
PlayedCommand ReadInTrack(std::span<const uint8_t> data, uint32_t at)
{
    const auto byte_at = [data](uint32_t address)
    {
        return address < data.size() ? data[address] : uint8_t{0};
    };
    return ReadPlayedCommand(byte_at, at);
}

// Whether a sequence at `at` starts with an allocation of tracks that fits in `data`.
bool StartsWithAllocation(std::span<const uint8_t> data, uint32_t at)
{
    return at + 3 <= data.size() && data[at] == kAllocateTracks;
}

// The allocation of tracks at `at`, with the console's value.
Command AllocationAt(std::span<const uint8_t> data, uint32_t at)
{
    Command command;
    command.kind = Kind::kAllocateTracks;
    command.value.value = data[at + 1] | data[at + 2] << 8;
    return command;
}

void Append16(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

// Appends `value` as a variable-length number in `bytes` bytes, or in the bytes that it requires where those are more.
void AppendVariableLength(std::vector<uint8_t>& out, uint32_t value, uint8_t bytes)
{
    bytes = std::max(bytes, VariableLengthBytes(value));
    for (int i = bytes - 1; i >= 0; i--)
    {
        const auto group = static_cast<uint8_t>(i >= 5 ? 0 : (value >> (7 * i)) & 0x7F);
        out.push_back(static_cast<uint8_t>(group | (i != 0 ? 0x80 : 0)));
    }
}

// The opcode that encodes `command`.
uint8_t CodeOf(const Command& command)
{
    if (command.kind == Kind::kNote)
    {
        if (command.key >= 0x80)
        {
            throw std::invalid_argument("a note's key above 127");
        }

        return command.key;
    }
    if (command.kind == Kind::kOther)
    {
        if (command.encoding.opcode < 0x80 || command.encoding.opcode > 0xFF)
        {
            throw std::invalid_argument("no opcode " + std::to_string(command.encoding.opcode));
        }

        return static_cast<uint8_t>(command.encoding.opcode);
    }

    const std::optional<uint8_t> code = OpcodeOf(command.kind, command.index);
    if (!code)
    {
        throw std::invalid_argument("no opcode for " + std::string(commands::Name(command.kind)) + " " +
                                    std::to_string(command.index));
    }

    return *code;
}

// Appends the bytes of `command`, whose target lies at byte offset `target`. Its values are the console's.
void AppendCommand(std::vector<uint8_t>& out, const Command& command, uint32_t target)
{
    if (command.kind == Kind::kData)
    {
        out.insert(out.end(), command.bytes.begin(), command.bytes.end());
        return;
    }
    if (command.kind == Kind::kAllocateTracks)
    {
        if (command.conditional || command.value.source != Source::kValue || command.encoding.unused_random ||
            command.encoding.unused_variable)
        {
            throw std::invalid_argument("an allocation of tracks takes no prefix");
        }

        out.push_back(kAllocateTracks);
        Append16(out, static_cast<uint32_t>(command.value.value));
        return;
    }

    const uint8_t code = CodeOf(command);
    if (!detail::TakesPrefixes(code) && command.value.source != Source::kValue)
    {
        throw std::invalid_argument(std::string(commands::Name(command.kind)) + " takes no random or variable value");
    }
    if (command.time)
    {
        throw std::invalid_argument(std::string(commands::Name(command.kind)) + " takes no time");
    }

    // The prefixes, in the order the driver reads them.
    if (command.conditional)
    {
        out.push_back(detail::kIfPrefix);
    }
    if (command.value.source == Source::kRandom || command.encoding.unused_random)
    {
        out.push_back(detail::kRandomPrefix);
    }
    if (command.value.source == Source::kVariable || command.encoding.unused_variable)
    {
        out.push_back(detail::kVariablePrefix);
    }

    out.push_back(code);
    if (command.kind == Kind::kNote)
    {
        out.push_back(static_cast<uint8_t>(command.velocity));
    }
    if (detail::TakesIndex(code))
    {
        out.push_back(command.index);
    }

    if (detail::TakesTarget(code))
    {
        if (target > kMaxOffset)
        {
            throw std::invalid_argument("a target past 24 bits");
        }

        out.push_back(static_cast<uint8_t>(target));
        Append16(out, target >> 8);
        return;
    }

    const Argument& argument = command.value;
    switch (argument.source)
    {
    case Source::kVariable:
        out.push_back(static_cast<uint8_t>(argument.value));
        return;
    case Source::kRandom:
        Append16(out, static_cast<uint32_t>(argument.value));
        Append16(out, static_cast<uint32_t>(argument.high));
        return;
    case Source::kValue:
        break;
    }

    switch (detail::WidthOf(code))
    {
    case Width::kByte:
        out.push_back(static_cast<uint8_t>(argument.value));
        break;
    case Width::kSigned16:
        Append16(out, static_cast<uint32_t>(argument.value));
        break;
    case Width::kVariableLength:
        AppendVariableLength(out, static_cast<uint32_t>(argument.value), command.encoding.value_bytes);
        break;
    case Width::kNone:
        break;
    }
}

// A conversion of a value by its scale: ToUnit, from the console's values to the units, or FromUnit, back to the
// nearest console values.
using Conversion = int64_t (*)(int64_t, const commands::Scale&);

// Converts `argument` with `convert`. A variable's number stays as it is.
void Convert(Argument& argument, const commands::Scale& scale, Conversion convert)
{
    if (argument.source == Source::kVariable)
    {
        return;
    }

    argument.value = convert(argument.value, scale);
    if (argument.source == Source::kRandom)
    {
        argument.high = convert(argument.high, scale);
    }
}

void Convert(Command& command, Conversion convert)
{
    if (command.kind == Kind::kData)
    {
        return;
    }

    command.velocity = convert(command.velocity, commands::Scale{});
    Convert(command.value, ScaleOf(command.kind), convert);
}

// Whether the bytes of `command` hold its value as a variable-length number, without a random or a variable prefix: a
// note's length, a wait, a program, and the other opcodes from 0x82 to 0x8F.
bool HoldsVariableLength(const Command& command)
{
    return command.value.source == Source::kValue && command.kind != Kind::kAllocateTracks &&
           detail::WidthOf(CodeOf(command)) == Width::kVariableLength;
}

// Whether the driver reads the bytes from `start` to the end of `bytes` as `command`, whose target lies at byte offset
// `target`. The command's values are the console's. An allocation of tracks reads as it does at a sequence's entry.
bool ReadsBack(std::span<const uint8_t> bytes, uint32_t start, const Command& command, uint32_t target)
{
    if (command.kind == Kind::kAllocateTracks)
    {
        return StartsWithAllocation(bytes, start) && start + 3 == bytes.size() && AllocationAt(bytes, start) == command;
    }

    const PlayedCommand played = ReadInTrack(bytes, start);
    Command read = played.command;
    if (commands::HasTarget(read.kind) && read.target == target)
    {
        read.target = command.target;
        read.target_outside = command.target_outside;
    }

    // A value that requires as many bytes as its padding or more takes the bytes that it requires
    // (AppendVariableLength). Its bytes then read back without padding.
    const uint8_t padding = command.encoding.value_bytes;
    if (read.encoding.value_bytes == 0 && padding != 0 && HoldsVariableLength(command) &&
        padding <= VariableLengthBytes(static_cast<uint32_t>(command.value.value)))
    {
        read.encoding.value_bytes = padding;
    }

    return played.next == bytes.size() && read == command;
}

// A command that the decoder read, at its offset.
struct Read
{
    Command command;
    uint32_t size = 0;
    bool raw = false; // the encoder would write other bytes: the item keeps the bytes
};

// What covers each byte: nothing yet, a command's first byte, or one of its other bytes.
enum class Cover : uint8_t
{
    kFree,
    kStart,
    kInside,
};

// The commands that the decoder has found, and the bytes they cover.
struct Found
{
    std::map<uint32_t, Read> reads;
    std::vector<Cover> cover;
};

// The command at `at`, or nothing where it runs past the end of `data`. At a sequence's entry, an allocation of tracks
// comes first.
std::optional<Read> ReadAt(std::span<const uint8_t> data, uint32_t at, bool entry)
{
    Read read;
    if (entry && StartsWithAllocation(data, at))
    {
        read.command = AllocationAt(data, at);
        read.size = 3;
    }
    else
    {
        const PlayedCommand played = ReadInTrack(data, at);
        if (played.next > data.size())
        {
            return std::nullopt;
        }

        read.command = played.command;
        read.size = played.next - at;
    }

    std::vector<uint8_t> again;
    AppendCommand(again, read.command, read.command.target);
    read.raw = !std::equal(again.begin(), again.end(), data.begin() + at, data.begin() + at + read.size);
    Convert(read.command, commands::ToUnit);

    return read;
}

bool Free(const Found& found, uint32_t at, uint32_t size)
{
    const auto start = found.cover.begin() + at;
    return std::all_of(start, start + size, [](Cover c) { return c == Cover::kFree; });
}

void Mark(Found& found, uint32_t at, Read read)
{
    found.cover[at] = Cover::kStart;
    std::fill(found.cover.begin() + at + 1, found.cover.begin() + at + read.size, Cover::kInside);
    found.reads.emplace(at, std::move(read));
}

// Follows the commands from each of `starts`, the labels and the entries, through every jump, call and track start.
// Returns false where a command overlaps another, or where the flow leads inside a command.
bool Follow(std::span<const uint8_t> data, const std::set<uint32_t>& starts, Found& found)
{
    std::vector<uint32_t> pending(starts.begin(), starts.end());
    while (!pending.empty())
    {
        uint32_t at = pending.back();
        pending.pop_back();
        bool entry = starts.contains(at);
        while (at < data.size() && found.cover[at] == Cover::kFree)
        {
            std::optional<Read> read = ReadAt(data, at, entry);
            entry = false;
            if (!read)
            {
                break; // the rest stays data
            }
            if (!Free(found, at, read->size))
            {
                return false;
            }

            if (commands::HasTarget(read->command.kind))
            {
                pending.push_back(read->command.target);
            }

            const bool next = FallsThrough(read->command);
            const uint32_t size = read->size;
            Mark(found, at, std::move(*read));
            if (!next)
            {
                break;
            }

            at += size;
        }
        if (at < data.size() && found.cover[at] == Cover::kInside)
        {
            return false;
        }
    }

    return true;
}

// Follows the sequences that no entry names: runs of bytes that start with an allocation of tracks where no command
// covers the byte before. A sequence archive keeps such a sequence when the tools take its entry out. Each one counts
// as an entry where its commands follow without overlapping others, and every flow inside it leads to a command's
// start. Adds the starts that count to `starts`.
void FollowOrphans(std::span<const uint8_t> data, std::set<uint32_t>& starts, Found& found)
{
    for (uint32_t at = 0; at < data.size(); at++)
    {
        const bool after_command = at == 0 || found.cover[at - 1] != Cover::kFree;
        if (found.cover[at] != Cover::kFree || !after_command || !StartsWithAllocation(data, at))
        {
            continue;
        }

        Found tried = found;
        if (!Follow(data, {at}, tried))
        {
            continue;
        }
        // The new commands' targets.
        const auto leads_inside = [&](const auto& read)
        {
            const Command& command = read.second.command;
            return !found.reads.contains(read.first) && commands::HasTarget(command.kind) &&
                   command.target < data.size() && tried.cover[command.target] != Cover::kStart;
        };
        if (std::any_of(tried.reads.begin(), tried.reads.end(), leads_inside))
        {
            continue;
        }

        found = std::move(tried);
        starts.insert(at);
    }
}

// Reads the commands in the bytes that the driver never reaches, such as the end after a track's last jump or a track
// that no command starts. A run of them counts from the start of the bytes that no command covers, up to an end, a jump
// or a return, or up to a command that the driver reaches. More runs may follow it, such as a subroutine that the
// first one calls. The most runs whose targets all lead to items count. Bytes that no such run covers stay as data.
void Guess(std::span<const uint8_t> data, const std::set<uint32_t>& cuts, Found& found)
{
    uint32_t at = 0;
    while (at < data.size())
    {
        if (found.cover[at] == Cover::kStart)
        {
            at += found.reads.at(at).size;
            continue;
        }

        // The bytes that no command covers, up to the next command or cut.
        uint32_t end = at + 1;
        while (end < data.size() && found.cover[end] == Cover::kFree && !cuts.contains(end))
        {
            end++;
        }

        // Runs of commands one after another, each up to a command that ends it.
        std::vector<std::pair<uint32_t, Read>> run;
        std::vector<std::size_t> run_ends; // the commands up to each run's end
        uint32_t next = at;
        while (next < end)
        {
            std::optional<Read> read = ReadAt(data, next, false);
            if (!read || read->raw || next + read->size > end)
            {
                break;
            }

            const uint32_t size = read->size;
            const bool counts = !FallsThrough(read->command) ||
                                (next + size == end && end < data.size() && found.cover[end] == Cover::kStart);
            run.emplace_back(next, std::move(*read));
            next += size;
            if (counts)
            {
                run_ends.push_back(run.size());
            }
        }

        // A target leads to a command, a cut, the end, or a command of the runs taken.
        std::size_t taken = 0;
        for (auto count = run_ends.rbegin(); count != run_ends.rend() && taken == 0; ++count)
        {
            const auto first = run.begin();
            const auto last = run.begin() + static_cast<std::ptrdiff_t>(*count);
            const auto leads = [&](uint32_t target)
            {
                return target >= data.size() || found.cover[target] == Cover::kStart || cuts.contains(target) ||
                       std::any_of(first, last, [&](const auto& read) { return read.first == target; });
            };
            const auto target_leads = [&](const auto& read)
            {
                const Command& command = read.second.command;
                return !commands::HasTarget(command.kind) || leads(command.target);
            };
            if (std::all_of(first, last, target_leads))
            {
                taken = *count;
            }
        }
        if (taken == 0)
        {
            at = end;
            continue;
        }

        at = run[taken - 1].first + run[taken - 1].second.size;
        for (std::size_t i = 0; i < taken; i++)
        {
            Mark(found, run[i].first, std::move(run[i].second));
        }
    }
}

// The items of `data` in the order of their bytes, from the commands in `found`. Data splits at `cuts`. Gives the item
// at each offset where one starts, and at the end. Without commands, the items are the whole bytecode as data, split at
// `cuts`.
commands::Sequence Items(std::span<const uint8_t> data, const Found& found, const std::set<uint32_t>& cuts,
                         std::map<uint32_t, uint32_t>& items)
{
    commands::Sequence sequence;
    uint32_t at = 0;
    while (at < data.size())
    {
        items[at] = static_cast<uint32_t>(sequence.items.size());
        const auto read = found.reads.find(at);
        if (read != found.reads.end() && !read->second.raw)
        {
            sequence.items.push_back(read->second.command);
            at += read->second.size;
            continue;
        }

        // A run of data up to the next command, cut or end. A raw command stands alone.
        uint32_t end = read != found.reads.end() ? at + read->second.size : static_cast<uint32_t>(data.size());
        if (read == found.reads.end())
        {
            const auto next_read = found.reads.upper_bound(at);
            if (next_read != found.reads.end())
            {
                end = std::min(end, next_read->first);
            }

            const auto next_cut = cuts.upper_bound(at);
            if (next_cut != cuts.end())
            {
                end = std::min(end, *next_cut);
            }
        }

        Command item;
        item.kind = Kind::kData;
        item.bytes.assign(data.begin() + at, data.begin() + end);
        sequence.items.push_back(std::move(item));
        at = end;
    }
    items[static_cast<uint32_t>(data.size())] = static_cast<uint32_t>(sequence.items.size());

    // The targets by item.
    for (Command& command : sequence.items)
    {
        if (!commands::HasTarget(command.kind))
        {
            continue;
        }

        const auto item = items.find(command.target);
        if (item == items.end())
        {
            command.target_outside = true; // past the end: the offset stays
            continue;
        }

        command.target = item->second;
    }

    return sequence;
}

} // namespace

namespace detail
{

Kind KindOf(uint8_t code)
{
    if (code >= 0xC0 && code < 0xC0 + std::size(kByteKinds))
    {
        return kByteKinds[code - 0xC0];
    }
    if (code >= 0xB0 && code <= 0xB6)
    {
        return static_cast<Kind>(static_cast<int>(Kind::kSetVariable) + (code - 0xB0));
    }
    if (code >= 0xB8 && code <= 0xBD)
    {
        return static_cast<Kind>(static_cast<int>(Kind::kCompareEqual) + (code - 0xB8));
    }

    switch (code)
    {
    case 0x80:
        return Kind::kWait;
    case 0x81:
        return Kind::kProgram;
    case 0x93:
        return Kind::kOpenTrack;
    case 0x94:
        return Kind::kJump;
    case 0x95:
        return Kind::kCall;
    case 0xE0:
        return Kind::kLfoDelay;
    case 0xE1:
        return Kind::kTempo;
    case 0xE3:
        return Kind::kSweepPitch;
    case 0xFC:
        return Kind::kLoopEnd;
    case 0xFD:
        return Kind::kReturn;
    case 0xFF:
        return Kind::kEnd;
    default:
        return Kind::kOther;
    }
}

} // namespace detail

commands::Scale ScaleOf(Kind kind)
{
    using commands::kOne;
    switch (kind)
    {
    case Kind::kPan:
        return {kOne, -64 * kOne}; // the driver takes 64 off
    case Kind::kTranspose:
    case Kind::kBendRange:
        return {100 * kOne, 0}; // semitones
    case Kind::kSweepPitch:
        return {100 * kOne / 64, 0}; // 64ths of a semitone
    case Kind::kPitchBend:
        return {kOne / 128, 0};
    case Kind::kLfoDepth:
        return {kOne * 127 / 16384, 0}; // the sine's peak of 127, times the depth and the range, over 2^14
    case Kind::kLfoSpeed:
        return {24570, 0}; // 1/512 of the driver's frames a second: 0.37491 Hz
    case Kind::kLfoDelay:
        return {341412, 0}; // a frame of the driver: 5.20953 ms
    default:
        return {};
    }
}

Decoded DecodeSequence(std::span<const uint8_t> data, std::span<const LabelOffset> labels,
                       std::span<const uint32_t> entries)
{
    // Every label and entry starts an item, and so does every target.
    std::set<uint32_t> cuts;
    for (const LabelOffset& label : labels)
    {
        if (label.offset > data.size())
        {
            throw std::invalid_argument("a label past the end of the sequence: " + label.name);
        }

        cuts.insert(label.offset);
    }
    for (const uint32_t entry : entries)
    {
        if (entry > data.size())
        {
            throw std::invalid_argument("an entry past the end of the sequence: " + std::to_string(entry));
        }

        cuts.insert(entry);
    }

    const std::set<uint32_t> entries_and_labels = cuts;
    std::set<uint32_t> starts = cuts;

    Found found;
    found.cover.assign(data.size(), Cover::kFree);
    bool opaque = !Follow(data, starts, found);
    if (!opaque)
    {
        FollowOrphans(data, starts, found);
        cuts.insert(starts.begin(), starts.end());
    }
    for (const auto& [at, read] : found.reads)
    {
        if (commands::HasTarget(read.command.kind))
        {
            cuts.insert(read.command.target);
        }
    }
    const auto inside = [&](uint32_t cut)
    {
        return cut < data.size() && found.cover[cut] == Cover::kInside;
    };
    opaque = opaque || std::any_of(cuts.begin(), cuts.end(), inside);

    // A track that jumps to an entry's allocation of tracks reads its bytes as other commands.
    for (const auto& [at, read] : found.reads)
    {
        if (commands::HasTarget(read.command.kind) && read.command.target < data.size())
        {
            const auto target = found.reads.find(read.command.target);
            opaque = opaque || (target != found.reads.end() && target->second.command.kind == Kind::kAllocateTracks);
        }
    }

    Decoded decoded;
    std::map<uint32_t, uint32_t> items; // byte offset to item
    if (opaque)
    {
        decoded.sequence = Items(data, Found{}, entries_and_labels, items);
        decoded.opaque = true;
    }
    else
    {
        Guess(data, cuts, found);
        decoded.sequence = Items(data, found, cuts, items);
    }

    for (const LabelOffset& label : labels)
    {
        decoded.sequence.labels.push_back({label.name, items.at(label.offset)});
    }
    for (const uint32_t entry : entries)
    {
        decoded.entries.push_back(items.at(entry));
    }

    return decoded;
}

std::optional<DecodedCommand> DecodeCommand(std::span<const uint8_t> data, uint32_t at)
{
    const std::optional<Read> read = ReadAt(data, at, false);
    if (!read)
    {
        return std::nullopt;
    }

    return DecodedCommand{read->command, read->size};
}

bool FallsThrough(const Command& command)
{
    switch (command.kind)
    {
    case Kind::kJump:
    case Kind::kReturn:
    case Kind::kEnd:
        return command.conditional;
    default:
        return true;
    }
}

Encoded EncodeSequence(const commands::Sequence& sequence)
{
    // The console's values nearest the units.
    std::vector<Command> items = sequence.items;
    for (Command& item : items)
    {
        Convert(item, commands::FromUnit);
    }

    // Each item's size doesn't depend on its target's offset. The sizes give the offsets.
    Encoded encoded;
    encoded.offsets.reserve(items.size() + 1);
    std::vector<uint8_t> scratch;
    uint32_t at = 0;
    for (const Command& item : items)
    {
        encoded.offsets.push_back(at);
        scratch.clear();
        AppendCommand(scratch, item, 0);
        at += static_cast<uint32_t>(scratch.size());
    }
    encoded.offsets.push_back(at);

    // Then the bytes, with each target's offset. The driver has to read each command's bytes back as the command.
    encoded.bytes.reserve(at);
    for (const Command& item : items)
    {
        uint32_t target = item.target;
        if (commands::HasTarget(item.kind) && !item.target_outside)
        {
            if (item.target >= encoded.offsets.size())
            {
                throw std::invalid_argument("a target past the items");
            }

            target = encoded.offsets[item.target];
        }

        const auto start = static_cast<uint32_t>(encoded.bytes.size());
        AppendCommand(encoded.bytes, item, target);
        if (item.kind != Kind::kData && !ReadsBack(encoded.bytes, start, item, target))
        {
            throw std::invalid_argument("the driver would read other commands from the bytes of " +
                                        std::string(commands::Name(item.kind)));
        }
    }

    return encoded;
}

} // namespace ntdlib::ntr::snd
