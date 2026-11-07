// SPDX-License-Identifier: MIT

// Module conversion: the model's output -> MIDI file + SoundFont instruments.

#pragma once

#include <cstdint>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/krawall/driver.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/krawall/soundfont.h"

namespace ntdlib::sequence_export::krawall
{

using ntdlib::agb::Rom;
using ntdlib::krawall::DriverInfo;
using ntdlib::sequence_export::agb::ConvertSettings;
using ntdlib::sequence_export::agb::SongReport;

// Works out a module's length, loop and channels the way ConvertSong() does, without writing anything.
// ConvertSettings::frame_timing has no effect on a module's conversion. The player's ticks fall on samples rather than
// frames.
SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

// Converts one module. If `shared` is supplied, the module's instruments are added to it for a shared SoundFont;
// otherwise a SoundFont is written next to the MIDI file.
SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       InstrumentSet* shared);

// A module's files in memory, and the report of their conversion.
struct SongFiles
{
    SongReport report;
    std::vector<uint8_t> midi;
    std::vector<uint8_t> sf2;
};

// Converts one module as ConvertSong() does with a SoundFont of its own, and gives the files' bytes in place of writing
// them. The report names no paths, and `settings.out_dir` and `settings.base_name` don't matter. The files are whole
// only when the report is ok and not silent.
SongFiles ConvertSongFiles(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

} // namespace ntdlib::sequence_export::krawall
