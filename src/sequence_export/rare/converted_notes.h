// SPDX-License-Identifier: MIT

// The notes that the conversion of a tune of Rare's driver writes, for the tune's performance.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/rare/driver.h"
#include "ntdlib/rare/sequencer.h"
#include "ntdlib/sequence_export/agb/settings.h"

namespace ntdlib::sequence_export::rare
{

// A note of the MIDI file, with the address of its command.
struct ConvertedNote
{
    int track = 0;
    uint32_t command = 0; // the note on's address
    uint64_t on = 0;      // the MIDI file's ticks
    uint64_t off = 0;
    int key = 0;
    int velocity = 0; // the driver's, from 0 to 127: for a key that two voices start in a frame, the sum less 1
    int program = -1;
};

// A change of a channel's volume that a track's command makes before the end.
struct ConvertedVolume
{
    int track = 0;
    uint64_t tick = 0;
    int channel = 0;
    int value = 0;
};

// A tune as its conversion plays it.
struct ConvertedTune
{
    std::vector<std::string> warnings;
    bool valid = false; // the tune's header was read
    bool truncated = false;
    uint32_t ticks_per_quarter = 0;
    bool loops = false;
    uint64_t loop_start = 0; // the MIDI file's ticks
    uint64_t loop_end = 0;
    uint64_t end = 0;
    double seconds = 0;
    std::vector<std::pair<uint64_t, uint32_t>> tempos; // each change of the tempo, as (tick, microseconds a quarter)
    std::vector<ntdlib::rare::TrackStretch> stretches;
    std::vector<ConvertedVolume> volumes;
    std::vector<ConvertedNote> notes; // in the order of the model's note ons
};

// Plays tune `song` as ConvertSong() does with `settings`. Each note of the MIDI file becomes a note of the result. A
// tune whose header can't be read gives a result that isn't valid, with a warning.
ConvertedTune ConvertedNotes(const ntdlib::agb::Rom& rom, const ntdlib::rare::DriverInfo& info, int song,
                             const agb::ConvertSettings& settings);

} // namespace ntdlib::sequence_export::rare
