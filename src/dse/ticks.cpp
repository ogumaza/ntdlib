// SPDX-License-Identifier: MIT

#include "ntdlib/dse/ticks.h"

#include <optional>
#include <string_view>

#include "ntdlib/dse/driver.h"

namespace ntdlib::dse
{

namespace
{

constexpr int kTick = Driver::kTickMicroseconds;
constexpr int kSlowTick = Driver::kSlowTickMicroseconds;

// A row: the game's code (three letters), the start of the paths that it covers (empty for every path), and the tick.
struct Row
{
    std::string_view game;
    std::string_view path;
    int tick_microseconds;
};

constexpr Row kRows[] = {
    // Pokémon Mystery Dungeon: Explorers of Time and of Darkness, the earlier build: the start-up (0x0205F830 in
    // Time's ARM9 binary) takes byte 0x0C of the settings, 4. Any byte but 0 there gives 10 ms.
    {"YFT", "", kTick},
    {"YFY", "", kTick},
    // Explorers of Sky: the game's sound start (0x02018C28) starts the driver with the default settings (0x0206C1C0),
    // whose byte 0x11 is 0.
    {"C2S", "", kTick},
    // Soma Bringer, the earlier build: the sound's start (0x020628B4) gives byte 0x0C = 8. Any byte but 0 there gives
    // 10 ms (0x0206AC7C).
    {"YBS", "", kTick},
    // Professor Layton and the Unwound Future: overlay 0's 0x0205FD5C sets byte 0x11 to 0.
    {"C3J", "", kTick},
    // Luminous Arc 3: Eyes: 0x020543D4 sets byte 0x11 to 0.
    {"BLU", "", kTick},
    // Inazuma Eleven 2: 0x0202B6E4 sets byte 0x11 to 0.
    {"BEE", "", kTick},
    // Professor Layton and the Last Specter. The main game's sound part (overlay 8's 0x020F0010) sets byte 0x11 to 1.
    // Its songs and sound effects are in the archive lt4_main_<language>.fa. London Life starts the driver again
    // (overlay 48's 0x020CF358), with byte 0x11 = 0 (0x020443E8 with 0 and 8, from 0x020CF394). Its files are in ll/.
    {"BLF", "lt4_main_", kSlowTick},
    {"BLF", "ll/", kTick},
    // Inazuma Eleven, the later build: the sound's start (0x02039C54) calls 0x0201F914(settings, 0, 4) at 0x02039CB0.
    // The call sets byte 0x11 to 0.
    {"YEE", "", kTick},
    // Fushigi no Dungeon: Fuurai no Shiren 4, the later build: the sound's start (Thumb code, 0x0200C1DC) calls
    // 0x0207EB9C(settings, 0, 4) at 0x0200C352. The call sets byte 0x11 to 0.
    {"B4F", "", kTick},
    // Professor Layton and the Diabolical Box, the earlier build: the sound's start (overlay 2's 0x0207F254) keeps
    // byte 0x0C of the settings at 4. Any byte but 0 there gives 10 ms (DSE's start-up, 0x02000EA8).
    {"YLT", "", kTick},
    // Luminous Arc 2, the middle build: the sound's start (0x0206B7FC) calls 0x0206CCF4(settings, 4, 0) at
    // 0x0206B86C. The call sets bytes 0x0C and 0x0D to 4 and 0. DSE's start-up reads them at 0x0206CDA0.
    {"YL2", "", kTick},
    // Fushigi no Dungeon: Fuurai no Shiren DS 2, the late middle build: the sound's start (0x0212DF60) calls
    // 0x02125828(settings, 4, 0) at 0x0212E00C. The call sets bytes 0x0C and 0x0D to 4 and 0. DSE's start-up reads byte
    // 0x0D at 0x021258E4.
    {"YWF", "", kTick},
    // Inazuma Eleven 3: Sekai e no Chousen!! Bomber and The Ogre, the later build: 0x0207738C(settings, 0, 4) at
    // 0x0202CE00 in Bomber, and 0x02074C58(settings, 0, 4) at 0x0202B4BC in The Ogre, set byte 0x11 to 0.
    {"BEZ", "", kTick},
    {"BOE", "", kTick},
    // Fushigi no Dungeon: Fuurai no Shiren 5, the later build in Thumb code: 0x0209069C(settings, 0, 4) at 0x0200D5C0
    // sets byte 0x11 to 0. DSE's start-up reads it at 0x02090744.
    {"B5F", "", kTick},
    // Ni no Kuni: Shikkoku no Madoushi, the later build: 0x0204EDFC(settings, 0, 8) at 0x02049F3C sets byte 0x11 to 0.
    // No code in the game loads a song, and the game never plays the six songs that its files hold.
    {"B2K", "", kTick},
};

} // namespace

GameTick TickOf(std::string_view game_code, std::string_view path)
{
    GameTick tick;
    const std::string_view game = game_code.substr(0, 3);
    for (const Row& row : kRows)
    {
        if (row.game != game)
        {
            continue;
        }

        tick.listed = true;
        if (path.starts_with(row.path))
        {
            tick.tick_microseconds = row.tick_microseconds;
            break;
        }
    }

    return tick;
}

} // namespace ntdlib::dse
