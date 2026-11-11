// SPDX-License-Identifier: MIT

// Which sounds of a Wii sound archive are music, for the rip tools' --bgm.

#pragma once

#include <cstdint>
#include <string>

#include "xsflib/ascii.h"

#include "ntdlib/rip/player_names.h"
#include "ntdlib/rsar/formats.h"

namespace ntdlib::rsar
{

// Whether sound `index` of `archive` is a sequence of music: its label holds BGM, in any case, or its player's name
// says music (rip::IsMusicPlayerName). Streams and wave sounds aren't.
inline bool IsMusic(const SoundArchive& archive, uint32_t index)
{
    if (index >= archive.Sounds().size() || archive.Sounds()[index].type != SoundType::kSequence)
    {
        return false;
    }

    const SoundInfo& sound = archive.Sounds()[index];
    if (xsflib::AsciiLower(sound.name).find("bgm") != std::string::npos)
    {
        return true;
    }

    if (sound.player_id >= archive.Players().size())
    {
        return false;
    }

    return rip::IsMusicPlayerName(archive.Players()[sound.player_id].name);
}

} // namespace ntdlib::rsar
