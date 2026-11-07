// SPDX-License-Identifier: MIT

// The notes that the conversion of a module of Krawall writes, for the module's performance.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/krawall/driver.h"
#include "ntdlib/sequence_export/agb/settings.h"

namespace ntdlib::sequence_export::krawall
{

// A note of the MIDI file, with the address of the row's entry that started it.
struct ConvertedNote
{
    int channel = 0;
    uint32_t entry = 0; // the entry's byte of flags, or 0 where no entry has played on the channel
    uint64_t on = 0;    // the MIDI file's ticks
    uint64_t off = 0;
    int key = 0;
    int program = 0;
};

// A row that the module played, at the MIDI tick of its first tick.
struct ConvertedRow
{
    uint64_t tick = 0;
    uint32_t data = 0; // the row's first byte
    uint32_t end = 0;  // the address after the 0 that ends it
    int order = 0;
    int row = 0;
};

// A channel's volume (0 to 64) and pan (-64 to 64) from a tick on, as the player keeps them.
struct ConvertedLevel
{
    int channel = 0;
    uint64_t tick = 0;
    int volume = 0;
    int pan = 0;
};

// A module as its conversion plays it.
struct ConvertedModule
{
    std::vector<std::string> warnings;
    bool valid = false; // the module was read
    bool truncated = false;
    uint32_t ticks_per_quarter = 0;
    bool loops = false;
    uint64_t loop_start = 0; // the MIDI file's ticks
    uint64_t loop_end = 0;
    uint64_t end = 0;
    double seconds = 0;
    std::vector<std::pair<uint64_t, uint32_t>> tempos; // each change of the tempo, as (tick, microseconds a quarter)
    std::vector<ConvertedRow> rows;                    // in the order that they played, up to the end
    std::vector<ConvertedLevel> levels;                // each channel's changes, in the order of their ticks
    std::vector<ConvertedNote> notes;                  // in the order of their channels, then of their ticks
};

// Plays piece `song` as ConvertSong() does with `settings`. Each note of the MIDI file becomes a note of the result. A
// module that can't be read gives a result that isn't valid, with a warning.
ConvertedModule ConvertedNotes(const ntdlib::agb::Rom& rom, const ntdlib::krawall::DriverInfo& info, int song,
                               const agb::ConvertSettings& settings);

} // namespace ntdlib::sequence_export::krawall
