// SPDX-License-Identifier: MIT

// A GBA ROM of AlphaDream's driver cut into pieces for an editor, and joined again, in the shapes of sar/layout.h.
// Each song that the song table names and that the decoder reads (commands.h) becomes a sequence piece. The ROM's
// other bytes become runs: the song table, the table of sound effects and the effects, the instrument table and its
// regions, the sample table and the samples, the driver's tables and everything else. Each word of the song table
// that names a song piece becomes a field. The pieces of a ROM join to give the ROM back byte for byte.
//
// The driver finds a song only through the song table. A song can therefore be anywhere in the ROM, on a multiple of 2
// bytes. A song whose bytes fit its place stays there, with zeros after them. A song that outgrows its place moves.
// Its place then holds zeros, or another song that moved. A song that moves goes, largest first, onto a multiple of 4
// bytes in the first of these places with room for it, where neighbouring places count as one. Failing that, it goes
// into the free space at the ROM's end, and the ROM grows when the songs there run past its end.
//
// A ROM can have no room at its end: it fills the GBA's 32 MiB. Join can then put a song into a run of filler bytes
// inside the ROM (FillerRuns), where the user allows it (JoinOptions::filler). Nothing shows that the game's other code
// doesn't read such a run.
//
// The sound effects are songs in the same form. They stay in the runs, with the table that names them.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/alphadream/driver.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/sar/layout.h"

namespace ntdlib::alphadream
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

    // Where the free space at the ROM's end starts, from the ROM's start. It is 256 bytes or more into the run of
    // 0xFF or of zeros at the ROM's end, on a multiple of 256 bytes, and past every byte of that run that the driver
    // reads. Without such a run, it is at the ROM's end, on a multiple of 4 bytes. It can be past the ROM's end.
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

    Version version = Version::kSuperstarSaga;
    std::vector<commands::Sequence> sequences; // in the order of their places
    std::vector<sar::Piece> pieces;            // in the order of the ROM's bytes: runs and sequence pieces

    // The song table's words that name a song piece, in the order of their places. Each holds the address of its third
    // place, the song's first item: 0x08000000 plus the distance from its second place, the ROM's first byte.
    std::vector<sar::Field> fields;
    FreeSpace free;
};

// Cuts `rom`, with the driver that `info` describes. Each song that the song table names becomes a sequence piece once,
// when the decoder reads it without keeping it as data, when it starts on a multiple of 2 bytes, and when its bytes
// are clear of every other song and of the driver's other data. Words that name the same song share a piece. A
// sequence piece's room is the size of its bytes. A ROM without such a song becomes one run.
Layout Cut(const Rom& rom, const DriverInfo& info);

// The ROM's bytes from its pieces and its fields, with the songs encoded for the driver's version `version`. See the
// top of this file for where each song goes. Throws std::invalid_argument for a piece of padding, a sequence piece
// without a sequence, a field outside the runs, a field that names a place missing from the pieces, and a ROM that
// would outgrow kLargestRom without room in the filler runs. Throws the exceptions of EncodeSequence.
std::vector<uint8_t> Join(Version version, std::span<const commands::Sequence> sequences,
                          std::span<const sar::Piece> pieces, std::span<const sar::Field> fields, const FreeSpace& free,
                          const JoinOptions& options = {});

// Join with the parts of `layout`.
std::vector<uint8_t> Join(const Layout& layout, const JoinOptions& options = {});

// The runs of filler bytes in `rom`, with the driver that `info` describes: runs of 0xFF in the order of their places,
// then runs of zeros in the order of their places. A filler run is part of a run of kLeastFillerRun or more bytes of
// 0xFF or of zeros. A run at the ROM's end is Join's free space, and doesn't count. The filler run starts
// kFreeSpaceMargin bytes or more into the run, on a multiple of kFreeSpaceMargin, and ends kFreeSpaceMargin bytes
// before the run's end. A run that holds a byte that the driver reads doesn't count. Those bytes are the driver's
// tables, the song table, the songs, the table of sound effects and the effects, the instrument table and its regions,
// the sample table and the samples. Zeros can be data of the game, such as blank pictures. Nothing shows that the
// game's other code doesn't read a filler run.
std::vector<Span> FillerRuns(const Rom& rom, const DriverInfo& info);

} // namespace ntdlib::alphadream
