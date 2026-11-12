// SPDX-License-Identifier: MIT

// What the games with GBAMOD30 modules give the driver: read from each game's code, with the addresses it comes from.

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/logikstate/driver.h"
#include "ntdlib/logikstate/mod30.h"
#include "ntdlib/logikstate/mod30_player.h"

namespace ntdlib::logikstate
{

namespace
{

// The bytes from the driver's table of periods to its table of steps for rate index 0, and between two tables of
// steps, one for each rate.
constexpr uint32_t kStepTables = 0x7A0;
constexpr uint32_t kStepTableSize = 0xF40;

// The most songs that a game plays with the loop flag at 0.
constexpr size_t kMaxNoLoop = 3;

// A game's settings, and the tables that the driver's code reads.
struct Game
{
    const char* code;
    uint32_t bank; // the bank that the init gets
    Mod30Group group;
    int rate_index;   // the init's rate index
    int mixer_mode;   // the init block's mixer mode, 0 in the builds without one
    int sfx_channels; // the init's sound effect channels
    int32_t master;   // the volumes that the game sets after the init
    int32_t music;
    int32_t sfx;
    uint32_t periods;    // the driver's table of periods
    uint32_t linear;     // the driver's table of linear frequencies
    uint32_t song_table; // the game's pointers to its modules in the order of its songs, or 0 for the modules in order
    int song_count;
    std::array<int, kMaxNoLoop> no_loop = {-1, -1, -1}; // the songs that the game plays with the loop flag at 0
    int fifos = 2;                                      // 1 for the output mode with FIFO A alone
    // A call of the pause in the game's flow that no init follows. The pause writes SOUNDCNT_X = 0. A pause that only
    // the game's pause menu calls doesn't count.
    uint32_t sound_off_call = 0;
};

constexpr std::array<int, kMaxNoLoop> kAllLoop = {-1, -1, -1};

// The FIFOs: groups A, B, C and C' write SOUNDCNT_H 0xFB0E, group D and the output mode 2 of group E 0xBB0E, and
// the output mode 1 of group E 0x0B0E. Only BD6E starts its output in mode 1. BMHE, AZFE, BAHP, BAWE and B2FE pause
// the driver with no new init after the pause. BMHE does so from its menus (through 0x081E1B38), and its music plays
// at half volume from there. The four others pause only from the pause menu, and the table leaves that out. The other
// games either never switch the sound off or start the driver again after it, and the new init writes SOUNDCNT_H
// again.
//
// The volumes are the ones that the game sets after the init. Where the game doesn't set one there, the table keeps
// 0x40. Only the music's volume changes what the model does. The tables of periods and linear frequencies are the
// first copies of their bytes in the ROM. The step tables that each driver reads follow the periods.
constexpr std::array kGames = {
    // Group A. The init wrapper 0x0802EC28 (revision 1: 0x0802F2F0) with rate index 4. The music and sound effect
    // volumes are 18 times the options' defaults of 5 (bytes at 0x0835C3F8). The play wrapper 0x0802ECB4 (0x0802F46C)
    // plays the songs of the table at 0x087E5D5C (0x087E6654). The build has no loop flag.
    Game{"AZ8E", 0x080E4B00, Mod30Group::kA, 4, 0, 0, 0x40, 0x5A, 0x5A, 0x0834A3B0, 0x087E5ED0, 0x087E5D5C, 24},
    Game{"AZ8E", 0x080E52B0, Mod30Group::kA, 4, 0, 0, 0x40, 0x5A, 0x5A, 0x0834AC30, 0x087E67C8, 0x087E6654, 24},
    // Group B. The init at 0x0832A6A8 (ARM) with rate index 0 and 3 sound effect channels, the volumes from the words
    // at 0x0832A664..0x0832A66C, and the songs from the table at 0x0832A650 (play wrapper 0x0832A6F4).
    Game{"AUXE", 0x083D2668, Mod30Group::kB, 0, 0, 3, 0x40, 0x40, 0x50, 0x083F26DC, 0x083FBC28, 0x0832A650, 5},
    // Group C. BMHE: the routine 0x081E1AAC with rate index 0 and 4 sound effect channels, and the volumes 0x40, 0x46
    // and 0x60. The game then sets the music to 23 times an option plus 1 (0x081E1BE8): 0x45 at the start, with the
    // option at 2. The play routine 0x081E1E74 plays 12 modules in their order in the ROM. AZFE: the routine
    // 0x0809CC44 with rate index 0 and 3 sound effect channels, the volumes 0x40 (levels of 8 << 3), and the table at
    // 0x083F253C.
    Game{"BMHE", 0x081690D8, Mod30Group::kC, 0, 0, 4, 0x40, 0x45, 0x60, 0x081F34C8, 0x08EEF070, 0, 0, kAllLoop, 2,
         0x081E678C},
    Game{"AZFE", 0x08031F08, Mod30Group::kC, 0, 0, 3, 0x40, 0x40, 0x40, 0x080B3960, 0x083CDBB4, 0x083F253C, 7},
    // Group C'. The init wrapper 0x0867DC48 with the block at 0x0867DBE8 (mixer mode 1), rate index 0 and 3 sound
    // effect channels, the volumes from the words at 0x0867DBDC..0x0867DBE4, and the table at 0x0867DBB0.
    Game{"BLXP", 0x086CA1A0, Mod30Group::kCPrime, 0, 1, 3, 0x50, 0x40, 0x40, 0x087EEF14, 0x087F876C, 0x0867DBB0, 11},
    // Group D. The init wrapper 0x080A6DA4 with the block at 0x02005094 (mixer mode 1), rate index 4 and 4 sound
    // effect channels. The volume routine 0x080A6D6C. The song routine 0x080A7244 plays 11 modules in their order in
    // the ROM, songs 6 and 7 with the loop flag at 0.
    Game{"BKNE", 0x08058814, Mod30Group::kD, 4, 1, 4, 0x64, 0x3F, 0x50, 0x080AFD80, 0x083D7960, 0, 0, {6, 7, -1}},
    // Group E1. The init callers: BNFE 0x080FCD2C, BD6E 0x081EEA94, BHEE 0x08107184, BP4P 0x082776A4, BILE 0x08040B38.
    // The volumes after the init, or after the options that the boot applies (BNFE, BP4P). BNFE's init wrapper sets
    // the music to its level of 4 << 3, and the call at 0x080FD108 then sets 9 times the level. BD6E sets the music to
    // 0x50 at each song's start (the call at 0x081EE70C) and the sound effects to 0x80. BHEE's table is copied to
    // 0x03000E74. BILE's routine 0x08040E8C plays 18 modules in their order in the ROM.
    Game{"BNFE", 0x080431FC, Mod30Group::kE1, 0, 0, 4, 0x40, 0x24, 0x40, 0x08138220, 0x08756EAC, 0x0877B834, 9},
    Game{"BD6E",
         0x080BDB90,
         Mod30Group::kE1,
         4,
         0,
         2,
         0x40,
         0x50,
         0x80,
         0x081F126C,
         0x08201348,
         0x08201A00,
         18,
         {3, 4, 5},
         1},
    Game{"BHEE", 0x080573D8, Mod30Group::kE1, 0, 0, 2, 0x3F, 0x3F, 0x3F, 0x08148460, 0x087922F0, 0x0879B9FC, 9},
    Game{"BP4P", 0x082FB134, Mod30Group::kE1, 4, 1, 4, 0x3F, 0x20, 0x3F, 0x0831F68C, 0x083C5020, 0x083C5890, 2},
    Game{"BILE", 0x080C9280, Mod30Group::kE1, 4, 1, 4, 0x64, 0x32, 0x50, 0x0804CB64, 0x087C2178, 0, 0},
    // Group E2. The init callers: BP5P 0x08254210, BDUE 0x08334B10, BTUE 0x08255314, BAHP 0x0864CD08, BG9E 0x08047360,
    // BAWE 0x0801DC7C, B2FE 0x08000B04, BNKE 0x08004214, B2LE 0x082B034C, B2NE 0x0828D0A4, BYLP 0x08232CC4, BB5E
    // 0x08000B64. BDUE sets the music to 0x50 at each song's start (the call at 0x08334784) and the sound effects to
    // 0x80. B2LE sets the music to 0x20 at each song's start (the call at 0x082AFFC4). BP5P's and B2FE's tables are
    // copied to IWRAM. BAWE's routine 0x0801DFE0 plays 8 modules in their order in the ROM, songs 6 and 7 with the loop
    // flag at 0.
    Game{"BP5P", 0x082D7530, Mod30Group::kE2, 4, 1, 4, 0x3F, 0x20, 0x3F, 0x082FFE5C, 0x083EEB54, 0x083EF3C4, 2},
    Game{"BDUE",
         0x0810E3F8,
         Mod30Group::kE2,
         4,
         0,
         2,
         0x40,
         0x50,
         0x80,
         0x08337368,
         0x08B28408,
         0x08B28BC0,
         22,
         {3, 4, 5}},
    Game{"BTUE", 0x0817A890, Mod30Group::kE2, 4, 0, 4, 0x50, 0x40, 0x78, 0x08257B94, 0x087EAF3C, 0x087ED4A4, 14},
    Game{"BAHP", 0x08693E68, Mod30Group::kE2, 4, 1, 4, 0x3F, 0x50, 0x64, 0x087CDA94, 0x087EC638, 0x087D8C74, 7},
    Game{"BG9E", 0x080D42C8, Mod30Group::kE2, 4, 1, 4, 0x40, 0x30, 0x40, 0x0811C990, 0x081FDDE0, 0x083F8AE0, 15},
    Game{"BAWE", 0x08097A0C, Mod30Group::kE2, 4, 1, 4, 0x64, 0x32, 0x50, 0x0802A9B8, 0x083B95A8, 0, 0, {6, 7, -1}},
    Game{"B2FE", 0x08047FF0, Mod30Group::kE2, 4, 0, 4, 0x50, 0x40, 0x50, 0x080B3480, 0x0826BD3C, 0x0826C4E4, 5},
    Game{"BNKE", 0x0809B2CC, Mod30Group::kE2, 4, 0, 4, 0x3F, 0x3F, 0x3F, 0x080E5BDC, 0x080EFA14, 0x080F01AC, 11},
    Game{"B2LE", 0x081B3598, Mod30Group::kE2, 4, 0, 4, 0x50, 0x20, 0x40, 0x082B2BCC, 0x087A4A84, 0x087A8A10, 11},
    Game{"B2NE", 0x080925E0, Mod30Group::kE2, 4, 0, 2, 0x40, 0x20, 0x30, 0x0828F95C, 0x08DAE1B0, 0x08DB1174, 17},
    Game{"BYLP", 0x081213CC, Mod30Group::kE2, 4, 0, 4, 0x50, 0x40, 0x78, 0x0823551C, 0x085DB5B8, 0x085E2378, 16},
    Game{"BB5E", 0x08072660, Mod30Group::kE2, 4, 0, 4, 0x50, 0x40, 0x40, 0x080DBC10, 0x08271848, 0x08271FF0, 10},
    // Group E3. The init callers: B3RE 0x0877A158 (ARM, with the block at 0x0877A104), BNWE 0x08125E48, BN7E
    // 0x08135DF8. The music volume of BNWE and BN7E is a level of 0x10 << 2, capped at 0x3F.
    Game{"B3RE", 0x087AEA94, Mod30Group::kE3, 0, 0, 2, 0x40, 0x40, 0x40, 0x087DB070, 0x087E4BF8, 0x0877A0D0, 10},
    Game{"BNWE", 0x08043E30, Mod30Group::kE3, 0, 1, 4, 0x40, 0x3F, 0x3F, 0x08141C54, 0x087C91C8, 0x087F4368, 5},
    Game{"BN7E", 0x08050FD4, Mod30Group::kE3, 0, 1, 4, 0x40, 0x3F, 0x3F, 0x08153BB4, 0x087BFD60, 0x087EE238, 5},
};

// Returns a volume as the log gives it. The mixer plays a channel as its record has it at kFullVolume, and a game can
// set more.
std::string VolumeText(int32_t volume)
{
    return std::to_string(volume) + (volume > kFullVolume ? ", above the full volume of " : " of ") +
           std::to_string(kFullVolume);
}

} // namespace

bool GameSettings(const Rom& rom, const Mod30Bank& bank, const std::vector<uint32_t>& modules, Mod30Settings& settings,
                  std::vector<Mod30Song>& songs)
{
    const std::string code = rom.GameCode();
    for (const Game& g : kGames)
    {
        if (code != g.code || bank.address != g.bank)
        {
            continue;
        }

        settings.build = BuildOf(g.group);
        settings.bank = bank.address;
        settings.header_size = bank.header_size;
        settings.rate_index = g.rate_index;
        settings.mixer_mode = g.mixer_mode;
        settings.sfx_channels = g.sfx_channels;
        settings.master = g.master;
        settings.music = g.music;
        settings.sfx = g.sfx;
        settings.loop = settings.build.loop_flag;
        settings.periods = g.periods;
        settings.steps = g.periods + kStepTables + uint32_t(g.rate_index) * kStepTableSize;
        settings.linear = g.linear;
        settings.fifos = g.fifos;
        settings.sound_off_call = g.sound_off_call;

        // The game's songs, then the modules that they don't name.
        songs.clear();
        const auto add = [&](uint32_t module)
        {
            const int song = int(songs.size());
            const bool no_loop = std::find(g.no_loop.begin(), g.no_loop.end(), song) != g.no_loop.end();
            songs.push_back({module, settings.loop && !no_loop});
        };
        for (int i = 0; i < g.song_count; i++)
        {
            add(rom.U32(g.song_table + 4 * uint32_t(i)));
        }
        for (uint32_t module : modules)
        {
            if (std::none_of(songs.begin(), songs.end(), [&](const Mod30Song& s) { return s.module == module; }))
            {
                add(module);
            }
        }

        return true;
    }

    return false;
}

bool DetectMod30(const Rom& rom, DriverInfo& info, std::string& error)
{
    error.clear();
    const std::vector<uint32_t> modules = FindMod30Modules(rom);
    if (modules.empty())
    {
        return false;
    }

    const std::string count = std::to_string(modules.size()) + " GBAMOD30 modules of Logik State's sound driver";
    Mod30Bank bank;
    if (!FindMod30Bank(rom, modules, bank))
    {
        error = "found " + count + ", but not their bank of samples";
        return false;
    }
    Mod30Settings settings;
    std::vector<Mod30Song> songs;
    if (!GameSettings(rom, bank, modules, settings, songs))
    {
        error = "found " + count + ", but not the game's settings of the driver";
        return false;
    }

    info.format = Format::kGbaMod30;
    info.mod30 = settings;
    info.fifos = settings.fifos;
    info.music_volume = settings.music;
    info.sound_off_call = settings.sound_off_call;
    for (const Mod30Song& s : songs)
    {
        Song song;
        song.module = s.module;
        song.bank = bank.address;
        song.loop_flag = s.loop;
        info.songs.push_back(song);
    }

    static constexpr std::array<const char*, 8> kGroupNames = {"A", "B", "C", "C'", "D", "E1", "E2", "E3"};
    info.log.push_back(count + ", the first at " + agb::Hex(modules.front()) + ", in the build of group " +
                       kGroupNames[size_t(settings.build.group)]);
    info.log.push_back("bank of samples at " + agb::Hex(bank.address) + " with headers of " +
                       std::to_string(bank.header_size) + " bytes");
    info.log.push_back("mixer at " + std::to_string(kMod30Rates[size_t(settings.rate_index)]) + " Hz in mode " +
                       std::to_string(settings.mixer_mode));
    info.log.push_back("master volume " + VolumeText(settings.master));
    info.log.push_back("music volume " + VolumeText(settings.music));
    if (info.fifos != 2)
    {
        info.log.push_back("one FIFO plays the mix (SOUNDCNT_H 0x0B0E)");
    }
    if (info.sound_off_call)
    {
        info.log.push_back(std::string(info.fifos == 2 ? "both FIFOs" : "the FIFO") +
                           " at half volume, after the game switches the sound off at " +
                           agb::Hex(info.sound_off_call));
    }

    return true;
}

} // namespace ntdlib::logikstate
