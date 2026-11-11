// SPDX-License-Identifier: MIT

// The channel commands of Quintet's GBA driver in the common form of `ntdlib::commands`, for every revision of the
// driver. A song is a header and the commands of its 6 channels. The header holds the song's length and the offset of
// each channel's commands, and a sequence holds the bytes after it, up to the song's length. The driver reads each
// channel from its start, one command after another, up to FF, which ends the channel or goes back to its loop point.
// Repeats go back too. No command names a place, so the decoder reads each channel straight through from its start to
// its FF. Each command becomes an item of its kind, and the bytes that no channel reaches stay as data. The encoder
// writes the items back, and gives the same bytes for a sequence that the decoder read.
//
// A note's length is a note value: 1/n of a whole note of 384 ticks. In the A and J revisions, the note's high nibble
// picks n from the driver's table of 13 note values, and in the S revision the byte after the note gives n.
// `encoding.length_code` holds the table's index, or in the S revision the byte, where it isn't the lowest one that
// gives the note's ticks.
//
// The kinds, with the commands in the A revision's numbering. The S revision numbers its commands from 80 (CommandOfS):
//   kNote          a note: its pitch, 0 to 11, in `key`, and its length in ticks in `value`. `index` 1: play the last
//                  note again, or on a PCM channel the sample at its rate
//   kNoteOff       a rest, with its length in `value`
//   kWait          the S revision's notes of other pitches, which only wait: the pitch in `key`
//   kTempo         7F
//   kVolume        `index` 0: 5E; 1: F1, a signed step
//   kTranspose     `index` 0: E4; 1: E9, the second transpose. Signed
//   kTune          `index` 0: D5, a signed byte; 1: E6, a signed halfword
//   kProgram       `index` 0: DC, or DD with DD in `encoding.opcode`. The driver plays both the same; 1: DB, the noise
//                  macro; 2: DA, the wave
//   kTie           D4
//   kLoopStart     D6, the start of a repeat
//   kLoopEnd       D7: the passes of the repeat in `value`, 1 to 256. The byte 0 makes 256 passes
//   kEnd           FF
//   kOther         every other command: its number in `encoding.opcode`, and its arguments in `bytes`. In the S
//                  revision, B0, which skips a byte, and the bytes from BB to FE, which do nothing, have kKeptByte plus
//                  the byte in `encoding.opcode`
//
// The other values stay in the driver's units.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/agb/encoded.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/quintet/driver.h"

namespace ntdlib::quintet
{

using ntdlib::agb::Rom;

// The flag in `encoding.opcode` of a byte of the S revision that keeps its number.
inline constexpr uint16_t kKeptByte = 0x100;

// What a revision of the driver reads differently in a channel.
struct Traits
{
    friend bool operator==(const Traits&, const Traits&) = default;

    bool byte_notes = false;              // a note is a byte for its pitch and a byte for its note value
    std::array<uint8_t, 13> lengths = {}; // the note values that a note's high nibble picks
};

// The traits of the driver that `info` describes in `rom`.
Traits TraitsOf(const Rom& rom, const DriverInfo& info);

// What the decoder made of a song's bytes.
struct Decoded
{
    commands::Sequence sequence;
    std::vector<uint32_t> entries; // the item where each channel starts

    // The song can't be read safely: commands that overlap, or a channel that runs past the bytes. The decoder then
    // keeps the whole song as data, split at the channels' starts, and `reason` says why.
    bool opaque = false;
    std::string reason;
};

// The bytes of a song's header: its length and the offset of each channel's commands.
inline constexpr uint32_t kHeaderBytes = 14;

// Decodes `data`, a song's bytes after its header, with its channels at `starts` from there. Throws
// std::invalid_argument for a start outside `data`.
Decoded DecodeSequence(std::span<const uint8_t> data, std::span<const uint32_t> starts, const Traits& traits);

// The songs of the music file, then its sound effects: SongCount(info) of them. Song `song` plays sequence `song`.
int SequenceCount(const DriverInfo& info);
int SequenceOfSong(const DriverInfo& info, int song);

// Decodes sequence `sequence`. Throws std::out_of_range for a sequence that the file lacks, and std::invalid_argument
// for a song whose header the driver can't read.
Decoded DecodeRomSequence(const Rom& rom, const DriverInfo& info, int sequence);

// The bytes of a song after its header, and each item's offset in them (agb/encoded.h).
using Encoded = agb::Encoded;

// Encodes `sequence`, with its channels at the items `entries`. Throws std::invalid_argument for a command that the
// driver's bytes can't hold, such as a length that no note value gives, and for a sequence that the driver wouldn't
// read as its items say: a channel that reaches data or bytes that no item starts at, or that runs past the bytes.
Encoded EncodeSequence(const commands::Sequence& sequence, std::span<const uint32_t> entries, const Traits& traits);

// The offset of each item of `sequence` in its bytes, then the end. Throws std::invalid_argument for a command that the
// driver's bytes can't hold.
std::vector<uint32_t> ItemOffsets(const commands::Sequence& sequence, const Traits& traits);

// The scale of the value of a command of `kind` (commands::UnitOf): the driver's units.
commands::Scale ScaleOf(commands::Kind kind);

} // namespace ntdlib::quintet
