// SPDX-License-Identifier: MIT

// The Standard MIDI Files that Nintendo R&D1's GBA driver plays, in the common form of `ntdlib::commands`. A sequence
// holds a song's whole MIDI file in the order of its bytes: the header chunk, then each track chunk. The decoder reads
// each track as the driver does, from its first delta time to its end of track. The encoder writes the items back, and
// gives the same bytes for a file that the decoder read.
//
// The chunks:
//   kTimebase   the header chunk. `value` is the division, in ticks a quarter note. `bytes` holds the chunk's bytes,
//               with zeros in place of the chunk's length, the number of tracks and the division. The encoder writes
//               those into its copy. A header without bytes takes 6 bytes of data and format 1.
//   kOpenTrack  a track chunk's header, with kTrackChunk in `encoding.opcode`, the track's number in `index` and the
//               item after it in `target`. `bytes` holds the chunk's 8 bytes of header, with zeros in place of the
//               chunk's length. The encoder writes the length into its copy, and numbers the tracks in order. A track
//               chunk without bytes takes "MTrk".
//   kData       the bytes after a track's end that its chunk's length counts. The driver never reads them.
//
// A track reads a delta time before each event. Each delta time is a kWait, a delta time of 0 included. Two waits in a
// row write their sum as one delta time, and an event without a wait before it takes a delta time of 0. The events:
//   kNote       9n with a velocity above 0: `key` and `velocity`. `value` is 0: the note sounds until a kNoteOff
//   kNoteOff    8n, with its release velocity in `velocity`, or 9n with a velocity of 0
//   kVolume     Bn 07
//   kPan        Bn 0A
//   kVolume2    Bn 0B: the expression
//   kLfoDepth   Bn 01: the modulation
//   kBendRange  Bn 14, in semitones
//   kLfoSpeed   Bn 15
//   kLfoType    Bn 16: what the LFO moves: 0 the pitch, 1 the volume, 2 the pan
//   kLfoDelay   Bn 1A, in frames
//   kPriority   Bn 21: added to the song's priority
//   kProgram    Cn
//   kPitchBend  En, 14 bits
//   kTempo      FF 51 with 3 bytes: `value` is the beats a minute that the driver takes from the microseconds a
//               quarter note, truncated. `bytes` holds the 3 bytes where they give the value
//   kLoopStart  FF 06 with the one byte '['
//   kLoopEnd    FF 06 with the one byte ']'
//   kEnd        FF 2F: the track's end. `bytes` holds its data, normally none
//   kOther      the other events, with their bytes after the status byte and the length in `bytes`: the other
//               controllers, An, Dn, F0 and F1 to FE, and the other meta events, with 0xFF00 plus the type in
//               `encoding.opcode`. So does a channel event whose bytes after its status byte aren't all below 0x80
//
// A channel event keeps its status byte in `encoding.opcode`: the driver ignores the channel, and plays the track's
// events on the track's channel. A new channel event takes the channel of the track's last channel event, or the low 4
// bits of the track's number. A kNoteOff with a status byte of 9n writes a note on with a velocity of 0. A new kNoteOff
// writes 8n.
//
// The driver keeps each status byte as the track's running status, meta events and system exclusive events included.
// An event whose status byte the file leaves out has `encoding.repeated` set. The encoder leaves the status byte out of
// such an event where the running status is the event's and the next byte is below 0x80. A delta time or a length
// that takes more bytes than it requires keeps them in `encoding.value_bytes`.
//
// A pan's unit is a position from the centre, at 64. A bend's unit is a fraction of the bend's range, and a bend
// range's unit is the cent. The other values stay in the driver's units.
//
// What the driver's bytes can't hold:
//   - A note's length. A note sounds from its kNote to the next kNoteOff of its key on the track's channel.
//   - More than 31 tracks. A player plays as many tracks as it has channels, and its song's other tracks never play.
//   - Calls, jumps and repeats. A song's one loop runs from the frame of its first '[' to its ']', and takes every
//     track back.
//   - Another bank. The programs come from the bank of the song's record.
//   - A key or a velocity above 127 in a kNote or a kNoteOff: the driver keeps 7 bits of each.
//   - Some tempos. A tempo's microseconds take 24 bits, and some large values of beats a minute have no microseconds
//     that give them.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/rd1/driver.h"

namespace ntdlib::rd1
{

// The flag in `encoding.opcode` of a track chunk's header.
inline constexpr uint16_t kTrackChunk = 0x100;

// The flag in `encoding.opcode` of a meta event of kOther, beside its type.
inline constexpr uint16_t kMetaEvent = 0xFF00;

// The most tracks that the driver reads from a file.
inline constexpr int kMaxTracks = 31;

// What the decoder made of a MIDI file's bytes.
struct Decoded
{
    commands::Sequence sequence;
    std::vector<uint32_t> tracks; // the item of each track chunk's header
    uint32_t size = 0;            // the file's bytes: its header chunk and its track chunks

    // The file can't be read safely: a header that the driver reads otherwise than its length says, a chunk that runs
    // past the bytes, a track that reads a byte before any status byte or past its chunk's end, or items that don't
    // give the bytes back. The decoder then keeps the whole file as one kData item, and `reason` says why.
    bool opaque = false;
    std::string reason;
};

// Decodes the MIDI file at the start of `data`. The bytes past the file's last track chunk aren't part of it. Throws
// std::invalid_argument for empty bytes.
Decoded DecodeSequence(std::span<const uint8_t> data);

// The MIDI files that the song table names: each file that a song's record names, once, in the order of the songs.
std::vector<uint32_t> SequenceAddresses(const Rom& rom, const DriverInfo& info);

// The number of MIDI files that the song table names.
int SequenceCount(const Rom& rom, const DriverInfo& info);

// The address of sequence `sequence`. Throws std::out_of_range past SequenceCount().
uint32_t SequenceAddress(const Rom& rom, const DriverInfo& info, int sequence);

// The sequence that song `song` plays. Throws std::out_of_range for an entry without a record that the driver plays.
int SequenceOfSong(const Rom& rom, const DriverInfo& info, int song);

// Decodes sequence `sequence`. Its bytes run from its start to the end of its last track chunk, or to the ROM's end.
// Throws std::out_of_range past SequenceCount().
Decoded DecodeRomSequence(const Rom& rom, const DriverInfo& info, int sequence);

// The bytes of a sequence, and each item's offset in them. `offsets` holds one more entry than the sequence has items:
// the end. An event's offset is that of its status byte, or of its first byte where it leaves the status byte out. A
// wait whose delta time an earlier wait writes has the offset of the next item.
struct Encoded
{
    std::vector<uint8_t> bytes;
    std::vector<uint32_t> offsets;
};

// Encodes `sequence`, in the driver's values nearest its units. Throws std::invalid_argument for a command that the
// bytes can't hold, and for a file that the driver wouldn't read as its items say: one that doesn't start with its
// header, more than kMaxTracks tracks, an event outside a track, a track without an end, a track that reaches data, or
// an event after a track's end.
Encoded EncodeSequence(const commands::Sequence& sequence);

// The offset of each item of `sequence` in its bytes, then the end. The items don't have to read back as a file. Throws
// std::invalid_argument for a command that the bytes can't hold.
std::vector<uint32_t> ItemOffsets(const commands::Sequence& sequence);

// The scale of the value of a command of `kind` (commands::UnitOf). A note's velocity takes the unchanged scale.
commands::Scale ScaleOf(commands::Kind kind);

} // namespace ntdlib::rd1
