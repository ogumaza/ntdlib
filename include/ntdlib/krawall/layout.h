// SPDX-License-Identifier: MIT

// A GBA ROM with Krawall cut into pieces for an editor, and joined again, in the shapes of sar/layout.h. Each module's
// patterns that the decoder reads (commands.h) become a sequence piece, and the ROM's other bytes become runs. The
// module's header, which follows its patterns in the games, lies in a run, and so do the game's tables. Each address in
// a header's table of patterns is a field that names the item where its pattern starts. The pieces of a ROM join to
// give the ROM back byte for byte.
//
// The player finds a module's patterns only through the table of patterns, by their addresses. A sequence can therefore
// lie anywhere in the ROM. A sequence whose bytes fit its place stays there, with zeros after them. A sequence that
// outgrows its place moves. Its place then holds zeros, or another sequence that moved. A sequence that moves goes,
// largest first, into the first of these places with room for it, where neighbouring places count as one. Failing that,
// it goes into the free space at the ROM's end, and the ROM grows when the sequences there run past its end. A moved
// sequence lands on a multiple of 4 bytes, as the patterns' padding requires.
//
// A module keeps its patterns: its orders and its count of patterns lie in its header, and Join writes only the
// patterns' addresses. In the modified build, the modules lie packed in the game's music files. The cut leaves them
// in the bytes.
//
// A ROM can lack room at its end: it fills the GBA's 32 MiB. Join can then put a sequence into a run of filler bytes
// inside the ROM (FillerRuns), where the user allows it (JoinOptions::filler). Nothing shows that the game's other code
// doesn't read such a run.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/krawall/commands.h"
#include "ntdlib/krawall/driver.h"
#include "ntdlib/sar/layout.h"

namespace ntdlib::krawall
{

using ntdlib::agb::Rom;

// The largest ROM that the GBA maps: 32 MiB.
inline constexpr uint32_t kLargestRom = 0x02000000;

// The bytes that the free space and a filler run leave clear of the data before them: data that happens to end with the
// run's byte. A filler run also ends this many bytes before its run's end.
inline constexpr uint32_t kFreeSpaceMargin = 256;

// The least run of 0xFF or of zeros that FillerRuns takes: 4 KiB.
inline constexpr uint32_t kLeastFillerRun = 0x1000;

// `size` bytes of a ROM from `start`, counted from the ROM's start.
struct Span
{
    friend bool operator==(const Span&, const Span&) = default;

    uint32_t start = 0;
    uint32_t size = 0;
};

// Where the sequences that move can go, besides the places of the sequences that move.
struct FreeSpace
{
    friend bool operator==(const FreeSpace&, const FreeSpace&) = default;

    // Where the free space at the ROM's end starts, from the ROM's start. It lies 256 bytes or more into the run of
    // 0xFF or of zeros at the ROM's end, on a multiple of 256 bytes, and past every byte of that run that the player
    // reads. Without such a run, it lies at the ROM's end, on a multiple of 4 bytes. It can lie past the ROM's end.
    // It lies past the bytes past the ROM's end that the player reads, as it does the bytes of the run: an entry of
    // the game's table of samples near the ROM's end, for one of an instrument's notes that no row plays, can lie
    // there.
    uint32_t start = 0;
    uint8_t padding = 0xFF;   // what fills the bytes that the ROM grows by, up to a sequence
    std::vector<Span> filler; // FillerRuns
};

// How Join writes a ROM.
struct JoinOptions
{
    // A sequence that would take the ROM past kLargestRom goes into a run of filler bytes inside the ROM (FillerRuns).
    bool filler = false;
};

struct Layout
{
    friend bool operator==(const Layout&, const Layout&) = default;

    Traits traits;
    std::vector<commands::Sequence> sequences; // in the order of their places
    std::vector<sar::Piece> pieces;            // in the order of the ROM's bytes: runs and sequence pieces

    // The fields of the tables of patterns, in the order of the modules and of their patterns. A field holds an
    // address: the distance from its second place, the ROM's start, to its third, the item where the pattern starts,
    // plus 0x08000000.
    std::vector<sar::Field> fields;
    FreeSpace free;
};

// Cuts `rom`, with the player that `info` describes. Each module's patterns become a sequence piece when the decoder
// reads them without keeping them as data, when their encoding gives their bytes back, and when their bytes lie in the
// ROM, clear of every other sequence's and of the player's other data: the tables of modules and of what the game
// plays, the modules' headers, the samples and instruments that their rows name, and the player's tables of waves,
// periods, fine tunes and linear frequencies. A sequence piece's room is the size of its bytes. A ROM without such a
// sequence becomes one run.
Layout Cut(const Rom& rom, const DriverInfo& info);

// The ROM's bytes from its pieces and its fields. See the top of this file for where each sequence goes. Throws
// std::invalid_argument for a piece of padding, a sequence piece without a sequence, a ROM whose first piece isn't a
// run, a field that doesn't lie in a run or names a place that the pieces lack, an entry of a pattern that names no
// item of its sequence, and a ROM that would outgrow kLargestRom without room in the filler runs. Throws the exceptions
// of EncodeSequence.
std::vector<uint8_t> Join(const Traits& traits, std::span<const commands::Sequence> sequences,
                          std::span<const sar::Piece> pieces, std::span<const sar::Field> fields, const FreeSpace& free,
                          const JoinOptions& options = {});

// Join with the parts of `layout`.
std::vector<uint8_t> Join(const Layout& layout, const JoinOptions& options = {});

// The runs of filler bytes in `rom`, with the player that `info` describes: runs of 0xFF in the order of their places,
// then runs of zeros in the order of their places. A filler run lies in a run of kLeastFillerRun or more bytes of 0xFF
// or of zeros. A run at the ROM's end is Join's free space, and doesn't count. The filler run starts kFreeSpaceMargin
// bytes or more into the run, on a multiple of kFreeSpaceMargin, and ends kFreeSpaceMargin bytes before the run's end.
// A run that holds a byte that the player reads doesn't count. Zeros can be data of the game, such as blank pictures.
// Nothing shows that the game's other code doesn't read a filler run.
std::vector<Span> FillerRuns(const Rom& rom, const DriverInfo& info);

} // namespace ntdlib::krawall
