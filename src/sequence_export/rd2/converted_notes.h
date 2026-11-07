// SPDX-License-Identifier: MIT

// The notes that the conversion of a sequence of R&D2's driver writes, for the sequence's performance.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/rd2/driver.h"
#include "ntdlib/rd2/sequencer.h"
#include "ntdlib/sequence_export/agb/settings.h"

namespace ntdlib::sequence_export::rd2
{

// A note of the MIDI file, with the time and the address of its command.
struct ConvertedNote
{
    int track = 0;
    uint64_t units = 0;   // the track's time when it read the note's command
    uint32_t command = 0; // the command's address
    uint32_t on_tick = 0; // the MIDI file's ticks
    uint32_t off_tick = 0;
    int key = 0;
    int velocity = 0; // the driver's
    int bank = 0;
    int instrument = 0;
};

// A sequence as its conversion plays it.
struct ConvertedSong
{
    std::vector<std::string> warnings;
    bool truncated = false;
    bool loops = false;
    uint64_t loop_start = 0; // units: the MIDI file's ticks on the beat
    uint64_t loop_end = 0;
    uint64_t end = 0;
    uint32_t end_tick = 0; // the MIDI file's end
    double seconds = 0;
    std::vector<std::pair<uint64_t, int>> tempos; // each change of the tempo before the end, as (units, tempo)
    std::vector<ntdlib::rd2::TrackRead> reads;    // each read of the tracks' data
    std::vector<ConvertedNote> notes;             // each track's notes in order, the tracks in order
};

// Plays `song` as ConvertSong() does with `settings`. Each note of the MIDI file becomes a note of the result. A song
// without a sequence or a track gives a result with no reads and a warning.
ConvertedSong ConvertedNotes(const ntdlib::agb::Rom& rom, const ntdlib::rd2::DriverInfo& info, int song,
                             const agb::ConvertSettings& settings);

} // namespace ntdlib::sequence_export::rd2
