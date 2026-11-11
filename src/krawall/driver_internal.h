// SPDX-License-Identifier: MIT

// Definitions that the parts of Krawall's detection share (driver.cpp, driver_songs.cpp and driver_files.cpp): the
// limit on the modules, the settings of games that detection can't read, and the functions of one part that another
// calls. The three files build as one translation unit. The definitions therefore keep internal linkage. Only those
// files include this header.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ntdlib/agb/arm.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/thumb.h"
#include "ntdlib/krawall/driver.h"

namespace ntdlib::krawall
{

using ntdlib::agb::ArmLiteral;
using ntdlib::agb::BlTarget;
using ntdlib::agb::FindCalls;
using ntdlib::agb::FindFirstThumb;
using ntdlib::agb::FindThumb;
using ntdlib::agb::Hex;
using ntdlib::agb::kRomBase;
using ntdlib::agb::MissingTable;
using ntdlib::agb::ParseThumbPattern;
using ntdlib::agb::RomTable;
using ntdlib::agb::ThumbLiteral;
using ntdlib::agb::ThumbPattern;

namespace
{

// The most modules detection lists, so that unrelated ROM data can't make the list endless.
constexpr size_t kMaxModules = 1024;

// The settings of games whose code sets them in a way that detection can't read: from the game's options, in ARM code,
// or each frame. The values come from the game's code.
struct GameSettings
{
    const char* game_code;
    int stereo = -1;             // kragInit()'s argument, or -1 for the one that detection finds
    uint32_t music_volume = 128; // krapSetMusicVol()'s volume
    uint32_t music_fade_in = 0;  // the volume's step a frame from 0 after each start, or 0 for none
    int quality = -1;            // kramQualityMode()'s argument, or -1 for the one that detection finds
    uint32_t once[4] = {};       // modules that the game plays once, in mode 0, through routines of its own
};

constexpr GameSettings kGameSettings[] = {
    // Eragon: its options' default music level, 7 of 10, at 0x08063A41. The table at 0x08063C8C gives level 7 a volume
    // of 161.
    {.game_code = "BENE", .music_volume = 161},
    // Eragon's European release: its options' default music level, 7 of 10, at 0x0806398D. The table at 0x08063BD8
    // gives level 7 a volume of 161.
    {.game_code = "BENP", .music_volume = 161},
    // Harry Potter and the Prisoner of Azkaban: its options' default music level, 10 of 10, at 0x0806B816. The routine
    // at 0x0803FF98 gives the library 12 times the level.
    {.game_code = "BHTE", .music_volume = 120},
    // Its Japanese release: the default, 10 of 10, at 0x0806B7A2. The routine at 0x08040000 gives the library 12 times
    // the level.
    {.game_code = "BHTJ", .music_volume = 120},
    // Juka and the Monophonic Menace: the routine at 0x08023A84 runs before the mixer's worker each frame. It sets the
    // music volume, which it raises from 0 by 2 a frame after each start.
    {.game_code = "BJKE", .music_fade_in = 2},
    // Its European release: the routine at 0x08023A38 does the same.
    {.game_code = "BJKP", .music_fade_in = 2},
    // Racing Fever: the routine at 0x080042A4 sets half the volume that the game gives it, 127 of 0xFF, before it
    // plays a module.
    {.game_code = "BRWP", .music_volume = 127},
    // Puppy Luv: Spa and Resort: the routine at 0x0801A548 gives the options' music volume a default of 64. The game
    // sets that volume after its call of krapSetMusicVol() with 60 at 0x08024BF8.
    {.game_code = "BYXE", .music_volume = 64},
    // The Koala Brothers: Outback Adventures calls krapSetMusicVol() with 64 before its title's song, at 0x0800097C.
    // Each level's routine starts its song at 0 and fades it to 64, as at 0x080019BA. The routine at 0x08003AAC fades
    // song 2 to 32 instead. Five other levels play song 2 at 64.
    {.game_code = "BAKE", .music_volume = 64},
    // Its European release makes the same call at the same address.
    {.game_code = "BAKP", .music_volume = 64},
    // The Lord of the Rings: The Third Age: at boot, the routine at 0x0803DA8C passes the music volume of the game's
    // settings to krapSetMusicVol(). Its default, at 0x08054849, is 153. The handler of a script's command 9, at
    // 0x080104F4, can set another volume for a scene.
    {.game_code = "B3AE", .music_volume = 153},
    // Its Japanese release: the routine at 0x0803DB28 does the same. The default, at 0x08054BF9, is 153.
    {.game_code = "B3AJ", .music_volume = 153},
    // Crash of the Titans: at boot, the routine at 0x08036A10 passes the music level of the game's settings to the
    // routine at 0x08036AC0, which calls krapSetMusicVol() with the level's volume from the table at 0x08063DBC. The
    // default level, at 0x08063A25, is 2, a volume of 96.
    {.game_code = "BQCE", .music_volume = 96},
    // Its European release: the routine at 0x08036A1C does the same, with the table at 0x08063E08. The default level,
    // at 0x08063A71, is 2, a volume of 96.
    {.game_code = "BQCP", .music_volume = 96},
    // Digimon Racing: after its language select, the game passes the music level of its settings to the routine at
    // 0x0800806C, which calls krapSetMusicVol() through 0x08013134 with the level's volume from the table at
    // 0x08040E88. The routine at 0x08007DB4 gives the level a default of 6, a volume of 180.
    {.game_code = "BDGE", .music_volume = 180},
    // Its European release has the same routines at 0x0800806C, 0x08013134 and 0x08007DB4, and the table at 0x08040EB8.
    {.game_code = "BDGP", .music_volume = 180},
    // Digimon Racing's Japanese release has the same code at other addresses. It has no language select. At boot, the
    // routine at 0x080080F0 calls krapSetMusicVol() through 0x080131B8 with the level's volume from the table at
    // 0x080401C8. The routine at 0x08007E38 gives the level a default of 6, a volume of 180.
    {.game_code = "BDJJ", .music_volume = 180},
    // The games with the modified build call kragInit() with their options' choice of stereo, 1 by default, and
    // kramQualityMode() with 0x21 in their sound init. Their start-up code then sets the options' music volume: Big
    // Mutha Truckers 100 at 0x08000684, Street Racing Syndicate 64 at 0x080003DA, M&M's Break 'Em 48 at 0x08000418 and
    // Princess Natasha 64 at 0x080008F4. Each of the first two games has another release that makes the call at the
    // same address.
    {.game_code = "B63E", .stereo = 1, .music_volume = 100, .quality = 0x21},
    {.game_code = "B63P", .stereo = 1, .music_volume = 100, .quality = 0x21},
    {.game_code = "BCZE", .stereo = 1, .music_volume = 64, .quality = 0x21},
    {.game_code = "BCZP", .stereo = 1, .music_volume = 64, .quality = 0x21},
    {.game_code = "BEME", .stereo = 1, .music_volume = 48, .quality = 0x21},
    {.game_code = "BNPE", .stereo = 1, .music_volume = 64, .quality = 0x21},
    // Cocoto Platform Jumper plays its modules through its routine at 0x08008C1C, in the mode that the caller gives, or
    // through its routine at 0x08008B88, which starts the module later in mode 1 or 0 by its third argument on the
    // stack. Mode 0 plays four modules once: from 0x0801375A, 0x08016B80 and 0x0801357E through the first, and from
    // 0x08013E4A through the second.
    {.game_code = "BC8P", .once = {0x08032C94, 0x0803520C, 0x0804EC30, 0x080565D4}},
    // Games that call krapSetMusicVol() once at boot or before their first module, with no options behind it. The
    // addresses are those of the calls: The Game of Life, Yahtzee and Payday 96 at 0x0801F51C, Candy Land 96 at
    // 0x0800CD58, Dora the Explorer: Dora's World Adventure 77 at 0x0800FDC0, and Shamu's Deep Sea Adventures 60 in ARM
    // code at 0x08000488.
    {.game_code = "B3UE", .music_volume = 96},
    {.game_code = "BC4E", .music_volume = 96},
    {.game_code = "BXPE", .music_volume = 77},
    {.game_code = "BBAE", .music_volume = 60},
    // Shamu's Deep Sea Adventures' European release is the same program, byte for byte, apart from the header.
    {.game_code = "BBAP", .music_volume = 60},
    // Marvel: Ultimate Alliance starts its modules at 0 and fades them to 64, at 0x0803AB06 and 0x0803AB30.
    // Harry Potter and the Order of the Phoenix starts them at 0, at 0x08032FFA. It fades them to 16 times the byte at
    // 0x02004F10, 5 by default, at 0x08033046.
    {.game_code = "B4ME", .music_volume = 64},
    {.game_code = "BJXE", .music_volume = 80},
    // Marvel: Ultimate Alliance's European release makes the same calls at 0x0803AADE and 0x0803AB08.
    {.game_code = "B4MP", .music_volume = 64},
    // The Bee Game calls krapSetMusicVol(64) in ARM code at 0x08028064.
    {.game_code = "BKFE", .music_volume = 64},
    // The Bee Game's European release, Die Biene Maja: Klatschmohnwiese in Gefahr, makes the same call at 0x08027FC8.
    {.game_code = "BKFX", .music_volume = 64},
    // The three Uno games call krapSetMusicVol(96) at boot, then their wrappers set the level from a table of the
    // game's: Uno 52 80 at 0x080290C8, Uno and Skip-Bo 80 at 0x0802CBD0, and Uno Free Fall 48 at 0x080162F4.
    {.game_code = "BU5E", .music_volume = 80},
    {.game_code = "BUQE", .music_volume = 80},
    {.game_code = "BUIE", .music_volume = 48},
    // Their European releases have the same tables and make the same calls at other addresses: Uno 52 at 0x0802941C,
    // Uno and Skip-Bo at 0x0802D654, and Uno Free Fall at 0x080169C0.
    {.game_code = "BU5P", .music_volume = 80},
    {.game_code = "BUQP", .music_volume = 80},
    {.game_code = "BUIP", .music_volume = 48},
    // Three games share a routine that passes a level from 0 to 9 through a table to krapSetMusicVol(), and keeps the
    // level in their settings. Another routine in two of them maps its argument to the argument of kramQualityMode().
    // The Legend of Spyro: The Eternal Night's table is at 0x0806F9E8. Its routine at 0x0804972C sets level 9 at boot,
    // a volume of 85. Its routine at 0x08049A40 gets 2 from 0x0804967C, a quality of 1.
    // Pirates of the Caribbean: Dead Man's Chest's table is at 0x08080710. Its routine at 0x080350F8 sets level 5, a
    // volume of 140. Its routine at 0x080353B8 gets 2 from 0x08035050, a quality of 1.
    // The Chronicles of Narnia's table at 0x0805449C gives level 9 a volume of 254.
    {.game_code = "BU7E", .music_volume = 85, .quality = 1},
    // The Legend of Spyro: The Eternal Night's European release has the same table at 0x0806FCB4. The routine at
    // 0x080499F8 sets level 9 at boot, and the routine at 0x08049D0C gets 2 from 0x08049948.
    {.game_code = "BU7P", .music_volume = 85, .quality = 1},
    {.game_code = "B8QE", .music_volume = 140, .quality = 1},
    {.game_code = "B2WE", .music_volume = 254},
    // The Sims 2 calls krapSetMusicVol(180) from the routine at 0x0804DFD0. That routine keeps the volume in the game's
    // settings.
    {.game_code = "B46E", .music_volume = 180},
};

// Lists the songs (driver_songs.cpp).
void ListSongs(const Rom& rom, DriverInfo& info, const std::vector<uint32_t>& more_jingles = {});

// Lists the modified build's modules (driver_files.cpp).
bool ListMusicFiles(const Rom& rom, uint32_t loader_code, const std::string& program, DriverInfo& info,
                    std::vector<uint32_t>& jingles, std::string& error);

} // namespace

} // namespace ntdlib::krawall
