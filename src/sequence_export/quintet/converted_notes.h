// SPDX-License-Identifier: MIT

// The notes that the conversion of a song of Quintet's driver writes, for the song's performance.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/quintet/driver.h"
#include "ntdlib/sequence_export/agb/settings.h"

namespace ntdlib::sequence_export::quintet
{

// A note of the MIDI file, with the address of the note or the "play again" that started it.
struct ConvertedNote
{
    int channel = 0;
    uint32_t command = 0;
    uint32_t on = 0; // the MIDI file's ticks
    uint32_t off = 0;
    int key = 0;
    int program = 0; // the instrument of the conversion
};

// A stretch of a channel's commands (ntdlib::quintet::TrackStretch), at its MIDI tick.
struct ConvertedStretch
{
    int channel = 0;
    uint32_t tick = 0;
    uint32_t first = 0; // addresses
    uint32_t last = 0;
    int depth = 0;
    bool loop = false;
};

// A change of a channel's volume that a command makes before the end, at its MIDI tick.
struct ConvertedVolume
{
    int channel = 0;
    uint32_t tick = 0;
    int value = 0;
};

// A song as its conversion plays it.
struct ConvertedSong
{
    std::vector<std::string> warnings;
    bool valid = false; // the song's header was read
    bool truncated = false;
    uint32_t ticks_per_quarter = 0;
    bool loops = false;
    uint32_t loop_start = 0; // the MIDI file's ticks
    uint32_t loop_end = 0;
    uint32_t end = 0;
    double seconds = 0;
    std::vector<std::pair<uint32_t, uint32_t>> tempos; // each change of the tempo, as (tick, microseconds a quarter)
    std::vector<ConvertedStretch> stretches;           // in the order that they began
    std::vector<ConvertedVolume> volumes;
    std::vector<ConvertedNote> notes; // channel by channel, in the order of their starts
};

// Plays song `song` as ConvertSong() does with `settings`. Each note of the MIDI file becomes a note of the result. A
// song whose header can't be read gives a result that isn't valid, with a warning.
ConvertedSong ConvertedNotes(const ntdlib::agb::Rom& rom, const ntdlib::quintet::DriverInfo& info, int song,
                             const agb::ConvertSettings& settings);

} // namespace ntdlib::sequence_export::quintet
