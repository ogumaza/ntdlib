// SPDX-License-Identifier: MIT

// Sequence conversion: the driver model's output -> MIDI file + SoundFont instruments.

#pragma once

#include <cstdint>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/rd2/driver.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/rd2/soundfont.h"

namespace ntdlib::sequence_export::rd2
{

using ntdlib::rd2::DriverInfo;
using ntdlib::sequence_export::agb::ConvertSettings;
using ntdlib::sequence_export::agb::SongReport;
using ntdlib::sequence_export::agb::SongVersion;

// Returns the versions of song `song` that ConvertSong() writes: one for each entry of the sequence's
// DriverInfo::music_settings. A version after the first is named after its music number where that differs from the
// music number of the first. A version that plays the tracks that Super Mario Advance 2 mutes unless the player rides
// Yoshi is named "riding Yoshi".
std::vector<SongVersion> SongVersions(const DriverInfo& info, int song);

// Works out a sequence's length, loop and tracks the way ConvertSong() does, without writing anything.
SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

// Converts one sequence. With `shared`, the sequence's presets go in the banks that `shared` gives the sequence.
// Without `shared`, the conversion writes a SoundFont next to the MIDI file.
SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       SoundfontBuilder* shared);

// A sequence's files in memory, and the report of their conversion.
struct SongFiles
{
    SongReport report;
    std::vector<uint8_t> midi;
    std::vector<uint8_t> sf2;
};

// Converts one sequence as ConvertSong() does with a SoundFont of its own, and gives the files' bytes in place of
// writing them. The report names no paths, and `settings.out_dir` and `settings.base_name` don't matter. The files are
// whole only when the report is ok and not silent.
SongFiles ConvertSongFiles(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

} // namespace ntdlib::sequence_export::rd2
