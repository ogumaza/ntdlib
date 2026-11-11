// SPDX-License-Identifier: MIT

// A GBA ROM of Nintendo R&D1's driver cut into pieces for an editor, and joined again, in the shapes of sar/layout.h.
// Each MIDI file that a song's record names and that the decoder reads (commands.h) becomes a sequence piece. The ROM's
// other bytes become runs: the song table, the records and their names, the bank table, the banks, the instrument
// records, the drum kits' and the key splits' tables, the samples, the driver's tables and everything else. Each
// record's first word holds its file's address, and becomes a field. The pieces of a ROM join to give the ROM back byte
// for byte.
//
// The driver finds a MIDI file only through a record. A file can therefore be anywhere in the ROM, on a multiple of 4
// bytes. A file whose bytes fit its place stays there, with zeros after them. A file that outgrows its place moves. Its
// place then holds zeros, or another file that moved. A file that moves goes, largest first, into the first of these
// places with room for it, where neighbouring places count as one. Failing that, it goes into the free space at the
// ROM's end, and the ROM grows when the files there run past its end.
//
// A ROM can have no room at its end: it fills the GBA's 32 MiB. Join can then put a file into a run of filler bytes
// inside the ROM (FillerRuns), where the user allows it (JoinOptions::filler). Nothing shows that the game's other code
// doesn't read such a run.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/agb/layout.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/rd1/driver.h"
#include "ntdlib/sar/layout.h"

namespace ntdlib::rd1
{

using agb::FreeSpace;
using agb::kFreeSpaceMargin;
using agb::kLargestRom;
using agb::Span;

// The least run of 0xFF or of zeros that FillerRuns takes: 4 KiB.
inline constexpr uint32_t kLeastFillerRun = 0x1000;

// How Join writes a ROM.
struct JoinOptions
{
    // A file that would take the ROM past kLargestRom goes into a run of filler bytes inside the ROM (FillerRuns).
    bool filler = false;
};

struct Layout
{
    friend bool operator==(const Layout&, const Layout&) = default;

    std::vector<commands::Sequence> sequences; // in the order of their places
    std::vector<sar::Piece> pieces;            // in the order of the ROM's bytes: runs and sequence pieces

    // The records' fields, in the order of the records' places. Each holds the address of its third place, the file's
    // first item: 0x08000000 plus the distance from its second place, the ROM's first byte.
    std::vector<sar::Field> fields;

    FreeSpace free;
};

// Cuts `rom`, with the driver that `info` describes. Each MIDI file that a record of the song table names becomes a
// sequence piece once, when the decoder reads it without keeping it as data, when it starts on a multiple of 4 bytes,
// and when its bytes are clear of every other file and of the driver's other data. Records that name the same file
// share a piece. A sequence piece's room is the size of its bytes. A ROM without such a file becomes one run.
Layout Cut(const Rom& rom, const DriverInfo& info);

// The ROM's bytes from its pieces and its fields. See the top of this file for where each file goes. Throws
// std::invalid_argument for a piece of padding, a sequence piece without a sequence, a field outside the runs, a field
// that names a place missing from the pieces, and a ROM that would outgrow kLargestRom without room in the filler runs.
// Throws the exceptions of EncodeSequence.
std::vector<uint8_t> Join(std::span<const commands::Sequence> sequences, std::span<const sar::Piece> pieces,
                          std::span<const sar::Field> fields, const FreeSpace& free, const JoinOptions& options = {});

// Join with the parts of `layout`.
std::vector<uint8_t> Join(const Layout& layout, const JoinOptions& options = {});

// The runs of filler bytes in `rom`, with the driver that `info` describes: runs of 0xFF in the order of their places,
// then runs of zeros in the order of their places. A filler run is part of a run of kLeastFillerRun or more bytes of
// 0xFF or of zeros. A run at the ROM's end is Join's free space, and doesn't count. The filler run starts
// kFreeSpaceMargin bytes or more into the run, on a multiple of kFreeSpaceMargin, and ends kFreeSpaceMargin bytes
// before the run's end. A run that holds a byte that the driver reads doesn't count. Those bytes are the driver's
// tables, the song table, the records and their names, the MIDI files, the banks, the instrument records, the kits' and
// the splits' tables and the samples. Zeros can be data of the game, such as blank pictures. Nothing shows that the
// game's other code doesn't read a filler run.
std::vector<Span> FillerRuns(const Rom& rom, const DriverInfo& info);

} // namespace ntdlib::rd1
