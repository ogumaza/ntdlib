// SPDX-License-Identifier: MIT

// Song conversion: the driver model's output -> MIDI file + SoundFont instruments.

#pragma once

#include <cstdint>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/quintet/driver.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/quintet/soundfont.h"

namespace ntdlib::sequence_export::quintet
{

using ntdlib::agb::Rom;
using ntdlib::quintet::DriverInfo;
using ntdlib::sequence_export::agb::ConvertSettings;
using ntdlib::sequence_export::agb::SongReport;

// Works out a song's length, loop and channels the way ConvertSong() does, without writing anything.
SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

// Converts one song. With `shared`, the song's presets go in the banks that `shared` gives the song. Without `shared`,
// the conversion writes a SoundFont next to the MIDI file.
SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       SoundfontBuilder* shared);

// A song's files in memory, and the report of their conversion.
struct SongFiles
{
    SongReport report;
    std::vector<uint8_t> midi;
    std::vector<uint8_t> sf2;
};

// Converts one song as ConvertSong() does without `shared`, and gives the files' bytes in place of writing them. The
// report names no paths, and `settings.out_dir` and `settings.base_name` don't matter. The files are whole only when
// the report is ok and not silent.
SongFiles ConvertSongFiles(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

} // namespace ntdlib::sequence_export::quintet
