// SPDX-License-Identifier: MIT

// Whether a player of a sound archive plays music, by the player's name. The rip tools' --bgm takes the sounds of such
// players.

#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

#include "xsflib/ascii.h"

namespace ntdlib::rip
{

// Whether a player named `name` plays music. The name's words come between its underscores, in any case. Each word
// counts without the digits it ends in (PLAYER_BGM1). A word BGM, MUSIC, SONG or FANFARE makes it music, and a word SE
// makes it not.
inline bool IsMusicPlayerName(std::string_view name)
{
    bool music = false;
    bool effects = false;
    const std::string lower = xsflib::AsciiLower(std::string(name));
    for (std::size_t start = 0; start <= lower.size();)
    {
        const std::size_t end = std::min(lower.find('_', start), lower.size());
        std::string word = lower.substr(start, end - start);
        word.erase(word.find_last_not_of("0123456789") + 1);
        music = music || word == "bgm" || word == "music" || word == "song" || word == "fanfare";
        effects = effects || word == "se";
        start = end + 1;
    }

    return music && !effects;
}

} // namespace ntdlib::rip
