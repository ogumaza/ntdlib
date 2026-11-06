// SPDX-License-Identifier: MIT

// Song conversion: the driver model's output -> MIDI file + SoundFont instruments.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/song_plan.h"
#include "ntdlib/mp2k/driver.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/mp2k/soundfont.h"

namespace ntdlib::sequence_export::mp2k
{
using ntdlib::agb::Rom;
using ntdlib::mp2k::DriverInfo;
using ntdlib::sequence_export::agb::ConvertSettings;
using ntdlib::sequence_export::agb::SongReport;

// Works out a song's length, loop and tracks the way ConvertSong() does, without writing anything.
SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

// Converts one song. If `shared` is supplied, adds its presets to that SoundFont (see
// SoundfontBuilder::SharedPreset()); otherwise writes a SoundFont next to the MIDI file.
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

// A voice group's SoundFont in memory, and the warnings of its conversion.
struct VoiceGroupFiles
{
    bool ok = false; // the SoundFont holds a preset
    std::vector<std::string> warnings;
    std::vector<uint8_t> sf2;
};

// Converts the voices at `programs` of the voice group at `voices` to a SoundFont titled `title`. Each voice becomes
// the instrument that ConvertSong's SoundFont holds for it, with a preset at its program in bank 0. A pulse or saw
// synth voice gets a zone for each key. A voice that has nothing to play gets no preset, and a warning. `settings`
// gives the program's name and psg_exclusive. Without a preset, the SoundFont stays empty.
VoiceGroupFiles ConvertVoiceGroup(const Rom& rom, const DriverInfo& info, uint32_t voices,
                                  const std::vector<int>& programs, const ConvertSettings& settings,
                                  const std::string& title);

} // namespace ntdlib::sequence_export::mp2k
