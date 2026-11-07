// SPDX-License-Identifier: MIT

// The track commands of Nintendo R&D2's GBA driver in the common form of `ntdlib::commands`, for each revision of the
// driver. A sequence is a run of bytes that counts every offset from its start. It starts with a header. The decoder
// follows each track of the header as the driver does: each jump, each call and its return, and each track that F8
// starts. Each command becomes an item of its kind, and the bytes that no track reaches stay as data. The encoder
// writes the items back, and gives the same bytes for a sequence that the decoder read.
//
// A music sequence's header holds a signed count of tracks and a byte of 0. The two stay a kData item of 2 bytes. A
// halfword for each track follows. A sound effects' sequence holds only the halfwords, up to the place where its first
// track starts. Each halfword is an item of kOpenTrack, with kHeaderEntry in `encoding.opcode` and the track's number
// in `index`. An offset of 0 names no track: its item has `target_outside` set and `target` 0.
//
// The kinds:
//   kNote             00-5F and 60-BF: `key` is the note, `velocity` its velocity and `value` its length
//   kWait             C0 and C1
//   kProgram          C2: the instrument
//   kPan              C3
//   kPriority         C4
//   kTie              C5 with `value` 1 and C6 with `value` 0: legato on or off, after a release of the track's notes
//   kBankSelect       C7
//   kNoteWait         C8 with `value` 1 and C9 with `value` 0: whether notes wait for their lengths
//   kUserProcedure    CA: `value` goes to the game's routine
//   kPortamentoFrom   Dx: a slide for the next note. `index` is x, `key` the other note, `value` the slide's length in
//                     256ths of the note's, and `velocity` the delay in frames, read where x is odd
//   kVolume           E0
//   kPitchBend        E1: a fraction of the bend's range, in 128ths
//   kBendRange        E2
//   kSend             E3: the echo send
//   kTempo            E4: 150 plays a tick a frame
//   kLfoDelay         E5, in frames
//   kLfoSpeed         E6
//   kLfoDepth         E7
//   kPortamento       E8: no slide for the next note
//   kTranspose        E9
//   kMainVolume       EA: the player's volume. The Super Mario Advance 2 revision reads no operand there
//   kJump             F0
//   kCall             F4
//   kOpenTrack        F8: starts track `index` at `target`
//   kReturn           FF that a track reaches only inside a call
//   kEnd              FF that a track reaches outside every call
//   kOther            the other bytes from C2 up, in `encoding.opcode`. They do nothing and take no operand
//
// A note of 00-5F and a wait of C0 take their values from the track's last note and wait, and such an item has
// `encoding.repeated` set. Its values are those that the track's first reading of it gives. The encoder ignores them
// and writes the one byte. An edit of such a value clears `repeated` too, and the encoder then writes the values. A
// length takes a byte, or two with the first byte's top bit set. `encoding.value_bytes` holds 2 where a length takes
// two bytes and requires one.
//
// A pan's unit is a position from the centre, at 64. A bend's unit is a fraction of the bend's range, and a transpose's
// and a bend range's unit is the cent. The other values stay in the driver's units.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/rd2/driver.h"

namespace ntdlib::rd2
{

// The flag in `encoding.opcode` of a halfword of a sequence's header.
inline constexpr uint16_t kHeaderEntry = 0x100;

// The entry of a track that the header leaves out with an offset of 0.
inline constexpr uint32_t kNoEntry = 0xFFFFFFFF;

// What the decoder made of a sequence's bytes.
struct Decoded
{
    commands::Sequence sequence;
    std::vector<uint32_t> entries; // the item where each track of the header starts, or kNoEntry

    // The sequence can't be read safely: a header that names more tracks than a player holds or a place outside the
    // bytes, commands that overlap, a track that reaches the header or that runs past the bytes. The decoder then keeps
    // the whole sequence as data, split at the entries, and `reason` says why.
    bool opaque = false;
    std::string reason;
};

// Decodes `data`, a sequence's bytes from its start, as a sound effects' sequence with `effects`. Throws
// std::invalid_argument for empty bytes.
Decoded DecodeSequence(std::span<const uint8_t> data, Revision revision, bool effects = false);

// The sequences that a ROM's driver holds: the sequences, then the sound effects' sequences.
int SequenceCount(const DriverInfo& info);

// The address of sequence `sequence`. Throws std::out_of_range past SequenceCount().
uint32_t SequenceAddress(const DriverInfo& info, int sequence);

// Whether sequence `sequence` is one of the sound effects' sequences.
bool IsEffectSequence(const DriverInfo& info, int sequence);

// The sequence that holds song `song`: a music sequence, or for a sound effect the sequence of its track. Throws
// std::out_of_range for a song that the driver lacks.
int SequenceOfSong(const DriverInfo& info, int song);

// Decodes sequence `sequence`. Its bytes run from its start to the last byte that a track reads, and on to the next
// sequence's start where that lies less than kSequenceGap bytes further: a sequence can hold a few bytes that no track
// reaches after its tracks. Throws std::out_of_range past SequenceCount(), and std::invalid_argument for a sequence
// that names no byte of the ROM.
Decoded DecodeRomSequence(const Rom& rom, const DriverInfo& info, int sequence);

// The most bytes between a sequence's tracks and the next sequence's start that DecodeRomSequence takes for the
// sequence.
inline constexpr uint32_t kSequenceGap = 16;

// The bytes of a sequence, and each item's offset in them. `offsets` holds one more entry than the sequence has items:
// the end.
struct Encoded
{
    std::vector<uint8_t> bytes;
    std::vector<uint32_t> offsets;
};

// Encodes `sequence`, in the console's values nearest its units. Throws std::invalid_argument for a command that the
// revision's bytes can't hold, a target more than 0xFFFF bytes from the sequence's start, and a sequence that the
// driver wouldn't read as its items say: a track that reaches data, or that reads a command that the items don't hold.
Encoded EncodeSequence(const commands::Sequence& sequence, Revision revision);

// The offset of each item of `sequence` in its bytes, then the end. The offsets don't depend on where the tracks start,
// and the items don't have to read back as a sequence. Throws std::invalid_argument for a command that the revision's
// bytes can't hold.
std::vector<uint32_t> ItemOffsets(const commands::Sequence& sequence, Revision revision);

// The scale of the value of a command of `kind` (commands::UnitOf). A note's velocity takes the unchanged scale.
commands::Scale ScaleOf(commands::Kind kind);

} // namespace ntdlib::rd2
