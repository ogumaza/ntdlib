// SPDX-License-Identifier: MIT

// A GBA ROM of Nintendo R&D2's driver cut into pieces for an editor, and joined again, in the shapes of sar/layout.h.
// Each sequence that the decoder reads (commands.h) becomes a sequence piece, and the ROM's other bytes become runs.
// The sequences' tables stay in runs, and their fields name each sequence. The pieces of a ROM join to give the ROM
// back byte for byte.
//
// The driver finds a sequence only through its table, and a sequence's bytes count from its start. A sequence can
// therefore lie anywhere after its table, on a multiple of 4 bytes: the driver reads its header's halfwords, and
// detection reads a table's first entry for the table's size. A sequence whose bytes fit its place stays there, with
// zeros after them. A sequence that outgrows its place moves. Its place then holds zeros, or another sequence that
// moved. A sequence that moves goes, largest first, into the first of these places with room for it, where neighbouring
// places count as one. Failing that, it goes into the free space at the ROM's end, and the ROM grows when the sequences
// there run past its end.
//
// A ROM can lack room at its end: it fills the GBA's 32 MiB. Join can then put a sequence into a run of filler bytes
// inside the ROM (FillerRuns), where the user allows it (JoinOptions::filler). Nothing shows that the game's other code
// doesn't read such a run.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/agb/layout.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/rd2/driver.h"
#include "ntdlib/sar/layout.h"

namespace ntdlib::rd2
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
    // A sequence that would take the ROM past kLargestRom goes into a run of filler bytes inside the ROM (FillerRuns).
    bool filler = false;
};

struct Layout
{
    friend bool operator==(const Layout&, const Layout&) = default;

    Revision revision = Revision::kLinkToThePast;
    std::vector<commands::Sequence> sequences; // in the order of their places
    std::vector<sar::Piece> pieces;            // in the order of the ROM's bytes: runs and sequence pieces

    // The tables' fields, in the order of the sequences and then of the sound effects' sequences. Each holds the
    // distance from its second place, the table's start, to its third, the sequence's first item, in 32 bits.
    std::vector<sar::Field> fields;
    FreeSpace free;
};

// Cuts `rom`, with the driver that `info` describes. Each sequence of the tables becomes a sequence piece once, when
// the decoder reads it without keeping it as data, when its encoding gives its bytes back, and when its bytes lie clear
// of every other sequence's and of the driver's tables, banks and sample sets. Entries that name the same address share
// a piece. A sequence piece's room is the size of its bytes. A ROM without such a sequence becomes one run.
Layout Cut(const Rom& rom, const DriverInfo& info);

// The ROM's bytes from its pieces and its fields. See the top of this file for where each sequence goes. Throws
// std::invalid_argument for a piece of padding, a sequence piece without a sequence, a field that doesn't lie in a run
// or names a place that the pieces lack, and a ROM that would outgrow kLargestRom without room in the filler runs.
// Throws the exceptions of EncodeSequence.
std::vector<uint8_t> Join(Revision revision, std::span<const commands::Sequence> sequences,
                          std::span<const sar::Piece> pieces, std::span<const sar::Field> fields, const FreeSpace& free,
                          const JoinOptions& options = {});

// Join with the parts of `layout`.
std::vector<uint8_t> Join(const Layout& layout, const JoinOptions& options = {});

// The runs of filler bytes in `rom`, with the driver that `info` describes: runs of 0xFF in the order of their places,
// then runs of zeros in the order of their places. A filler run lies in a run of kLeastFillerRun or more bytes of 0xFF
// or of zeros. A run at the ROM's end is Join's free space, and doesn't count. The filler run starts kFreeSpaceMargin
// bytes or more into the run, on a multiple of kFreeSpaceMargin, and ends kFreeSpaceMargin bytes before the run's end.
// A run that holds a byte that the driver reads doesn't count. Those bytes are the settings, the tables, each
// sequence's bytes, the 64 KiB that a bank's offsets reach, and every sample that a sample set names. Zeros can be data
// of the game, such as blank pictures. Nothing shows that the game's other code doesn't read a filler run.
std::vector<Span> FillerRuns(const Rom& rom, const DriverInfo& info);

} // namespace ntdlib::rd2
