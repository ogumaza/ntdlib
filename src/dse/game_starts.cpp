// SPDX-License-Identifier: MIT

#include "ntdlib/dse/game_starts.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/text.h"

#include "ntdlib/dse/files.h"
#include "ntdlib/dse/sets.h"
#include "ntdlib/ntr/code.h"

namespace ntdlib::dse
{
namespace
{

// A song that a game starts below full level or in several states, and its states in order. `games` lists the codes of
// the games that the row covers, with a space between them. A path that ends with a slash covers every song in that
// folder.
struct Row
{
    std::string_view games;
    std::string_view path;
    std::initializer_list<SongState> states;
};

// The rule of the rows.
//
// - A start counts once for each scene. In the three games, a start of the song that already plays does nothing. The
//   later scripts of a scene that name the song therefore start nothing. A count of script commands would count them.
// - A fade at the start and a change of the level later in a scene are the scene's action. Neither is a state.
// - The first state is the level of most scenes. The song of a dungeon's floors plays for as long as the player stays
//   there, and its level counts as the level of most starts. A tie goes to the level that the main story starts first,
//   then to the level of the first track.
// - Another level gets a state when at least two scenes use it, at least a quarter of the song's scenes use it, and it
//   differs from the first state by 1 dB or more. A level of one scene is that scene's mix. The volume acts on the
//   amplitude by about its square: Explorers of Sky's bgm0089 plays 4.6 dB lower at a level of 200 than at full level
//   on the game's code. By that law a level of 240 is 1.1 dB below full level, and one of 255 is 0.1 dB below it.
// - A game's sound test starts every song at full level. It shows no state of a song and gives none.
// - A song gets a row when its first state isn't full level or it has another state.
//
// Pokémon Mystery Dungeon: Explorers of Sky (North America, C2SE). The game's start of music at a level,
// PlayBgmByIdVolume(number, fade in frames, level) at 0x02017E70, queues the number, the level and the fade for the
// sound thread. PlayBgmById (0x02017DF4) queues the same with level 256 and no fade. Both return at once when the
// number is the playing piece's (0x02017E7C to 0x02017E94). The sound thread's PlayBgm (0x020198B8) starts DSE with
// the volume level × 127 >> 8 and the fade as the volume's time, fade × 1000 / 60 ms. The second track's start
// (0x020180A0) does the same for a piece that plays over the first.
//
// The scripts start music with bgm_PlayFadeIn (number, fade, level) and bgm_Play (number, at full level), and on the
// second track with bgm2_PlayFadeIn and bgm2_Play. Overlay 11 runs them (its opcode table at 0x02318610; bgm_PlayFadeIn
// is opcode 32, and reaches 0x02017E70 at 0x022E2118). The dungeons start their music at full level (overlay 29,
// 0x022EB0E8), and none of the rows' pieces has a dungeon's name (B_DUN_). The Sky Jukebox (overlay 9, 0x0233DA2C) and
// the game's other starts call PlayBgmById. The rows count the scenes of the 3,760 script files (SCRIPT/*.ssb), on both
// tracks. A scene is a script's name without its last two digits: m26a0301 to m26a0308 are the scripts of scene m26a03.
// The m scripts are the main story, in the order of their names.
//
// - bgm0086: 180 in scenes m26a05 and m26a06, and 256 in n01a31 and s22p15. The main story starts it at 180.
// - bgm0087 and bgm0089: 200 in scene m26a06 alone. bgm0121: 192 in m26a04 and m26a05.
// - bgm0102: 180 on the first track and 140 on the second, both in scene m03a13.
// - bgm0125: 256 in four scenes, 190 in m09a06 and m10a09, and 160 and 128 in one scene each.
// - bgm0127: 128 in six scenes, and 64, 92 and 190 in one each.
// - bgm0181: 256 and 190 in scenes n04a01 and n04a26 each. The first track's start is at 256.
// - bgm0182: 190 in scenes m12a01 and n01a07, and 256 in n02a09. bgm0189: 190 in s31a28 and s31a34, and 256 in n06a01.
// - Not rows: bgm0084 starts at 256 in four scenes, at 200 in scene m26a03 alone and at 160 in m26a08 alone. bgm0100
//   starts at 256 in 43 scenes and at 128 in 10.
//
// Explorers of Time and of Darkness (North America, YFTE and YFYE) contain the same code and the same starts in their
// scripts. The start at a level is 0x02012490, with the same check of the playing piece, and the sound thread makes
// the volume at 0x0205E588 to 0x0205E59C. The scripts are overlay 4's (bgm_PlayFadeIn is opcode 28). The dungeons
// (overlay 5, 0x022E4744) and the menus start at full level.
//
// - bgm0086: 180 in scenes m26a05 and m26a06, and 256 in s22p15 alone. bgm0087 and bgm0089: 200 in m26a06 alone.
// - bgm0102: 180 and 140 in scene m03a13, as in Sky. bgm0121: 192 in m26a04 and m26a05.
// - bgm0125: as in Sky. bgm0127: 128 in scenes m17a01 and s22p06, and 64 and 190 in one each.
// - Not a row: bgm0084 starts at 256, at 200 and at 160 in one scene each. The main story starts it at 256 first.
//
// Fushigi no Dungeon: Fuurai no Shiren DS 2 (Japan, YWFJ). A request (0x0212F3B8: song, level) does nothing for the
// song that plays. Its start (0x0212EDA0) gives DSE the volume 127 for the level -1 and level × 127 / 256 otherwise
// (0x0212EDEC to 0x0212EDF8), and the centre pan. The scripts of the 179 events (overlays 6 to 54; their handlers'
// table at 0x0215C39C) start a song at full level with opcode 25 and at a level with opcode 26. A scene is an event.
// The code starts every song at full level: the floors' songs from the table at 0x0215E0E4 (0x0202E010), the menus,
// the title and the staff roll. Opcode 28 starts a town's song at full level. The story's state chooses the song.
// A level of 255 counts as full level: each song with such a start has a start at full level too.
//
// - ATEKA2: 240 in all four events. ATEKA2_S: 240 in its one event. ATEKA2_L: 235 in its one event.
// - CASTLE: 240 in all five events. CASTLE_S: 240 in all three. OPENING_CASTLE: 240 in event 0, the opening.
// - LOW_TONE: 220 in six events. Full level in event 9, and in the towns in one state of the story.
// - BOOM2: full level on its floors, and 220 in events 5, 51 and 53.
// - G_BOSS: full level on its floor, and 240 in events 39 and 40.
// - Not a row: OUGON starts at full level on its floors and at 235 in event 41 alone.
//
// Professor Layton and the Diabolical Box (YLTE) and Professor Layton and the Unwound Future (C3JE) have no rows. Each
// start of a song passes the level 1.0, for a volume of 127 (0x0207E1A4 in the first game, 0x0205F240 in the second),
// and neither game sets a song's pan. Their starts differ in the fade alone.
//
// Luminous Arc (North America, ANIE) starts every song in data/Snd.iear at one volume, with no fade (0x02095030,
// through 0x020BFE98). The volume is the music's level × 127 × 0xFF / 255 >> 8, times 0.8 and truncated (0x02094D0C).
// The music's level is 0xFF at the game's start (0x0209455C). That gives a volume of 100. The game's option for the
// music's level (0x020952D4) selects 0, 0x87 or 0xFF. The row's level of 202 stands for that volume: 202 × 127 >> 8 is
// 100.
constexpr std::string_view kSky = "C2SE";
constexpr std::string_view kTime = "YFTE YFYE";
constexpr std::string_view kShirenDs2 = "YWFJ";
constexpr std::string_view kLuminousArc = "ANIE";
const Row kRows[] = {
    {kSky, "SOUND/BGM/bgm0086.smd", {{"", 180}, {"_LEVEL256", kFullLevel}}},
    {kSky, "SOUND/BGM/bgm0087.smd", {{"", 200}}},
    {kSky, "SOUND/BGM/bgm0089.smd", {{"", 200}}},
    {kSky, "SOUND/BGM/bgm0102.smd", {{"", 180}}},
    {kSky, "SOUND/BGM/bgm0121.smd", {{"", 192}}},
    {kSky, "SOUND/BGM/bgm0125.smd", {{"", kFullLevel}, {"_LEVEL190", 190}}},
    {kSky, "SOUND/BGM/bgm0127.smd", {{"", 128}}},
    {kSky, "SOUND/BGM/bgm0181.smd", {{"", kFullLevel}, {"_LEVEL190", 190}}},
    {kSky, "SOUND/BGM/bgm0182.smd", {{"", 190}}},
    {kSky, "SOUND/BGM/bgm0189.smd", {{"", 190}}},

    {kTime, "SOUND/BGM/bgm0086.smd", {{"", 180}}},
    {kTime, "SOUND/BGM/bgm0087.smd", {{"", 200}}},
    {kTime, "SOUND/BGM/bgm0089.smd", {{"", 200}}},
    {kTime, "SOUND/BGM/bgm0102.smd", {{"", 180}}},
    {kTime, "SOUND/BGM/bgm0121.smd", {{"", 192}}},
    {kTime, "SOUND/BGM/bgm0125.smd", {{"", kFullLevel}, {"_LEVEL190", 190}}},
    {kTime, "SOUND/BGM/bgm0127.smd", {{"", 128}}},

    {kShirenDs2, "mid/SND_BGM_M_ATEKA2.SMD", {{"", 240}}},
    {kShirenDs2, "mid/SND_BGM_M_ATEKA2_L.SMD", {{"", 235}}},
    {kShirenDs2, "mid/SND_BGM_M_ATEKA2_S.SMD", {{"", 240}}},
    {kShirenDs2, "mid/SND_BGM_M_BOOM2.SMD", {{"", kFullLevel}, {"_LEVEL220", 220}}},
    {kShirenDs2, "mid/SND_BGM_M_CASTLE.SMD", {{"", 240}}},
    {kShirenDs2, "mid/SND_BGM_M_CASTLE_S.SMD", {{"", 240}}},
    {kShirenDs2, "mid/SND_BGM_M_G_BOSS.SMD", {{"", kFullLevel}, {"_LEVEL240", 240}}},
    {kShirenDs2, "mid/SND_BGM_M_LOW_TONE.SMD", {{"", 220}, {"_LEVEL256", kFullLevel}}},
    {kShirenDs2, "mid/SND_BGM_M_OPENING_CASTLE.SMD", {{"", 240}}},

    {kLuminousArc, "data/Snd.iear/", {{"", 202}}},
};

// The length of a game's code, the DSE volume of a start at full level, and the shift of the level's scale.
constexpr std::size_t kCodeLength = 4;
constexpr int kFullVolume = 127;
constexpr int kLevelShift = 8;

// Whether a row's `row_path` covers the song at `path`: the same path, or the folder of a path that ends with a slash.
bool CoversPath(std::string_view row_path, std::string_view path)
{
    if (row_path.ends_with('/'))
    {
        return path.starts_with(row_path) && path.find('/', row_path.size()) == std::string_view::npos;
    }

    return row_path == path;
}

// Whether `games`, a row's list of codes, names `game_code`.
bool Covers(std::string_view games, std::string_view game_code)
{
    if (game_code.size() != kCodeLength)
    {
        return false;
    }

    for (std::size_t at = 0; at + kCodeLength <= games.size(); at += kCodeLength + 1)
    {
        if (games.substr(at, kCodeLength) == game_code)
        {
            return true;
        }
    }

    return false;
}

// Where a game's code keeps a value: the immediate operand of an instruction that moves it into a register. An ARM
// instruction is MOV Rd, #immediate (the 8 bits rotated right by twice the 4 bits above them). A Thumb instruction is
// MOVS Rd, #immediate (8 bits).
struct Immediate
{
    uint32_t address = 0;
    bool thumb = false;
};

// A table of levels by song number in a game's binary: 8-byte entries, a signed 32-bit number and the level in the next
// byte, up to the number -1. The songs are the entries of a pack, by its index: a 32-bit offset in the pack for each
// entry, then the pack's size. An entry's number is `number_base` plus its place in the index. The code returns the
// level at `unlisted` for a number that the table doesn't list.
struct LevelTable
{
    std::string_view pack;
    std::string_view index;
    uint32_t table = 0;
    int32_t number_base = 0;
    Immediate unlisted;
};

// A start of a song in the code of one of the game's events, in Thumb code in the ARM9 overlay `overlay`: the call at
// `start` of the events' music start, with the song's number and the request's volume at the immediates `song` and
// `volume`. An event that lowers the song in the same frame then calls the events' change of the music's volume at
// `change`, with the immediates `change_volume` and `change_frames`. `change` is 0 for a start without a change.
// `event` is the event's number in the game's table of events. Each row is the only start of the song in its event, and
// each event is a scene.
struct EventStart
{
    uint32_t event = 0;
    uint32_t overlay = 0;
    uint32_t start = 0;
    Immediate song;
    Immediate volume;
    uint32_t change = 0;
    Immediate change_volume;
    Immediate change_frames;
};

// A game whose request of a song makes DSE's volume from the song's level in a table and the level of the music's
// option: level × option level / 127, rounded down, then × the request's volume / 127, rounded down. The option's level
// is entry `option` of the `option_count` halfwords at `option_levels`. The settings of a new game hold the option at
// the immediate `option`. The game's code requests a song at the volume 127, apart from the starts in `event_starts`.
// The volume 127 leaves the level as it is. Their routines are the events' music start `event_start` (the number and
// the volume, passed to the request of music that reads the first of `tables`) and the events' change of the music's
// volume `event_change` (the volume and the frames). The game makes the target of the change from the song's levels as
// the request does.
struct TableGame
{
    std::string_view game_code;
    uint8_t revision = 0;
    Immediate option;
    uint32_t option_levels = 0;
    uint32_t option_count = 0;
    std::array<LevelTable, 2> tables;
    uint32_t event_start = 0;
    uint32_t event_change = 0;
    std::span<const EventStart> event_starts = {};
};

// Fushigi no Dungeon: Fuurai no Shiren 4: Kami no Hitomi to Akuma no Heso (Japan, B4FJ), Thumb code. The request of
// music (0x0200CA98: the sound object at [0x020FEBEC], the number, the volume 127 and 1) plays entry N of sound/bgm.bin
// by the offsets in sound/bgm.ent. It takes the song's level from the table at 0x020C535C (0x0200D850, 127 for a number
// that the table doesn't list at 0x0200D86E), the option's level from the halfwords at 0x020C5214 by the option at
// +0x6C of the sound object (0x0200D07C), and makes the volume at 0x0200D4DC. The request of a short piece (0x0200CD34:
// 0x401 + N) plays entry N of sound/me_bgm.bin by sound/me_bgm.ent with the table at 0x020C52AC (0x0200D87C, 127 at
// 0x0200D89A). The option's setter (0x0200D018) keeps it from 0 to 4. The sound object's start-up sets the option to 3
// (0x0200C158), and the settings' reset sets their music option to 3 (0x02013DF4, at 0x02013E40). On a blank backup
// memory, the game resets its settings before any music. Every start of music up to the menu of a new game then plays
// at option 3: the title's song 66 at its level of 110 gives 93.
//
// Fushigi no Dungeon: Fuurai no Shiren 5: Fortune Tower to Unmei no Dice (Japan, B5FJ), Shiren 4's code at other
// addresses. The request of music is 0x0200DE60, with the table at 0x020E032C (0x0200EF3C, 127 at 0x0200EF7C), and the
// request of a short piece 0x0200E134, with the table at 0x020E025C (0x0200EF88, 127 at 0x0200EFC8). The option at
// +0x70 of the sound object picks one of the 6 halfwords at 0x020E01BC (0x0200E5FC). The setter (0x0200E57C) keeps it
// from 0 to 5. The settings' reset (0x02014E48) sets the music option to 4 (0x02014E58), and the game gives it to the
// setter (0x020154A0) on a blank backup memory before any music. The title's song 70 at 104 gives 73.
//
// Shiren 5's events are Thumb code in ARM9 overlays at 0x021E5900, run by number from the table at 0x0216CCC4 (overlay
// 12, 0x02168534 and 0x02168550). They start music with overlay 12's 0x02161DC4 (the number in r2, the fade in frames
// in r3 and the volume on the stack). That routine requests the song at that volume (0x0200DE60), or at 0 with a fade
// to it for a fade above 0. Every start passes 127. Some events lower the song in the same frame with overlay 12's
// 0x02161E44 (the frames in r2 and the volume in r3). 0x02161E44 calls 0x0200E648. That routine makes its target as the
// request makes its volume (0x0200EC20) and sets the sound object's fader (0x0200D264). The fader reaches the target
// over the frames (0x0200D280, then 0x0200D244 gives DSE the volume). A change in the same frame is part of the start.
// The rows list every start in the events of each song that an event lowers. Neither the floors' songs (the tables at
// 0x020BDEFC and 0x020BDEFE, 0x0202EFC8 to 0x0202EFE4) nor the code's other requests start these songs. Those requests
// start the numbers 55 and 70 (overlay 15 0x021C5536, overlay 18 0x021C255E and 0x021C616E), or start again the song
// that played (0x0200EDB6, and overlay 38 0x021C520E after a short piece).
//
// - BGM_THE_RIIBA_2 (entry 73): events 217 and 226 lower it to 102, and event 393 starts it at 127. 102 is the level
//   of most events. Overlay 193's story steps run event 217 below the step 11 and event 226 at the step 11 (0x021E5DEE
//   to 0x021E5E02, the step at 0x0213F60C + 8).
// - BGM_EVE_JIROKIC (entry 59): event 233 lowers it to 114. Its only start.
// - BGM_THE_RIIBA_1 (entry 72): event 186 lowers it to 100. Its only start.
// - Not rows: BGM_THE_SHIREN_ (entry 74) starts at 114 in event 254 and at 127 in event 393, one event each. The
//   game's code doesn't show which of the two the story reaches first. Event 254 runs from event 216's chain
//   (overlay 106, 0x021E592A). Nothing found shows what runs event 393: every event goes through the same loader
//   (overlay 12, 0x0214F174). Entry 1, a boss's song, starts at 102 in event 217, and the boss floors start it at 127
//   (0x0202EF9C to 0x0202EFC4).
//
// The request at 127 and then the change give DSE 77, then 61, for entry 73; 75, then 67, for entry 59; and 88, then
// 69, for entry 72.
constexpr EventStart kShirenFiveEvents[] = {
    {.event = 217,
     .overlay = 106,
     .start = 0x021E6142,
     .song = {.address = 0x021E613E, .thumb = true},
     .volume = {.address = 0x021E6138, .thumb = true},
     .change = 0x021E6150,
     .change_volume = {.address = 0x021E614E, .thumb = true},
     .change_frames = {.address = 0x021E5982, .thumb = true}},
    {.event = 226,
     .overlay = 106,
     .start = 0x021E7460,
     .song = {.address = 0x021E745A, .thumb = true},
     .volume = {.address = 0x021E7456, .thumb = true},
     .change = 0x021E746E,
     .change_volume = {.address = 0x021E746C, .thumb = true},
     .change_frames = {.address = 0x021E746A, .thumb = true}},
    {.event = 393,
     .overlay = 98,
     .start = 0x021E82C8,
     .song = {.address = 0x021E82C4, .thumb = true},
     .volume = {.address = 0x021E82BE, .thumb = true}},
    {.event = 233,
     .overlay = 106,
     .start = 0x021E8544,
     .song = {.address = 0x021E8540, .thumb = true},
     .volume = {.address = 0x021E853A, .thumb = true},
     .change = 0x021E8552,
     .change_volume = {.address = 0x021E8550, .thumb = true},
     .change_frames = {.address = 0x021E854E, .thumb = true}},
    {.event = 186,
     .overlay = 118,
     .start = 0x021E88B4,
     .song = {.address = 0x021E88B0, .thumb = true},
     .volume = {.address = 0x021E88AA, .thumb = true},
     .change = 0x021E88C2,
     .change_volume = {.address = 0x021E88C0, .thumb = true},
     .change_frames = {.address = 0x021E88BE, .thumb = true}},
};

constexpr std::array<TableGame, 2> kTableGames = {{
    {.game_code = "B4FJ",
     .option = {.address = 0x02013E40},
     .option_levels = 0x020C5214,
     .option_count = 5,
     .tables = {{{.pack = "sound/bgm.bin",
                  .index = "sound/bgm.ent",
                  .table = 0x020C535C,
                  .unlisted = {.address = 0x0200D86E, .thumb = true}},
                 {.pack = "sound/me_bgm.bin",
                  .index = "sound/me_bgm.ent",
                  .table = 0x020C52AC,
                  .number_base = 0x401,
                  .unlisted = {.address = 0x0200D89A, .thumb = true}}}}},
    {.game_code = "B5FJ",
     .option = {.address = 0x02014E58, .thumb = true},
     .option_levels = 0x020E01BC,
     .option_count = 6,
     .tables = {{{.pack = "sound/bgm.bin",
                  .index = "sound/bgm.ent",
                  .table = 0x020E032C,
                  .unlisted = {.address = 0x0200EF7C, .thumb = true}},
                 {.pack = "sound/me_bgm.bin",
                  .index = "sound/me_bgm.ent",
                  .table = 0x020E025C,
                  .number_base = 0x401,
                  .unlisted = {.address = 0x0200EFC8, .thumb = true}}}},
     .event_start = 0x02161DC4,
     .event_change = 0x02161E44,
     .event_starts = kShirenFiveEvents},
}};

// A game whose sound player starts every song with DSE's volume (A × B × C >> 7) / 1000, from its halfwords A, B and
// C. The player's start-up sets A and B to the immediate `level` and C to the immediate `unit`, the fade's unit.
struct PlayerGame
{
    std::string_view game_code;
    uint8_t revision = 0;
    Immediate level;
    Immediate unit;
};

// Inazuma Eleven 1, 2, 3 Bomber and 3 The Ogre (Japan; the first two in revision 1), ARM code. The player's start-up
// sets three levels to 127 and two units to 1000 (Inazuma Eleven 2: 0x0202B7EC to 0x0202B804). Its start of a song
// gives DSE the volume (level × level × unit >> 7) / 1000 (Inazuma Eleven 2: 0x0202D638 to 0x0202D67C, from the
// halfwords at 0x10, 0x12 and 0x16 of the levels). The volume is 126 for 127 and 1000. The four games therefore start
// every song at 126.
constexpr std::array<PlayerGame, 4> kPlayerGames = {{
    {.game_code = "YEEJ", .revision = 1, .level = {.address = 0x02039D60}, .unit = {.address = 0x02039D70}},
    {.game_code = "BEEJ", .revision = 1, .level = {.address = 0x0202B7EC}, .unit = {.address = 0x0202B7FC}},
    {.game_code = "BEZJ", .level = {.address = 0x0202CE98}, .unit = {.address = 0x0202CEAC}},
    {.game_code = "BOEJ", .level = {.address = 0x0202B554}, .unit = {.address = 0x0202B568}},
}};

// The divisor of the fade's unit and the shift of the player's volume, and the divisor of a table game's option level.
constexpr int kUnitDivisor = 1000;
constexpr int kPlayerShift = 7;
constexpr int kOptionDivisor = 127;

// The ARM9 binary of a game or one of its ARM9 overlays, decompressed, and reads of its bytes at main-memory addresses.
class Binary
{
public:
    explicit Binary(const ntr::Rom& rom) : code_(ntr::Arm9Code(rom)), name_("the ARM9 binary")
    {
    }

    // The ARM9 overlay `id`. Throws xsflib::FormatError when the image has no such overlay.
    Binary(const ntr::Rom& rom, uint32_t id) : name_("the ARM9 overlay " + std::to_string(id))
    {
        const auto overlay = std::ranges::find(rom.Arm9Overlays(), id, &ntr::Overlay::id);
        if (overlay == rom.Arm9Overlays().end() || overlay->file_id >= rom.Files().size())
        {
            throw xsflib::FormatError("the image has no ARM9 overlay " + std::to_string(id));
        }

        code_ = {.address = overlay->ram_address, .bytes = ntr::OverlayBytes(rom.FileData(overlay->file_id), *overlay)};
    }

    uint32_t Read(uint32_t address, int bytes) const
    {
        if (address < code_.address || address - code_.address + static_cast<uint32_t>(bytes) > code_.bytes.size())
        {
            throw xsflib::FormatError(name_ + " doesn't hold " + xsflib::Hex(address, 8, true));
        }

        uint32_t value = 0;
        for (int i = 0; i < bytes; i++)
        {
            value |= uint32_t{code_.bytes[address - code_.address + static_cast<uint32_t>(i)]} << (8 * i);
        }

        return value;
    }

    // The immediate of the instruction at `at`.
    int ImmediateAt(const Immediate& at) const
    {
        if (at.thumb)
        {
            const uint32_t instruction = Read(at.address, 2);
            if ((instruction & 0xF800) != 0x2000)
            {
                throw xsflib::FormatError("no MOVS of an immediate at " + xsflib::Hex(at.address, 8, true));
            }

            return static_cast<int>(instruction & 0xFF);
        }

        const uint32_t instruction = Read(at.address, 4);
        if ((instruction & 0x0FEF0000) != 0x03A00000)
        {
            throw xsflib::FormatError("no MOV of an immediate at " + xsflib::Hex(at.address, 8, true));
        }

        const uint32_t rotation = 2 * ((instruction >> 8) & 0xF);
        const uint32_t value = instruction & 0xFF;

        return static_cast<int>(rotation == 0 ? value : (value >> rotation | value << (32 - rotation)));
    }

    // The routine that the Thumb BL or BLX at `address` calls.
    uint32_t ThumbCallTarget(uint32_t address) const
    {
        const uint32_t high = Read(address, 2);
        const uint32_t low = Read(address + 2, 2);
        if ((high & 0xF800) != 0xF000 || (low & 0xE800) != 0xE800)
        {
            throw xsflib::FormatError("no Thumb call at " + xsflib::Hex(address, 8, true));
        }

        // A 23-bit offset from the halves, from the address after the call. BLX goes to ARM code on a word.
        uint32_t offset = (high & 0x7FF) << 12 | (low & 0x7FF) << 1;
        if ((offset & 0x400000) != 0)
        {
            offset |= 0xFF800000;
        }

        const uint32_t target = address + 4 + offset;

        return (low & 0xF800) == 0xE800 ? target & ~3u : target;
    }

private:
    ntr::LoadedCode code_;
    std::string name_;
};

// The bytes of the file at `path` in `rom`.
std::span<const uint8_t> FileAt(const ntr::Rom& rom, std::string_view path)
{
    const ntr::RomFile* file = rom.Find(path);
    if (file == nullptr)
    {
        throw xsflib::FormatError("the image has no " + std::string(path));
    }

    return rom.FileData(file->id);
}

// The level of each number in `table`'s table.
std::vector<std::pair<int32_t, int>> TableLevels(const Binary& binary, const LevelTable& table)
{
    constexpr std::size_t kMostEntries = 4096;
    constexpr uint32_t kEntryBytes = 8;
    constexpr int32_t kEnd = -1;

    std::vector<std::pair<int32_t, int>> levels;
    for (uint32_t at = table.table;; at += kEntryBytes)
    {
        const auto number = static_cast<int32_t>(binary.Read(at, 4));
        if (number == kEnd)
        {
            break;
        }
        if (levels.size() == kMostEntries)
        {
            throw xsflib::FormatError("the table at " + xsflib::Hex(table.table, 8, true) + " doesn't end");
        }

        levels.emplace_back(number, static_cast<int>(binary.Read(at + 4, 1)));
    }

    return levels;
}

// The DSE volume of a request of the song `number` at the volume 127: the song's level in `levels`, or `unlisted` for a
// number that they don't list, times the option's level `option_level` / 127.
int TableVolume(const std::vector<std::pair<int32_t, int>>& levels, int unlisted, int option_level, int32_t number)
{
    const auto listed = std::ranges::find(levels, number, &std::pair<int32_t, int>::first);

    return (listed == levels.end() ? unlisted : listed->second) * option_level / kOptionDivisor;
}

// The state of each song that `game`'s events start (TableGame::event_starts), by the song's number. `volume_of` gives
// a song's DSE volume at the request's volume of 127. A start's volume is that × the start's volume / 127, rounded
// down, and a change's target is that × the change's volume / 127, as the game makes them. The state is the level of
// most events, with the change of that event's start.
std::map<int32_t, SongState> EventStates(const ntr::Rom& rom, const TableGame& game,
                                         const std::function<int(int32_t)>& volume_of)
{
    // The state of each event's start of each song.
    std::map<int32_t, std::vector<SongState>> by_song;
    std::map<uint32_t, Binary> overlays;
    for (const EventStart& start : game.event_starts)
    {
        const Binary& code = overlays.try_emplace(start.overlay, rom, start.overlay).first->second;
        if (code.ThumbCallTarget(start.start) != game.event_start)
        {
            throw xsflib::FormatError("the call at " + xsflib::Hex(start.start, 8, true) + " in the event " +
                                      std::to_string(start.event) + " isn't the music start");
        }

        const int32_t number = code.ImmediateAt(start.song);
        const int full = volume_of(number);
        SongState state = {.level = LevelOfVolume(full * code.ImmediateAt(start.volume) / kFullVolume)};
        if (start.change != 0)
        {
            if (code.ThumbCallTarget(start.change) != game.event_change)
            {
                throw xsflib::FormatError("the call at " + xsflib::Hex(start.change, 8, true) + " in the event " +
                                          std::to_string(start.event) + " isn't the change of the music's volume");
            }

            const int volume = code.ImmediateAt(start.change_volume);
            state.change = StartChange{
                .start_level = state.level, .volume = volume, .frames = code.ImmediateAt(start.change_frames)};
            state.level = LevelOfVolume(full * volume / kFullVolume);
        }

        by_song[number].push_back(state);
    }

    std::map<int32_t, SongState> states;
    for (const auto& [number, events] : by_song)
    {
        std::map<int, int> counts;
        for (const SongState& event : events)
        {
            counts[event.level]++;
        }

        const auto most = std::ranges::max_element(counts, {}, &std::pair<const int, int>::second);
        if (std::ranges::count(counts | std::views::values, most->second) > 1)
        {
            throw xsflib::FormatError("the events start the song " + std::to_string(number) +
                                      " at two levels in as many events");
        }

        states[number] = *std::ranges::find(events, most->first, &SongState::level);
    }

    return states;
}

// The state of each song of `table`'s pack, by its path: the state in `event_states` of a song that the game's events
// start, or else the level whose start has the volume that the song's level in the table gives at the option's level
// `option_level`.
void ReadTable(const ntr::Rom& rom, const Binary& binary, const LevelTable& table, int option_level,
               const std::map<int32_t, SongState>& event_states,
               std::map<std::string, SongState, std::less<>>& song_states)
{
    const std::vector<std::pair<int32_t, int>> levels = TableLevels(binary, table);
    const int unlisted = binary.ImmediateAt(table.unlisted);

    const std::span<const uint8_t> index = FileAt(rom, table.index);
    std::vector<uint32_t> offsets;
    for (std::size_t at = 0; at + 4 <= index.size(); at += 4)
    {
        offsets.push_back(xsflib::LoadLe32(index.data() + at));
    }
    if (offsets.size() < 2)
    {
        throw xsflib::FormatError(std::string(table.index) + " holds no entry");
    }

    const std::span<const uint8_t> pack = FileAt(rom, table.pack);
    const std::vector<NamedFile> found = FindFiles(std::vector<NamedFile>{{std::string(table.pack), pack}});
    for (const NamedFile& file : found)
    {
        if (file.bytes.size() < 4 || !std::ranges::equal(file.bytes.first(4), std::string_view("smdl")))
        {
            continue;
        }

        // The entry whose range holds the song.
        const auto offset = static_cast<uint32_t>(file.bytes.data() - pack.data());
        const auto next = std::ranges::upper_bound(offsets.begin(), offsets.end() - 1, offset);
        if (next == offsets.begin())
        {
            continue;
        }

        const auto number = table.number_base + static_cast<int32_t>(next - offsets.begin() - 1);
        const auto event = event_states.find(number);
        if (event != event_states.end())
        {
            song_states[file.path] = event->second;
            continue;
        }

        song_states[file.path] = {.level = LevelOfVolume(TableVolume(levels, unlisted, option_level, number))};
    }
}

} // namespace

std::vector<SongState> SongStatesOf(std::string_view game_code, std::string_view path)
{
    const auto listed = [&](const Row& row)
    {
        return CoversPath(row.path, path) && Covers(row.games, game_code);
    };
    const auto row = std::ranges::find_if(kRows, listed);
    if (row == std::end(kRows))
    {
        return {SongState{}};
    }

    return {row->states.begin(), row->states.end()};
}

GameStarts::GameStarts(const ntr::Rom& rom) : game_code_(rom.Header().game_code)
{
    const uint8_t revision = rom.Header().version;
    for (const TableGame& game : kTableGames)
    {
        if (game.game_code != game_code_ || game.revision != revision)
        {
            continue;
        }

        const Binary binary(rom);
        const int option = binary.ImmediateAt(game.option);
        if (option < 0 || static_cast<uint32_t>(option) >= game.option_count)
        {
            throw xsflib::FormatError("the option " + std::to_string(option) + " has no level");
        }

        const auto option_level =
            static_cast<int>(binary.Read(game.option_levels + 2 * static_cast<uint32_t>(option), 2));
        const LevelTable& music = game.tables[0];
        const auto volume_of =
            [&, levels = TableLevels(binary, music), unlisted = binary.ImmediateAt(music.unlisted)](int32_t number)
        {
            return TableVolume(levels, unlisted, option_level, number);
        };
        const std::map<int32_t, SongState> event_states = EventStates(rom, game, volume_of);
        for (const LevelTable& table : game.tables)
        {
            ReadTable(rom, binary, table, option_level, event_states, song_states_);
        }

        read_ = true;
    }

    for (const PlayerGame& game : kPlayerGames)
    {
        if (game.game_code != game_code_ || game.revision != revision)
        {
            continue;
        }

        const Binary binary(rom);
        const int level = binary.ImmediateAt(game.level);
        const int unit = binary.ImmediateAt(game.unit);
        every_song_ = LevelOfVolume((level * level * unit >> kPlayerShift) / kUnitDivisor);
        read_ = true;
    }
}

std::vector<SongState> GameStarts::StatesOf(std::string_view path) const
{
    if (!read_)
    {
        return SongStatesOf(game_code_, path);
    }
    if (every_song_)
    {
        return {SongState{.suffix = "", .level = *every_song_}};
    }

    const auto song = song_states_.find(path);
    if (song == song_states_.end())
    {
        return {SongState{}};
    }

    return {song->second};
}

int LevelOfVolume(int volume)
{
    const int clamped = std::clamp(volume, 0, kFullVolume);

    return (clamped * (1 << kLevelShift) + kFullVolume - 1) / kFullVolume;
}

StartParameters StartOf(const SongState& state)
{
    StartParameters start;
    start.volume = static_cast<int16_t>(state.level * kFullVolume >> kLevelShift);

    return start;
}

} // namespace ntdlib::dse
