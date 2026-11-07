// SPDX-License-Identifier: MIT

// A GBA ROM of Quintet's driver cut into pieces for an editor, and joined again, in the shapes of sar/layout.h. The
// driver's songs lie back to back in a list: the songs of a music file, or its sound effects. The game finds a song by
// skipping the songs before it, each by the length in its header. A list therefore keeps its place, and its songs keep
// their order. Each song's commands that the decoder reads (commands.h) become a sequence piece, and its header a run
// of 14 bytes before it. The header's length and the offsets of its channels are fields of 2 bytes. A song that the cut
// leaves in the bytes stays a run, header and all, and moves with the list. The ROM's other bytes become runs, and keep
// their places. The pieces of a ROM join to give the ROM back byte for byte.
//
// A list has a room: the bytes from its first song to the end of the run of one byte after it, in a music file up to
// the end of the file's part. A list that shrinks fills the end of its room with that byte. A list can grow into its
// room, and no further: the songs can't move out of their list, and the parts of a music file can't move. A song's
// bytes take an even size, as the driver skips a song by its length without its lowest bit: Join adds a zero after an
// odd sequence.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/quintet/commands.h"
#include "ntdlib/quintet/driver.h"
#include "ntdlib/sar/layout.h"

namespace ntdlib::quintet
{

using ntdlib::agb::Rom;

// The end of a list's room.
struct Room
{
    friend bool operator==(const Room&, const Room&) = default;

    uint32_t piece = 0; // the first piece after the list, which starts at `end`
    uint32_t end = 0;   // the room's end, from the ROM's start
    uint8_t fill = 0;   // the byte that fills the room after the list's last piece
};

struct Layout
{
    friend bool operator==(const Layout&, const Layout&) = default;

    Traits traits;
    std::vector<commands::Sequence> sequences; // in the order of their places
    std::vector<sar::Piece> pieces;            // in the order of the ROM's bytes: runs and sequence pieces

    // The fields of the songs' headers: each song's length, then the offset of each of its channels. A field holds the
    // distance from its second place, the song's header, to its third: the next piece for the length, and the item
    // where the channel starts for an offset. The fields are 2 bytes.
    std::vector<sar::Field> fields;
    std::vector<Room> rooms; // in the order of their places
};

// Cuts `rom`, with the music files that `files` describe (ReadMusicFiles). The songs and the sound effects of each
// file are lists. A song becomes a sequence piece when the decoder reads it without keeping it as data, when its
// encoding gives its bytes back and when the length in its header is even. A ROM without such a song becomes one run.
Layout Cut(const Rom& rom, std::span<const DriverInfo> files);

// The ROM's bytes from its pieces, its fields and its rooms. Throws std::invalid_argument for a piece of padding, a
// sequence piece without a sequence, a field that doesn't lie in a run or names a place that the pieces lack, a
// distance past 2 bytes, an entry of a channel that names no item of its sequence, and a list that outgrows its room.
// Throws the exceptions of EncodeSequence.
std::vector<uint8_t> Join(const Traits& traits, std::span<const commands::Sequence> sequences,
                          std::span<const sar::Piece> pieces, std::span<const sar::Field> fields,
                          std::span<const Room> rooms);

// Join with the parts of `layout`.
std::vector<uint8_t> Join(const Layout& layout);

} // namespace ntdlib::quintet
