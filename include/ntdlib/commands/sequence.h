// SPDX-License-Identifier: MIT

// A common form for the commands of the consoles' sequences. A sequence is a list of items in the order of its bytes:
// commands, and runs of bytes that no command reaches. Jumps, calls and track starts give their targets as item
// indices, not as byte offsets. Labels name the items where sounds start. Values are in the units of units.h, the same
// for every console: the Switch's volume of 100 is a level of 100, and its transpose of -1 is -100 cents. Each
// console's decoder converts its values to the units, and its encoder converts them back.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ntdlib::commands
{

// What a command does. Each console's encoder maps a kind and an index to the console's opcode.
enum class Kind : uint8_t
{
    kNote, // `key`, `velocity`, and the length in `value`
    kWait,
    kProgram,
    kOpenTrack, // starts track `index` at `target`
    kJump,
    kCall,
    kReturn,
    kLoopStart, // `value` passes, or 0 for ever
    kLoopEnd,
    kEnd,
    kAllocateTracks, // `value` is a mask of the tracks
    kTimebase,
    kEnvelopeHold,
    kMonophonic,
    kVelocityRange,
    kBiquadType,
    kBiquadValue,
    kBankSelect,
    kFrontBypass,
    kPan,
    kVolume,
    kMainVolume,
    kTranspose,
    kPitchBend,
    kBendRange,
    kPriority,
    kNoteWait,
    kTie,
    kPortamentoFrom,
    kPortamento,
    kPortamentoTime,
    kAttack,
    kDecay,
    kSustain,
    kRelease,
    kEnvelopeReset,
    kVolume2,
    kPrintVariable,
    kSurroundPan,
    kLowPass,
    kSend, // `index` is the bus: 0 to 2 for A to C
    kMainSend,
    kInitialPan,
    kMute,
    kDamper,
    kTempo,
    kSweepPitch,
    kLfoCurve, // `index` is the LFO, from 0
    kLfoPhase,
    kLfoDepth,
    kLfoSpeed,
    kLfoType,
    kLfoRange,
    kLfoDelay,
    kLfoPeriod,
    kSetVariable, // `index` is the variable
    kAddVariable,
    kSubtractVariable,
    kMultiplyVariable,
    kDivideVariable,
    kShiftVariable,
    kRandomVariable,
    kAndVariable,
    kOrVariable,
    kXorVariable,
    kNotVariable,
    kModuloVariable,
    kCompareEqual, // compares variable `index` with `value`
    kCompareGreaterOrEqual,
    kCompareGreater,
    kCompareLessOrEqual,
    kCompareLess,
    kCompareNotEqual,
    kUserProcedure,
    kOther, // an opcode that does nothing on the console, in `encoding.opcode`
    kData,  // bytes that no command reaches, in `bytes`
};

// The number of kinds.
inline constexpr int kKindCount = static_cast<int>(Kind::kData) + 1;

// Where an argument's value comes from when the command runs.
enum class Source : uint8_t
{
    kValue,    // `value` itself
    kRandom,   // a random value from `value` to `high`
    kVariable, // the variable numbered `value`
};

// A command's argument. Its values are in the unit of the command's kind (units.h), in fixed point.
struct Argument
{
    friend bool operator==(const Argument&, const Argument&) = default;

    Source source = Source::kValue;
    int64_t value = 0; // the value, the lowest random value, or the variable's number
    int64_t high = 0;  // the highest random value
};

// What the console's bytes held beyond the command's meaning. An encoder that keeps these writes the bytes that it
// read. A new command leaves them at their defaults, and the encoder writes the console's usual bytes.
struct Encoding
{
    friend bool operator==(const Encoding&, const Encoding&) = default;

    uint16_t opcode = 0;          // a kOther command's opcode: a byte, or 0xF000 plus the byte after 0xF0
    uint8_t value_bytes = 0;      // the bytes of a variable-length value, where it took more than it requires; else 0
    uint8_t unused_time = 0;      // a time prefix that the command reads no time for: its byte, or 0
    bool unused_random = false;   // a random prefix that the command reads no argument for
    bool unused_variable = false; // a variable prefix that the command reads no argument for
};

struct Command
{
    friend bool operator==(const Command&, const Command&) = default;

    Kind kind = Kind::kOther;
    bool conditional = false; // runs only if the last comparison held
    uint8_t key = 0;
    int64_t velocity = 0;         // a note's velocity: a level (units.h)
    uint8_t index = 0;            // the track, bus, LFO or variable that the command addresses
    Argument value;               // a note's length, a wait, or a setting's value
    std::optional<Argument> time; // the time over which a setting moves to its value
    uint32_t target = 0;          // the item that a jump, a call or a track start goes to
    bool target_outside = false;  // `target` is a byte offset past the end of the bytes, where no item starts
    Encoding encoding;
    std::vector<uint8_t> bytes; // a kData item's bytes
};

struct Label
{
    friend bool operator==(const Label&, const Label&) = default;

    std::string name;
    uint32_t item = 0;
};

struct Sequence
{
    friend bool operator==(const Sequence&, const Sequence&) = default;

    std::vector<Command> items;
    std::vector<Label> labels;
};

// A kind's name in the text form, such as "volume".
std::string_view Name(Kind kind);

// The kind with `name`, or nothing.
std::optional<Kind> KindNamed(std::string_view name);

// Whether a command of `kind` names its target item: a jump, a call or a track start.
bool HasTarget(Kind kind);

} // namespace ntdlib::commands
