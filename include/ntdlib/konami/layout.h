// SPDX-License-Identifier: MIT

// A GBA ROM of Konami's driver cut into pieces for an editor, and joined again, in the shapes of sar/layout.h. Each
// song that the decoder reads (commands.h) becomes a sequence piece, and the ROM's other bytes become runs. The song
// table stays in a run. Its fields name each song: a song's base through a field of 32 bits, and the start of each of
// its tracks through a field of 16 bits. The pieces of a ROM join to give the ROM back byte for byte.
//
// The driver finds a song only through the song table, and a song's bytes count from its base. A song can therefore lie
// anywhere in the ROM, on any byte. A song whose bytes fit its place stays there, with zeros after them. A song that
// outgrows its place moves. Its place then holds zeros, or another song that moved. A song that moves goes, largest
// first, into the first of these places with room for it, where neighbouring places count as one. Failing that, it goes
// into the free space at the ROM's end, and the ROM grows when the songs there run past its end.
//
// A ROM can lack room at its end: it fills the GBA's 32 MiB. Join can then put a song into a run of filler bytes inside
// the ROM (FillerRuns), where the user allows it (JoinOptions::filler). Nothing shows that the game's other code
// doesn't read such a run.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/konami/driver.h"
#include "ntdlib/konami/seqformat.h"
#include "ntdlib/sar/layout.h"

namespace ntdlib::konami
{

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

// Where the songs that move can go, besides the places of the songs that move.
struct FreeSpace
{
    friend bool operator==(const FreeSpace&, const FreeSpace&) = default;

    // Where the free space at the ROM's end starts, from the ROM's start. It lies 256 bytes or more into the run of
    // 0xFF or of zeros at the ROM's end, on a multiple of 256 bytes, and past every byte of that run that the driver
    // reads. Without such a run, it lies at the ROM's end, on a multiple of 4 bytes. It can lie past the ROM's end.
    uint32_t start = 0;
    uint8_t padding = 0xFF;   // what fills the bytes that the ROM grows by, up to a song
    std::vector<Span> filler; // FillerRuns
};

// How Join writes a ROM.
struct JoinOptions
{
    // A song that would take the ROM past kLargestRom goes into a run of filler bytes inside the ROM (FillerRuns).
    bool filler = false;
};

struct Layout
{
    friend bool operator==(const Layout&, const Layout&) = default;

    Revision revision = Revision::kUltimateMasters;
    std::vector<commands::Sequence> sequences; // in the order of their places
    std::vector<sar::Piece> pieces;            // in the order of the ROM's bytes: runs and sequence pieces

    // The song table's fields, in the order of its entries: each entry's base, then the start of each of its tracks.
    // A base's field holds an address: the distance from its first place, the ROM's start, to its second, the song's
    // first item, plus 0x08000000. A track's field holds the distance from the song's first item to the item where the
    // track starts, in 16 bits.
    std::vector<sar::Field> fields;
    FreeSpace free;
};

// Cuts `rom`, with the driver that `info` describes. Each song of the song table becomes a sequence piece once, when
// the decoder reads it without keeping it as data, when its encoding gives its bytes back, and when its bytes lie clear
// of every other song's and of the song table. Entries that give the same base and the same tracks share a piece. A
// sequence piece's room is the size of its bytes. A ROM without such a song becomes one run.
Layout Cut(const Rom& rom, const DriverInfo& info);

// The item where each track of a sequence starts, by the index of the sequence's piece, from `fields`: a song's fields
// of 16 bits give its tracks in order. Takes as many fields as revision `revision`'s songs have tracks. A run's entry
// is empty.
std::vector<std::vector<uint32_t>> TrackEntries(std::span<const sar::Piece> pieces, std::span<const sar::Field> fields,
                                                Revision revision);

// The ROM's bytes from its pieces and its fields. See the top of this file for where each song goes. Throws
// std::invalid_argument for a piece of padding, a sequence piece without a sequence, a field that doesn't lie in a run
// or names a place that the pieces lack, a song's track past 0xFFFF bytes from its base, and a ROM that would outgrow
// kLargestRom without room in the filler runs. Throws the exceptions of EncodeSequence.
std::vector<uint8_t> Join(Revision revision, std::span<const commands::Sequence> sequences,
                          std::span<const sar::Piece> pieces, std::span<const sar::Field> fields, const FreeSpace& free,
                          const JoinOptions& options = {});

// Join with the parts of `layout`.
std::vector<uint8_t> Join(const Layout& layout, const JoinOptions& options = {});

// The runs of filler bytes in `rom`, with the driver that `info` describes: runs of 0xFF in the order of their places,
// then runs of zeros in the order of their places. A filler run lies in a run of kLeastFillerRun or more bytes of 0xFF
// or of zeros. A run at the ROM's end is Join's free space, and doesn't count. The filler run starts kFreeSpaceMargin
// bytes or more into the run, on a multiple of kFreeSpaceMargin, and ends kFreeSpaceMargin bytes before the run's end.
// A run that holds a byte that the driver reads doesn't count. Those bytes are the song table, each song's bytes, the
// sample tables with every sample that they name, and the driver's other tables. Zeros can be data of the game, such as
// blank pictures. Nothing shows that the game's other code doesn't read a filler run.
std::vector<Span> FillerRuns(const Rom& rom, const DriverInfo& info);

} // namespace ntdlib::konami
