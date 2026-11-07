// SPDX-License-Identifier: MIT

// Song conversion for Ubisoft Milan's driver: the model's notes -> MIDI file + SoundFont instruments.

#pragma once

#include <cstdint>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/ubimilan/soundfont.h"
#include "ntdlib/ubimilan/driver.h"

namespace ntdlib::sequence_export::ubimilan
{

using ntdlib::sequence_export::agb::ConvertSettings;
using ntdlib::sequence_export::agb::SongReport;
using ntdlib::sequence_export::agb::SongVersion;

// Returns the versions of a song: the song as the game starts it, then the song with the kit for a song that the game
// starts with channel 9's kit switched off.
std::vector<SongVersion> SongVersions(const DriverInfo& info, int song);

// Works out a song's length, loop and channels the way ConvertSong() does, without writing anything.
SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

// Converts one song. If `shared` is supplied, the song's instruments are added to it for a shared SoundFont; otherwise
// a SoundFont is written next to the MIDI file.
SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       InstrumentSet* shared);

// A song's files in memory, and the report of their conversion.
struct SongFiles
{
    SongReport report;
    std::vector<uint8_t> midi;
    std::vector<uint8_t> sf2;
};

// Converts one song as ConvertSong() does with a separate SoundFont, and returns the bytes of the files instead of
// writing them. The report names no paths, and `settings.out_dir` and `settings.base_name` are ignored. The files are
// complete only when the report is ok and not silent.
SongFiles ConvertSongFiles(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

} // namespace ntdlib::sequence_export::ubimilan
