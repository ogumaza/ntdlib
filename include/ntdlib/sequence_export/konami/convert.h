// SPDX-License-Identifier: MIT

// Song conversion: sequencer output -> MIDI file + SoundFont instruments.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/konami/driver.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/konami/soundfont.h"

namespace ntdlib::sequence_export::konami
{

using ntdlib::sequence_export::agb::ConvertSettings;
using ntdlib::sequence_export::agb::SongReport;
using ntdlib::sequence_export::agb::SongVersion;

// Returns the versions of a song that ConvertSong() writes: the song as a request leaves it, and for a song of
// DriverInfo::live_songs the song as the live stage plays it.
std::vector<SongVersion> SongVersions(const DriverInfo& info, int song);

// Works out a song's length, loop and tracks the way ConvertSong() does, without writing anything.
SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

// Converts one song. If `shared` is supplied, adds instruments to it with bank = song; otherwise writes a separate SF2
// next to the MIDI file.
SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       SoundfontBuilder* shared);

// A song's files in memory, and the report of their conversion.
struct SongFiles
{
    SongReport report;
    std::vector<uint8_t> midi;
    std::vector<uint8_t> sf2;
};

// Converts one song as ConvertSong() does with a SoundFont of its own, and gives the files' bytes in place of writing
// them. The report names no paths, and `settings.out_dir` and `settings.base_name` don't matter. The files are whole
// only when the report is ok and not silent.
SongFiles ConvertSongFiles(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

// Writes a text listing of every command of every track of a song. Returns false and sets `error` on failure.
bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error);

} // namespace ntdlib::sequence_export::konami
