// SPDX-License-Identifier: MIT

// The files of Square's Red driver on a GameCube disc: the pieces of music, the wave banks, the blocks of effects and
// the effects that play alone. The game names each file by a number. A piece names its wave bank in its header.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "ntdlib/disc/game.h"

namespace ntdlib::red
{

// The form of a name of a Red game's file: a prefix, the number in `digits` digits or more, and the extension. The game
// writes the names with sprintf.
struct FileName
{
    std::string_view prefix;
    std::size_t digits = 0;
    std::string_view extension;
};

inline constexpr FileName kPiece = {"music", 3, ".bgm"};
inline constexpr FileName kBank = {"wave", 4, ".wd"};
inline constexpr FileName kBlock = {"se", 3, ".seb"};
inline constexpr FileName kEffect = {"se", 6, ".sep"};

// The game's files in `folder`, a path in lower case that ends in '/', that have the form of `name`, by their
// numbers. The game's disc library matches paths in any case. The match therefore ignores case. A file whose number
// has other digits than the game's sprintf writes is left out. A further leading zero is one example. The game never
// reads such a file.
std::map<uint32_t, std::string> Files(const disc::GameFiles& game, std::string_view folder, const FileName& name);

// The name of the game's file at `path`, of the form `name`, without its folder and its extension.
std::string Label(const std::string& path, const FileName& name);

// The number of the wave bank that the piece `piece` plays: the big-endian half at 6, after the magic "BGM ". Nothing
// if `piece` doesn't start with the magic or ends before the bank.
std::optional<uint32_t> PieceBank(std::span<const uint8_t> piece);

} // namespace ntdlib::red
