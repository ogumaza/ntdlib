// SPDX-License-Identifier: MIT

// Song conversion for Nintendo R&D1's MIDI-file sound driver.

#pragma once

#include <string>

#include "ntdlib/agb/rom.h"
#include "ntdlib/rd1/driver.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/rd1/soundfont.h"

namespace ntdlib::sequence_export::rd1
{

using ntdlib::agb::Rom;
using ntdlib::rd1::DriverInfo;
using ntdlib::sequence_export::agb::ConvertSettings;
using ntdlib::sequence_export::agb::SongReport;

// Works out a song's length, loop and tracks the way ConvertSong() does, without writing anything.
SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

// Converts a song to a MIDI file and a SoundFont. With `shared`, its instruments go there instead of in a SoundFont of
// the song.
SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       InstrumentSet* shared);

} // namespace ntdlib::sequence_export::rd1
