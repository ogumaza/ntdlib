// SPDX-License-Identifier: MIT

// The catalogue of Nintendo Puzzle Collection (see ntdlib/musyx/catalogue.h).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "musyx/catalogue_parts.h"
#include "ntdlib/disc/game.h"
#include "ntdlib/musyx/catalogue.h"
#include "ntdlib/rel_module.h"
#include "ntdlib/yay0.h"

namespace ntdlib::musyx::detail
{
namespace
{

// The bytes of an object of `module` (RelModule::Object) at `offset` in section `section`.
std::vector<uint8_t> ModuleObject(const RelModule& module, uint32_t section, uint32_t offset)
{
    const std::span<const uint8_t> object = module.Object(module.At(section, offset));
    return {object.begin(), object.end()};
}

// Where a module keeps a MusyX group's files: the group's ID, then the offsets of its project, its pool and its sample
// directory in one section of the module.
struct ModuleGroup
{
    uint16_t id = 0;
    uint32_t project = 0, pool = 0, sdir = 0;
};

// The file set of `group` in section `section` of `module`, with the samples `samples`.
FileSet ModuleSet(const RelModule& module, uint32_t section, const ModuleGroup& group, std::vector<uint8_t> samples)
{
    return {.project = ModuleObject(module, section, group.project),
            .pool = ModuleObject(module, section, group.pool),
            .sdir = ModuleObject(module, section, group.sdir),
            .samples = std::move(samples)};
}

// The name of a song of a module: the module's name, a hyphen, then the song's name, or its ID in two digits if
// `names` has no name for it.
std::string SongName(const char* module, std::span<const char* const> names, uint16_t id)
{
    if (id < names.size())
    {
        return std::string(module) + "-" + names[id];
    }

    const std::string number = std::to_string(id);
    return std::string(module) + "-" + (number.size() < 2 ? "0" : "") + number;
}

// The sounds of a module that the catalogue gathers, and the effects. The effects follow the songs of every module.
struct PuzzleSounds
{
    Catalogue catalogue;
    std::vector<Sound> effects;
};

// Adds the effects of the effects groups of set `set` to `sounds`, in the order of the groups' tables. Each effect
// pushes `groups` first and plays in module `module`. Its name is the module's name `name`, "-se-" and its ID in four
// digits, such as "panepon-se-0123".
void AddModuleEffects(PuzzleSounds& sounds, const char* name, std::size_t set, const std::vector<PushedGroup>& groups,
                      std::size_t module)
{
    for (const ProjectEffect& effect : EffectsOf(sounds.catalogue.sets.at(set).project))
    {
        Sound sound;
        sound.name = EffectName(std::string(name) + "-se", effect.row.id);
        sound.kind = SoundKind::kEffect;
        sound.id = effect.row.id;
        sound.groups = groups;
        sound.module = module;
        sounds.effects.push_back(std::move(sound));
    }
}

// The size of the menu's table of volumes: two rows of four words, at 0 in the module's section 4.
constexpr std::size_t kMenuVolumeRows = 2, kMenuVolumeColumns = 4, kMenuVolumeWord = 4;

// The volume of the menu's song `id` of group `group`, from the menu's table of volumes `table`. The low byte of a word
// of the table gives a volume. The menu's code at 0x3A44 in section 1 picks a word by the song's group and ID. The code
// at 0x3B68 in section 1 starts the song, then passes the volume to sndVolume with a time of 1 ms and group 0xFD, the
// songs' groups. The catalogue gives the song the volume instead (sndSeqVolume).
uint8_t MenuVolume(std::span<const uint8_t> table, uint16_t group, uint16_t id)
{
    std::size_t row = 0, column = kMenuVolumeColumns - 1;
    if (group == 1)
    {
        column = 2;
    }
    else if (group == 2 && id == 1)
    {
        row = 1;
        column = 1;
    }
    else if (group == 2 && (id == 2 || id == 3))
    {
        column = id == 2 ? 0 : 1;
    }
    else if (group == 3)
    {
        row = 1;
        column = id == 4 ? 0 : id == 5 ? 2 : kMenuVolumeColumns - 1;
    }

    const std::size_t word = (row * kMenuVolumeColumns + column) * kMenuVolumeWord;
    return table[word + kMenuVolumeWord - 1];
}

// The four parts of menu_relsamp.szp, `file` once decoded, after a header that gives their offsets: the menu's module,
// then the samples of groups 2, 0 and 4.
constexpr std::size_t kMenuParts = 4;
using MenuParts = std::array<std::span<const uint8_t>, kMenuParts>;

MenuParts MenuPartsOf(std::span<const uint8_t> file)
{
    const std::span<const uint8_t> header = Part(file, 0, kMenuParts * 4, "menu_relsamp.szp's header");
    MenuParts parts;
    for (std::size_t i = 0; i < kMenuParts; i++)
    {
        const std::size_t start = xsflib::LoadBe32(header.data() + i * 4);
        const std::size_t end = i + 1 < kMenuParts ? xsflib::LoadBe32(header.data() + i * 4 + 4) : file.size();
        parts[i] = Part(file, start, end < start ? 0 : end - start, "a part of menu_relsamp.szp");
    }

    return parts;
}

// A song of the menu: its group, its ID, and where the module holds it.
struct MenuSong
{
    uint16_t group = 0, id = 0;
    std::size_t at = 0;
};

// The songs of the menu's `module`, from its table of each group's table of songs at `tables`, in the order of their
// IDs.
std::vector<MenuSong> MenuSongsOf(const RelModule& module, std::size_t tables)
{
    std::vector<MenuSong> songs;
    for (const auto& [group, table] : module.TablePointers(tables))
    {
        for (const auto& [id, at] : module.TablePointers(table))
        {
            songs.push_back({.group = group, .id = id, .at = at});
        }
    }

    const auto by_id = [](const MenuSong& a, const MenuSong& b)
    {
        return a.id < b.id;
    };
    std::stable_sort(songs.begin(), songs.end(), by_id);

    return songs;
}

// The menu's songs and effects (see ReadPuzzleCollection). menu_relsamp.szp unpacks to four parts. Its first four
// words give their offsets: the menu's module, then the samples of groups 2, 0 and 4. A part runs up to the next part,
// and the last one up to the end of the file. The module's section 4 holds a table of six pointers at 0x448. Pointer g
// points at the table of group g's songs, for g from 1 to 5. A group's table holds a pointer to each of its songs,
// under the song's ID. The menu's code gives the offsets of each group's files in section 4: at 0x56EC in section 1 for
// groups 0, 2 and 4, 0x58B8 for group 1, 0x5988 for group 3 and 0x5A58 for group 5. Files in title/ hold the samples of
// groups 1, 3 and 5. The menu pushes group 0, its effects, at start-up, and a song's group before the song. Each song
// takes a volume from the table at 0 in section 4 (MenuVolume). The menu's code (0x5580 in section 1) gives aux A's
// high-quality reverb its settings. The code gives the chorus a base delay of 13 ms, a variation of 2 ms and a period
// of 500 ms. Nothing names the menu's songs. A song's name gives its ID, such as "menu-01". The songs come in the order
// of their IDs.
//
// Group 0 holds the menu's effects. The menu starts them through a function of its own (0x8594 in section 1), with a
// volume and a pan of 0xFF and studio 0. At start-up, the menu calls sndVolume(127, 0, 0xFF) (0x8320 in section 1),
// then sndVolume(110, 0, 0xFF) (0x3458). A song's start sets the song's group again. The effects keep 110. An effect
// pushes group 0.
void AddMenu(PuzzleSounds& sounds, const AnyCaseFiles& files)
{
    constexpr uint8_t kGroupVolume = 110;
    constexpr uint32_t kData = 4, kVolumes = 0, kSongTables = 0x448;

    // Each group's files, and the part of menu_relsamp.szp or the file that holds its samples.
    struct Group
    {
        ModuleGroup files;
        std::size_t part = 0;
        const char* samples = nullptr;
    };
    constexpr std::array<Group, 6> kGroups{{
        {.files = {.id = 0, .project = 0x111884, .pool = 0x1116c0, .sdir = 0x111938}, .part = 2},
        {.files = {.id = 1, .project = 0x135184, .pool = 0x133fd8, .sdir = 0x1352b8},
         .samples = "title/yoshi_title.samp"},
        {.files = {.id = 2, .project = 0x113d84, .pool = 0x111bc4, .sdir = 0x113fec}, .part = 1},
        {.files = {.id = 3, .project = 0x133a50, .pool = 0x1339ac, .sdir = 0x133b60},
         .samples = "title/agb_title.samp"},
        {.files = {.id = 4, .project = 0x11e284, .pool = 0x11d0d4, .sdir = 0x11e42c}, .part = 3},
        {.files = {.id = 5, .project = 0x12842c, .pool = 0x125e4c, .sdir = 0x128600},
         .samples = "title/grand_staff.samp"},
    }};

    Catalogue& catalogue = sounds.catalogue;
    const std::size_t module_number = catalogue.modules.size();
    catalogue.modules.push_back({.chorus = ChorusSettings{13, 2, 500}});

    const std::vector<uint8_t> file = ntdlib::DecodeYay0(files.Read("menu_relsamp.szp"));
    const MenuParts parts = MenuPartsOf(file);

    const RelModule module(parts[0], "menu_relsamp.szp's module");
    std::map<uint16_t, std::size_t> set_of_group;
    for (const Group& group : kGroups)
    {
        std::vector<uint8_t> samples = group.samples != nullptr
                                           ? files.Read(group.samples)
                                           : std::vector<uint8_t>(parts[group.part].begin(), parts[group.part].end());
        set_of_group[group.files.id] = catalogue.sets.size();
        catalogue.sets.push_back(ModuleSet(module, kData, group.files, std::move(samples)));
    }

    const std::vector<MenuSong> songs = MenuSongsOf(module, module.At(kData, kSongTables));
    const std::span<const uint8_t> volumes =
        Part(parts[0], module.At(kData, kVolumes), kMenuVolumeRows * kMenuVolumeColumns * kMenuVolumeWord,
             "the menu's volumes");
    const PushedGroup effects_group = {.id = 0, .set = set_of_group.at(0)};
    for (const MenuSong& song : songs)
    {
        const auto set = set_of_group.find(song.group);
        if (set == set_of_group.end())
        {
            throw xsflib::FormatError("the menu has no files for the group of its song " + std::to_string(song.id));
        }

        const std::span<const uint8_t> data = module.Object(song.at);
        Sound sound;
        sound.name = SongName("menu", {}, song.id);
        sound.volume = MenuVolume(volumes, song.group, song.id);
        sound.id = song.id;
        sound.song_group = song.group;
        sound.groups = {effects_group, {.id = song.group, .set = set->second}};
        sound.module = module_number;
        AddSong(catalogue, std::move(sound), {data.begin(), data.end()});
    }

    const std::size_t first_effect = sounds.effects.size();
    AddModuleEffects(sounds, "menu", effects_group.set, {effects_group}, module_number);
    for (std::size_t e = first_effect; e < sounds.effects.size(); e++)
    {
        sounds.effects[e].group_volume = kGroupVolume;
    }
}

// One of the games whose module holds its songs and its one set of MusyX's files: Dr. Mario and Panel de Pon (see
// AddPuzzleGame).
struct PuzzleGame
{
    const char* name = nullptr;           // the start of the sounds' names
    const char* file = nullptr;           // the module's file
    uint32_t song_section = 0, songs = 0; // the table of songs: the section that holds it, and its offset there
    uint32_t file_section = 0;            // the section that holds MusyX's files
    ModuleGroup files;                    // their offsets in it
    const char* samples = nullptr;        // the file of the samples
    std::span<const char* const> names;   // the songs' names, by their IDs
    ChorusSettings chorus = {};
    uint32_t volumes = 0;                // the table of the songs' volumes in `song_section`, or 0 for none
    uint32_t effects = 0;                // the table of the effects' volumes and pans in `song_section`, or 0 for none
    std::optional<uint8_t> group_volume; // the volume that the game gives every group after sndVolume(127)
};

// The names of Dr. Mario's songs, by their IDs: the songs' symbols in the debug module Dr_MARIO.plf, without the
// prefix "song_seq_". A name that ends in "f" names the fast form of the song before it.
constexpr std::array<const char*, 24> kDrMarioSongs{
    "fever",  "feverf",  "chil",         "chilf",   "game_c",  "game_cf", "game_d", "game_df",
    "game_e", "game_ef", "game_opening", "tile",    "menu",    "staff",   "end_a",  "end_b",
    "end_bs", "end_c",   "end_cs",       "story_a", "story_b", "story_c", "ending", "coffee"};

// Dr. Mario (see AddPuzzleGame). Dr_MARIO.szp unpacks to its module. The module's section 5 holds the table of songs
// at 0x10. Section 4 holds MusyX's files: the project at 0x10E424, the pool at 0x1035B0 and the sample directory at
// 0x10F2F4. dr_test.samp holds the samples. During play, the songs play at MusyX's default volume. The game lowers a
// song's volume during a pause (dm_set_pause_and_volume, 0x13DF4 in section 1), in its manual and in some other states.
// The catalogue gives a song its volume during play. The game's code (0x248 in section 1) gives aux A's high-quality
// reverb its settings. The code gives the chorus a base delay of 13 ms, a variation of 2 ms and a period of 500 ms. At
// start-up, the game calls sndVolume(127, 0, 0xFF) (0x5B9D4 in section 1), then sndVolume(110, 0, 0xFF) (0x438). The
// effects keep 110.
constexpr PuzzleGame kDrMario = {.name = "dr_mario",
                                 .file = "Dr_MARIO.szp",
                                 .song_section = 5,
                                 .songs = 0x10,
                                 .file_section = 4,
                                 .files = {.id = 1, .project = 0x10e424, .pool = 0x1035b0, .sdir = 0x10f2f4},
                                 .samples = "dr_test.samp",
                                 .names = kDrMarioSongs,
                                 .chorus = {13, 2, 500},
                                 .group_volume = 110};

// The names of Panel de Pon's songs, by their IDs: the songs' symbols in the debug module PANEPON.plf, without the "_"
// and the "_song" around them.
constexpr std::array<const char*, 59> kPanelDePonSongs{"flower_01",
                                                       "flower_02",
                                                       "jewel_01",
                                                       "jewel_02",
                                                       "water_01",
                                                       "water_02",
                                                       "sea_01",
                                                       "sea_02",
                                                       "cloud_01",
                                                       "cloud_02",
                                                       "moon_01",
                                                       "moon_02",
                                                       "green_01",
                                                       "green_02",
                                                       "fire_01",
                                                       "fire_02",
                                                       "ice_01",
                                                       "ice_02",
                                                       "prince_01",
                                                       "prince_02",
                                                       "lion_01",
                                                       "lion_02",
                                                       "dragon_01",
                                                       "dragon_02",
                                                       "joker_01",
                                                       "joker_02",
                                                       "sanatos_01",
                                                       "sanatos_02",
                                                       "goddess_01",
                                                       "goddess_02",
                                                       "whale_01",
                                                       "whale_02",
                                                       "opening",
                                                       "select",
                                                       "ending",
                                                       "ending_easymode",
                                                       "you_lose",
                                                       "you_win",
                                                       "intro_beginning",
                                                       "keyroad_part2",
                                                       "puzzle_edit",
                                                       "keyload_part1",
                                                       "goddess_appear",
                                                       "joker_apper",
                                                       "which_appear",
                                                       "ending_frog",
                                                       "obaba_01",
                                                       "obaba_02",
                                                       "tutorial",
                                                       "intro_storm",
                                                       "ending_whale_eyes",
                                                       "ending_which_photo",
                                                       "keyroad_part3",
                                                       "ending_staff_credits",
                                                       "ending_whale_gets_back_eyes",
                                                       "opening_skip",
                                                       "reslut",
                                                       "reserved1",
                                                       "reserved2"};

// Panel de Pon (see AddPuzzleGame). panepon.szp unpacks to its module. The module's section 5 holds the table of songs
// at 0xBD0, and MusyX's files: the project at 0x20350, the pool at 0x8FC0 and the sample directory at 0x23010.
// panepon.samp holds the samples. The game plays a song through PlayMIDI (0xAD230 in section 1). The table at 0x134148
// in section 5 gives each song 4 bytes. The second byte gives the song's volume. A volume above 127 gives the game's
// default, 110 (0x8BA40 in section 1). The game's code (0x8664 in section 1) gives aux A's high-quality reverb its
// settings. The code gives the chorus a base delay of 15 ms, no variation and a period of 500 ms. The game's set-up
// calls sndVolume(127, 0, 0xFF) alone.
//
// Panel de Pon starts most of its effects through a table at 0x165678 in section 5 (0xAD2A4 and 0xAD328 in section 1).
// The table gives each effect 16 bytes, in the order of the effects' IDs. The byte at +4 gives the effect's volume, and
// the byte at +5 its pan. 0x8BBD8 turns a volume of 0xFF into 110, and a volume or a pan above 127 into 0xFF. It then
// scales the volume by 1.0 and starts the effect through 0xADF50 with the volume, the pan and studio 0. MusyX takes the
// row's volume or pan for 0xFF. 0xAD328 then sets the pan by a player's place. Other calls start an effect through
// 0xADF50 with a volume and a pan of 0xFF.
constexpr PuzzleGame kPanelDePon = {.name = "panepon",
                                    .file = "panepon.szp",
                                    .song_section = 5,
                                    .songs = 0xbd0,
                                    .file_section = 5,
                                    .files = {.id = 1, .project = 0x20350, .pool = 0x8fc0, .sdir = 0x23010},
                                    .samples = "panepon.samp",
                                    .names = kPanelDePonSongs,
                                    .chorus = {15, 0, 500},
                                    .volumes = 0x134148,
                                    .effects = 0x165678};

// The volume that the game gives a song or an effect whose volume is 0xFF or above 127.
constexpr uint8_t kGameDefaultVolume = 110;

// Gives the effects of Panel de Pon's table of effects their volumes and pans (see kPanelDePon). The table is at offset
// `table` of `file`. The effects are `effects`.
void SetTableLevels(std::span<Sound> effects, std::span<const uint8_t> file, std::size_t table)
{
    constexpr std::size_t kEffectRow = 16, kRowVolume = 4, kRowPan = 5;
    constexpr uint8_t kMaxLevel = 0x7f, kUnset = 0xff;

    for (Sound& effect : effects)
    {
        const std::span<const uint8_t> row =
            Part(file, table + std::size_t{effect.id} * kEffectRow, kEffectRow, "a table of effects");
        if (row[kRowVolume] == kUnset)
        {
            effect.volume = kGameDefaultVolume;
        }
        else if (row[kRowVolume] <= kMaxLevel)
        {
            effect.volume = row[kRowVolume];
        }

        if (row[kRowPan] <= kMaxLevel)
        {
            effect.pan = row[kRowPan];
        }
    }
}

// Adds the songs and the effects of `game`. The game pushes groups 1 and 0 of the set. The songs play in group 1. A
// song's ID is the song's number in the table of songs. Group 0 holds the game's effects. The game starts them through
// a function of its own (Dr. Mario's at 0x5BC4C in section 1, Panel de Pon's at 0xADF50), with a volume and a pan of
// 0xFF and studio 0, unless its table of effects gives them others. Its group volume applies to them (see AddMenu).
void AddPuzzleGame(PuzzleSounds& sounds, const AnyCaseFiles& files, const PuzzleGame& game)
{
    constexpr uint16_t kMusic = 1, kEffects = 0;
    constexpr uint8_t kMaxVolume = 0x7f;
    constexpr std::size_t kVolumeEntry = 4;

    Catalogue& catalogue = sounds.catalogue;
    const std::size_t module_number = catalogue.modules.size();
    catalogue.modules.push_back({.chorus = game.chorus});

    const std::vector<uint8_t> file = ntdlib::DecodeYay0(files.Read(game.file));
    const RelModule module(file, game.file);
    const std::size_t set = catalogue.sets.size();
    catalogue.sets.push_back(ModuleSet(module, game.file_section, game.files, files.Read(game.samples)));
    const std::vector<PushedGroup> groups = {{.id = kMusic, .set = set}, {.id = kEffects, .set = set}};
    for (const auto& [id, at] : module.TablePointers(module.At(game.song_section, game.songs)))
    {
        const std::span<const uint8_t> data = module.Object(at);
        Sound sound;
        sound.name = SongName(game.name, game.names, id);
        if (game.volumes != 0)
        {
            const std::size_t row = module.At(game.song_section, game.volumes) + id * kVolumeEntry;
            const uint8_t volume = Part(file, row, kVolumeEntry, "a table of songs' volumes")[1];
            sound.volume = volume > kMaxVolume ? kGameDefaultVolume : volume;
        }

        sound.id = id;
        sound.song_group = kMusic;
        sound.groups = groups;
        sound.module = module_number;
        AddSong(catalogue, std::move(sound), {data.begin(), data.end()});
    }

    const std::size_t first_effect = sounds.effects.size();
    AddModuleEffects(sounds, game.name, set, groups, module_number);
    const std::span<Sound> effects = std::span(sounds.effects).subspan(first_effect);
    for (Sound& effect : effects)
    {
        effect.group_volume = game.group_volume;
    }

    if (game.effects != 0)
    {
        SetTableLevels(effects, file, module.At(game.song_section, game.effects));
    }
}

// Yoshi's Cookie's songs and effects (see ReadPuzzleCollection). ycookie.szp unpacks to the game's module.
// ycookie.bin holds the game's sound files as items. The table at 0xD16D0 in the module's section 5 gives each item's
// offset in units of 32 bytes. An item runs up to the next item's offset. Items 0 to 8 hold the project, the pool and
// the sample directory of groups 0, 1 and 2, in that order. Items 9 to 34 hold songs 0 to 25, and items 35 to 37 the
// samples of groups 0 to 2 (0x4574 in section 1). The table of songs at 0x17928C in section 5 gives each song 44 bytes:
// its group and its ID in big-endian words, then the path of its file in 32 bytes. A song's name gives the path's file
// name. Group 1 holds the game's effects. The game pushes groups 0 and 1 at start-up, and group 2 before a song of that
// group. The game's code (0x41E18 in section 1) gives aux A MusyX's standard reverb, and aux B no effect. At start-up,
// the game also limits MusyX to 40 voices for music and 24 for effects (sndSetMaxVoices, 0x414E4 in section 1).
//
// The game's start-up calls sndVolume(127, 0, 0xFD) and sndVolume(127, 0, 0xFE) (0x4504 in section 1). It then sets
// MusyX's master volumes for music and for effects to the volumes of the two levels in the game's options
// (sndMasterVolume, 0x41B34 and 0x41D58 in section 1). The tables at 0x179B74 and 0x179B7F in section 5 give the volume
// of each level, for music and for effects. Both levels start at 5 (0x1250 in section 1).
//
// The game queues an effect by its row in the table at 0x1797B4 in section 5 (0x41F54 in section 1). A row holds 12
// bytes: the effect's ID in a big-endian word, then the effect's volume, pan, surround pan, motion and reverb send. A
// motion of 0 keeps the pan still. Every row holds a motion of 0. The game starts a queued effect through sndFXStartEx
// with the row's volume and pan and studio 0 (0x42D94 in section 1). The game then sets controller 0x5B to the reverb
// send and controller 0x83 to the surround pan. In stereo, the game gives every effect a surround pan of 64. The table
// holds 80 rows. A row whose effect the project lacks starts nothing. An effect is named "ycookie-se-" and its ID in
// four digits.
void AddYoshisCookie(PuzzleSounds& sounds, const AnyCaseFiles& files)
{
    constexpr uint32_t kData = 5, kItems = 0xd16d0, kSongs = 0x17928c, kMusicLevels = 0x179b74;
    constexpr uint32_t kEffectLevels = 0x179b7f, kEffectRows = 0x1797b4;
    constexpr std::size_t kItemUnit = 32, kSongEntry = 44, kPathOffset = 8, kPathSize = 32, kDefaultLevel = 5;
    constexpr std::size_t kEffectRow = 12, kEffectRowCount = 80, kRowVolume = 4, kRowPan = 5, kRowReverb = 8;
    constexpr uint32_t kGroups = 3, kFirstSong = 9, kSongCount = 26, kFirstSamples = 35;
    constexpr std::array<uint16_t, 2> kStartGroups = {0, 1};
    constexpr uint16_t kEffectGroup = 1;
    constexpr uint8_t kReverbSend = 0x5b, kSurroundPan = 0x83, kStereoSurround = 0x40;

    Catalogue& catalogue = sounds.catalogue;
    const std::size_t module_number = catalogue.modules.size();
    catalogue.modules.push_back({.standard_reverb = true, .voices = VoiceLimits{.music = 40, .effects = 24}});

    const std::vector<uint8_t> file = ntdlib::DecodeYay0(files.Read("ycookie.szp"));
    const RelModule module(file, "ycookie.szp");
    const std::vector<uint8_t> items = files.Read("ycookie.bin");
    const std::size_t offsets = module.At(kData, kItems);
    const auto item = [&](uint32_t number)
    {
        const std::span<const uint8_t> bounds = Part(file, offsets + number * 4, 8, "ycookie.bin's table of items");
        const std::size_t start = xsflib::LoadBe32(bounds.data()) * kItemUnit;
        const std::size_t end = xsflib::LoadBe32(bounds.data() + 4) * kItemUnit;
        const std::span<const uint8_t> bytes =
            Part(items, start, end < start ? 0 : end - start, "an item of ycookie.bin");
        return std::vector<uint8_t>(bytes.begin(), bytes.end());
    };

    const std::size_t first_set = catalogue.sets.size();
    for (uint32_t g = 0; g < kGroups; g++)
    {
        catalogue.sets.push_back({.project = item(g * 3),
                                  .pool = item(g * 3 + 1),
                                  .sdir = item(g * 3 + 2),
                                  .samples = item(kFirstSamples + g)});
    }

    std::vector<PushedGroup> start_groups;
    for (const uint16_t start_group : kStartGroups)
    {
        start_groups.push_back({.id = start_group, .set = first_set + start_group});
    }

    const auto level = [&](uint32_t table, const char* what)
    {
        return Part(file, module.At(kData, table), kDefaultLevel + 1, what)[kDefaultLevel];
    };
    const uint8_t music_level = level(kMusicLevels, "Yoshi's Cookie's volumes for music");
    const std::size_t songs = module.At(kData, kSongs);
    for (uint32_t s = 0; s < kSongCount; s++)
    {
        const std::span<const uint8_t> row = Part(file, songs + s * kSongEntry, kSongEntry, "Yoshi's Cookie's songs");
        const uint32_t group = xsflib::LoadBe32(row.data()), id = xsflib::LoadBe32(row.data() + 4);
        if (group >= kGroups || id > 0xffff)
        {
            throw xsflib::FormatError("Yoshi's Cookie's song " + std::to_string(s) + " has no group");
        }

        const std::string name = Stem(TextBeforeZero(row.subspan(kPathOffset, kPathSize)));
        Sound sound;
        sound.name = "ycookie-" + name;
        sound.id = static_cast<uint16_t>(id);
        sound.song_group = static_cast<uint16_t>(group);
        sound.groups = start_groups;
        if (std::find(kStartGroups.begin(), kStartGroups.end(), group) == kStartGroups.end())
        {
            sound.groups.push_back({.id = sound.song_group, .set = first_set + group});
        }

        sound.master_volume = music_level;
        sound.module = module_number;
        AddSong(catalogue, std::move(sound), item(kFirstSong + s));
    }

    std::set<uint16_t> known;
    for (const ProjectEffect& effect : EffectsOf(catalogue.sets.at(first_set + kEffectGroup).project))
    {
        known.insert(effect.row.id);
    }

    const uint8_t effect_level = level(kEffectLevels, "Yoshi's Cookie's volumes for effects");
    const std::span<const uint8_t> rows =
        Part(file, module.At(kData, kEffectRows), kEffectRow * kEffectRowCount, "Yoshi's Cookie's effects");
    for (std::size_t r = 0; r < kEffectRowCount; r++)
    {
        const std::span<const uint8_t> row = rows.subspan(r * kEffectRow, kEffectRow);
        const uint32_t id = xsflib::LoadBe32(row.data());
        if (id > 0xffff || !known.contains(static_cast<uint16_t>(id)))
        {
            continue;
        }

        Sound sound;
        sound.name = EffectName("ycookie-se", static_cast<uint16_t>(id));
        sound.kind = SoundKind::kEffect;
        sound.volume = row[kRowVolume];
        sound.pan = row[kRowPan];
        sound.id = static_cast<uint16_t>(id);
        sound.groups = start_groups;
        sound.controls = {{.controller = kReverbSend, .value = row[kRowReverb]},
                          {.controller = kSurroundPan, .value = kStereoSurround}};
        sound.master_volume = effect_level;
        sound.module = module_number;
        sounds.effects.push_back(std::move(sound));
    }
}

} // namespace

// Nintendo Puzzle Collection's songs and effects. The disc holds a menu and three games: Dr. Mario, Panel de Pon and
// Yoshi's Cookie. main.dol loads each of them as a module of code, from a file that Yay0 packs. Some of the paths in
// the code differ in case from the disc's names. The disc's library matches names in any case. Each module sets up
// studio 0's aux effects for its sounds (Catalogue::modules). The menu, Dr. Mario and Panel de Pon give aux A the
// high-quality reverb and aux B a chorus. Yoshi's Cookie gives aux A the standard reverb, and aux B no effect. The
// catalogue holds the menu's songs, then those of Dr. Mario, Panel de Pon and Yoshi's Cookie. The effects of the menu
// and the three games follow, in the same order. A sound's name starts with the module's name, such as
// "panepon-flower_01" or "panepon-se-0012".
Catalogue ReadPuzzleCollection(const disc::GameFiles& game)
{
    const AnyCaseFiles files(game);
    PuzzleSounds sounds;
    AddMenu(sounds, files);
    AddPuzzleGame(sounds, files, kDrMario);
    AddPuzzleGame(sounds, files, kPanelDePon);
    AddYoshisCookie(sounds, files);
    for (Sound& effect : sounds.effects)
    {
        sounds.catalogue.sounds.push_back(std::move(effect));
    }

    return std::move(sounds.catalogue);
}

} // namespace ntdlib::musyx::detail
