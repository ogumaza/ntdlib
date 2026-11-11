// SPDX-License-Identifier: MIT

// A GBA ROM of Rare's driver cut into pieces for an editor, and joined again, in the shapes of sar/layout.h. Each
// tune's tracks that the decoder reads (commands.h) become a sequence piece, and the ROM's other bytes become runs. The
// tune table, the tunes' headers and their track lists stay in runs. Each address in a track list is a field that names
// the item where its track starts. The pieces of a ROM join to give the ROM back byte for byte.
//
// The driver finds a tune's tracks only through its track list, by their addresses. A sequence can therefore lie
// anywhere in the ROM. A sequence whose bytes fit its place stays there, with zeros after them. A sequence that
// outgrows its place moves. Its place then holds zeros, or another sequence that moved. A sequence that moves goes,
// largest first, into the first of these places with room for it, where neighbouring places count as one. Failing that,
// it goes into the free space at the ROM's end, and the ROM grows when the sequences there run past its end. A moved
// sequence lands on a multiple of 4 bytes.
//
// A tune keeps its tracks: the track list and the count of tracks in its header lie in runs, and Join writes only the
// tracks' addresses.
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
#include "ntdlib/rare/commands.h"
#include "ntdlib/rare/driver.h"
#include "ntdlib/sar/layout.h"

namespace ntdlib::rare
{

using ntdlib::agb::Rom;

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

    Traits traits;
    std::vector<commands::Sequence> sequences; // in the order of their places
    std::vector<sar::Piece> pieces;            // in the order of the ROM's bytes: runs and sequence pieces

    // The track lists' fields, in the order of the tunes and of their tracks. A field holds an address: the distance
    // from its second place, the ROM's start, to its third, the item where the track starts, plus 0x08000000.
    std::vector<sar::Field> fields;
    FreeSpace free;
};

// Cuts `rom`, with the driver that `info` describes. Each tune's tracks become a sequence piece once, when the decoder
// reads them without keeping them as data, when their encoding gives their bytes back, and when their bytes lie clear
// of every other sequence's and of the driver's other data: the tune table, the tunes' headers, track lists, program
// maps and instrument tables, the instruments, their key maps and samples, and the pitch and sine tables. Tunes whose
// track lists name the same tracks share a piece. A sequence piece's room is the size of its bytes. A ROM without such
// a sequence becomes one run.
Layout Cut(const Rom& rom, const DriverInfo& info);

// The ROM's bytes from its pieces and its fields. See the top of this file for where each sequence goes. Throws
// std::invalid_argument for a piece of padding, a sequence piece without a sequence, a ROM whose first piece isn't a
// run, a field that doesn't lie in a run or names a place that the pieces lack, an entry of a track that names no item
// of its sequence, and a ROM that would outgrow kLargestRom without room in the filler runs. Throws the exceptions of
// EncodeSequence.
std::vector<uint8_t> Join(const Traits& traits, std::span<const commands::Sequence> sequences,
                          std::span<const sar::Piece> pieces, std::span<const sar::Field> fields, const FreeSpace& free,
                          const JoinOptions& options = {});

// Join with the parts of `layout`.
std::vector<uint8_t> Join(const Layout& layout, const JoinOptions& options = {});

// The runs of filler bytes in `rom`, with the driver that `info` describes: runs of 0xFF in the order of their places,
// then runs of zeros in the order of their places. A filler run lies in a run of kLeastFillerRun or more bytes of 0xFF
// or of zeros. A run at the ROM's end is Join's free space, and doesn't count. The filler run starts kFreeSpaceMargin
// bytes or more into the run, on a multiple of kFreeSpaceMargin, and ends kFreeSpaceMargin bytes before the run's end.
// A run that holds a byte that the driver reads doesn't count. Zeros can be data of the game, such as blank pictures.
// Nothing shows that the game's other code doesn't read a filler run.
std::vector<Span> FillerRuns(const Rom& rom, const DriverInfo& info);

} // namespace ntdlib::rare
