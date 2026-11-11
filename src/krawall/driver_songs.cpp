// SPDX-License-Identifier: MIT

// Krawall's detection of the songs: the settings of the pieces that games play in ways that detection can't read,
// the games' routines that play a module from a table, and the list of songs.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "driver_internal.h"

namespace ntdlib::krawall
{

namespace
{

// The pieces that a game plays in a mode or at a music volume that detection can't read. A row names a module, and one
// of the songs that its orders separate with +++, or -1 for a module without markers. The values come from the game's
// code.
struct SongSettings
{
    const char* game_code;
    uint32_t module;
    int song = -1;         // the song's number in the module, or -1 for a module without markers
    int mode = -1;         // krapPlay()'s mode, or -1 for the one that the song and jingle give
    int music_volume = -1; // krapSetMusicVol()'s volume after krapPlay(), or -1 for the game's
};

constexpr SongSettings kSongSettings[] = {
    // Care Bears: Care Quest plays songs 0 and 8 at 128. Each level's routine starts its song at 0 and fades it to
    // 64: song 1 at 0x08003400, song 4 at 0x08004BCC, song 7 at 0x08007218 and song 6 at 0x08008524. The routine at
    // 0x080038BC plays song 2 once, in the song mode, at 128. The routine at 0x0800A1AC plays songs 4 and 5 as jingles
    // in a level. Song 5 then plays at the level's 64.
    {.game_code = "BEAE", .module = 0x08360B0C, .song = 1, .music_volume = 64},
    {.game_code = "BEAE", .module = 0x08360B0C, .song = 2, .mode = 2},
    {.game_code = "BEAE", .module = 0x08360B0C, .song = 4, .music_volume = 64},
    {.game_code = "BEAE", .module = 0x08360B0C, .song = 5, .mode = 6, .music_volume = 64},
    {.game_code = "BEAE", .module = 0x08360B0C, .song = 6, .music_volume = 64},
    {.game_code = "BEAE", .module = 0x08360B0C, .song = 7, .music_volume = 64},
    // Its European release fades its levels' songs to 64 at other addresses: song 1 at 0x080035A4, song 4 at
    // 0x08004D44, song 7 at 0x08007380 and song 6 at 0x08008722. The routine at 0x08003B70 plays song 3 once, not
    // song 2. The routine at 0x0800A428 plays the jingles.
    {.game_code = "BEAP", .module = 0x08361B28, .song = 1, .music_volume = 64},
    {.game_code = "BEAP", .module = 0x08361B28, .song = 3, .mode = 2},
    {.game_code = "BEAP", .module = 0x08361B28, .song = 4, .music_volume = 64},
    {.game_code = "BEAP", .module = 0x08361B28, .song = 5, .mode = 6, .music_volume = 64},
    {.game_code = "BEAP", .module = 0x08361B28, .song = 6, .music_volume = 64},
    {.game_code = "BEAP", .module = 0x08361B28, .song = 7, .music_volume = 64},
    // The Koala Brothers: Outback Adventures plays songs 4 and 5 only as jingles, as at 0x0800171A and 0x0800172E.
    {.game_code = "BAKE", .module = 0x0858C284, .song = 4, .mode = 6},
    {.game_code = "BAKE", .module = 0x0858C284, .song = 5, .mode = 6},
    // Its European release makes the same calls at the same addresses, with the module elsewhere.
    {.game_code = "BAKP", .module = 0x0858BF70, .song = 4, .mode = 6},
    {.game_code = "BAKP", .module = 0x0858BF70, .song = 5, .mode = 6},
    // Franklin the Turtle's ARM code plays nine of its modules only as jingles, at 41 calls of krapPlay(), as at
    // 0x08008848, 0x080088F8 and 0x08013AB8. Each of its 20 mini-games calls krapSetMusicVol(64) and then plays piece
    // 1, 2 or 3, as at 0x08008290 and 0x08008400. krapPlay() keeps the music volume. The title plays piece 0 at 128.
    {.game_code = "BFKE", .module = 0x083321A8, .music_volume = 64},
    {.game_code = "BFKE", .module = 0x08334368, .music_volume = 64},
    {.game_code = "BFKE", .module = 0x08335A18, .music_volume = 64},
    {.game_code = "BFKE", .module = 0x08335F60, .mode = kModeJingle},
    {.game_code = "BFKE", .module = 0x083361F4, .mode = kModeJingle},
    {.game_code = "BFKE", .module = 0x0833647C, .mode = kModeJingle},
    {.game_code = "BFKE", .module = 0x083366F0, .mode = kModeJingle},
    {.game_code = "BFKE", .module = 0x0833697C, .mode = kModeJingle},
    {.game_code = "BFKE", .module = 0x08336C28, .mode = kModeJingle},
    {.game_code = "BFKE", .module = 0x08336E84, .mode = kModeJingle},
    {.game_code = "BFKE", .module = 0x08337104, .mode = kModeJingle},
    {.game_code = "BFKE", .module = 0x0833737C, .mode = kModeJingle},
    // Its European release has three revisions under one game code. Each has the code and the modules at other
    // addresses. The revisions make these calls in the same order, with the modules in the same order.
    {.game_code = "BFKP", .module = 0x0832D3C0, .music_volume = 64},
    {.game_code = "BFKP", .module = 0x0832F580, .music_volume = 64},
    {.game_code = "BFKP", .module = 0x08330C30, .music_volume = 64},
    {.game_code = "BFKP", .module = 0x08331178, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x0833140C, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08331694, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08331908, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08331B94, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08331E40, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x0833209C, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x0833231C, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08332594, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08332FAC, .music_volume = 64},
    {.game_code = "BFKP", .module = 0x0833516C, .music_volume = 64},
    {.game_code = "BFKP", .module = 0x0833681C, .music_volume = 64},
    {.game_code = "BFKP", .module = 0x08336D64, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08336FF8, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08337280, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x083374F4, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08337780, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08337A2C, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08337C88, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08337F08, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08338180, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x083321A8, .music_volume = 64},
    {.game_code = "BFKP", .module = 0x08334368, .music_volume = 64},
    {.game_code = "BFKP", .module = 0x08335A18, .music_volume = 64},
    {.game_code = "BFKP", .module = 0x08335F60, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x083361F4, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x0833647C, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x083366F0, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x0833697C, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08336C28, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08336E84, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08337104, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x0833737C, .mode = kModeJingle},
    // Cocoto Kart Racer's routine at 0x0800BC70 plays a race's module and sets the music volume: 100 for the first four
    // tracks at 0x0800C936, and 90 for the fifth at 0x0800C94E. On the last lap, 0x08005426 plays a jingle and
    // 0x0800542E sets 90. krapPlay() keeps the music volume. 90 therefore holds for the last lap's module, from
    // 0x08005638, for the jingle at the race's end, from 0x080053BA to 0x080053E6, and for the results' module, from
    // 0x0800B400 and 0x0800BB3C. The cup's end plays the race's jingles again at 128, at 0x08009692 to 0x080096D6. The
    // menus set 128. The countdown's jingle, from 0x0800BC9C, plays at 128 in a single race and a cup's first race, and
    // at 90 in the cup's other races. It has no row.
    {.game_code = "BC5P", .module = 0x082415F0, .music_volume = 90},
    {.game_code = "BC5P", .module = 0x082405FC, .music_volume = 90},
    {.game_code = "BC5P", .module = 0x0823F658, .music_volume = 90},
    {.game_code = "BC5P", .module = 0x0822A254, .music_volume = 100},
    {.game_code = "BC5P", .module = 0x08230458, .music_volume = 100},
    {.game_code = "BC5P", .module = 0x082344B8, .music_volume = 90},
    {.game_code = "BC5P", .module = 0x08253D18, .music_volume = 90},
    {.game_code = "BC5P", .module = 0x08256C0C, .music_volume = 90},
    {.game_code = "BC5P", .module = 0x0826181C, .music_volume = 100},
    {.game_code = "BC5P", .module = 0x08261D7C, .music_volume = 90},
    {.game_code = "BC5P", .module = 0x082726E8, .music_volume = 100},
    // Cocoto Platform Jumper's routine at 0x08008C1C sets the music volume after krapPlay(). Its caller gives a factor,
    // and says whether the options' music level scales it. The level's default is 8 of 10, at 0x08000506. A level's
    // module has a factor of 110, from 0x08003A8C. The routine then sets 88. In the first four worlds, a tip stops the
    // music 51 frames after the start. When the tip ends, 0x08016EB2 sets the factor times the level over 12: 73. The
    // pause's end, at 0x08016790, sets 73 too. A restart after a lost life plays at 88 without a tip. The fifth world's
    // levels have no tips. Its module plays at 88, and at 120 at the game's end, from 0x08006118. The flood when the
    // clock runs out plays at 80, from 0x080165A8 with a factor of 100. The module from 0x08013E4A plays at 88. Those
    // from 0x0801357E and 0x0801375A play at 110, and those from 0x0800588C, 0x08005D20, 0x08008AAC and 0x08016B80 at
    // 120, without the level.
    {.game_code = "BC8P", .module = 0x0802DE90, .music_volume = 73},
    {.game_code = "BC8P", .module = 0x080322F0, .music_volume = 73},
    {.game_code = "BC8P", .module = 0x08032C94, .music_volume = 110},
    {.game_code = "BC8P", .module = 0x080349C0, .music_volume = 120},
    {.game_code = "BC8P", .module = 0x0803520C, .music_volume = 120},
    {.game_code = "BC8P", .module = 0x08041C38, .music_volume = 88},
    {.game_code = "BC8P", .module = 0x080439E4, .music_volume = 80},
    {.game_code = "BC8P", .module = 0x0804EC30, .music_volume = 88},
    {.game_code = "BC8P", .module = 0x08056334, .music_volume = 73},
    {.game_code = "BC8P", .module = 0x080565D4, .music_volume = 110},
    {.game_code = "BC8P", .module = 0x0813D328, .music_volume = 120},
    {.game_code = "BC8P", .module = 0x081448FC, .music_volume = 120},
    {.game_code = "BC8P", .module = 0x081499C8, .music_volume = 73},
    // The Bee Game's ARM routine at 0x08027CDC plays each entry of its table of modules at 0x08027C1C in the song mode.
    // That mode goes back to a module's first order rather than its restart order.
    {.game_code = "BKFE", .module = 0x08380B3C, .mode = kModeSong},
    {.game_code = "BKFE", .module = 0x08389078, .mode = kModeSong},
    {.game_code = "BKFE", .module = 0x0837DA44, .mode = kModeSong},
    {.game_code = "BKFE", .module = 0x08377CB8, .mode = kModeSong},
    {.game_code = "BKFE", .module = 0x08374BBC, .mode = kModeSong},
    {.game_code = "BKFE", .module = 0x08374F9C, .mode = kModeSong},
    {.game_code = "BKFE", .module = 0x08385BF8, .mode = kModeSong},
    {.game_code = "BKFE", .module = 0x0837ADE4, .mode = kModeSong},
    {.game_code = "BKFE", .module = 0x08375378, .mode = kModeSong},
    {.game_code = "BKFE", .module = 0x08374658, .mode = kModeSong},
    // The Bee Game's European release has the same routine at 0x08027C40 and the same modules, 0x5A68 bytes lower.
    // The routine plays the entries of the table at 0x08027B80.
    {.game_code = "BKFX", .module = 0x0837B0D4, .mode = kModeSong},
    {.game_code = "BKFX", .module = 0x08383610, .mode = kModeSong},
    {.game_code = "BKFX", .module = 0x08377FDC, .mode = kModeSong},
    {.game_code = "BKFX", .module = 0x08372250, .mode = kModeSong},
    {.game_code = "BKFX", .module = 0x0836F154, .mode = kModeSong},
    {.game_code = "BKFX", .module = 0x0836F534, .mode = kModeSong},
    {.game_code = "BKFX", .module = 0x08380190, .mode = kModeSong},
    {.game_code = "BKFX", .module = 0x0837537C, .mode = kModeSong},
    {.game_code = "BKFX", .module = 0x0836F910, .mode = kModeSong},
    {.game_code = "BKFX", .module = 0x0836EBF0, .mode = kModeSong},
};

// The tables with a music volume for each entry of a game's table of modules.
struct SongLevels
{
    const char* game_code;
    uint32_t levels;
};

constexpr SongLevels kSongLevels[] = {
    // Postman Pat and the Greendale Rocket: the routine at 0x08003B54 plays entry n of its table of modules and then
    // calls krapSetMusicVol() at 0x08003B76 with byte n of this table, times the options' level over 128. The level's
    // default is 128.
    {"BROP", 0x08032C48},
};

// A game's call of krapPlay() for an entry of a table of modules: ldr r1, =table; lsls r0, rN, #2; adds r0, r0, r1;
// ldr r0, [r0]; movs r1, #mode; movs r2, #song; bl krapPlay. Digimon Racing's routine at 0x080132D0 plays its jingle
// that way. The routine starts with push {..., lr}; lsls r0, r0, #24; lsrs rN, r0, #24. The routine's first argument
// then picks the entry. The game's calls of the routine give the entry's number with movs r0, #number.
constexpr const char* kPlayEntryPattern = "49xx 00xx 1840 6800 21xx 22xx Fxxx Fxxx";

// A game's direct call of krapPlay(): ldr r0, =module; movs r1, #mode, then movs r2, #song or not, and bl krapPlay.
// Cocoto Kart Racer plays some of its modules once that way, in mode 0 or 4.
constexpr const char* kPlayCallPatterns[] = {"48xx 21xx 22xx Fxxx Fxxx", "48xx 21xx Fxxx Fxxx"};

// A game's call of krapPlay() for an entry of a table of what it plays: ldr r1, =table; lsls r0, rN, #3; adds r0, r0,
// r1; ldr r3, [r0]; ldr r4, [r0, #4]; ldrb r1, [r0, #4]; lsrs r2, r4, #8; lsls r2, r2, #24; lsrs r2, r2, #24; adds r0,
// r3, #0; bl krapPlay. An entry has 8 bytes: the module's address, krapPlay()'s mode and song, and 2 bytes of 0.
// Eragon's routine at 0x0803FD04 plays its entries that way.
constexpr const char* kPlayTablePattern = "49xx 00xx 1840 6803 6844 7901 0A22 0612 0E12 1C18 Fxxx Fxxx";

// A game's routine that plays entry n of its table of modules in the mode of its table of modes, a word an entry. It
// then calls krapSetMusicVol() with the volume of its table of volumes, a byte an entry: push {r4, lr}; adds r4, r0,
// #0; ldr r0, =modules; lsls r1, r4, #2; adds r0, r1, r0; ldr r0, [r0]; ldr r3, =modes; adds r1, r1, r3; ldr r1, [r1];
// bl krapPlay; ldr r0, =volumes; adds r0, r4, r0; ldrb r0, [r0]; movs r1, #0; bl krapSetMusicVol. Franklin's Great
// Adventures' routine at 0x0800362C plays its modules that way. Its start-up code copies the table of modules to RAM.
constexpr const char* kPlayVolumePattern =
    "B510 1C04 48xx 00A1 1808 6800 4Bxx 18C9 6809 Fxxx Fxxx 48xx 1820 7800 2100 Fxxx Fxxx";

// The bytes of an entry of such a table, and the most entries that detection reads.
constexpr uint32_t kPlayEntrySize = 8;
constexpr uint32_t kMaxPlayEntries = 256;

// The most bytes from the start of such a routine to its call of krapPlay().
constexpr uint32_t kMaxPlayEntryOffset = 64;

// Returns the start of the routine whose call of krapPlay() kPlayEntryPattern matches at `at`. The routine has to pass
// its first argument to the register that picks the table's entry. Returns 0 otherwise.
uint32_t PlayEntryRoutine(const Rom& rom, uint32_t at)
{
    const uint16_t entry_register = (rom.U16(at + 2) >> 3) & 7;
    for (uint32_t back = 2; back <= kMaxPlayEntryOffset && at - back >= kRomBase; back += 2)
    {
        const uint32_t start = at - back;
        if ((rom.U16(start) & 0xFF00) == 0xB500)
        {
            const bool argument = rom.U16(start + 2) == 0x0600 && rom.U16(start + 4) == (0x0E00 | entry_register);
            return argument ? start : 0;
        }
    }

    return 0;
}

// Returns the modules that the game plays once, as jingles: the entries of a table of modules that a routine of the
// game plays with krapPlay() in the jingle mode (kPlayEntryPattern), by the number that each call of the routine gives.
// The call's mode has to leave the loop bit clear. With that bit, krapPlay() loops the module in the jingle mode too.
// Logs each such routine, and warns of a call whose number detection can't read.
std::vector<uint32_t> FindJingles(const Rom& rom, DriverInfo& info)
{
    std::vector<uint32_t> jingles;
    for (uint32_t at : FindThumb(rom, ParseThumbPattern(kPlayEntryPattern)))
    {
        const bool lsls = (rom.U16(at + 2) & 0xFFC7) == 0x0080;
        const int mode = rom.U16(at + 8) & 0xFF;
        if (!lsls || (mode & (kModeJingle | kModeLoop)) != kModeJingle || BlTarget(rom, at + 12) != info.play)
        {
            continue;
        }
        const uint32_t routine = PlayEntryRoutine(rom, at);
        if (!routine)
        {
            continue;
        }

        const uint32_t table = ThumbLiteral(rom, at);
        for (uint32_t call : FindCalls(rom, routine))
        {
            const uint16_t before = rom.U16(call - 2);
            const uint32_t entry = table + 4 * uint32_t(before & 0xFF);
            if ((before & 0xFF00) != 0x2000 || !rom.Contains(entry, 4))
            {
                info.warnings.push_back("the game plays a module once from " + Hex(call) +
                                        ", by a number that detection can't read. That module converts as a loop");
                continue;
            }

            jingles.push_back(rom.U32(entry));
        }

        info.log.push_back("the game's routine that plays a module once, as a jingle: " + Hex(routine));
    }

    return jingles;
}

// Returns the modules that the game plays once through its direct calls of krapPlay() (kPlayCallPatterns): the listed
// modules that no such call plays with the loop bit. Each has the mode of its first such call in the ROM. The game's
// row of kGameSettings adds the modules that it plays once through routines of its own, in mode 0. Logs them.
std::vector<std::pair<uint32_t, int>> FindPlaysOnce(const Rom& rom, DriverInfo& info)
{
    // Each call's module and mode, in the ROM's order, and the modules that a call loops.
    std::map<uint32_t, std::pair<uint32_t, int>> calls;
    std::set<uint32_t> looped;
    for (const char* text : kPlayCallPatterns)
    {
        const std::vector<ThumbPattern> pattern = ParseThumbPattern(text);
        const uint32_t bl = 2 * uint32_t(pattern.size()) - 4;
        for (uint32_t at : FindThumb(rom, pattern))
        {
            const uint32_t module = ThumbLiteral(rom, at);
            const int mode = rom.U16(at + 2) & 0xFF;
            const bool listed = std::find(info.modules.begin(), info.modules.end(), module) != info.modules.end();
            if (!listed || BlTarget(rom, at + bl) != info.play)
            {
                continue;
            }

            calls[at] = {module, mode};
            if (mode & kModeLoop)
            {
                looped.insert(module);
            }
        }
    }

    std::vector<std::pair<uint32_t, int>> once;
    std::set<uint32_t> seen;
    std::string listed;
    for (const auto& [at, call] : calls)
    {
        const auto [module, mode] = call;
        if (looped.contains(module) || !seen.insert(module).second)
        {
            continue;
        }

        once.emplace_back(module, mode);
        listed += (listed.empty() ? "" : ", ") + Hex(module) + " (mode " + std::to_string(mode) + ")";
    }
    // The modules that the game plays once through routines of its own.
    for (const GameSettings& game : kGameSettings)
    {
        if (rom.GameCode() != game.game_code)
        {
            continue;
        }

        for (uint32_t module : game.once)
        {
            const bool listed_module =
                std::find(info.modules.begin(), info.modules.end(), module) != info.modules.end();
            if (module && listed_module && seen.insert(module).second)
            {
                once.emplace_back(module, 0);
                listed += (listed.empty() ? "" : ", ") + Hex(module) + " (mode 0)";
            }
        }
    }

    if (!listed.empty())
    {
        info.log.push_back("modules that the game plays once: " + listed);
    }

    return once;
}

// Returns the entries of the game's table of what it plays (kPlayTablePattern), up to the first that isn't one: an
// entry needs a module that the list has, a mode of 0 to 7 and 2 bytes of 0. An entry that repeats an earlier one is
// left out. Sets the table's address in `info`, or leaves it 0 if the game has no such table, and logs the table.
std::vector<ModuleSong> ReadPlayTable(const Rom& rom, DriverInfo& info)
{
    std::vector<ModuleSong> entries;
    for (uint32_t at : FindThumb(rom, ParseThumbPattern(kPlayTablePattern)))
    {
        if (BlTarget(rom, at + 20) == info.play)
        {
            info.play_table = ThumbLiteral(rom, at);
            break;
        }
    }
    if (!info.play_table)
    {
        return entries;
    }

    uint32_t n = 0;
    for (; n < kMaxPlayEntries; n++)
    {
        const uint32_t entry = info.play_table + kPlayEntrySize * n;
        if (!rom.Contains(entry, kPlayEntrySize))
        {
            break;
        }
        const uint32_t module = rom.U32(entry);
        const int mode = rom.U8(entry + 4);
        const int song = rom.U8(entry + 5);
        const bool listed = std::find(info.modules.begin(), info.modules.end(), module) != info.modules.end();
        if (!listed || mode > 7 || rom.U16(entry + 6) != 0)
        {
            break;
        }

        // The song counts for a module with markers alone. Without them, every song starts at the first order.
        ModuleInfo header;
        const bool markers = ReadModule(rom, info.build, module, header) && SongsInModule(header) > 0;
        const ModuleSong piece{module, markers ? song : -1, false, mode};
        if (std::find(entries.begin(), entries.end(), piece) == entries.end())
        {
            entries.push_back(piece);
        }
    }

    info.log.push_back("the game's table of what it plays: " + Hex(info.play_table) + " (" + std::to_string(n) +
                       (n == 1 ? " entry)" : " entries)"));

    return entries;
}

// Returns the pieces that the game's routine of kPlayVolumePattern plays: each entry of the table of modules that the
// scan found, in the entry's mode and at its music volume. An entry that repeats an earlier one is left out. Returns
// none if the game has no such routine or a mode isn't one of krapPlay()'s, and logs the tables.
std::vector<ModuleSong> ReadModuleSettings(const Rom& rom, DriverInfo& info)
{
    std::vector<ModuleSong> entries;
    const uint32_t count = uint32_t(info.table_count);
    for (uint32_t at : FindThumb(rom, ParseThumbPattern(kPlayVolumePattern)))
    {
        // The routine's table of modules is the one that the scan found, or a copy of it in RAM.
        const uint32_t modules = ThumbLiteral(rom, at + 4);
        const uint32_t modes = ThumbLiteral(rom, at + 12);
        const uint32_t volumes = ThumbLiteral(rom, at + 22);
        if (BlTarget(rom, at + 18) != info.play || count == 0 ||
            (modules != info.module_table && rom.Contains(modules, 4)) || !rom.Contains(modes, 4 * count) ||
            !rom.Contains(volumes, count))
        {
            continue;
        }

        for (uint32_t n = 0; n < count; n++)
        {
            const uint32_t mode = rom.U32(modes + 4 * n);
            if (mode > 7)
            {
                return {};
            }
            const ModuleSong piece{info.modules[n], -1, false, int(mode), rom.U8(volumes + n)};
            if (std::find(entries.begin(), entries.end(), piece) == entries.end())
            {
                entries.push_back(piece);
            }
        }

        info.log.push_back("the game's routine at " + Hex(at) + " plays each module in the mode of the table at " +
                           Hex(modes) + " and at the music volume of the table at " + Hex(volumes));
        break;
    }

    return entries;
}

// Lists the modules that no entry of the game's table plays. One without markers converts as a whole, and one with
// markers song by song. A whole module in `jingles` converts in the jingle mode, and one in `direct_once` in the mode
// of the game's call. Logs the jingles and the modules with markers.
void ListUnplayedModules(const Rom& rom, DriverInfo& info, const std::vector<uint32_t>& jingles,
                         const std::vector<std::pair<uint32_t, int>>& direct_once)
{
    std::string listed;
    for (uint32_t at : info.modules)
    {
        const auto played = [at](const ModuleSong& s)
        {
            return s.module == at && s.mode >= 0;
        };
        if (std::any_of(info.songs.begin(), info.songs.end(), played))
        {
            continue;
        }

        ModuleInfo module;
        const int songs = ReadModule(rom, info.build, at, module) ? SongsInModule(module) : 0;
        const auto this_module = [at](const std::pair<uint32_t, int>& p)
        {
            return p.first == at;
        };
        const auto direct = std::find_if(direct_once.begin(), direct_once.end(), this_module);
        if (songs == 0 && direct != direct_once.end())
        {
            info.songs.push_back({at, -1, false, direct->second});
            continue;
        }
        if (songs == 0)
        {
            const bool jingle = std::find(jingles.begin(), jingles.end(), at) != jingles.end();
            if (jingle)
            {
                listed += (listed.empty() ? "" : ", ") + std::to_string(info.songs.size());
            }
            info.songs.push_back({at, -1, jingle});
            continue;
        }

        for (int s = 0; s < songs; s++)
        {
            info.songs.push_back({at, s});
        }

        char text[160];
        std::snprintf(text, sizeof text, "the module at %s separates %d %s with +++", Hex(at).c_str(), songs,
                      songs == 1 ? "song" : "songs");
        info.log.push_back(text);
    }

    if (!listed.empty())
    {
        info.log.push_back("jingles, which play once: " + listed);
    }
}

// Applies the rows of the pieces whose mode or music volume detection can't read from the game's code, and the games'
// tables of music volumes.
void ApplySongSettings(const Rom& rom, DriverInfo& info)
{
    for (const SongSettings& row : kSongSettings)
    {
        const auto piece = std::find_if(info.songs.begin(), info.songs.end(), [&row](const ModuleSong& s)
                                        { return s.module == row.module && s.song == row.song; });
        if (rom.GameCode() != row.game_code || piece == info.songs.end())
        {
            continue;
        }

        std::string text = "the game plays song " + std::to_string(piece - info.songs.begin());
        if (row.mode >= 0)
        {
            piece->mode = row.mode;
            text += " in mode " + std::to_string(row.mode);
        }
        if (row.music_volume >= 0)
        {
            piece->music_volume = row.music_volume;
            text += " at a music volume of " + std::to_string(row.music_volume);
        }
        info.log.push_back(text);
    }

    for (const SongLevels& table : kSongLevels)
    {
        if (rom.GameCode() != table.game_code || !rom.Contains(table.levels, uint32_t(info.songs.size())))
        {
            continue;
        }

        for (size_t i = 0; i < info.songs.size(); i++)
        {
            info.songs[i].music_volume = rom.U8(table.levels + uint32_t(i));
        }
        info.log.push_back("the game gives each song a music volume, from the table at " + Hex(table.levels));
    }
}

// Lists the songs. The entries of the game's table of what it plays come first, each in the entry's mode. A module that
// no entry plays follows. One without markers converts as a whole, and one with markers song by song, each in the
// player's song mode. A whole module that the game plays once converts in the jingle mode. Logs the songs that play
// once and the jingles.
void ListSongs(const Rom& rom, DriverInfo& info, const std::vector<uint32_t>& more_jingles)
{
    info.songs = ReadPlayTable(rom, info);
    if (info.songs.empty())
    {
        info.songs = ReadModuleSettings(rom, info);
    }

    std::string once;
    for (size_t n = 0; n < info.songs.size(); n++)
    {
        if (!(info.songs[n].mode & kModeLoop))
        {
            once += (once.empty() ? "" : ", ") + std::to_string(n);
        }
    }
    if (!once.empty())
    {
        info.log.push_back("songs whose mode doesn't loop: " + once);
    }

    std::vector<uint32_t> jingles = FindJingles(rom, info);
    jingles.insert(jingles.end(), more_jingles.begin(), more_jingles.end());
    ListUnplayedModules(rom, info, jingles, FindPlaysOnce(rom, info));
    ApplySongSettings(rom, info);
}

} // namespace

} // namespace ntdlib::krawall
