// SPDX-License-Identifier: MIT

// The bytecode of the DS's sequences (SSEQ files, and the sequences of SSAR files) in the common form of
// `ntdlib::commands`. The driver (driver.h) reads each command through ReadPlayedCommand, and the decoder reads the
// commands the same way. The decoder starts at the labels and at the entries, and follows every jump, call and track
// start. In the bytes that the driver never reaches, it reads the runs of commands that end cleanly. The other bytes
// stay as data. The encoder writes the items back, and gives the same bytes for a sequence that the decoder read.
//
// The driver reads an if prefix (0xA2), then a random prefix (0xA0), then a variable prefix (0xA1), each where the byte
// before gives it. A variable prefix after a random one takes its place. The prefixes give the source of the last
// argument of a note (its length) and of the commands from 0x80 to 0x8F and 0xB0 to 0xEF. The other commands read no
// argument for them. A sequence's entry may start with 0xFE and a mask of the tracks that the sequence allocates. The
// driver reads it there only (kAllocateTracks). Elsewhere, 0xFE reads nothing and does nothing.
//
// The values convert to the units of commands/units.h as on NintendoWare's consoles, but for the LFO's. An LFO's depth
// is in steps of 127/16384 of its range, its speed in 128ths of a cycle each 4 frames of the driver, and its delay in
// frames (snd::kFrameClocks of the system clock, about 5.21 ms).

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"

namespace ntdlib::ntr::snd
{

// A command as the driver reads it. Its values are the console's, not the units of units.h, and a jump's, a call's or a
// track start's target is a byte offset from the sequence's start.
struct PlayedCommand
{
    commands::Command command;
    uint8_t code = 0;  // a note's key, or the opcode
    uint32_t next = 0; // the address after the command
};

namespace detail
{

constexpr uint8_t kRandomPrefix = 0xA0;
constexpr uint8_t kVariablePrefix = 0xA1;
constexpr uint8_t kIfPrefix = 0xA2;

// How a command stores its value when no prefix gives the value's source.
enum class Width : uint8_t
{
    kNone,
    kByte,
    kSigned16,
    kVariableLength,
};

// Whether a random or a variable prefix gives the source of the command's last argument.
constexpr bool TakesPrefixes(uint8_t code)
{
    return code < 0x90 || (code >= 0xB0 && code < 0xF0);
}

constexpr Width WidthOf(uint8_t code)
{
    if (code < 0x90)
    {
        return Width::kVariableLength;
    }
    if (code >= 0xB0 && code < 0xC0)
    {
        return Width::kSigned16;
    }
    if (code >= 0xC0 && code < 0xE0)
    {
        return Width::kByte;
    }
    if (code >= 0xE0 && code < 0xF0)
    {
        return Width::kSigned16;
    }

    return Width::kNone;
}

// The commands that read a byte before their value or target: a track start's track and a variable's number.
constexpr bool TakesIndex(uint8_t code)
{
    return code == 0x93 || (code >= 0xB0 && code < 0xC0);
}

constexpr bool TakesTarget(uint8_t code)
{
    return code >= 0x93 && code <= 0x95;
}

// The kind of the command with opcode `code`, from 0x80 up. The opcodes that do nothing are kOther.
commands::Kind KindOf(uint8_t code);

} // namespace detail

// Reads the command at address `at` as the driver reads it. `byte_at(address)` gives the byte at an address. The
// addresses count up from `at` and wrap at 2^32.
template <typename ByteAt>
PlayedCommand ReadPlayedCommand(const ByteAt& byte_at, uint32_t at)
{
    using commands::Source;
    using detail::Width;

    PlayedCommand played;
    commands::Command& command = played.command;
    const auto next_byte = [&byte_at, &at]
    {
        return static_cast<uint8_t>(byte_at(at++));
    };
    const auto signed16 = [&next_byte]
    {
        const uint32_t low = next_byte();
        return static_cast<int16_t>(low | static_cast<uint32_t>(next_byte()) << 8);
    };

    uint8_t code = next_byte();
    if (code == detail::kIfPrefix)
    {
        command.conditional = true;
        code = next_byte();
    }
    bool random = false;
    bool variable = false;
    if (code == detail::kRandomPrefix)
    {
        random = true;
        code = next_byte();
    }
    if (code == detail::kVariablePrefix)
    {
        variable = true;
        code = next_byte();
    }

    // The prefixes that the command takes give its value's source. The others stay in the encoding.
    const bool prefixed = detail::TakesPrefixes(code);
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
        command.kind = commands::Kind::kNote;
        command.key = code;
        command.velocity = next_byte();
    }
    else
    {
        command.kind = detail::KindOf(code);
        if (command.kind == commands::Kind::kOther)
        {
            command.encoding.opcode = code;
        }
    }

    if (detail::TakesIndex(code))
    {
        command.index = next_byte();
    }

    if (detail::TakesTarget(code))
    {
        uint32_t target = next_byte();
        target |= static_cast<uint32_t>(next_byte()) << 8;
        target |= static_cast<uint32_t>(next_byte()) << 16;
        command.target = target;
    }
    else if (source == Source::kVariable)
    {
        command.value.source = source;
        command.value.value = next_byte();
    }
    else if (source == Source::kRandom)
    {
        command.value.source = source;
        command.value.value = signed16();
        command.value.high = signed16();
    }
    else
    {
        switch (detail::WidthOf(code))
        {
        case Width::kByte:
            command.value.value = next_byte();
            break;
        case Width::kSigned16:
            command.value.value = signed16();
            break;
        case Width::kVariableLength:
            {
                // The driver shifts the value left 7 bits for each byte, in 32 bits.
                uint32_t value = 0;
                uint8_t bytes = 0;
                uint8_t byte = 0;
                do
                {
                    byte = next_byte();
                    value = value << 7 | (byte & 0x7F);
                    bytes++;
                } while ((byte & 0x80) != 0);
                command.value.value = static_cast<int32_t>(value);
                uint8_t required = 1;
                for (uint32_t rest = value; rest >= 0x80; rest >>= 7)
                {
                    required++;
                }
                if (bytes != required)
                {
                    command.encoding.value_bytes = bytes;
                }
                break;
            }
        case Width::kNone:
            break;
        }

        // The driver reads a transpose and a pitch bend as signed bytes.
        if (command.kind == commands::Kind::kTranspose || command.kind == commands::Kind::kPitchBend)
        {
            command.value.value = static_cast<int8_t>(command.value.value);
        }
    }

    played.code = code;
    played.next = at;
    return played;
}

// A label of a sequence: its name and its offset into the bytecode.
struct LabelOffset
{
    std::string name;
    uint32_t offset = 0;
};

// What the decoder made of a sequence's bytes.
struct Decoded
{
    commands::Sequence sequence;
    std::vector<uint32_t> entries; // the item at each entry

    // Commands that overlap, or a target, a label or an entry inside a command. The decoder then keeps the whole
    // bytecode as data, split at the labels and the entries.
    bool opaque = false;
};

// Decodes `data`, a sequence's bytecode from its start, from the labels in `labels` and the offsets in `entries`: where
// sequences start without a label. An SSEQ file's sequence starts at offset 0, and an SSAR file's sequences at their
// offsets. Throws std::invalid_argument for a label or an entry past the end of `data`.
Decoded DecodeSequence(std::span<const uint8_t> data, std::span<const LabelOffset> labels,
                       std::span<const uint32_t> entries = {});

// One command that DecodeCommand read, and the bytes it takes. Its values are in the units of units.h. A jump's, a
// call's or a track start's target is a byte offset into the bytecode.
struct DecodedCommand
{
    commands::Command command;
    uint32_t size = 0;
};

// Reads the command at byte offset `at` of `data` as the driver reads it in a track, or nothing when the command runs
// past the end of `data`.
std::optional<DecodedCommand> DecodeCommand(std::span<const uint8_t> data, uint32_t at);

// Whether a track goes on to the next command after `command`. An unconditional jump, return or end ends it.
bool FallsThrough(const commands::Command& command);

// The bytes of a sequence, and each item's offset in them. `offsets` holds one more entry than the sequence has items:
// the end.
struct Encoded
{
    std::vector<uint8_t> bytes;
    std::vector<uint32_t> offsets;
};

// Encodes `sequence`, with the console's values nearest its units. Throws std::invalid_argument for a command that the
// bytecode can't hold, such as a kOther command without an opcode, a value past the console's range or a target past
// the items.
Encoded EncodeSequence(const commands::Sequence& sequence);

// The scale of the value of a command of `kind` (commands::UnitOf). A note's velocity and a time take the unchanged
// scale.
commands::Scale ScaleOf(commands::Kind kind);

} // namespace ntdlib::ntr::snd
