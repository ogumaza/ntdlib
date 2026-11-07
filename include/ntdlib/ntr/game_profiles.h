// SPDX-License-Identifier: MIT

// What known DS games' code sets as it starts a sequence of their sound archives, and the states of a sequence that
// the code keeps set while it plays. The profiles come from the games' code. The comments in game_profiles.cpp give the
// addresses.
//
// A profile finds a sequence by the game's code in the cartridge's header, the path of the image's file that holds the
// sound archive, the archive's place among that file's archives (sdat::FindArchives) and the sequence's label (Label).
// A sequence that no profile lists plays as the archive gives it: the default GameProfile.
//
// A host plays a sequence as the game starts it with ArchivePlayer::Start(sequence, profile.settings, player). It then
// sends profile.commands, when it holds any, with ArchivePlayer::Send before the next frame. The driver runs them in
// the sequence's first frame, after its start. The commands address the driver's player 0.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/ntr/snd/driver.h"

namespace ntdlib::ntr
{

// A state of a game that stays set while a sequence plays and changes what it plays, such as a variable that the
// game's code writes before it starts the sequence, or a tempo that it sets for the end of a race. A host plays the
// state as the profile with these changes:
//
// - `variables` gives values in place of those of GameSettings::variables. Indices 0 to 15 are the player's
//   variables, and 16 to 31 the global variables.
// - `commands` follow GameProfile::commands in the same list. Where both set a value, the state's takes effect.
//
// A state's name is the sequence's label followed by `suffix`.
struct ProfileArrangement
{
    std::string suffix;
    std::map<uint8_t, int16_t> variables;
    snd::CommandList commands;
};

// How a known game starts a sequence, and the other states that its code gives the sequence while it plays.
struct GameProfile
{
    // What the game's code sets before the sequence starts: the variables, the channels of the archive's players and
    // of some tracks, and the capture units' reverb or output effect.
    GameSettings settings;

    // The driver's commands that the game's code sends once the sequence has started, in their order: the player's
    // volume, the tracks' volumes, the tempo's ratio, the tracks' pitch and the mutes.
    snd::CommandList commands;

    // The sequence's other states, in the order of the profile's table.
    std::vector<ProfileArrangement> arrangements;
};

// The profile of the sequence `label` of the archive at place `index_in_file` of the image's file `file`, in the game
// whose header gives the game code `game_code` (4 letters, such as "ADAE"). Some rows hold for every region of a game.
// They match the code's first three letters. An unknown game, archive or sequence gives the default GameProfile.
GameProfile GameProfileOf(std::string_view game_code, std::string_view file, uint32_t index_in_file,
                          std::string_view label);

} // namespace ntdlib::ntr
