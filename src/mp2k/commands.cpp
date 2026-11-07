// SPDX-License-Identifier: MIT

#include "ntdlib/mp2k/commands.h"

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

namespace ntdlib::mp2k
{
namespace
{

using commands::Argument;
using commands::Command;
using commands::Kind;
using commands::Source;

// The kinds of the commands from 0xBA (PRIO) to 0xC8 (TUNE). The codes between them that end the track are kOther.
constexpr Kind kByteKinds[] = {
    Kind::kPriority, Kind::kTempo,     Kind::kTranspose, Kind::kProgram,  Kind::kVolume,
    Kind::kPan,      Kind::kPitchBend, Kind::kBendRange, Kind::kLfoSpeed, Kind::kLfoDelay,
    Kind::kLfoDepth, Kind::kLfoType,   Kind::kOther,     Kind::kOther,    Kind::kTune,
};

// The kinds of the extended commands, by their byte.
constexpr Kind kExtendedKinds[kXcmdCount] = {
    Kind::kEnd,       Kind::kSampleWave, Kind::kVoiceType, Kind::kEnd,          Kind::kAttack,
    Kind::kDecay,     Kind::kSustain,    Kind::kRelease,   Kind::kEchoVolume,   Kind::kEchoLength,
    Kind::kPsgLength, Kind::kSweep,      Kind::kWait,      Kind::kSampleOffset,
};

// The number of length codes: TIE's 0, then N01 to N96.
constexpr int kLengthCodes = 49;

// The bytes that a note and an end of a tie hold at most after their code, where a note reads up to `note_arguments`.
int MostArguments(Kind kind, int note_arguments)
{
    return kind == Kind::kNoteOff ? 1 : note_arguments;
}

// The length code of the longest length up to `length`.
int UsualLengthCode(int64_t length)
{
    int code = 0;
    for (int i = 0; i < kLengthCodes && ClockLength(i) <= length; i++)
    {
        code = i;
    }

    return code;
}

// The code of the wait of `length` ticks, or nothing where no code's length is `length`.
std::optional<uint8_t> WaitCode(int64_t length)
{
    for (int i = 0; i < kLengthCodes; i++)
    {
        if (ClockLength(i) == length)
        {
            return static_cast<uint8_t>(kCmdWait + i);
        }
    }

    return std::nullopt;
}

// The extended command's byte of `kind`, or nothing for a kind that no extended command stands for.
std::optional<uint8_t> ExtendedOf(Kind kind)
{
    for (uint8_t x = 0; x < kXcmdCount; x++)
    {
        if (kExtendedKinds[x] == kind && kind != Kind::kEnd && kind != Kind::kWait)
        {
            return x;
        }
    }

    return std::nullopt;
}

// The code of a command of a kind of kByteKinds, or nothing.
std::optional<uint8_t> ByteCodeOf(Kind kind)
{
    for (std::size_t i = 0; i < std::size(kByteKinds); i++)
    {
        if (kByteKinds[i] == kind && kind != Kind::kOther)
        {
            return static_cast<uint8_t>(kCmdPrio + i);
        }
    }

    return std::nullopt;
}

void Append32(std::vector<uint8_t>& out, uint32_t value)
{
    for (int i = 0; i < 4; i++)
    {
        out.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
}

// `value`, which has to lie from `low` to `high` for a command of `kind`.
int64_t Checked(int64_t value, int64_t low, int64_t high, Kind kind)
{
    if (value < low || value > high)
    {
        throw std::invalid_argument(std::string(commands::Name(kind)) + " " + std::to_string(value) +
                                    " lies outside the range that its bytes hold");
    }

    return value;
}

uint8_t Byte(int64_t value, int64_t low, int64_t high, Kind kind)
{
    return static_cast<uint8_t>(Checked(value, low, high, kind));
}

// The code byte of `command`, which its bytes leave out where it repeats the running status.
uint8_t CodeOf(const Command& command)
{
    switch (command.kind)
    {
    case Kind::kNote:
        {
            const int code =
                command.encoding.length_code ? *command.encoding.length_code : UsualLengthCode(command.value.value);
            if (code >= kLengthCodes)
            {
                throw std::invalid_argument("a note's length code past 48");
            }

            return static_cast<uint8_t>(kCmdTie + code);
        }
    case Kind::kNoteOff:
        return kCmdEot;
    case Kind::kWait:
        if (command.encoding.opcode == (kCmdXcmd << 8 | kXcmdWait))
        {
            return kCmdXcmd;
        }
        else if (const std::optional<uint8_t> code = WaitCode(command.value.value))
        {
            return *code;
        }
        throw std::invalid_argument("a wait of " + std::to_string(command.value.value) +
                                    " ticks, which no code's length gives");
    case Kind::kJump:
        return kCmdGoto;
    case Kind::kCall:
        return kCmdPatt;
    case Kind::kReturn:
        return kCmdPend;
    case Kind::kRepeat:
        return kCmdRept;
    case Kind::kSetVariable:
    case Kind::kAddVariable:
    case Kind::kSubtractVariable:
    case Kind::kJumpIfEqual:
    case Kind::kJumpIfNotEqual:
    case Kind::kJumpIfGreater:
    case Kind::kJumpIfGreaterOrEqual:
    case Kind::kJumpIfLessOrEqual:
    case Kind::kJumpIfLess:
        return kCmdMemAcc;
    case Kind::kRegister:
        return kCmdPort;
    case Kind::kEnd:
        {
            const uint16_t opcode = command.encoding.opcode;
            if (opcode == 0)
            {
                return kCmdFine;
            }
            if (opcode < 0x100 && EndsTrack(static_cast<uint8_t>(opcode)))
            {
                return static_cast<uint8_t>(opcode);
            }
            if (opcode >> 8 == kCmdXcmd && detail::KindOfExtended(static_cast<uint8_t>(opcode)) == Kind::kEnd)
            {
                return kCmdXcmd;
            }
            throw std::invalid_argument("no end has opcode " + std::to_string(opcode));
        }
    case Kind::kOther:
        if (command.encoding.opcode == kCmdMemAcc)
        {
            return kCmdMemAcc;
        }
        throw std::invalid_argument("no command of MP2K does nothing with opcode " +
                                    std::to_string(command.encoding.opcode));
    default:
        break;
    }

    if (const std::optional<uint8_t> code = ByteCodeOf(command.kind))
    {
        return *code;
    }
    if (ExtendedOf(command.kind))
    {
        return kCmdXcmd;
    }

    throw std::invalid_argument("MP2K has no command for " + std::string(commands::Name(command.kind)));
}

// Appends the bytes of `command`, whose target lies at address `target`, for a driver whose notes read up to
// `note_arguments` bytes. Its values are the console's.
void AppendCommand(std::vector<uint8_t>& out, const Command& command, uint32_t target, int note_arguments)
{
    if (command.kind == Kind::kData)
    {
        out.insert(out.end(), command.bytes.begin(), command.bytes.end());
        return;
    }

    const bool variable = command.value.source == Source::kVariable;
    const bool memacc_value = command.kind >= Kind::kSetVariable && command.kind <= Kind::kSubtractVariable;
    const bool memacc_jump = command.kind >= Kind::kJumpIfEqual && command.kind <= Kind::kJumpIfLess;
    if (command.conditional || command.time || command.value.source == Source::kRandom ||
        (variable && !memacc_value && !memacc_jump))
    {
        throw std::invalid_argument(std::string(commands::Name(command.kind)) +
                                    " takes no condition, time, random value or variable on MP2K");
    }
    const bool lfo = command.kind == Kind::kLfoSpeed || command.kind == Kind::kLfoDelay ||
                     command.kind == Kind::kLfoDepth || command.kind == Kind::kLfoType;
    if (lfo && command.index != 0)
    {
        throw std::invalid_argument("MP2K has one LFO");
    }

    const uint8_t code = CodeOf(command);
    if (command.encoding.repeated)
    {
        if (code < kCmdVoice)
        {
            throw std::invalid_argument(std::string(commands::Name(command.kind)) + " can't repeat the running status");
        }
    }
    else
    {
        out.push_back(code);
    }

    const int64_t value = command.value.value;
    switch (command.kind)
    {
    case Kind::kNote:
    case Kind::kNoteOff:
        {
            int extra = 0;
            if (command.kind == Kind::kNote)
            {
                extra = static_cast<int>(value - ClockLength(code - kCmdTie));
                if (extra < 0 || extra > 0x7F)
                {
                    throw std::invalid_argument("a note of " + std::to_string(value) +
                                                " ticks, which its length code can't give");
                }
                if (extra != 0 && note_arguments < 3)
                {
                    throw std::invalid_argument("a note of " + std::to_string(value) +
                                                " ticks, which no length code gives without an extra length");
                }
            }
            const int usual = command.kind == Kind::kNoteOff ? 1 : extra != 0 ? 3 : 2;
            const int count = command.encoding.arguments ? *command.encoding.arguments : usual;
            if (count > MostArguments(command.kind, note_arguments) ||
                (command.kind == Kind::kNote && extra != 0 && count < 3))
            {
                throw std::invalid_argument(std::string(commands::Name(command.kind)) + " with " +
                                            std::to_string(count) + " arguments");
            }
            if (count >= 1)
            {
                out.push_back(Byte(command.key, 0, 0x7F, command.kind));
            }
            if (count >= 2)
            {
                out.push_back(Byte(command.velocity, 0, 0x7F, command.kind));
            }
            if (count >= 3)
            {
                out.push_back(static_cast<uint8_t>(extra));
            }
            return;
        }
    case Kind::kWait:
        if (code == kCmdXcmd)
        {
            out.push_back(kXcmdWait);
            out.push_back(static_cast<uint8_t>(Checked(value, 0, 0xFFFF, command.kind)));
            out.push_back(static_cast<uint8_t>(value >> 8));
        }
        return;
    case Kind::kJump:
    case Kind::kCall:
        Append32(out, target);
        return;
    case Kind::kRepeat:
        out.push_back(Byte(value, 0, 0xFF, command.kind));
        Append32(out, target);
        return;
    case Kind::kRegister:
        out.push_back(command.index);
        out.push_back(Byte(value, 0, 0xFF, command.kind));
        return;
    case Kind::kOther:
        out.insert(out.end(), command.bytes.begin(), command.bytes.end());
        return;
    case Kind::kEnd:
        if (code == kCmdXcmd)
        {
            out.push_back(static_cast<uint8_t>(command.encoding.opcode));
        }
        return;
    case Kind::kReturn:
        return;
    default:
        break;
    }

    if (memacc_value || memacc_jump)
    {
        const int op =
            memacc_value
                ? static_cast<int>(command.kind) - static_cast<int>(Kind::kSetVariable) + (variable ? 3 : 0)
                : 6 + static_cast<int>(command.kind) - static_cast<int>(Kind::kJumpIfEqual) + (variable ? 6 : 0);
        out.push_back(static_cast<uint8_t>(op));
        out.push_back(command.index);
        out.push_back(Byte(value, 0, 0xFF, command.kind));
        if (memacc_jump)
        {
            Append32(out, target);
        }
        return;
    }

    if (code == kCmdXcmd)
    {
        const uint8_t x = *ExtendedOf(command.kind);
        out.push_back(x);
        switch (detail::kExtendedArguments[x])
        {
        case 1:
            out.push_back(Byte(value, 0, 0xFF, command.kind));
            break;
        case 4:
            Append32(out, static_cast<uint32_t>(Checked(value, 0, 0xFFFFFFFF, command.kind)));
            break;
        default:
            break;
        }
        return;
    }

    // The commands of a byte. A key shift is signed.
    out.push_back(command.kind == Kind::kTranspose ? static_cast<uint8_t>(Byte(value, -0x80, 0x7F, command.kind))
                                                   : Byte(value, 0, 0xFF, command.kind));
}

// A conversion of a value by its scale: ToUnit, from the console's values to the units, or FromUnit, back to the
// nearest console values.
using Conversion = int64_t (*)(int64_t, const commands::Scale&);

void Convert(Command& command, Conversion convert)
{
    if (command.kind == Kind::kData)
    {
        return;
    }

    command.velocity = convert(command.velocity, commands::Scale{});
    if (command.value.source != Source::kVariable)
    {
        command.value.value = convert(command.value.value, ScaleOf(command.kind));
    }
}

// The state that a command takes from the track: the running status for a repeated command, and the key and velocity
// that a note or an end of a tie leaves out.
RunningState StateOf(const Command& command, uint8_t code)
{
    RunningState state;
    state.running = command.encoding.repeated ? code : 0;
    state.key = command.key;
    state.velocity = static_cast<uint8_t>(command.velocity);

    return state;
}

// Whether the driver reads `bytes` as `command`, with the state that the command takes, where a note reads up to
// `note_arguments` bytes. Its values are the console's, and its target lies at address `target`. A byte from 0x80 up
// follows the bytes.
bool ReadsBack(std::span<const uint8_t> bytes, const Command& command, uint32_t target, int note_arguments)
{
    const auto byte_at = [bytes](uint32_t address)
    {
        return address < bytes.size() ? bytes[address] : uint8_t{kCmdFine};
    };
    const PlayedCommand played = ReadPlayedCommand(byte_at, 0, StateOf(command, CodeOf(command)), note_arguments);
    Command read = played.command;
    if (commands::HasTarget(read.kind) && read.target == target)
    {
        read.target = command.target;
        read.target_outside = command.target_outside;
    }

    return played.valid && played.next == bytes.size() && read == command;
}

// A command that the decoder read, at its offset.
struct Read
{
    Command command; // in the units, with the target's address
    uint32_t size = 0;
    bool raw = false;        // the encoder would write other bytes: the item keeps the bytes
    RunningState after = {}; // the track's state after the command
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

// The bytes of a song from address `base`, for a driver whose notes read up to `note_arguments` bytes.
struct Bytes
{
    std::span<const uint8_t> data;
    uint32_t base = 0;
    int note_arguments = 3;

    bool Inside(uint32_t address) const
    {
        return address >= base && address - base < data.size();
    }

    // The byte at `address`, or 0 past the bytes: a note there would read it, and run past them.
    uint8_t At(uint32_t address) const
    {
        return Inside(address) ? data[address - base] : uint8_t{0};
    }
};

// The command at offset `at` with the track's state `state`, or nothing where it runs past the bytes or needs a running
// status that the state lacks.
std::optional<Read> ReadAt(const Bytes& bytes, uint32_t at, const RunningState& state)
{
    const PlayedCommand played = ReadPlayedCommand([&bytes](uint32_t address) { return bytes.At(address); },
                                                   bytes.base + at, state, bytes.note_arguments);
    if (!played.valid || played.next - bytes.base > bytes.data.size())
    {
        return std::nullopt;
    }

    Read read;
    read.command = played.command;
    read.size = played.next - bytes.base - at;
    read.after = played.after;

    std::vector<uint8_t> again;
    AppendCommand(again, read.command, read.command.target, bytes.note_arguments);
    read.raw = !std::equal(again.begin(), again.end(), bytes.data.begin() + at, bytes.data.begin() + at + read.size);
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

// A place that the flow reaches: an offset, the track's state there, and the addresses that the open pattern calls
// return to.
struct Place
{
    auto operator<=>(const Place&) const = default;

    uint32_t at = 0;
    RunningState state;
    std::vector<uint32_t> returns;
};

// The deepest pattern calls: a call from the third ends the track.
constexpr std::size_t kPatternDepth = 3;

// Follows the tracks from each of `entries`, with a track's state at its start, through every jump, call, repeat and
// conditional jump. Adds each target inside the bytes to `targets`. Returns false where a command overlaps another,
// where the flow leads inside a command, or where two tracks read a command with other values.
bool Follow(const Bytes& bytes, std::span<const uint32_t> entries, Found& found, std::set<uint32_t>& targets)
{
    std::vector<Place> pending;
    for (const uint32_t entry : entries)
    {
        pending.push_back({entry, RunningState{}, {}});
    }
    std::set<Place> visited;
    while (!pending.empty())
    {
        Place place = std::move(pending.back());
        pending.pop_back();
        while (place.at < bytes.data.size() && visited.insert(place).second)
        {
            if (found.cover[place.at] == Cover::kInside)
            {
                return false;
            }

            std::optional<Read> read = ReadAt(bytes, place.at, place.state);
            if (!read)
            {
                break; // the rest stays data
            }
            const auto known = found.reads.find(place.at);
            if (known != found.reads.end())
            {
                if (known->second.command != read->command || known->second.size != read->size)
                {
                    return false;
                }
            }
            else if (!Free(found, place.at, read->size))
            {
                return false;
            }

            const Command command = read->command;
            const uint32_t next = place.at + read->size;
            const RunningState after = read->after;
            if (known == found.reads.end())
            {
                Mark(found, place.at, std::move(*read));
            }

            const bool inside = commands::HasTarget(command.kind) && bytes.Inside(command.target);
            if (inside)
            {
                targets.insert(command.target - bytes.base);
            }

            place.state = after;
            if (command.kind == Kind::kCall)
            {
                if (place.returns.size() >= kPatternDepth || !inside)
                {
                    break;
                }
                place.returns.push_back(bytes.base + next);
                place.at = command.target - bytes.base;
                continue;
            }
            if (command.kind == Kind::kReturn && !place.returns.empty())
            {
                const uint32_t back = place.returns.back();
                place.returns.pop_back();
                if (!bytes.Inside(back))
                {
                    break;
                }
                place.at = back - bytes.base;
                continue;
            }
            if (inside && command.kind != Kind::kJump)
            {
                pending.push_back({command.target - bytes.base, place.state, place.returns});
            }
            if (command.kind == Kind::kJump)
            {
                if (!inside)
                {
                    break;
                }
                place.at = command.target - bytes.base;
                continue;
            }
            if (!FallsThrough(command))
            {
                break;
            }

            place.at = next;
        }
    }

    return true;
}

// What a guess knows of a track's state: nothing at the start of a run of commands, and what the run's commands give.
struct Guessed
{
    RunningState state;
    bool running = false;
    bool key = false;
    bool velocity = false;
};

// The argument bytes of a note or an end of a tie that the decoder read.
int ArgumentsOf(const Read& read)
{
    return static_cast<int>(read.size) - (read.command.encoding.repeated ? 0 : 1);
}

// Whether `read` takes only the parts of the track's state that `guessed` knows.
bool Knows(const Read& read, const Guessed& guessed)
{
    if (read.command.encoding.repeated && !guessed.running)
    {
        return false;
    }

    const Kind kind = read.command.kind;
    if (kind != Kind::kNote && kind != Kind::kNoteOff)
    {
        return true;
    }
    const int count = ArgumentsOf(read);

    return (count >= 1 || guessed.key) && (kind == Kind::kNoteOff || count >= 2 || guessed.velocity);
}

// Reads the commands in the bytes that no track reaches, such as an end after a track's last jump. A run of them counts
// from the start of the bytes that no command covers, up to an end, a jump or a return, or up to a command that a track
// reaches. More runs may follow it, such as a pattern that the first one calls. The most runs whose targets all lead to
// items count. A command that takes a part of the track's state that the run doesn't give ends the run. Bytes that no
// such run covers stay as data.
void Guess(const Bytes& bytes, const std::set<uint32_t>& cuts, Found& found)
{
    const auto size = static_cast<uint32_t>(bytes.data.size());
    uint32_t at = 0;
    while (at < size)
    {
        if (found.cover[at] == Cover::kStart)
        {
            at += found.reads.at(at).size;
            continue;
        }

        // The bytes that no command covers, up to the next command or cut.
        uint32_t end = at + 1;
        while (end < size && found.cover[end] == Cover::kFree && !cuts.contains(end))
        {
            end++;
        }

        // Runs of commands one after another, each up to a command that ends it.
        std::vector<std::pair<uint32_t, Read>> run;
        std::vector<std::size_t> run_ends; // the commands up to each run's end
        uint32_t next = at;
        Guessed guessed;
        while (next < end)
        {
            std::optional<Read> read = ReadAt(bytes, next, guessed.state);
            if (!read || read->raw || next + read->size > end || !Knows(*read, guessed))
            {
                break;
            }

            // What the command gives of the state.
            const Kind kind = read->command.kind;
            guessed.running = guessed.running || read->after.running != guessed.state.running;
            if (kind == Kind::kNote || kind == Kind::kNoteOff)
            {
                const int count = ArgumentsOf(*read);
                guessed.key = guessed.key || count >= 1;
                guessed.velocity = guessed.velocity || (kind == Kind::kNote && count >= 2);
            }
            guessed.state = read->after;

            const uint32_t read_size = read->size;
            const bool counts = !FallsThrough(read->command) ||
                                (next + read_size == end && end < size && found.cover[end] == Cover::kStart);
            run.emplace_back(next, std::move(*read));
            next += read_size;
            if (counts)
            {
                run_ends.push_back(run.size());
            }
        }

        // A target leads to a command, a cut, a place outside the bytes, or a command of the runs taken.
        std::size_t taken = 0;
        for (auto count = run_ends.rbegin(); count != run_ends.rend() && taken == 0; ++count)
        {
            const auto first = run.begin();
            const auto last = run.begin() + static_cast<std::ptrdiff_t>(*count);
            const auto leads = [&](uint32_t target)
            {
                if (!bytes.Inside(target))
                {
                    return true;
                }
                const uint32_t offset = target - bytes.base;
                return found.cover[offset] == Cover::kStart || cuts.contains(offset) ||
                       std::any_of(first, last, [&](const auto& read) { return read.first == offset; });
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

// The items of the bytes in their order, from the commands in `found`. Data splits at `cuts`. Gives the item at each
// offset where one starts, and at the end. Without commands, the items are the whole song as data, split at `cuts`.
commands::Sequence Items(const Bytes& bytes, const Found& found, const std::set<uint32_t>& cuts,
                         std::map<uint32_t, uint32_t>& items)
{
    commands::Sequence sequence;
    const auto size = static_cast<uint32_t>(bytes.data.size());
    uint32_t at = 0;
    while (at < size)
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
        uint32_t end = read != found.reads.end() ? at + read->second.size : size;
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
        item.bytes.assign(bytes.data.begin() + at, bytes.data.begin() + end);
        sequence.items.push_back(std::move(item));
        at = end;
    }
    items[size] = static_cast<uint32_t>(sequence.items.size());

    // The targets by item. A target outside the bytes keeps its address.
    for (Command& command : sequence.items)
    {
        if (!commands::HasTarget(command.kind))
        {
            continue;
        }

        const auto item = bytes.Inside(command.target) ? items.find(command.target - bytes.base) : items.end();
        if (item == items.end())
        {
            command.target_outside = true;
            continue;
        }

        command.target = item->second;
    }

    return sequence;
}

} // namespace

namespace detail
{

Kind KindOfCode(uint8_t code)
{
    return code >= kCmdPrio && code < kCmdPrio + std::size(kByteKinds) ? kByteKinds[code - kCmdPrio] : Kind::kOther;
}

Kind KindOfExtended(uint8_t x)
{
    return x < kXcmdCount ? kExtendedKinds[x] : Kind::kEnd;
}

} // namespace detail

commands::Scale ScaleOf(Kind kind)
{
    using commands::kOne;
    switch (kind)
    {
    case Kind::kTempo:
        return {2 * kOne, 0}; // half the BPM
    case Kind::kTranspose:
    case Kind::kBendRange:
        return {100 * kOne, 0}; // semitones
    case Kind::kPan:
        return {kOne, -64 * kOne}; // the driver takes 0x40 off
    case Kind::kPitchBend:
        return {kOne / 64, -kOne}; // 64ths of the range, with 0x40 the centre
    case Kind::kTune:
        return {100 * kOne / 64, -100 * kOne}; // 64ths of a semitone, with 0x40 the centre
    default:
        return {};
    }
}

Decoded DecodeSequence(std::span<const uint8_t> data, uint32_t base, std::span<const uint32_t> entries,
                       int note_arguments)
{
    const Bytes bytes{data, base, note_arguments};
    std::set<uint32_t> cuts;
    std::vector<uint32_t> offsets;
    for (const uint32_t entry : entries)
    {
        if (!bytes.Inside(entry))
        {
            throw std::invalid_argument("a track outside the song's bytes");
        }

        offsets.push_back(entry - base);
        cuts.insert(entry - base);
    }

    Found found;
    found.cover.assign(data.size(), Cover::kFree);
    std::set<uint32_t> targets;
    bool opaque = !Follow(bytes, offsets, found, targets);
    cuts.insert(targets.begin(), targets.end());
    const auto inside = [&](uint32_t cut)
    {
        return cut < data.size() && found.cover[cut] == Cover::kInside;
    };
    opaque = opaque || std::any_of(cuts.begin(), cuts.end(), inside);

    Decoded decoded;
    std::map<uint32_t, uint32_t> items; // byte offset to item
    if (opaque)
    {
        std::set<uint32_t> entry_cuts(offsets.begin(), offsets.end());
        decoded.sequence = Items(bytes, Found{}, entry_cuts, items);
        decoded.opaque = true;
    }
    else
    {
        Guess(bytes, cuts, found);
        decoded.sequence = Items(bytes, found, cuts, items);
    }

    for (const uint32_t offset : offsets)
    {
        decoded.entries.push_back(items.at(offset));
    }

    return decoded;
}

Decoded DecodeSong(const Rom& rom, const SongHeader& header, int note_arguments)
{
    if (header.tracks.empty())
    {
        throw std::invalid_argument("a song without tracks");
    }

    const uint32_t first = *std::min_element(header.tracks.begin(), header.tracks.end());
    if (first >= header.address || !rom.Contains(first, header.address - first))
    {
        throw std::invalid_argument("a song whose tracks don't lie in the ROM before its header");
    }

    return DecodeSequence(std::span<const uint8_t>(rom.Ptr(first), header.address - first), first, header.tracks,
                          note_arguments);
}

bool FallsThrough(const commands::Command& command)
{
    switch (command.kind)
    {
    case Kind::kJump:
    case Kind::kEnd:
        return false;
    case Kind::kRepeat:
        return command.value.value != 0;
    default:
        return true;
    }
}

Encoded EncodeSequence(const commands::Sequence& sequence, uint32_t base, int note_arguments)
{
    // The console's values nearest the units.
    std::vector<commands::Command> items = sequence.items;
    for (commands::Command& item : items)
    {
        Convert(item, commands::FromUnit);
    }

    // Each item's size doesn't depend on its target's address. The sizes give the offsets.
    Encoded encoded;
    encoded.offsets.reserve(items.size() + 1);
    std::vector<uint8_t> scratch;
    uint32_t at = 0;
    for (const commands::Command& item : items)
    {
        encoded.offsets.push_back(at);
        scratch.clear();
        AppendCommand(scratch, item, 0, note_arguments);
        at += static_cast<uint32_t>(scratch.size());
    }
    encoded.offsets.push_back(at);

    // Then the bytes, with each target's address. The driver has to read each command's bytes back as the command.
    encoded.bytes.reserve(at);
    for (const commands::Command& item : items)
    {
        uint32_t target = item.target;
        if (commands::HasTarget(item.kind) && !item.target_outside)
        {
            if (item.target >= encoded.offsets.size())
            {
                throw std::invalid_argument("a target past the items");
            }

            target = base + encoded.offsets[item.target];
        }

        const auto start = encoded.bytes.size();
        AppendCommand(encoded.bytes, item, target, note_arguments);
        if (item.kind != Kind::kData &&
            !ReadsBack(std::span(encoded.bytes).subspan(start), item, target, note_arguments))
        {
            throw std::invalid_argument("the driver would read other commands from the bytes of " +
                                        std::string(commands::Name(item.kind)));
        }
    }

    // A note or an end of a tie that leaves out its last arguments reads the next byte below 0x80 as one.
    for (std::size_t i = 0; i < items.size(); i++)
    {
        const commands::Command& item = items[i];
        if (item.kind != Kind::kNote && item.kind != Kind::kNoteOff)
        {
            continue;
        }

        const uint32_t end = encoded.offsets[i + 1];
        const uint32_t start = encoded.offsets[i];
        const int count = static_cast<int>(end - start) - (item.encoding.repeated ? 0 : 1);
        if (count < MostArguments(item.kind, note_arguments) &&
            (end >= encoded.bytes.size() || encoded.bytes[end] < 0x80))
        {
            throw std::invalid_argument(
                "a note or an end of a tie that leaves out its arguments before a byte below "
                "0x80");
        }
    }

    return encoded;
}

} // namespace ntdlib::mp2k
