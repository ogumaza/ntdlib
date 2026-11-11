// SPDX-License-Identifier: MIT

// Nintendo R&D1's steps of archive mode (xsflib/player/archive_source.h): one song of a GBA ROM, played by
// rd1::ArchivePlayer on the game's copy of the driver and the model of the sound hardware. A program gives the type of
// its sets, and finds in a set what to play:
//
//   using Set                  the program's loaded set. set.File(path) gives the set's file at `path` as a
//                              std::vector<uint8_t>, or null when the set has none.
//   Select(set, selection)     sets the ROM's path, the song, the overrides of the driver's detection and the
//                              player's options from the set. Returns the error, or nothing.
//
// The samples go out at the player's Rate() as 16-bit values, the left and the right side in turn.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

#include "ntdlib/agb/archive_source.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/rd1/archive_player.h"
#include "ntdlib/rd1/driver.h"
#include "ntdlib/rd1/game_driver.h"

namespace ntdlib::rd1
{

// What archive mode plays: song `song` of the ROM at `rom_path` among the set's files, with the driver that detection
// finds there, and the player's options.
struct ArchiveSelection
{
    std::string rom_path;
    int song = 0;
    DriverOverrides overrides;
    ArchivePlayerOptions options;
};

// A ROM and its driver.
struct LoadedRom
{
    // Throws xsflib::FormatError when detection finds no driver in `image`, or when the driver's code isn't laid out
    // as FindGameRoutines() requires.
    LoadedRom(std::vector<uint8_t> image, const DriverOverrides& overrides)
    {
        rom.Assign(std::move(image));
        std::string error;
        if (!DetectDriver(rom, overrides, info, error))
        {
            throw xsflib::FormatError(error.empty() ? "the ROM shows no sign of R&D1's driver" : error);
        }

        if (!FindGameRoutines(rom, info))
        {
            throw xsflib::FormatError("the driver's code isn't laid out as the known revisions lay it out");
        }
    }

    LoadedRom(const LoadedRom&) = delete;
    LoadedRom& operator=(const LoadedRom&) = delete;

    Rom rom;
    DriverInfo info;
};

// Archive mode's steps. StartSequence returns the messages of ArchivePlayer::Start: the entry has no song, or an option
// is out of range. It also returns the message of a failure of the game's code.
template <typename Program>
struct ArchiveTraits : agb::RomArchiveTraits<Program, ArchiveSelection, LoadedRom, ArchivePlayer>
{
};

} // namespace ntdlib::rd1
