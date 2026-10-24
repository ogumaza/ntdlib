// SPDX-License-Identifier: MIT

// NintendoWare's sequence bytecode in the common form of `ntdlib::commands`, for the Wii, the 3DS, the Wii U and the
// Switch. The console's sequence player (sequence.h) reads each command through ReadPlayedCommand, and the decoder
// reads the commands the same way. The decoder starts at the labels and at the sounds' starts, and follows every jump,
// call and track start. In the bytes that the player never reaches, it reads the runs of commands that end cleanly. The
// other bytes stay as data. The encoder writes the items back, and gives the same bytes for a sequence that the decoder
// read.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/commands/sequence.h"

namespace ntdlib::nwsnd
{

// The rules by which a console's sequence player reads its bytecode.
struct Grammar
{
    bool little_endian = true;      // the byte order of 16- and 24-bit arguments
    bool time_prefixes = true;      // 0xA3 to 0xA5 are time prefixes, not commands
    bool prefixes_chain = false;    // a variable prefix (0xA1) after a random one (0xA0) is a prefix too
    bool extra_lfo_bytes = true;    // 0xF0 with 0xA0 to 0xBF takes a byte: the extra LFOs' settings
    bool end_is_conditional = true; // an end (0xFF) after a comparison that failed doesn't end the track
    int sweep_steps = 64;           // the steps of a semitone in a sweep's pitch (0xE3)
};

// The grammar of a console's sequence player, from its policy (console.h) and its library's build.
template <typename Policy>
Grammar GrammarOf(const typename Policy::Build& build = {})
{
    Grammar grammar;
    grammar.little_endian = Policy::kLittleEndianArguments;
    grammar.time_prefixes = Policy::HasCommand(build, 0xA3);
    grammar.prefixes_chain = Policy::kArgPrefixesChain;
    grammar.extra_lfo_bytes = Policy::HasCommand(build, 0xF0A0);
    grammar.end_is_conditional = Policy::kEndIsConditional;
    grammar.sweep_steps = static_cast<int>(1.0f / Policy::SweepPitchOf(build, 1));

    return grammar;
}

// A label of a sequence file: its name and its offset into the bytecode.
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

// Decodes `data`, a sequence file's bytecode, from the labels in `labels` and the offsets in `entries`: where sounds
// start without a label. Throws std::invalid_argument for a label or an entry past the end of `data`.
Decoded DecodeSequence(std::span<const uint8_t> data, std::span<const LabelOffset> labels, const Grammar& grammar,
                       std::span<const uint32_t> entries = {});

// One command that DecodeCommand read, and the bytes it takes. Its values are in the units of units.h. A jump's, a
// call's or a track start's target is a byte offset into the bytecode.
struct DecodedCommand
{
    commands::Command command;
    uint32_t size = 0;
};

// Reads the command at byte offset `at` of `data` as the console's sequence player parses it. Returns nothing when the
// command runs past the end of `data`.
std::optional<DecodedCommand> DecodeCommand(std::span<const uint8_t> data, uint32_t at, const Grammar& grammar);

// A command as the console's sequence player reads it. Its values are the console's, not the units of units.h, and a
// jump's, a call's or a track start's target is a byte offset into the bytecode. The player reads an end (0xFF) at each
// byte past the bytecode's end.
struct PlayedCommand
{
    commands::Command command;
    uint16_t code = 0; // a note's key, an opcode, or 0xF000 plus the byte after 0xF0
    uint32_t next = 0; // the offset after the command, at most one past the bytecode's end
};

// Reads the command at byte offset `at` of `data` as the console's sequence player reads it, past the end too.
PlayedCommand ReadPlayedCommand(std::span<const uint8_t> data, uint32_t at, const Grammar& grammar);

// Whether a track goes on to the next command after `command`. An unconditional jump, return or end ends it, and so
// does a conditional end where the grammar's ends ignore the condition.
bool FallsThrough(const commands::Command& command, const Grammar& grammar);

// The bytes of a sequence, and each item's offset in them. `offsets` holds one more entry than the sequence has items:
// the end.
struct Encoded
{
    std::vector<uint8_t> bytes;
    std::vector<uint32_t> offsets;
};

// Encodes `sequence`, with the console's values nearest its units. Throws std::invalid_argument for a command that the
// grammar can't hold, such as a kOther command without an opcode, a value past the console's range or a target past
// the items.
Encoded EncodeSequence(const commands::Sequence& sequence, const Grammar& grammar);

} // namespace ntdlib::nwsnd
