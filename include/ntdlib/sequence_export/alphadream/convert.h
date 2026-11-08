// SPDX-License-Identifier: MIT

// Song conversion for AlphaDream's driver.

#pragma once

#include <string>

#include "ntdlib/agb/rom.h"
#include "ntdlib/alphadream/driver.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/alphadream/soundfont.h"

namespace ntdlib::sequence_export::alphadream
{

using ntdlib::agb::Rom;
using ntdlib::alphadream::DriverInfo;
using ntdlib::sequence_export::agb::ConvertSettings;
using ntdlib::sequence_export::agb::SongReport;

// Works out a song's length, loop and tracks as ConvertSong() does, without writing anything.
SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

// Converts a song to a MIDI file and a SoundFont. With `shared`, its instruments go there instead.
SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       InstrumentSet* shared);

// Writes a listing of each track's commands. Returns false and sets `error` if it can't.
bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error);

} // namespace ntdlib::sequence_export::alphadream
