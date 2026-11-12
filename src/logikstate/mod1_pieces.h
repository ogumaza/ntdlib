// SPDX-License-Identifier: MIT

// How a game with GBAMOD1 modules starts each piece: which pieces loop as music and which play once as jingles.

#pragma once

#include <cstdint>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/logikstate/driver.h"

namespace ntdlib::logikstate
{

// Marks each song of `info` that the game plays as a jingle as one that plays once, from what the game's data says
// about the pieces of `modules`. A piece that the game starts as music, and a piece that nothing names, play as the
// driver plays them. A game without an entry in the table of games keeps every song as the driver plays it, and the
// log says so.
void ClassifyMod1Pieces(const Rom& rom, const std::vector<uint32_t>& modules, DriverInfo& info);

} // namespace ntdlib::logikstate
