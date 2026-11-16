// SPDX-License-Identifier: MIT

// The length of DSE's tick in known games. A game's code sets it in the driver's settings before the driver's start-up.
// The start-up reads one or two bytes of the settings, by build. In the earlier build, byte 0x0C of 0 gives
// Driver::kSlowTickMicroseconds and any other value Driver::kTickMicroseconds. In the middle builds, byte 0x0D of 0
// gives Driver::kTickMicroseconds and any other value Driver::kSlowTickMicroseconds. The middle build also takes the
// slow tick when byte 0x0C is 0. In the later build, byte 0x11 of 0 gives Driver::kTickMicroseconds and any other
// value Driver::kSlowTickMicroseconds. The tick belongs to the game's settings, not to the build of its driver: one
// image can start the same driver twice with different settings. A game's rows therefore give the tick by the path of
// the songs and the banks of sound effects. ticks.cpp gives the addresses where each game sets it.

#pragma once

#include <optional>
#include <string_view>

namespace ntdlib::dse
{

// The tick that TickOf gives.
struct GameTick
{
    // Whether a row lists the game.
    bool listed = false;

    // The tick in microseconds. Nothing where the game is listed and none of its rows covers `path`: the tick is then
    // unknown, and a converter leaves the files out.
    std::optional<int> tick_microseconds;
};

// The tick of the songs and the sound effects at `path`, a path that FindFiles gives, in the game whose header gives
// `game_code` (4 letters). The rows match the code's first three letters, for every region of a game.
GameTick TickOf(std::string_view game_code, std::string_view path);

} // namespace ntdlib::dse
