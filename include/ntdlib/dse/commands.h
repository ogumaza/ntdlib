// SPDX-License-Identifier: MIT

// The tracks of DSE's songs and sound effects (formats.h) in the common form of `ntdlib::commands`. A sequence holds
// the items of a song's or a sound effect's track chunks, in the order of their bytes. Its bytes start at the first
// chunk's header. No event of DSE names a place in the bytes: a track's end goes back to the track's last loop point
// (0x99), and a repeat's end to the repeat's start.
//
// Each track starts with a kData item of 20 bytes: the chunk's header and the track's preamble. The encoder writes the
// chunk's size into its copy of those bytes, and pads the chunk to a multiple of 4 bytes with 0x98. A kData item after
// the track's end holds the bytes that the chunk's size counts and the driver never reads.
//
// An event's bytes depend on the track's state as the driver reads it. A note gives its octave as a change from the
// note before, and can leave its length out to take the length before. A wait can take the wait before, or change it.
// The decoder reads each track once through, and a note holds its key and its length, a wait its ticks. The encoder
// follows the same state, and writes the bytes that give each item's values. An edit therefore leaves the values of
// the items after it alone. In `Encoding`:
//
//   opcode       a wait's or a tempo's code, where it isn't the code that a new one takes, and the code of a track's
//                end other than 0x98: a code that the driver takes as invalid
//   arguments    a note's key and change of octave: the low 6 bits of its first argument, where they aren't the ones
//                that a new note takes
//   value_bytes  a note's bytes of length, where they aren't the fewest that hold it
//   repeated     a note leaves its length out
//
// value_bytes of 1 on a track's end marks the end that lies in the chunk's padding (Track::end_in_padding). The encoder
// keeps each of these while it still gives the item's values. A note whose octave lies out of reach of the note before
// starts its bytes with the command that sets the octave (0xA0). So does a new note that follows a loop's start (0x99)
// or a repeat's start (0x9C) with no such command between. The driver keeps a note's change of octave across a pass.
// The commands that the common form has no kind for are kOther, with the code in `encoding.opcode` and the arguments
// in `bytes`.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/dse/formats.h"

namespace ntdlib::dse
{

// The items of `tracks`.
commands::Sequence DecodeTracks(std::span<const Track> tracks);

// The item that starts a track of channel `channel` with the number `number`. A sequence's tracks follow one another,
// and each ends with kEnd.
commands::Command TrackOpening(uint8_t number, uint8_t channel);

// The number and the channel of a track, as the item that starts it gives them.
struct TrackStart
{
    friend bool operator==(const TrackStart&, const TrackStart&) = default;

    uint8_t number = 0;
    uint8_t channel = 0;
};

// The number and the channel of the track that `command` starts: a kData item of 20 bytes that starts with the label
// of a track chunk and the 8 bytes after it, as TrackOpening makes it. Nothing for another item.
std::optional<TrackStart> ReadTrackOpening(const commands::Command& command);

// Writes `start` into `opening`, an item that starts a track. Keeps its other bytes. Throws std::invalid_argument for
// an item that ReadTrackOpening doesn't read.
void SetTrackOpening(commands::Command& opening, const TrackStart& start);

// The bytes of a sequence, and each item's offset in them. `offsets` holds one more entry than the sequence has items:
// the end. A note that starts its bytes with the command that sets the octave has the offset of its code, where the
// driver's note event starts. Those two bytes then lie between the two items' offsets.
struct Encoded
{
    std::vector<uint8_t> bytes;
    std::vector<uint32_t> offsets;
};

// Encodes `sequence`, with the driver's values nearest its units. The tracks that DecodeTracks read come back as the
// file's writers lay them out. Throws std::invalid_argument for a sequence that doesn't start with a track, and for a
// command that the bytes can't hold: another kind, a value out of range, a kOther command without the arguments of its
// code, or a value from a random number, a variable, a condition or a time.
Encoded EncodeTracks(const commands::Sequence& sequence);

// The scale of the value of a command of `kind` (commands::UnitOf). A note's velocity takes the unchanged scale.
commands::Scale ScaleOf(commands::Kind kind);

} // namespace ntdlib::dse
