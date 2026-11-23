// SPDX-License-Identifier: MIT

// What a rip and a conversion play of the sounds of Nintendo SPD's driver beyond a plain start of each sound.
//
// A sound may only start another sound on its player: its start action starts that sound at once, and the player
// drops the first. Such a sound plays exactly what the other plays, and gets no file of its own. RedirectOf() finds
// it.
//
// A game's code may also set state that changes what a sound plays. Super Princess Peach sets the branch flag that
// events 0xFC and 0xFD of player 0 test (the ARM9's command 10). With the flag at 1, two pieces play sections that
// they don't reach with the flag at 0. Those sections are sounds of the table too, and a start of one alone plays only
// a part of its piece. A game's profile lists the variants of the pieces and their sections.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "ntdlib/spd/driver.h"
#include "ntdlib/spd/song.h"

namespace ntdlib::spd
{

// A state of a sound that the game's code can set, and that changes what the sound plays.
struct Variant
{
    std::string_view sound;  // the sound's name, such as "02_007"
    int branch_flag = 0;     // the value that 0xFC and 0xFD test on player 0
    std::string_view suffix; // added to the sound's name
};

// A sound that only a variant of a piece reaches: a section of that piece.
struct Section
{
    std::string_view sound;
    std::string_view piece;
};

// What the code of one game sets.
struct Profile
{
    std::span<const Variant> variants;
    std::span<const Section> sections;
};

// The profile of the game whose code is `game_code` (4 characters, such as "ASPE"). Empty for a game without one.
Profile ProfileOf(std::string_view game_code);

// The section entry of `name` in `profile`, or none.
std::optional<Section> SectionOf(const Profile& profile, std::string_view name);

// The song that song `song` only starts: its start action starts that song on the same player at once, and the model
// gives the same writes in every frame for both. None for any other song. `data` is the sound data file, and `blocks`
// the blocks that ReadBlockData() gives.
std::optional<int> RedirectOf(std::span<const uint8_t> data, const std::vector<Block>& blocks, const DriverInfo& info,
                              int song);

} // namespace ntdlib::spd
