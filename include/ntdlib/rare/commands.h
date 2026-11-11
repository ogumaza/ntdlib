// SPDX-License-Identifier: MIT

// The track commands of Rare's GBA driver in the common form of `ntdlib::commands`, in both of its formats. A tune's
// header names its tracks by their addresses. A sequence holds a tune's tracks: the bytes from the first track's start
// to the last byte that a track reads. The decoder follows each track as the driver does, from its start to its end
// command, or to its loop's end (controller 103) after its loop's start (controller 102). The driver goes back to the
// loop's start there, and reads no further. Each command becomes an item of its kind, and the bytes that no track
// reaches stay as data. The encoder writes the items back, and gives the same bytes for a sequence that the decoder
// read.
//
// Commands 4 to 10 apply to a MIDI channel. In the channel-byte format, the channel is a byte after the command's, and
// `index` holds that byte. In the channel-nibble format, the channel is the command byte's high nibble. `index` then
// holds the high nibble of every command, the channel's or the bits that the driver ignores in the others.
//
// The kinds:
//   kTempo         command 0: microseconds a quarter note
//   kWait          commands 1, 2 and 3: a delay in ticks, in 1, 2 or 3 bytes. `encoding.value_bytes` holds the bytes
//                  where they're more than the delay requires
//   kNote          commands 5 and 4: a note on, with `key` and `velocity`, and a length of 0. A note of command 4 holds
//                  4 in `encoding.opcode`. The driver plays both the same
//   kNoteOff       command 6: ends the channel's first note of `key` that's on
//   kProgram       command 8
//   kPitchBend     command 10: a fraction of the instrument's bend range
//   kEnd           command 11
//   kLfoDepth      controller 1: the depth of the channel's vibrato
//   kVolume        controller 7
//   kAttack, kDecay, kSustain, kRelease
//                  controllers 20 to 23, in the revision that reads them (Traits::envelope_controllers)
//   kLoopStart     controller 102, with a `value` of 0: for ever
//   kLoopEnd       controller 103
//   kMonophonic    controllers 126 (`value` 1) and 127 (`value` 0)
//   kOther         command 9, a channel pressure that the driver ignores, and command 12, which does nothing, with
//                  the command in `encoding.opcode`. A controller without a kind, with kController plus the controller
//                  in `encoding.opcode` and its value in `value`
//
// The driver ignores the second byte of a note off, of a channel pressure and of controllers 102, 103, 126 and 127.
// `encoding.arguments` holds that byte where it isn't 0. A bend's unit is a fraction of the range: 0x2000 is the
// centre, and 0x4000 a whole range up. The other values stay in the driver's units.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/agb/encoded.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/rare/driver.h"
#include "ntdlib/rare/song.h"

namespace ntdlib::rare
{

using ntdlib::agb::Rom;

// The flag in `encoding.opcode` of a controller without a kind.
inline constexpr uint16_t kController = 0x100;

// What a revision of the driver reads differently in a track.
struct Traits
{
    friend bool operator==(const Traits&, const Traits&) = default;

    Format format = Format::kChannelByte;
    bool envelope_controllers = false; // controllers 20 to 23 set the channel's envelope
};

// The traits of the driver that `info` describes.
Traits TraitsOf(const DriverInfo& info);

// What the decoder made of a sequence's bytes.
struct Decoded
{
    commands::Sequence sequence;
    std::vector<uint32_t> entries; // the item where each track starts, in the order of the tune's track list

    // The sequence can't be read safely: a command that the driver has no code for, commands that overlap, or a track
    // that runs past the bytes. The decoder then keeps the whole sequence as data, split at the tracks' starts, and
    // `reason` says why.
    bool opaque = false;
    std::string reason;
};

// Decodes `data`, a sequence's bytes from its start, with its tracks at the offsets `starts`. Throws
// std::invalid_argument for empty bytes or a start outside them.
Decoded DecodeSequence(std::span<const uint8_t> data, std::span<const uint32_t> starts, const Traits& traits);

// The sequences that a ROM's driver holds: one for each tune of the tune table.
int SequenceCount(const DriverInfo& info);

// The sequence that holds song `song`: the tune itself. Throws std::out_of_range for a song that the driver lacks.
int SequenceOfSong(const DriverInfo& info, int song);

// The address of sequence `sequence`'s first byte: its first track's start, or 0 where the tune's header can't be
// read. Throws std::out_of_range past SequenceCount().
uint32_t SequenceAddress(const Rom& rom, const DriverInfo& info, int sequence);

// Decodes sequence `sequence`. Its bytes run from its first track's start to the last byte that a track reads, and on
// to the next sequence's start where that lies less than kSequenceGap bytes further: a sequence can hold a few bytes
// that no track reaches after its tracks. Throws std::out_of_range past SequenceCount(), and std::invalid_argument for
// a tune whose header can't be read.
Decoded DecodeRomSequence(const Rom& rom, const DriverInfo& info, int sequence);

// The most bytes between a sequence's tracks and the next sequence's start that DecodeRomSequence takes for the
// sequence.
inline constexpr uint32_t kSequenceGap = 16;

// The bytes of a sequence, and each item's offset in them (agb/encoded.h).
using Encoded = agb::Encoded;

// Encodes `sequence`, with its tracks at the items `entries`, in the console's values nearest its units. Throws
// std::invalid_argument for a command that the format's bytes can't hold, an entry past the items, and a sequence that
// the driver wouldn't read as its items say: a track that reaches data, that reads a command that the items don't hold
// or that runs past the bytes.
Encoded EncodeSequence(const commands::Sequence& sequence, std::span<const uint32_t> entries, const Traits& traits);

// The offset of each item of `sequence` in its bytes, then the end. The offsets don't depend on where the tracks start,
// and the items don't have to read back as a sequence. Throws std::invalid_argument for a command that the format's
// bytes can't hold.
std::vector<uint32_t> ItemOffsets(const commands::Sequence& sequence, const Traits& traits);

// The scale of the value of a command of `kind` (commands::UnitOf). A note's velocity takes the unchanged scale.
commands::Scale ScaleOf(commands::Kind kind);

} // namespace ntdlib::rare
