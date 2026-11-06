// SPDX-License-Identifier: MIT

// The levels at which a Bezel Engine game plays its sounds, from its sound sheets (Data/SheetBin/SoundVoice_*.elf). A
// record of a sheet names a way to play a sound of the sound archive, and gives the level among its settings. The game
// multiplies the sound by that level. The sound archive doesn't hold it.
//
// WarioWare: Get It Together!'s code takes the record at 0x7e8c70 in its main, merges it with the default record and
// gives the level to the sound's volume at 0xbfc640, over 0 frames. 0xbfc410 multiplies the volume into the sound's
// total. WarioWare: Move It! does the same at 0xcef920 and 0xdbd5bc. A run of those functions on every record of the
// two games' sheets gave the levels that AddSheet reads.

#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "ntdlib/bezel/sheet.h"

namespace ntdlib::bezel
{

class SoundLevels
{
public:
    // Adds the records of a sound sheet: those of its table (s_VoiceDataTable, or s_VoiceDataList in Move It!) and of
    // its other lists (s_voiceData_*, or s_VoiceDataList_*). The size of s_DefaultVoiceData gives the size of a record:
    // 0x98 bytes in Get It Together! and 0xB0 in Move It!. The symbols' sizes give the number of records. A sheet can
    // keep a table in .bss. It then has no records in the file.
    void AddSheet(const Sheet& sheet);

    // The level of the sound `sound`, as a factor. The record named after the sound gives it. A sound without one takes
    // the level that most of the records that play it give, and the higher one when they tie. A sound without a record
    // plays at 1. The game's code chooses among several records as it plays. A rip holds one level for each sound.
    float LevelOf(std::string_view sound) const;

    // The number of sounds with a record.
    std::size_t Count() const
    {
        return sounds_.size();
    }

private:
    struct Records
    {
        std::optional<float> named;      // the record named after the sound
        std::map<float, int> by_level{}; // the number of other records at each level
    };

    std::map<std::string, Records, std::less<>> sounds_;
};

// Whether `name`, a file's name, is a sound sheet whose records can name a sequence. The per-language voice sheets
// (SoundVoice_Voice_*) only name voice clips, and are left out.
bool IsSoundSheet(std::string_view name);

// The levels from the sound sheets in `folder`. A file that can't be read as a sheet adds no record.
SoundLevels ReadSoundLevels(const std::filesystem::path& folder);

// The folder of the sound sheets that belong to the sound archive at `archive`. An archive and the sheets lie in
// different Bezel Engine archives of the game. Once those are unpacked, the search looks for Data/SheetBin in the
// archive's folder and in each of the three folders above it, and in the folders directly below each of them. Returns
// the first that holds a sound sheet.
std::optional<std::filesystem::path> FindSoundSheets(const std::filesystem::path& archive);

} // namespace ntdlib::bezel
