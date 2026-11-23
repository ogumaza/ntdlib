// SPDX-License-Identifier: MIT

// The levels at which known games start their songs on DSE. A game's code starts a song with a level, where 256 is full
// level, and DSE's start sets the song's volume from it (StartOf). A song has a state for each level that the game
// starts it at in more than one scene. The first state is the level of most of the song's starts, and a rip or a
// conversion plays it by default. Each other state gets a file whose name ends with the state's suffix. A fade at the
// start is part of the start, not a state: the level after the fade is the state. game_starts.cpp gives the rule in
// full and the evidence of each row.
//
// Some games compute a song's start from values in their binary, such as a table of levels by song. GameStarts reads
// those values from a game's image, by a profile of where the game's code keeps them.

#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/dse/driver.h"
#include "ntdlib/ntr/rom.h"

namespace ntdlib::dse
{

// The level of a start at full level.
inline constexpr int kFullLevel = 256;

// The game's change of the music's volume that follows its start of a song in the same frame. The change is part of the
// start, and the level of the song's state is the level after it.
struct StartChange
{
    friend bool operator==(const StartChange&, const StartChange&) = default;

    int start_level = kFullLevel; // the level of the start, before the change
    int volume = 0;               // the volume that the game's code passes to its change, from 0 to 127
    int frames = 0;               // the video frames that the change takes
};

// A level at which a game starts a song, and the suffix of the name of the file that plays it. The first state has no
// suffix. The suffix of another state is "_LEVEL" and the level. `change` is set where the game's code reaches the
// level by a change right after the start.
struct SongState
{
    friend bool operator==(const SongState&, const SongState&) = default;

    std::string suffix;
    int level = kFullLevel;
    std::optional<StartChange> change = {};
};

// The states of the song at `path`, a path that FindFiles gives, in the game whose header gives `game_code` (4
// letters). A song that no row lists gives one state at full level. A row may cover every song in a folder.
std::vector<SongState> SongStatesOf(std::string_view game_code, std::string_view path);

// The states of the songs of a cartridge image. A game whose profile says where its code keeps the values of its
// songs' starts gets one state for each song, at the level that gives the volume that the game's code makes from those
// values (game_starts.cpp gives each profile). A profile may also list the starts of a song in the game's events. The
// song's state is then the level of most of those events, with the change of each start that the event lowers in the
// same frame. Any other game gets the rows of SongStatesOf.
class GameStarts
{
public:
    // Reads the values of the game in `rom`, whose image must outlive the call only. Throws xsflib::FormatError when
    // the code or the data at a profile's addresses isn't what the profile says, or when a profile's file is missing.
    explicit GameStarts(const ntr::Rom& rom);

    // The states of the song at `path`, a path that FindFiles gives for the files of the image.
    std::vector<SongState> StatesOf(std::string_view path) const;

private:
    std::string game_code_;
    bool read_ = false;             // whether a profile gave the game's levels
    std::optional<int> every_song_; // the level of every song, for a game that starts all at one
    std::map<std::string, SongState, std::less<>> song_states_; // the state of each song by its path
};

// The level whose start (StartOf) has DSE's volume `volume`, from 0 to 127: the lowest level that gives it.
int LevelOfVolume(int volume);

// DSE's start of a song in `state`: the volume level × 127 / 256, rounded down, with flag 4, at once. The games of the
// rows all make that volume: Explorers of Sky's sound thread at 0x02019AB8 to 0x02019AC0, Explorers of Time's at
// 0x0205E588 to 0x0205E59C and Fuurai no Shiren DS 2's request at 0x0212EDA0. Luminous Arc makes its volume of 100
// with another formula (0x02094D0C), and its row gives the level that makes the same volume. A level of 256 gives the
// default StartParameters.
StartParameters StartOf(const SongState& state);

} // namespace ntdlib::dse
