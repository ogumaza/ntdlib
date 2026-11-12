// SPDX-License-Identifier: MIT

// The beat and the bars of a module's MIDI file, for the conversions of the drivers that play modules: the choice
// between 4/4 and 12/8, the bars that the patterns give, and the time signatures.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ntdlib/sequence_export/agb/midi.h"

namespace ntdlib::sequence_export::agb
{

// A step of a module's playback that a conversion gives a MIDI tick: a tick of the player's, or a row.
struct ModuleStep
{
    bool row = false;       // a row starts on the step
    int order = 0;          // the row's order
    int pattern_row = 0;    // the row's number in its pattern
    uint32_t row_ticks = 0; // the MIDI ticks of the step's row
};

// A run of rows within one order. A new run starts when the order changes or playback returns to the same or an earlier
// row.
struct OrderRun
{
    size_t start = 0; // the step of its first row
    size_t steps = 0; // the steps to the next run, or to the end
    int order = 0;
    int first_row = 0; // its first row's number in the pattern, which a break can make more than 0
};

// Returns the runs of rows that the module's orders play.
std::vector<OrderRun> OrderRuns(const std::vector<ModuleStep>& steps);

// MIDI ticks per quarter note and the choice of 4/4 or 12/8. In 12/8, the beat is a dotted quarter note.
struct Meter
{
    uint32_t quarter_ticks = 1;
    bool compound = false;
};

// Returns the ticks in a full bar of `meter`.
size_t BarTicks(const Meter& meter);

// Chooses a beat and meter using row lengths and pattern boundaries. In 4/4, a quarter note spans a power-of-two number
// of rows; in 12/8, each eighth note does. The target beat tempo is at least 105 BPM and below 210, with `row_ticks`
// MIDI ticks per row and `tick_seconds` seconds per MIDI tick. 12/8 wins when more of the runs' MIDI ticks, in `ticks`,
// fit whole bars of it than of 4/4, as 48-row patterns do at two rows per eighth note. `ticks` holds the MIDI tick of
// each step, and of the end. The count leaves out the first run, which may be a lead-in, and the last, which may end
// early.
Meter ChooseMeter(const std::vector<uint32_t>& ticks, const std::vector<OrderRun>& runs, uint32_t row_ticks,
                  double tick_seconds);

// Returns bar start positions in MIDI ticks, with `bar` MIDI ticks in a full bar. Bars align to each pattern's first
// row, and any rows that a run skips count as long as its first row. A pattern that ends early has a short final bar.
// If a break or jump cuts the first order short, as with a game's lead-in, its partial bar comes at the start instead,
// as a pickup, each time the first order plays.
std::vector<uint32_t> BarStarts(const std::vector<ModuleStep>& steps, const std::vector<uint32_t>& ticks,
                                const std::vector<OrderRun>& runs, size_t bar);

// Writes a time signature to `conductor` wherever the length of the bars that start at `bars` changes. Full bars use
// 4/4 or 12/8. A shorter bar counts the largest note value that divides it evenly, from quarter notes in 4/4 or eighth
// notes in 12/8 down to 128th notes. The last bar keeps the previous signature, even if the file ends partway through
// it.
void WriteBarSignatures(MidiTrack& conductor, const std::vector<uint32_t>& bars, const Meter& meter);

} // namespace ntdlib::sequence_export::agb
