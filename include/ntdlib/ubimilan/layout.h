// SPDX-License-Identifier: MIT

// A GBA ROM of Ubisoft Milan's driver cut into pieces for an editor, and joined again, in the shapes of sar/layout.h.
// Each sequence resource of the sound bank that the decoder reads (commands.h) becomes a sequence piece, and the ROM's
// other bytes become runs. The resource table stays in a run. Its fields name each sequence: the distance from the
// bank's start to the sequence's first item. The pieces of a ROM join to give the ROM back byte for byte.
//
// The driver finds a sequence only through the resource table, and reads its bytes one at a time. A sequence can
// therefore lie anywhere in the ROM, on any byte. A sequence whose bytes fit its place stays there, with zeros after
// them. A sequence that outgrows its place moves. Its place then holds zeros, or another sequence that moved. A
// sequence that moves goes, largest first, into the first of these places with room for it, where neighbouring places
// count as one. Failing that, it goes into the free space at the ROM's end, and the ROM grows when the sequences there
// run past its end.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/ubimilan/driver.h"

namespace ntdlib::ubimilan
{

// The largest ROM that the GBA maps: 32 MiB.
inline constexpr uint32_t kLargestRom = 0x02000000;

// The bytes that the free space leaves clear of the data before it: data that happens to end with the run's byte.
inline constexpr uint32_t kFreeSpaceMargin = 256;

// Where the sequences that move can go, besides the places of the sequences that move.
struct FreeSpace
{
    friend bool operator==(const FreeSpace&, const FreeSpace&) = default;

    // Where the free space at the ROM's end starts, from the ROM's start. It lies 256 bytes or more into the run of
    // 0xFF or of zeros at the ROM's end, on a multiple of 256 bytes, and past every byte of that run that the driver
    // reads. Without such a run, it lies at the ROM's end, on a multiple of 4 bytes. It can lie past the ROM's end.
    uint32_t start = 0;
    uint8_t padding = 0xFF; // what fills the bytes that the ROM grows by, up to a sequence
};

struct Layout
{
    friend bool operator==(const Layout&, const Layout&) = default;

    std::vector<commands::Sequence> sequences; // in the order of their places
    std::vector<int> resources;                // the first resource that names each sequence
    std::vector<sar::Piece> pieces;            // in the order of the ROM's bytes: runs and sequence pieces

    // The resource table's fields that name sequences, in the order of the table. Each field holds 32 bits: the
    // distance from the bank's start, a place in a run, to the sequence's first item.
    std::vector<sar::Field> fields;
    FreeSpace free;
};

// Cuts `rom`, with the driver that `info` describes. Each sequence resource becomes a sequence piece once, when the
// decoder reads it without keeping it as data, when its encoding gives its bytes back, and when its bytes lie clear of
// every other sequence's, of the bank's header and of the resource table. Entries that name the same sequence share a
// piece. A sequence piece's room is the size of its bytes. A ROM without such a sequence becomes one run.
Layout Cut(const Rom& rom, const DriverInfo& info);

// The ROM's bytes from its pieces and its fields. See the top of this file for where each sequence goes. Throws
// std::invalid_argument for a piece of padding, a sequence piece without a sequence, a field that doesn't lie in a run
// or names a place that the pieces lack, and a ROM that would outgrow kLargestRom. Throws the exceptions of
// EncodeSequence.
std::vector<uint8_t> Join(std::span<const commands::Sequence> sequences, std::span<const sar::Piece> pieces,
                          std::span<const sar::Field> fields, const FreeSpace& free);

// Join with the parts of `layout`.
std::vector<uint8_t> Join(const Layout& layout);

} // namespace ntdlib::ubimilan
