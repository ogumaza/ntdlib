// SPDX-License-Identifier: MIT

// Tune conversion: the driver model's output -> MIDI file + SoundFont instruments.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/rare/driver.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/rare/soundfont.h"

namespace ntdlib::sequence_export::rare
{

using ntdlib::agb::Rom;
using ntdlib::rare::DriverInfo;
using ntdlib::sequence_export::agb::ConvertSettings;
using ntdlib::sequence_export::agb::SongReport;
using ntdlib::sequence_export::agb::SongVersion;

// ConvertSettings::version picks a version of the tune in DriverInfo::water: the arrangement above water or the one
// under water.
constexpr int kAboveWater = 0;
constexpr int kUnderWater = 1;

// Returns the versions of tune `song` that ConvertSong() writes: one, or for the tune in DriverInfo::water, the one
// above water and the one under water.
std::vector<SongVersion> TuneVersions(const DriverInfo& info, int song);

// Works out a tune's length, loop and tracks the way ConvertSong() does, without writing anything.
SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

// Converts one tune. With `shared`, the tune's presets go in bank `bank` of that SoundFont. Without `shared`, the
// conversion writes a SoundFont next to the MIDI file.
SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       SoundfontBuilder* shared, int bank = 0);

// A tune's files in memory, and the report of their conversion.
struct SongFiles
{
    SongReport report;
    std::vector<uint8_t> midi;
    std::vector<uint8_t> sf2;
};

// Converts one tune as ConvertSong() does with a SoundFont of its own, and gives the files' bytes in place of writing
// them. The report names no paths, and `settings.out_dir` and `settings.base_name` don't matter. The files are whole
// only when the report is ok and not silent.
SongFiles ConvertSongFiles(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

// Writes a text listing of every command of every track of a tune. Returns false and sets `error` on failure.
bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error);

} // namespace ntdlib::sequence_export::rare
