// SPDX-License-Identifier: MIT

// The performance of a piece of Krawall for an editor. ntdlib's model of the player (ntdlib::krawall::Player) plays the
// piece as ConvertSong does. The performance holds the notes of ConvertSong's MIDI file, each with the row's entry that
// started it. It also holds the stretches of rows that the module played, and the changes of each channel's volume and
// pan.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/krawall/driver.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/performance_base.h"

namespace ntdlib::sequence_export::krawall
{

// A recorded piece: PerformanceBase, with the records for an editor.
//
// The ticks are the MIDI file's: a tick of the player's takes the MIDI ticks that the conversion's timing gives it.
// The module's first 16 channels play on the tracks of the same numbers. A preset is the note's program in the
// conversion: a sample and the point it starts from. Every note has the velocity 127. The channel's levels give its
// loudness. Offsets count from `base`, the address of the module's first pattern. ntdlib::krawall::DecodeRomSequence
// decodes the bytes from there, and ntdlib::krawall::SequenceOfSong gives the sequence. In the modified build, the
// addresses lie in the unpacked modules (DriverInfo::unpacked).
struct Performance : PerformanceBase
{
    uint32_t base = 0;

    // Each track's stretches of rows, in the order that they played, and the entry of each note, at the note's ID less
    // 1 (TrackEvent::note). Every channel plays the module's rows, so each track that the module's channels reach holds
    // the same stretches. Each row that played is a stretch, at the row's tick. A row's speed sets its length, and a
    // speed effect can change it within a pattern. A stretch then holds its row's entries and the wait that ends the
    // row. A row that the rows reach out of turn (at an order's end, a break, a jump, a pattern loop or the module's
    // restart) has Entry::kJump when it comes later than the row before, and Entry::kLoop when it doesn't. Every other
    // row has Entry::kOpen. `first`
    // gives the offset of the item that holds the row's first byte. `last` gives the offset of the item that holds the
    // 0 that ends the row: a row's 0 lies in a wait. The player has no calls: each stretch's depth is 0. A note's
    // offset is that of the byte of flags of its channel's entry, where an item starts. `note_commands[i].stretch`
    // names the note's row in the stretches of the note's track.
    std::array<std::vector<Stretch>, 16> stretches;
    std::vector<NoteCommand> note_commands;

    // Each track's changes of its channel's volume (Mix::kVolume, from 0 to 64) and pan (Mix::kPan, from -64 for the
    // left to 64 for the right), as the player keeps them after a tick, in the order of their ticks. The changes at
    // tick 0 give the values after the first tick. The volume is the one that notes, the volume column and the volume
    // effects set. The module's global volume, the envelopes and the tremolo don't count.
    std::array<std::vector<MixChange>, 16> mix;
};

// Plays piece `song` as ConvertSong does with `settings`, and records its performance. Each note of the MIDI file that
// ConvertSong writes is a note of the performance. A module that can't be read gives a performance without tracks,
// with a warning. A piece that plays no note gives a performance with no notes, and its stretches and its mix stay.
// The notes of the channels past the 16th are left out, with a warning.
Performance PerformSong(const ntdlib::agb::Rom& rom, const ntdlib::krawall::DriverInfo& info, int song,
                        const agb::ConvertSettings& settings);

} // namespace ntdlib::sequence_export::krawall
