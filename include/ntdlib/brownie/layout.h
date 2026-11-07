// SPDX-License-Identifier: MIT

// A GBA ROM of Brownie Brown's driver cut into pieces for an editor, and joined again, in the shapes of sar/layout.h.
// Each region of the ROM's sounds (commands.h) becomes a sequence piece, and the ROM's other bytes become runs. The
// song table stays in a run. Its fields name each sound's list. The pieces of a ROM join to give the ROM back byte for
// byte.
//
// The driver finds a sound only through the song table, and every other address that a sound's bytes hold names a place
// in the sound's region. A region can therefore lie anywhere in the ROM. A region whose bytes fit its place stays
// there, with zeros after them. A region that outgrows its place moves, and its place then holds zeros, or another
// region that moved. A region that moves goes, largest first, into the first of these places with room for it, where
// neighbouring places count as one. Failing that, it goes into the free space at the ROM's end, and the ROM grows when
// the regions there run past its end. A region that moves keeps its start's remainder modulo 4, and its bytes keep
// their sizes.

#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/brownie/driver.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/sar/layout.h"

namespace ntdlib::brownie
{

// The largest ROM that the GBA maps: 32 MiB.
inline constexpr uint32_t kLargestRom = 0x02000000;

// The bytes that the free space leaves clear of the data before it: data that happens to end with the run's byte.
inline constexpr uint32_t kFreeSpaceMargin = 256;

// Where the regions that move can go, besides the places of the regions that move.
struct FreeSpace
{
    friend bool operator==(const FreeSpace&, const FreeSpace&) = default;

    // Where the free space at the ROM's end starts, from the ROM's start. It lies 256 bytes or more into the run of
    // 0xFF or of zeros at the ROM's end, on a multiple of 256 bytes, and past every byte of that run that the driver
    // reads. Without such a run, it lies at the ROM's end, on a multiple of 4 bytes. It can lie past the ROM's end.
    uint32_t start = 0;
    uint8_t padding = 0xFF; // what fills the bytes that the ROM grows by, up to a region
};

struct Layout
{
    friend bool operator==(const Layout&, const Layout&) = default;

    Revision revision = Revision::kSwordOfMana;
    std::vector<commands::Sequence> sequences; // in the order of their places
    std::vector<sar::Piece> pieces;            // in the order of the ROM's bytes: runs and sequence pieces

    // The song table's fields, in the order of its entries. A field holds an address: the distance from its first
    // place, the ROM's start, to its second, the item where the sound's list starts, plus 0x08000000.
    std::vector<sar::Field> fields;
    FreeSpace free;
};

// Cuts `rom`, with the driver that `info` describes. Each region becomes a sequence piece, when the decoder reads it
// without keeping it as data, when its encoding gives its bytes back, and when its bytes lie clear of the song table. A
// sequence piece's room is the size of its bytes. A ROM without such a region becomes one run.
Layout Cut(const Rom& rom, const DriverInfo& info);

// Where a sound's list lies.
struct SoundPlace
{
    friend bool operator==(const SoundPlace&, const SoundPlace&) = default;

    uint32_t sequence = 0; // the sequence, in Layout::sequences
    uint32_t list = 0;     // the item where the list starts (TracksOfList)
};

// The place of each sound of the song table that has a field, by the sound's number.
std::map<int, SoundPlace> SoundPlaces(const Layout& layout, const DriverInfo& info);

// The ROM's bytes from its pieces and its fields. See the top of this file for where each region goes. Throws
// std::invalid_argument for a piece of padding, a sequence piece without a sequence, a field that doesn't lie in a run
// or names a place that the pieces lack, and a ROM that would outgrow kLargestRom. Throws the exceptions of
// EncodeSequence.
std::vector<uint8_t> Join(Revision revision, std::span<const commands::Sequence> sequences,
                          std::span<const sar::Piece> pieces, std::span<const sar::Field> fields,
                          const FreeSpace& free);

// Join with the parts of `layout`.
std::vector<uint8_t> Join(const Layout& layout);

} // namespace ntdlib::brownie
