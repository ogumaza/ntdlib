// SPDX-License-Identifier: MIT

#include "ntdlib/nwsnd/commands.h"

#include <algorithm>
#include <array>
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
#include "ntdlib/nwsnd/units.h"

namespace ntdlib::nwsnd
{
namespace
{

using commands::Argument;
using commands::Command;
using commands::Kind;
using commands::Source;

constexpr uint8_t kRandomPrefix = 0xA0;
constexpr uint8_t kVariablePrefix = 0xA1;
constexpr uint8_t kIfPrefix = 0xA2;
constexpr uint8_t kTimePrefix = 0xA3; // a 16-bit time follows the value
constexpr uint8_t kTimeRandomPrefix = 0xA4;
constexpr uint8_t kTimeVariablePrefix = 0xA5;
constexpr uint8_t kExtended = 0xF0;

// The largest offset that a jump, a call or a track start can hold: 24 bits.
constexpr uint32_t kMaxOffset = 0xFFFFFF;

// How a command stores its value when no prefix gives the value's type.
enum class Width
{
    kNone, // the command takes no value
    kByte,
    kSigned16,
    kUnsigned16,
    kVariableLength,
};

// The LFOs' opcodes. LFO 0 has single bytes. LFOs 1 to 3 follow 0xF0 in groups of six settings, and of two for their
// delays and periods.
constexpr uint16_t kLfoZero[] = {0xBE, 0xBD, 0xCA, 0xCB, 0xCC, 0xCD}; // curve, phase, depth, speed, type, range
constexpr uint16_t kLfoZeroDelay = 0xE0;
constexpr uint16_t kLfoZeroPeriod = 0xE4;
constexpr int kLfoCount = 4;

// A setting's place among an LFO's six settings, or -1 for the other kinds.
int LfoSetting(Kind kind)
{
    switch (kind)
    {
    case Kind::kLfoCurve:
        return 0;
    case Kind::kLfoPhase:
        return 1;
    case Kind::kLfoDepth:
        return 2;
    case Kind::kLfoSpeed:
        return 3;
    case Kind::kLfoType:
        return 4;
    case Kind::kLfoRange:
        return 5;
    default:
        return -1;
    }
}

// The opcode of `kind` with `index`, or nothing if the bytecode has none. A code is a byte, or 0xF000 plus the byte
// after 0xF0. Notes, kOther and kData have no single opcode.
std::optional<uint16_t> OpcodeOf(Kind kind, uint8_t index)
{
    // The kinds whose index picks the opcode: the LFOs' settings and the sends.
    if (const int setting = LfoSetting(kind); setting >= 0)
    {
        if (index >= kLfoCount)
        {
            return std::nullopt;
        }

        return index == 0 ? kLfoZero[setting] : static_cast<uint16_t>(0xF0A0 + (index - 1) * 6 + setting);
    }
    if (kind == Kind::kLfoDelay || kind == Kind::kLfoPeriod)
    {
        if (index >= kLfoCount)
        {
            return std::nullopt;
        }

        const bool period = kind == Kind::kLfoPeriod;
        if (index == 0)
        {
            return period ? kLfoZeroPeriod : kLfoZeroDelay;
        }

        return static_cast<uint16_t>(0xF0E1 + (index - 1) * 2 + (period ? 1 : 0));
    }
    if (kind == Kind::kSend)
    {
        constexpr uint16_t kSends[] = {0xD9, 0xDA, 0xDE};
        if (index >= std::size(kSends))
        {
            return std::nullopt;
        }

        return kSends[index];
    }

    // The variables' operations and comparisons, in the order of Kind.
    if (kind >= Kind::kSetVariable && kind <= Kind::kModuloVariable)
    {
        return static_cast<uint16_t>(0xF080 + (static_cast<int>(kind) - static_cast<int>(Kind::kSetVariable)));
    }
    if (kind >= Kind::kCompareEqual && kind <= Kind::kCompareNotEqual)
    {
        return static_cast<uint16_t>(0xF090 + (static_cast<int>(kind) - static_cast<int>(Kind::kCompareEqual)));
    }

    switch (kind)
    {
    case Kind::kWait:
        return 0x80;
    case Kind::kProgram:
        return 0x81;
    case Kind::kOpenTrack:
        return 0x88;
    case Kind::kJump:
        return 0x89;
    case Kind::kCall:
        return 0x8A;
    case Kind::kTimebase:
        return 0xB0;
    case Kind::kEnvelopeHold:
        return 0xB1;
    case Kind::kMonophonic:
        return 0xB2;
    case Kind::kVelocityRange:
        return 0xB3;
    case Kind::kBiquadType:
        return 0xB4;
    case Kind::kBiquadValue:
        return 0xB5;
    case Kind::kBankSelect:
        return 0xB6;
    case Kind::kFrontBypass:
        return 0xBF;
    case Kind::kPan:
        return 0xC0;
    case Kind::kVolume:
        return 0xC1;
    case Kind::kMainVolume:
        return 0xC2;
    case Kind::kTranspose:
        return 0xC3;
    case Kind::kPitchBend:
        return 0xC4;
    case Kind::kBendRange:
        return 0xC5;
    case Kind::kPriority:
        return 0xC6;
    case Kind::kNoteWait:
        return 0xC7;
    case Kind::kTie:
        return 0xC8;
    case Kind::kPortamentoFrom:
        return 0xC9;
    case Kind::kPortamento:
        return 0xCE;
    case Kind::kPortamentoTime:
        return 0xCF;
    case Kind::kAttack:
        return 0xD0;
    case Kind::kDecay:
        return 0xD1;
    case Kind::kSustain:
        return 0xD2;
    case Kind::kRelease:
        return 0xD3;
    case Kind::kLoopStart:
        return 0xD4;
    case Kind::kVolume2:
        return 0xD5;
    case Kind::kPrintVariable:
        return 0xD6;
    case Kind::kSurroundPan:
        return 0xD7;
    case Kind::kLowPass:
        return 0xD8;
    case Kind::kMainSend:
        return 0xDB;
    case Kind::kInitialPan:
        return 0xDC;
    case Kind::kMute:
        return 0xDD;
    case Kind::kDamper:
        return 0xDF;
    case Kind::kTempo:
        return 0xE1;
    case Kind::kSweepPitch:
        return 0xE3;
    case Kind::kEnvelopeReset:
        return 0xFB;
    case Kind::kLoopEnd:
        return 0xFC;
    case Kind::kReturn:
        return 0xFD;
    case Kind::kAllocateTracks:
        return 0xFE;
    case Kind::kEnd:
        return 0xFF;
    case Kind::kUserProcedure:
        return 0xF0E0;
    default:
        return std::nullopt;
    }
}

// The kind and index of each opcode, the reverse of OpcodeOf: 256 bytes, then 256 bytes after 0xF0. Opcodes that no
// kind has are kOther.
struct OpcodeKind
{
    Kind kind = Kind::kOther;
    uint8_t index = 0;
};

const std::array<OpcodeKind, 512>& OpcodeKinds()
{
    static const std::array<OpcodeKind, 512> kKinds = []
    {
        std::array<OpcodeKind, 512> kinds{};
        for (int k = 0; k < commands::kKindCount; ++k)
        {
            const auto kind = static_cast<Kind>(k);
            for (uint8_t index = 0; index < kLfoCount; ++index)
            {
                const std::optional<uint16_t> code = OpcodeOf(kind, index);
                if (code)
                {
                    const std::size_t slot = *code >= 0xF000 ? 256 + (*code & 0xFF) : *code;
                    if (kinds[slot].kind == Kind::kOther)
                    {
                        kinds[slot] = {kind, index};
                    }
                }
            }
        }

        return kinds;
    }();

    return kKinds;
}

// The kind and index of opcode `code` on the grammar's console.
OpcodeKind KindOf(uint32_t code, const Grammar& grammar)
{
    if (code >= 0xF000 && !grammar.extra_lfo_bytes && (code & 0xFF) >= 0xA0 && (code & 0xFF) <= 0xBF)
    {
        return {}; // a console without the extra LFOs reads no byte, and the command does nothing
    }

    return OpcodeKinds()[code >= 0xF000 ? 256 + (code & 0xFF) : code];
}

// How the command with opcode `code` stores its value on the grammar's console.
Width WidthOf(uint32_t code, const Grammar& grammar)
{
    if (code < 0x80)
    {
        return Width::kVariableLength; // a note's length
    }
    if (code >= 0xF000)
    {
        const uint32_t sub = code & 0xF0;
        if (sub == 0x80 || sub == 0x90)
        {
            return Width::kSigned16;
        }
        if (sub == 0xA0 || sub == 0xB0)
        {
            return grammar.extra_lfo_bytes ? Width::kByte : Width::kNone;
        }

        return sub == 0xE0 ? Width::kUnsigned16 : Width::kNone;
    }

    switch (code & 0xF0)
    {
    case 0x80:
        return code == 0x80 || code == 0x81 ? Width::kVariableLength : Width::kNone;
    case 0xB0:
    case 0xC0:
    case 0xD0:
        return Width::kByte;
    case 0xE0:
        return Width::kSigned16;
    default:
        return code == 0xFE ? Width::kUnsigned16 : Width::kNone;
    }
}

// Whether a random or variable prefix gives the type of the value of the command with opcode `code`. The extra LFOs'
// settings and the tracks to allocate take plain bytes.
bool TakesPrefixes(uint32_t code)
{
    if (code < 0x80)
    {
        return true;
    }
    if (code >= 0xF000)
    {
        const uint32_t sub = code & 0xF0;
        return sub == 0x80 || sub == 0x90 || sub == 0xE0;
    }

    return code == 0x80 || code == 0x81 || (code >= 0xB0 && code <= 0xEF);
}

// Whether the command with opcode `code` reads a time after its value when a time prefix comes before it.
bool TakesTime(uint32_t code)
{
    return code >= 0xB0 && code <= 0xDF;
}

// The bytes that `value` takes as a variable-length number.
uint8_t VariableLengthBytes(uint32_t value)
{
    uint8_t bytes = 1;
    while (value >= 0x80)
    {
        value >>= 7;
        ++bytes;
    }

    return bytes;
}

// A reader of the bytecode that records a read past the end. The player's reads go on past it.
class Reader
{
public:
    Reader(std::span<const uint8_t> data, std::size_t at, bool little_endian)
        : data_(data), at_(at), little_endian_(little_endian)
    {
    }

    uint8_t Byte()
    {
        if (at_ >= data_.size())
        {
            // The player reads an end there.
            past_end_ = true;
            ++at_;
            return 0xFF;
        }

        return data_[at_++];
    }

    uint32_t Bits16()
    {
        const uint32_t first = Byte();
        const uint32_t second = Byte();
        return little_endian_ ? first | second << 8 : first << 8 | second;
    }

    uint32_t Bits24()
    {
        const uint32_t first = Byte();
        const uint32_t second = Byte();
        const uint32_t third = Byte();
        return little_endian_ ? first | second << 8 | third << 16 : first << 16 | second << 8 | third;
    }

    // A variable-length number, and the bytes it took.
    std::pair<uint32_t, uint8_t> VariableLength()
    {
        uint32_t value = 0;
        uint8_t bytes = 0;
        uint8_t byte = 0;
        do
        {
            byte = Byte();
            value = (byte & 0x7F) | value << 7;
            ++bytes;
        } while ((byte & 0x80) != 0 && at_ <= data_.size());

        return {value, bytes};
    }

    std::size_t At() const
    {
        return at_;
    }

    bool PastEnd() const
    {
        return past_end_;
    }

private:
    std::span<const uint8_t> data_;
    std::size_t at_ = 0;
    bool little_endian_ = true;
    bool past_end_ = false;
};

// Reads an argument of `width`, or of the prefix's type.
Argument ReadArgument(Reader& r, Width width, Source source, Command& command)
{
    Argument argument;
    argument.source = source;
    if (source == Source::kVariable)
    {
        argument.value = r.Byte();
        return argument;
    }
    if (source == Source::kRandom)
    {
        argument.value = static_cast<int16_t>(r.Bits16());
        argument.high = static_cast<int16_t>(r.Bits16());
        return argument;
    }

    switch (width)
    {
    case Width::kByte:
        argument.value = r.Byte();
        break;
    case Width::kSigned16:
        argument.value = static_cast<int16_t>(r.Bits16());
        break;
    case Width::kUnsigned16:
        argument.value = static_cast<int32_t>(r.Bits16());
        break;
    case Width::kVariableLength:
        {
            const auto [value, bytes] = r.VariableLength();
            argument.value = value;
            if (bytes != VariableLengthBytes(value))
            {
                command.encoding.value_bytes = bytes;
            }
            break;
        }
    case Width::kNone:
        break;
    }

    return argument;
}

// Reads one command as the player parses it: an if prefix, a time prefix, a random or a variable prefix, the opcode
// and its arguments. A jump's, a call's or a track start's target is a byte offset.
Command ParseCommand(Reader& r, const Grammar& grammar)
{
    Command command;
    uint32_t code = r.Byte();
    if (code == kIfPrefix)
    {
        command.conditional = true;
        code = r.Byte();
    }

    uint8_t time_prefix = 0;
    if (grammar.time_prefixes && code >= kTimePrefix && code <= kTimeVariablePrefix)
    {
        time_prefix = static_cast<uint8_t>(code);
        code = r.Byte();
    }

    bool random = false;
    bool variable = false;
    if (code == kRandomPrefix)
    {
        random = true;
        code = r.Byte();
    }
    if (code == kVariablePrefix && (!random || grammar.prefixes_chain))
    {
        variable = true;
        code = r.Byte();
    }
    if (code == kExtended)
    {
        code = 0xF000 | r.Byte();
    }

    // The prefixes that the command takes give its value's source. The others stay in the encoding. A variable prefix
    // after a random one takes the place of the random one.
    const bool prefixed = TakesPrefixes(code);
    Source source = Source::kValue;
    if (prefixed && variable)
    {
        source = Source::kVariable;
        command.encoding.unused_random = random;
    }
    else if (prefixed && random)
    {
        source = Source::kRandom;
    }
    else
    {
        command.encoding.unused_random = random;
        command.encoding.unused_variable = variable;
    }

    if (code < 0x80)
    {
        command.kind = Kind::kNote;
        command.key = static_cast<uint8_t>(code);
        command.velocity = r.Byte();
    }
    else
    {
        const OpcodeKind kind = KindOf(code, grammar);
        command.kind = kind.kind;
        command.index = kind.index;
        if (kind.kind == Kind::kOther)
        {
            command.encoding.opcode = static_cast<uint16_t>(code);
        }
    }

    // The operands before the value: a track's number and a variable's.
    if (code == 0x88 || (code >= 0xF080 && code <= 0xF09F))
    {
        command.index = r.Byte();
    }

    if (code == 0x88 || code == 0x89 || code == 0x8A)
    {
        command.target = r.Bits24();
    }
    else
    {
        const Width width = WidthOf(code, grammar);
        if (width != Width::kNone || source != Source::kValue)
        {
            command.value = ReadArgument(r, width, prefixed ? source : Source::kValue, command);
        }
    }

    // A time prefix gives the commands from 0xB0 to 0xDF a time after their value. The player reads a plain time as an
    // unsigned number.
    if (time_prefix != 0 && TakesTime(code))
    {
        const Source time_source = time_prefix == kTimePrefix         ? Source::kValue
                                   : time_prefix == kTimeRandomPrefix ? Source::kRandom
                                                                      : Source::kVariable;
        command.time = ReadArgument(r, Width::kUnsigned16, time_source, command);
    }
    else
    {
        command.encoding.unused_time = time_prefix;
    }

    // The engine reads a transpose and a pitch bend as signed bytes.
    if ((command.kind == Kind::kTranspose || command.kind == Kind::kPitchBend) &&
        command.value.source == Source::kValue)
    {
        command.value.value = static_cast<int8_t>(command.value.value);
    }

    return command;
}

void AppendBits16(std::vector<uint8_t>& out, uint32_t value, bool little_endian)
{
    const auto low = static_cast<uint8_t>(value);
    const auto high = static_cast<uint8_t>(value >> 8);
    out.push_back(little_endian ? low : high);
    out.push_back(little_endian ? high : low);
}

void AppendBits24(std::vector<uint8_t>& out, uint32_t value, bool little_endian)
{
    const auto low = static_cast<uint8_t>(value);
    const auto middle = static_cast<uint8_t>(value >> 8);
    const auto high = static_cast<uint8_t>(value >> 16);
    out.push_back(little_endian ? low : high);
    out.push_back(middle);
    out.push_back(little_endian ? high : low);
}

void AppendVariableLength(std::vector<uint8_t>& out, uint32_t value, uint8_t bytes)
{
    if (bytes == 0)
    {
        bytes = VariableLengthBytes(value);
    }

    for (int i = bytes - 1; i >= 0; --i)
    {
        const auto group = static_cast<uint8_t>(i >= 5 ? 0 : (value >> (7 * i)) & 0x7F);
        out.push_back(static_cast<uint8_t>(group | (i != 0 ? 0x80 : 0)));
    }
}

void AppendArgument(std::vector<uint8_t>& out, const Argument& argument, Width width, uint8_t value_bytes,
                    bool little_endian)
{
    switch (argument.source)
    {
    case Source::kVariable:
        out.push_back(static_cast<uint8_t>(argument.value));
        return;
    case Source::kRandom:
        AppendBits16(out, static_cast<uint32_t>(argument.value), little_endian);
        AppendBits16(out, static_cast<uint32_t>(argument.high), little_endian);
        return;
    case Source::kValue:
        break;
    }

    switch (width)
    {
    case Width::kByte:
        out.push_back(static_cast<uint8_t>(argument.value));
        break;
    case Width::kSigned16:
    case Width::kUnsigned16:
        AppendBits16(out, static_cast<uint32_t>(argument.value), little_endian);
        break;
    case Width::kVariableLength:
        AppendVariableLength(out, static_cast<uint32_t>(argument.value), value_bytes);
        break;
    case Width::kNone:
        break;
    }
}

// The opcode that encodes `command`.
uint32_t CodeOf(const Command& command)
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
        return command.encoding.opcode;
    }

    const std::optional<uint16_t> code = OpcodeOf(command.kind, command.index);
    if (!code)
    {
        throw std::invalid_argument("no opcode for " + std::string(commands::Name(command.kind)) + " " +
                                    std::to_string(command.index));
    }

    return *code;
}

// Appends the bytes of `command`, whose target lies at byte offset `target`.
void AppendCommand(std::vector<uint8_t>& out, const Command& command, uint32_t target, const Grammar& grammar)
{
    if (command.kind == Kind::kData)
    {
        out.insert(out.end(), command.bytes.begin(), command.bytes.end());
        return;
    }

    const uint32_t code = CodeOf(command);
    const bool prefixed = TakesPrefixes(code);
    if (!prefixed && command.value.source != Source::kValue)
    {
        throw std::invalid_argument(std::string(commands::Name(command.kind)) + " takes no random or variable value");
    }
    if (command.time && !TakesTime(code))
    {
        throw std::invalid_argument(std::string(commands::Name(command.kind)) + " takes no time");
    }

    // The prefixes, in the order the player reads them.
    if (command.conditional)
    {
        out.push_back(kIfPrefix);
    }
    if (command.time)
    {
        out.push_back(command.time->source == Source::kValue    ? kTimePrefix
                      : command.time->source == Source::kRandom ? kTimeRandomPrefix
                                                                : kTimeVariablePrefix);
    }
    else if (command.encoding.unused_time != 0)
    {
        out.push_back(command.encoding.unused_time);
    }
    if (command.value.source == Source::kRandom || command.encoding.unused_random)
    {
        out.push_back(kRandomPrefix);
    }
    if (command.value.source == Source::kVariable || command.encoding.unused_variable)
    {
        out.push_back(kVariablePrefix);
    }

    if (code >= 0xF000)
    {
        out.push_back(kExtended);
    }
    out.push_back(static_cast<uint8_t>(code));
    if (command.kind == Kind::kNote)
    {
        out.push_back(static_cast<uint8_t>(command.velocity));
    }
    if (code == 0x88 || (code >= 0xF080 && code <= 0xF09F))
    {
        out.push_back(command.index);
    }

    if (code == 0x88 || code == 0x89 || code == 0x8A)
    {
        if (target > kMaxOffset)
        {
            throw std::invalid_argument("a target past 24 bits");
        }

        AppendBits24(out, target, grammar.little_endian);
    }
    else
    {
        AppendArgument(out, command.value, WidthOf(code, grammar), command.encoding.value_bytes, grammar.little_endian);
    }

    if (command.time)
    {
        AppendArgument(out, *command.time, Width::kUnsigned16, 0, grammar.little_endian);
    }
}

// Converts `argument` from the console's values to the units, or back to the nearest console values.
void Convert(Argument& argument, const Scale& scale, bool to_units)
{
    if (argument.source == Source::kVariable)
    {
        return;
    }

    argument.value = to_units ? ToUnit(argument.value, scale) : FromUnit(argument.value, scale);
    if (argument.source == Source::kRandom)
    {
        argument.high = to_units ? ToUnit(argument.high, scale) : FromUnit(argument.high, scale);
    }
}

// Converts the values of `command` from the console's values to the units, or back to the nearest console values.
void Convert(Command& command, const Grammar& grammar, bool to_units)
{
    if (command.kind == Kind::kData)
    {
        return;
    }

    command.velocity = to_units ? ToUnit(command.velocity, kLevelScale) : FromUnit(command.velocity, kLevelScale);
    Convert(command.value, ScaleOf(command.kind, grammar), to_units);
    if (command.time)
    {
        Convert(*command.time, kTicksScale, to_units);
    }
}

// Whether the player reads the bytes from `start` to the end of `bytes` as `command`, whose target lies at byte offset
// `target`. The command's values are the console's.
bool ReadsBack(std::span<const uint8_t> bytes, std::size_t start, const Command& command, uint32_t target,
               const Grammar& grammar)
{
    Reader r(bytes, start, grammar.little_endian);
    Command read = ParseCommand(r, grammar);
    if (commands::HasTarget(read.kind) && read.target == target)
    {
        read.target = command.target;
        read.target_outside = command.target_outside;
    }

    return !r.PastEnd() && r.At() == bytes.size() && read == command;
}

// Whether the track goes on to the next command after `command`.
bool FallsThrough(const Command& command, const Grammar& grammar)
{
    switch (command.kind)
    {
    case Kind::kJump:
    case Kind::kReturn:
        return command.conditional;
    case Kind::kEnd:
        return command.conditional && grammar.end_is_conditional;
    default:
        return true;
    }
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

// The command at `at`, or nothing where it runs past the end of `data`.
std::optional<Read> ReadAt(std::span<const uint8_t> data, uint32_t at, const Grammar& grammar)
{
    Reader r(data, at, grammar.little_endian);
    Read read;
    read.command = ParseCommand(r, grammar);
    if (r.PastEnd())
    {
        return std::nullopt;
    }

    read.size = static_cast<uint32_t>(r.At() - at);
    std::vector<uint8_t> again;
    AppendCommand(again, read.command, read.command.target, grammar);
    read.raw = !std::equal(again.begin(), again.end(), data.begin() + at, data.begin() + at + read.size);
    Convert(read.command, grammar, true);

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

// Follows the commands from each offset in `pending`, through every jump, call and track start. Returns false where a
// command overlaps another, or where the flow leads inside a command.
bool Follow(std::span<const uint8_t> data, std::vector<uint32_t> pending, const Grammar& grammar, Found& found)
{
    while (!pending.empty())
    {
        uint32_t at = pending.back();
        pending.pop_back();
        while (at < data.size() && found.cover[at] == Cover::kFree)
        {
            std::optional<Read> read = ReadAt(data, at, grammar);
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

            const bool next = FallsThrough(read->command, grammar);
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

// Reads the commands in the bytes that the player never reaches, such as the end after a track's last jump or a track
// that no command starts. A run of them counts from the start of the bytes that no command covers, up to an end, a jump
// or a return, or up to a command that the player reaches. Each of its targets has to lead to an item. Bytes that no
// such run covers stay as data.
void Guess(std::span<const uint8_t> data, const std::set<uint32_t>& cuts, const Grammar& grammar, Found& found)
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
            ++end;
        }

        std::vector<std::pair<uint32_t, Read>> run;
        uint32_t next = at;
        bool counts = false;
        while (next < end && !counts)
        {
            std::optional<Read> read = ReadAt(data, next, grammar);
            if (!read || read->raw || next + read->size > end)
            {
                break;
            }

            const uint32_t size = read->size;
            counts = !FallsThrough(read->command, grammar) ||
                     (next + size == end && end < data.size() && found.cover[end] == Cover::kStart);
            run.emplace_back(next, std::move(*read));
            next += size;
        }

        // A target leads to a command, a cut, the end, or a command of the run.
        const auto leads = [&](uint32_t target)
        {
            return target >= data.size() || found.cover[target] == Cover::kStart || cuts.contains(target) ||
                   std::any_of(run.begin(), run.end(), [&](const auto& read) { return read.first == target; });
        };
        counts = counts && std::all_of(run.begin(), run.end(),
                                       [&](const auto& read)
                                       {
                                           const Command& command = read.second.command;
                                           return !commands::HasTarget(command.kind) || leads(command.target);
                                       });
        if (!counts)
        {
            at = end;
            continue;
        }

        for (auto& [offset, read] : run)
        {
            Mark(found, offset, std::move(read));
        }
        at = next;
    }
}

// The whole bytecode as data, split at `cuts`. Gives the item at each cut and at the end.
commands::Sequence Opaque(std::span<const uint8_t> data, std::set<uint32_t> cuts, std::map<uint32_t, uint32_t>& items)
{
    commands::Sequence sequence;
    cuts.insert(0);
    for (auto cut = cuts.begin(); cut != cuts.end(); ++cut)
    {
        const uint32_t end = std::next(cut) == cuts.end() ? static_cast<uint32_t>(data.size()) : *std::next(cut);
        items[*cut] = static_cast<uint32_t>(sequence.items.size());
        if (end > *cut)
        {
            Command item;
            item.kind = Kind::kData;
            item.bytes.assign(data.begin() + *cut, data.begin() + end);
            sequence.items.push_back(std::move(item));
        }
    }
    items[static_cast<uint32_t>(data.size())] = static_cast<uint32_t>(sequence.items.size());

    return sequence;
}

// The items of `data` in the order of their bytes, from the commands in `found`. Data splits at `cuts`. Gives the item
// at each offset where one starts, and at the end.
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

Decoded DecodeSequence(std::span<const uint8_t> data, std::span<const LabelOffset> labels, const Grammar& grammar,
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

    Found found;
    found.cover.assign(data.size(), Cover::kFree);
    bool opaque = !Follow(data, std::vector<uint32_t>(cuts.begin(), cuts.end()), grammar, found);
    for (const auto& [at, read] : found.reads)
    {
        if (commands::HasTarget(read.command.kind))
        {
            cuts.insert(read.command.target);
        }
    }
    opaque = opaque || std::any_of(cuts.begin(), cuts.end(), [&](uint32_t cut)
                                   { return cut < data.size() && found.cover[cut] == Cover::kInside; });

    Decoded decoded;
    std::map<uint32_t, uint32_t> items; // byte offset to item
    if (opaque)
    {
        std::set<uint32_t> starts;
        for (const LabelOffset& label : labels)
        {
            starts.insert(label.offset);
        }
        starts.insert(entries.begin(), entries.end());
        decoded.sequence = Opaque(data, starts, items);
        decoded.opaque = true;
    }
    else
    {
        Guess(data, cuts, grammar, found);
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

Encoded EncodeSequence(const commands::Sequence& sequence, const Grammar& grammar)
{
    // The console's values nearest the units.
    std::vector<Command> items = sequence.items;
    for (Command& item : items)
    {
        Convert(item, grammar, false);
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
        AppendCommand(scratch, item, 0, grammar);
        at += static_cast<uint32_t>(scratch.size());
    }
    encoded.offsets.push_back(at);

    // Then the bytes, with each target's offset. The player has to read each command's bytes back as the command.
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

        const std::size_t start = encoded.bytes.size();
        AppendCommand(encoded.bytes, item, target, grammar);
        if (item.kind != Kind::kData && !ReadsBack(encoded.bytes, start, item, target, grammar))
        {
            throw std::invalid_argument("the player would read other commands from the bytes of " +
                                        std::string(commands::Name(item.kind)));
        }
    }

    return encoded;
}

} // namespace ntdlib::nwsnd
