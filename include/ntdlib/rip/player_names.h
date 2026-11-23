// SPDX-License-Identifier: MIT

// Whether a player of a sound archive plays music, by the player's name. The rip tools' --bgm takes the sounds of such
// players.

#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/ascii.h"

namespace ntdlib::rip
{

// The words of a player's name `name`, in lower case. Underscores separate the words. Each word counts without the
// digits it ends in (PLAYER_BGM1 gives "player" and "bgm").
inline std::vector<std::string> PlayerNameWords(std::string_view name)
{
    std::vector<std::string> words;
    const std::string lower = xsflib::AsciiLower(std::string(name));
    for (std::size_t start = 0; start <= lower.size();)
    {
        const std::size_t end = std::min(lower.find('_', start), lower.size());
        std::string word = lower.substr(start, end - start);
        word.erase(word.find_last_not_of("0123456789") + 1);
        words.push_back(std::move(word));
        start = end + 1;
    }

    return words;
}

// Whether a player named `name` plays music. A word BGM, MUSIC, SONG or FANFARE among the name's words
// (PlayerNameWords) makes it music, and a word SE makes it not.
inline bool IsMusicPlayerName(std::string_view name)
{
    bool music = false;
    bool effects = false;
    for (const std::string& word : PlayerNameWords(name))
    {
        music = music || word == "bgm" || word == "music" || word == "song" || word == "fanfare";
        effects = effects || word == "se";
    }

    return music && !effects;
}

} // namespace ntdlib::rip
