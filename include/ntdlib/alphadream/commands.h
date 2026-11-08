// SPDX-License-Identifier: MIT

// The songs of AlphaDream's GBA driver in the common form of `ntdlib::commands`, for its three versions. A sequence
// holds one song: its header, then its tracks' bytes in the order of their places. The header is a 16-bit mask of the
// tracks that the song plays, then a 16-bit offset from the song's start for each of tracks 0 to 11 in the mask, in
// order. A track's bytes are commands, and its jumps go by 16-bit offsets from after the command.
//
// The decoder follows each track as the driver does, through both sides of each counted jump. Each command that a
// track reaches becomes an item. The bytes between them that no track reads stay as data. The encoder writes the items
// back, and gives the same bytes for a song that the decoder read.
//
// The driver counts time in ticks of the track's tempo: a tick passes each time a track's count passes the player's
// divisor. The values stay in the driver's units.
//
// The kinds:
//   kAllocateTracks  the header's mask, in `value`. Bits 12 to 15 play no track
//   kOpenTrack       a track's offset in the header: `index` is the track and `target` the item where it starts
//   kNote            a byte below 0xE0 or another byte that isn't a command: the note's length in ticks in `value`,
//                    then its key in `key`. A length of 0 ties the note to the next. The next note then changes the
//                    pitch without starting the note again. kRestFlag in `encoding.opcode` holds the key byte's bit 7.
//                    In B85A's and A84J's versions, the track's volume follows the key, in `velocity`
//   kWait            with `encoding.opcode` 0xF6: F6, a rest. Without an opcode, a byte of ticks that a note's bytes
//                    lead the driver to: the rest after a note that isn't tied, once a note that isn't tied has had
//                    bit 7 in its key; or the wait after a tied note with bit 7 in its key
//   kOther           E0 and E3, the pitch and the level LFO's depth, period and delay in `bytes`; E1, E2, E4 and E5,
//                    which turn them on and off. The opcode is in `encoding.opcode`
//   kJumpIfGreater   E8 to EB: adds 1 to counter `index` (0 to 3), and goes to `target` once the counter passes the
//                    count in `value`. While it doesn't, the counter goes back to 0
//   kJumpIfLessOrEqual  EC to EF: adds 1 to counter `index`, and goes to `target` and sets the counter to 0 while the
//                    count in `value` is at least the counter
//   kProgram         F0: the instrument
//   kVolume          F1 in A88E's version: the track's volume, 0 to 255
//   kPan             F2: 0x7F at the centre
//   kTune            F3: the fine tune, signed, in 256ths of a semitone
//   kBendRange       F4: in semitones
//   kPitchBend       F5: signed
//   kNoteOff         F7: the check for the end of a note. Every note that isn't tied has ended before the driver
//                    reads a command. The check leaves a tied note alone. F7 therefore ends no note
//   kJump            F8
//   kTempo           F9
//   kPriority        FA in B85A's version: the channel's priority against sound effects
//   kEnd             FF: the track's end
//   kData            bytes that no track reads
//
// A track's bytes after a note depend on what the track read before: once a note that isn't tied has bit 7 in its
// key, every later note that isn't tied takes a rest after it. Two paths can reach a note that isn't tied and has no
// bit 7, one with that state and one without. That note then takes a rest on one path and not on the other. The
// decoder then keeps the song as data.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/alphadream/driver.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"

namespace ntdlib::alphadream
{

// The flag in `encoding.opcode` of a kNote whose key byte has bit 7 set.
inline constexpr uint16_t kRestFlag = 0x80;

// What the decoder made of a song's bytes.
struct Decoded
{
    commands::Sequence sequence;
    std::vector<uint32_t> tracks; // the item where each track of the header starts, in the header's order

    // The bytes from the song's start to the end of the last byte that a track reads.
    uint32_t size = 0;

    // The song can't be read safely: it starts outside the ROM, or a track starts inside the header, runs past the
    // ROM, reads bytes that also read as another command, or reaches a note that takes a rest on one path and not on
    // another. The decoder then keeps the header's bytes and those up to the furthest byte read as one kData item. A
    // song that starts outside the ROM has no items. `reason` says why.
    bool opaque = false;
    std::string reason;
};

// Decodes the song at `address`, with the driver's version `version`.
Decoded DecodeSequence(const Rom& rom, Version version, uint32_t address);

// The songs that the song table names: each address once, in the order of the songs.
std::vector<uint32_t> SequenceAddresses(const Rom& rom, const DriverInfo& info);

// The number of songs that SequenceAddresses gives.
int SequenceCount(const Rom& rom, const DriverInfo& info);

// The address of sequence `sequence`. Throws std::out_of_range past SequenceCount().
uint32_t SequenceAddress(const Rom& rom, const DriverInfo& info, int sequence);

// The sequence that song `song` plays. Throws std::out_of_range for a song outside the table.
int SequenceOfSong(const Rom& rom, const DriverInfo& info, int song);

// Decodes sequence `sequence`. Throws std::out_of_range past SequenceCount().
Decoded DecodeRomSequence(const Rom& rom, const DriverInfo& info, int sequence);

// The bytes of a sequence, and each item's offset in them. `offsets` holds one more entry than the sequence has items:
// the end.
struct Encoded
{
    std::vector<uint8_t> bytes;
    std::vector<uint32_t> offsets;
};

// Encodes `sequence` for the driver's version `version`. Throws std::invalid_argument for a command that the version's
// bytes can't hold, for a target missing from the sequence, for a sequence that doesn't start with its header, for an
// offset past 16 bits, and for a song that the driver wouldn't read as its items say: a track that reaches the header,
// data, a byte of ticks without its note or the end of the items, a note without the byte of ticks that it takes after
// it, a note that takes a rest on one path and not on another, and a track that reads or jumps outside the 64 KiB from
// its start. Items that no track reaches stay in the bytes. The driver doesn't read them.
Encoded EncodeSequence(const commands::Sequence& sequence, Version version);

// The offset of each item of `sequence` in its bytes, then the end. Throws std::invalid_argument for a command that the
// version's bytes can't hold.
std::vector<uint32_t> ItemOffsets(const commands::Sequence& sequence, Version version);

// The scale of the value of a command of `kind` (commands::UnitOf): every value stays in the driver's units.
commands::Scale ScaleOf(commands::Kind kind);

// Whether a track's byte `first` starts a note in the driver's version `version`: every byte that isn't one of the
// version's commands.
bool StartsNote(uint8_t first, Version version);

// The bytes of the command or the note that starts with byte `first` in the driver's version `version`, without the
// byte of ticks that can follow a note.
uint32_t CommandSize(uint8_t first, Version version);

} // namespace ntdlib::alphadream
