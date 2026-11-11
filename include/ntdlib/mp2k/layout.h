// SPDX-License-Identifier: MIT

// A GBA ROM cut into pieces for an editor, and joined again, in the shapes of sar/layout.h. Each song that MP2K's
// decoder reads (commands.h) becomes a sequence piece: the bytes from its first track's start up to its header. The
// ROM's other bytes become runs. A song's header stays in the run after its piece. The fields hold addresses: each
// track's address in a header names the item where the track starts, and each entry of the song table names its
// song's header. The pieces of a ROM join to give the ROM back byte for byte.
//
// Code and tables all over a ROM hold addresses in it, so the pieces keep their places. A sequence piece's room is the
// size of its place (sar::SequencePiece::room). Join writes a song in its place when its bytes fit there, and zeros
// after them up to the header. A song that outgrows its place goes into the ROM's free space instead, with a copy of
// its header after it, and its place holds zeros. The song table then names the copy. The header in the run gives the
// tracks' new addresses too, for code that names it directly. The decoder reads a song that moved, for its header
// still follows its tracks.
//
// The fields of a header give its tracks (HeaderTracks). An editor adds a track with a field at the header's next
// place, and removes one by taking its field out and moving the fields after it up. A header in its place takes fewer
// tracks there, and the addresses after them stay as data. A header can't take more tracks in its place: the bytes
// after it hold other data. A song whose header gains tracks therefore goes into the free space, and the copy of its
// header holds every track. The header in the run keeps the tracks that it held.
//
// A ROM can lack room at its end: it fills the GBA's 32 MiB. Join can then put a song that outgrows its place into a
// run of filler bytes inside the ROM (FillerRuns), where the user allows it (JoinOptions::filler). Nothing shows that
// the game's other code doesn't read such a run.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/agb/layout.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/mp2k/driver.h"
#include "ntdlib/sar/layout.h"

namespace ntdlib::mp2k
{

using agb::kLargestRom;
using agb::Span;

// The free space where Join puts a song that outgrows its place: from a multiple of 256 bytes that lies 256 bytes or
// more into the run of 0xFF bytes at the ROM's end, or else from the ROM's end. The 256 bytes keep the songs clear of
// data at the run's start that happens to end with 0xFF.
using agb::kFreeSpaceMargin;

// Where Join's free space starts, from the ROM's start, in a ROM whose last bytes are `end`, from offset `offset`. Where
// `end` ends with a run of 0xFF bytes, the free space starts on the first multiple of kFreeSpaceMargin that lies
// kFreeSpaceMargin bytes or more into the run. The run counts only the bytes of `end`. Otherwise the free space starts
// at the ROM's end, rounded up to a multiple of 4 bytes.
uint64_t FreeSpaceStart(std::span<const uint8_t> end, uint64_t offset = 0);

// The least run of 0xFF bytes that FillerRuns takes: 16 KiB.
inline constexpr uint32_t kLeastFillerRun = 0x4000;

// How Join writes a ROM.
struct JoinOptions
{
    // A song that would take the ROM past kLargestRom goes into a run of filler bytes inside the ROM (FillerRuns).
    bool filler = false;

    // The bytes that a note reads at most: Layout::note_arguments.
    int note_arguments = 3;
};

struct Layout
{
    friend bool operator==(const Layout&, const Layout&) = default;

    std::vector<commands::Sequence> sequences; // in the order of their places
    std::vector<sar::Piece> pieces;            // in the order of the ROM's bytes: runs and sequence pieces
    std::vector<sar::Field> fields;            // the tracks' addresses in the songs' headers
    int note_arguments = 3; // the bytes that a note of the ROM's driver reads at most (NoteArguments)
};

// Cuts `rom`, with the driver that `info` describes. Each song of the song table becomes a sequence piece once, when
// the decoder reads it without keeping it as data, when its encoding gives its bytes back, and when its bytes and its
// header lie clear of every other such song's and of the song table. A field holds an address: the distance from its
// first place, the ROM's start, to its second, plus 0x08000000. The second place of a track's field is the item where
// the track starts, and that of a song table entry's field is the start of the run that holds the song's header. A ROM
// without such a song becomes one run. The songs decode with the driver's count of a note's arguments (NoteArguments),
// and the layout keeps it.
Layout Cut(const agb::Rom& rom, const DriverInfo& info);

// The tracks of the header of each sequence piece of `pieces`, by the piece's index: one for each place of a track's
// address up to the last field in the run after the piece that names an item of the piece. Track t's address lies 8 +
// 4t bytes into the header. A piece whose run holds no such field keeps the count of the header's first byte. A run's
// entry is 0.
std::vector<uint32_t> HeaderTracks(std::span<const sar::Piece> pieces, std::span<const sar::Field> fields);

// The ROM's bytes from its pieces and its fields. The sequences encode with `options.note_arguments`, the count that
// the layout keeps. Each sequence encodes at its place, or in the free space when its bytes outgrow the place or its
// header gains tracks (HeaderTracks). The run after a sequence piece starts with the song's header, and a song in the
// free space takes a copy of the header after its bytes. A field that names the run's start then names the copy, and a
// field in the header is written into the copy too. The header's first byte takes the count of its tracks, in the copy
// and, up to the count that the run holds, in its place. The address of a track past that count goes only into the
// copy. The songs in the free space follow one another on multiples of 4 bytes, in the order of their pieces, and the
// ROM grows when they run past its end. With `options.filler`, a song that would take the ROM past kLargestRom goes
// into the first filler run with room for it and its header, on a multiple of 4 bytes. The runs come from the ROM that
// the pieces give, with the songs that keep their places and the headers of the songs that move (FillerRuns).
// DetectDriver finds that ROM's driver. Throws std::invalid_argument for a piece of padding, a sequence piece without a
// sequence or a run after it that can't hold the header that its first byte gives, a header of more than kMaxTracks
// tracks, a field that doesn't lie in a run or names a place that the pieces lack, an address past 32 bits, and a ROM
// that would outgrow kLargestRom without room in the filler runs. Throws the exceptions of EncodeSequence.
std::vector<uint8_t> Join(std::span<const commands::Sequence> sequences, std::span<const sar::Piece> pieces,
                          std::span<const sar::Field> fields, const JoinOptions& options = {});

// The runs of filler bytes in `rom`, with the driver that `info` describes, in the order of their places. A filler run
// lies in a run of kLeastFillerRun or more bytes of 0xFF, the byte that unwritten flash and the tools' padding hold. A
// run at the ROM's end is Join's free space, and doesn't count. The filler run starts kFreeSpaceMargin bytes or more
// into the run, on a multiple of kFreeSpaceMargin, and ends kFreeSpaceMargin bytes before the run's end. A run that
// holds a byte that the driver reads doesn't count. Those bytes are the song table, the music players' table, each
// song's header, the commands that its tracks reach, the 128 voices of its voice group with their key and drum tables,
// and the samples and wave patterns that those voices play. Every voice of the group counts, whether or not a command
// selects it: an edit can select any of them. Nothing shows that the game's other code doesn't read a filler run.
std::vector<Span> FillerRuns(const agb::Rom& rom, const DriverInfo& info);

} // namespace ntdlib::mp2k
