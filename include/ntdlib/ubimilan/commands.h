// SPDX-License-Identifier: MIT

// The commands of Ubisoft Milan's GBA driver in the common form of `ntdlib::commands`. A sequence is a resource of the
// sound bank. Its bytes are its kind and three bytes that the driver doesn't read, then
//
//     <countdown> { <command> <countdown> }* <end>
//
// A countdown is 2 bytes, low first. The decoder reads the bytes in order: the driver's commands hold no jumps or
// calls. The first 4 bytes become an item of kData, each countdown an item of kWait, and each command an item of its
// kind. The encoder writes the items back, and gives the same bytes. Bytes after the end stay out of the sequence.
//
// The driver counts time in frames, and a sequence has one track of commands for its 16 channels. `index` holds a
// command's channel. A wait's value is the driver's countdown, not a length of time. A countdown of 0 runs the next
// command in the same frame. A countdown of n runs it n + 1 frames later. No command therefore runs a single frame
// after another. The first countdown of a piece's first sequence runs its first command n frames in.
//
// The kinds:
//   kWait       a countdown
//   kNote       0x90: `key` and `velocity`. A note's length stays 0: a note sounds until a note-off of its key, or for
//               the kit until any note-off, or until its sample ends
//   kNoteOff    0x80: `key`, and in `velocity` the second byte. The driver doesn't read that byte
//   kProgram    0xC0: `value`
//   kOther      0xE0, in `encoding.opcode`: a command without an effect
//   kEnd        0xFC: the sequence's end
//   kData       the kind and the three bytes after it, and in a sequence that an edit changed, bytes after the end
//
// The values stay in the driver's units.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/ubimilan/driver.h"

namespace ntdlib::ubimilan
{

// What the decoder made of a sequence's bytes.
struct Decoded
{
    commands::Sequence sequence;

    // The bytes that the items take, up to the end. Without an end or with a command on which the game resets, the
    // decoder keeps all the bytes as data, and `reason` says why.
    uint32_t size = 0;
    bool opaque = false;
    std::string reason;

    // The bytes that the driver reads, from the sequence's start: `size`, or up to the command on which the game resets
    // in a sequence kept as data.
    uint32_t read = 0;
};

// Decodes the sequence at the start of `data`.
Decoded DecodeSequence(std::span<const uint8_t> data);

// Decodes resource `id`, a sequence. Takes the bytes up to the ROM's end. Throws std::invalid_argument for a resource
// that names no byte of the ROM, or isn't a sequence.
Decoded DecodeResource(const Rom& rom, const DriverInfo& info, int id);

// The bytes of a sequence, and each item's offset in them. `offsets` holds one more entry than the sequence has items:
// the end.
struct Encoded
{
    std::vector<uint8_t> bytes;
    std::vector<uint32_t> offsets;
};

// Encodes `sequence`. Throws std::invalid_argument for a value that the driver's bytes can't hold, and for a sequence
// that the driver wouldn't read as its items say: one that doesn't start with the 4 bytes of a sequence and a wait, a
// command without a wait after it, a kind that the driver doesn't have, a sequence without an end, or a command after
// the end.
Encoded EncodeSequence(const commands::Sequence& sequence);

// The scale of the value of a command of `kind` (commands::UnitOf): every value stays in the driver's units.
commands::Scale ScaleOf(commands::Kind kind);

} // namespace ntdlib::ubimilan
