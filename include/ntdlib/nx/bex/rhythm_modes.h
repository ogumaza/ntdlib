// SPDX-License-Identifier: MIT

// The rhythm modes of two Bezel Engine games on the Switch: Super Mario Party Jamboree's rhythm mode and Super Mario
// Party's Sound Stage. Each piece of a mode waits for values that the game gives its sequence's global variables, and
// plays beside the mode's controller, a sequence that the game starts first.

#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace ntdlib::nx::bex
{

// A piece of a rhythm mode: its label in the sound archive, the number that its sequence writes to the global
// variable 29 as it starts, and its tempo. The game gives the piece's number to the global variable 28 and the tempo to
// the global variable 27.
struct RhythmPiece
{
    std::string_view label;
    int16_t number = 0;
    int16_t bpm = 120;
};

// A rhythm mode's controller: the sequence that the game plays beside each piece, and the file name, without its
// extension, of the sound archive that contains it.
struct RhythmController
{
    std::string_view label;
    std::string_view archive;
};

// The rhythm pieces of a sound archive, and their mode's controller.
struct RhythmMode
{
    std::span<const RhythmPiece> pieces;
    RhythmController controller;
};

// The rhythm pieces of the sound archive `archive`, and their controller. A table recognizes the archive by its size
// and SHA-1. Any other archive has none.
RhythmMode RhythmModeOf(std::span<const uint8_t> archive);

// The piece labelled `label` among `pieces`, or null.
const RhythmPiece* RhythmPieceOf(std::span<const RhythmPiece> pieces, std::string_view label);

} // namespace ntdlib::nx::bex
