// SPDX-License-Identifier: MIT

// The track commands of Konami's GBA driver in the common form of `ntdlib::commands`, for every revision of the driver
// (seqformat.h). A song is a run of bytes from its base, and each track starts at an offset from there. A track's bytes
// are <delay> { <command> <delay> }*. The decoder follows every track as the driver does: each call, each loop point
// and, where a track of the song loops it, each restart of the song. Each delay becomes an item of kWait, each command
// an item of its kind, and the bytes that no track reaches stay as data. The encoder writes the items back, and gives
// the same bytes for a song that the decoder read.
//
// The driver counts time in frames: one tick of a song is one frame of the driver's routine, at the GBA's 59.7275 Hz.
// A wait's value and a note's length are in those ticks. The driver's tempo never changes, and a song holds no tempo. A
// note's length stays 0: a note sounds until the track's next note, or its next rest (kNoteOff).
//
// The kinds:
//   kWait       a delay
//   kProgram    `index` 0: a duty byte, or in Dungeon Dice Monsters a track's sample; 1: a wave; 2: an entry of the
//               table of instruments that the game gives the driver (F6)
//   kNote       `velocity` is the volume. A sample note has `sample`, with its semitones as a byte in `key`, and in
//               Dungeon Dice Monsters `index` 1 where the next track plays it (Ax). A note without `sample` has its
//               note in `key`, and in Dungeon Dice Monsters `index` 1 for 9x: the track's sample at the note
//   kVolume     `index` 0: C0-CF; 1: Dungeon Dice Monsters' Dx; 2: Eternal Duelist's F0 on a sample track, with the next
//               track's volume in `velocity`
//   kNoteOff    a rest
//   kRelease    Dungeon Dice Monsters. `index` 0: FB; 1: a fade of this track (F8); 2: a fade of the next track (F9)
//   kPan        the Ultimate Masters builds: F0's position, with 0x40 the centre. The older revisions' F0 xy, left level
//               x and right level y: right minus left as the position, and the larger level in `velocity`. A new
//               command with a `velocity` of 0 takes level 15
//   kLfoDepth   the vibrato's depth
//   kPitchBend  `index` 0: F2, or in Dungeon Dice Monsters F3; 1: Dungeon Dice Monsters' F2; 2: its F1, for the next track
//   kLoopStart  the track's loop point. `bytes` holds the bytes that the driver skips after it
//   kLoopEnd    a loop of the song: every track goes back to its loop point, or to its start
//   kEnd        `index` 0: the end of the track, until a song loop; 1: the end for good (Ultimate Masters' FD); 2: a stop
//               of the song
//   kCall       `key` is the opcode and gives what the call sets first (CallSetsFirst), `value` the count of
//               commands, `target` the item where they start and `index` the track whose start the offset counts from
//   kAttack, kDecay, kEchoVolume (F7), kEchoLength (F8), kMute (FA in the later builds), kMainVolume (the song volume)
//               the command's byte
//   kOther      the opcode in `encoding.opcode` and its arguments in `bytes`: a command that does nothing, and the
//               commands without a kind, such as F9's echo route and the PSG's pan bits and volumes
//
// A command's bytes can say more than its meaning: most opcodes ignore some bits. Where a command's or a delay's bytes
// aren't the usual ones for its values, `encoding.opcode` holds kKeptBytes plus the first byte and `bytes` the others.
// The encoder writes those bytes while they read as the item's values. After an edit that changes the values, it writes
// the usual bytes.
//
// A pitch bend's unit is a fraction of a range of 2 semitones: the driver's value is in 32nds of a semitone, or 16ths in
// Dungeon Dice Monsters. The other values stay in the driver's units.
//
// A track's calls count from the track's start in the song table. Dungeon Dice Monsters' F5 sets a duty or a wave from
// the delay byte that follows it, and its FA's two bytes are a delay and a command that only a song loop reads. Those
// stay in the loop point's `bytes`.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/konami/driver.h"
#include "ntdlib/konami/seqformat.h"

namespace ntdlib::konami
{

// The flag in `encoding.opcode` of an item that keeps its bytes.
inline constexpr uint16_t kKeptBytes = 0x100;

// What the decoder made of a song's bytes.
struct Decoded
{
    commands::Sequence sequence;
    std::vector<uint32_t> entries; // the item where each track starts

    // The song can't be read safely: bytes that read both as a delay and as a command, commands that overlap, a command
    // that two tracks read with other values, a call that two tracks read with other targets, a track that runs past
    // the bytes, or an opcode that the driver doesn't read. The decoder then keeps the whole song as data, split at the
    // entries, and `reason` says why.
    bool opaque = false;
    std::string reason;
};

// Decodes `data`, a song's bytes from its base, with its tracks at the offsets `offsets`. Throws std::invalid_argument
// for an offset outside `data`.
Decoded DecodeSequence(std::span<const uint8_t> data, std::span<const uint16_t> offsets, Revision revision);

// Decodes song `song` of the song table. The song's bytes run from its base to the last byte that a track reads, and on
// to the next song's base where that lies less than kSongGap bytes further: a song can hold a few bytes that no track
// reaches after its tracks. Throws std::invalid_argument for a song whose entry is empty or names no byte of the ROM.
Decoded DecodeSong(const Rom& rom, const DriverInfo& info, int song);

// The most bytes between a song's tracks and the next song's base that DecodeSong takes for the song.
inline constexpr uint32_t kSongGap = 16;

// The bytes of a song, and each item's offset in them. `offsets` holds one more entry than the sequence has items: the
// end.
struct Encoded
{
    std::vector<uint8_t> bytes;
    std::vector<uint32_t> offsets;
};

// Encodes `sequence`, with its tracks at the items `entries`, in the console's values nearest its units. Throws
// std::invalid_argument for a command that the revision's bytes can't hold, a call's target before its track's start or
// more than 0xFFFF bytes after it, a track's start past 0xFFFF bytes, and a sequence that the driver wouldn't read as
// its items say: a track that reads a delay where a command lies, or the other way round, that reaches data or that
// runs past the bytes.
Encoded EncodeSequence(const commands::Sequence& sequence, std::span<const uint32_t> entries, Revision revision);

// The offset of each item of `sequence` in its bytes, then the end. The offsets don't depend on where the tracks start,
// and the items don't have to read back as a song. Throws std::invalid_argument for a command that the revision's bytes
// can't hold.
std::vector<uint32_t> ItemOffsets(const commands::Sequence& sequence, Revision revision);

// The scale of the value of a command of `kind` in revision `revision` (commands::UnitOf). A note's velocity takes the
// unchanged scale.
commands::Scale ScaleOf(commands::Kind kind, Revision revision);

// A new loop point for track `track`: the revision reads extra bytes after it on some tracks, and zeros fill them.
commands::Command NewLoopPoint(int track, Revision revision);

} // namespace ntdlib::konami
