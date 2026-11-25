// SPDX-License-Identifier: MIT

// The Dolphin runs of Luigi's Mansion's boss states (the USA release), for luigis_mansion_states_test and
// sequence_export_jaudio_test. Each run started the piece through the game's start of a piece, with the state that the
// start passes to 0x80188898 patched, and read the volumes of the root track's 16 children and the piece's tempo. No
// run called 0x8018c77c, the function that sets TelesaBattle's tempo in state 1.

#pragma once

#include <cstdint>
#include <string_view>

namespace luigis_mansion_runs
{

// What a run read for one state: the volumes of children 0 to 15 as '1' (1.0) or '0' (0.0), and the bits of the
// tempo's float.
struct BossRun
{
    uint32_t sound = 0;
    uint8_t state = 0;
    std::string_view volumes;
    uint32_t tempo_bits = 0x3f800000;
};

inline constexpr BossRun kBossRuns[] = {
    {0x8000002b, 0, "0111111100000011"},
    {0x8000002b, 1, "1011001101110111"},
    {0x8000002b, 2, "1011001110111111", 0x3f947ae1},
    {0x8000002e, 0, "1111111100000111"},
    {0x8000002e, 1, "1111001000111111"},
    {0x8000002e, 2, "1110101011111111", 0x3f8ba5e3},
    {0x80000031, 0, "1111111111110000"},
    {0x80000031, 1, "0101011100001111"},
    {0x80000052, 0, "1111111111100000"},
    {0x80000052, 1, "0001001011011111", 0x3fbc28f6},
};

// The children that a run read at a volume of 1 (bit n for child n).
constexpr uint16_t PlayingOf(const BossRun& run)
{
    uint16_t playing = 0;
    for (unsigned child = 0; child < 16 && child < run.volumes.size(); child++)
    {
        if (run.volumes[child] == '1')
        {
            playing = static_cast<uint16_t>(playing | 1u << child);
        }
    }

    return playing;
}

} // namespace luigis_mansion_runs
