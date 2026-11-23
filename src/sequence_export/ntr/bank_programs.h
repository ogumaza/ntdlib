// SPDX-License-Identifier: MIT

// The programs of a DS bank that its conversion writes.

#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ntdlib::sequence_export::detail
{

// The programs of `programs` for which `plays` returns true, in ascending order, each once. The SoundFonts' runs and
// the SFZ files' names take them in that order. Each other program adds a warning that it plays nothing in bank `bank`.
template <typename Plays>
std::vector<uint32_t> PlayingPrograms(std::span<const uint32_t> programs, uint32_t bank, const Plays& plays,
                                      std::vector<std::string>& warnings)
{
    std::vector<uint32_t> playing;
    for (const uint32_t p : programs)
    {
        if (!plays(p))
        {
            warnings.push_back("program " + std::to_string(p) + " of bank " + std::to_string(bank) + " plays nothing");
            continue;
        }

        playing.push_back(p);
    }

    std::ranges::sort(playing);
    playing.erase(std::ranges::unique(playing).begin(), playing.end());

    return playing;
}

} // namespace ntdlib::sequence_export::detail
