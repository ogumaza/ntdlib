// SPDX-License-Identifier: MIT

// Pikmin 2's pieces of music, as a ripper's game mode plays them through the game's sound layer (PSSystem, PSGame and
// PSM, on JAudio1). The table holds every file of the sequence archive but the effects sequence, se.bms, in the
// archive's order, then the variants. The Wii release, New Play Control! Pikmin 2, has the GameCube release's files
// in its sequence archive, and its code makes the pieces' objects as the GameCube release's code does. The addresses
// below are in the main.dol of each release (USA): the GameCube's first, then the Wii's.
//
// The game makes most pieces' objects in PikSceneMgr::initMainBgm, initBossBgm and initAdditionalBgm. A piece's scene
// loads one of the wave system's 49 groups (wScene_*.aw) besides group 0. A cave floor's conductor comes from a list
// for each area (BgmList_*.txt) or from the challenge mode's list (ChallengeBgmList.txt). The conductor picks the
// phrases that the cave music's tracks play. Its name gives the floor's piece and wave group
// (ConductorList::getSeqAndWaveFromConductor, Wii 0x803b3b3c). A piece plays with the first conductor of its family,
// such as caveconc_00_0.cnd and new_00_0.cnd. The game's code names caveconc_00_0.cnd itself, for a boss's floor. A
// boss starts its music when it appears, at a part of the music: the start point. The middle bosses start m_boss at
// their appearance (9). The final boss starts l_boss at its part for four weapons (1). A start point of 0 leaves the
// start to the scene. The music then starts at part 0, where JumpBgmSeq's constructor sets the part. The Waterwraith's
// chase starts m_boss there. At part 0, a track of l_boss jumps to itself without end.
//
// The variants. Each other conductor that the lists name gets a variant of its piece, with the wave group that its
// name gives. The bosses' actions jump to the other parts of their music (JumpBgmSeq::requestJumpBgmOnBeat). Each
// other part of m_boss and l_boss gets a variant that starts there and plays what follows. m_boss's parts 5 to 7 play
// in turn with part 3 for an attack. Nobody has found code in the game that asks for part 12 of l_boss. Its variant
// plays it, as the pieces that the game never plays do. l_boss has no variant at part 0. The attacks (parts 3 and 5 to
// 7 of both pieces, and m_boss's long attack, 8) write to the boss's object. A ripper's driver gives them a stand-in.
//
// The game makes l_boss only while the final boss's sound object exists (initBossBgm, 0x8033606c, Wii 0x803b5740).
// Its directors then take tracks 14 and 15 (0x80470240, Wii 0x804f5d24) and mute them. Tracks 10 and 11 play in full.
// tentionD turns track 15 on while a captain stands within 300 units of the boss (0x8046cc70, Wii 0x804f2130), and
// only parts 4 and 11 hold notes on that track. Those two parts therefore get one more variant each, labelled with
// "-near". pikatkD turns track 14 on as Pikmin attack. That track holds one note, in parts 1 and 12, and gets no
// variant.
//
// Neither the game's code nor its lists of music start 14 of the files: 2pbattle, a_iron, a_zawinul01, caveboss,
// c_result, the six ff_ pieces, id_test, result and u_result. The game plays streams of the same names in place of
// c_result, result, u_result and the ff_ pieces. The calls that each sequence makes to the game choose its object.
// 2pbattle calls as battle_t does. a_iron and caveboss call as the caves' pieces do. They play with iron.cnd and
// caveboss.cnd. The game has no track map for them, and they play without directors. a_zawinul01 has no conductor of
// its own and plays with iron.cnd. id_test calls as a directed piece does, and plays without directors. The others
// only play. The ripper finds the wave groups of these pieces.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace ntdlib::jaudio
{

// One of Pikmin 2's pieces of music: its file in the sequence archive, how the game makes its object, the wave scene
// that the game loads for it (a group of wave system 0, besides group 0), an AutoBgm's conductor, the object's sound
// information, and the beat interval that the game gives the root track (0 keeps the default). A boss's music then has
// a start point: the part of the music that the boss starts. A variant of a piece plays it in another state of the
// game: with another conductor, or from another start point. Its label names it.
struct Pikmin2Piece
{
    // How a ripper's driver makes a piece's object. Each is a way in which the game makes a piece's object.
    enum class Kind : uint32_t
    {
        kPlain = 0,       // a BgmSeq that only plays
        kMain = 1,        // an area's music: a JumpBgmSeq with the scene's directors
        kDirected = 2,    // a boss's music, or the music of 2-player battles
        kAuto = 3,        // a cave's music: an AutoBgm. Its conductor picks what the tracks play
        kBattle = 4,      // a DirectedBgm with the 2-player battle directors that the game gives battle_t
        kFree = 5,        // a DirectedBgm without directors
        kAutoFree = 6,    // an AutoBgm without directors or the arrangement of its melody
        kBigBoss = 7,     // the final boss's music, with the final boss's sound object in place
        kBigBossNear = 8, // the same, with the tension director on: a captain stands near the boss
    };

    // The JAInter::SoundInfo that the game gives a piece's object, in words: the flags, then the priority and the
    // sequence's slot in the top two bytes, the pitch (1.0) and the volume in the top byte. The game's list of volumes
    // (BgmList.txt) then sets the volume.
    using SoundInfo = std::array<uint32_t, 4>;

    // initMainBgm's sound information. A piece takes it unless its entry gives another.
    static constexpr SoundInfo kAreaInfo = {0x1f00, 127u << 24 | 1u << 16, 0x3f800000, 50u << 24};

    // The wave scene of a piece that the game never plays. The ripper finds the groups that hold its waves.
    static constexpr uint8_t kFindWaves = 0xff;

    // The piece's name: its label, or for a piece without one, its file without the extension.
    constexpr std::string_view Name() const
    {
        const std::string_view file = sequence;
        return *label ? std::string_view(label) : file.substr(0, file.rfind('.'));
    }

    const char* sequence;
    Kind kind;
    uint8_t wave_scene;
    const char* conductor = "";
    SoundInfo info = kAreaInfo;
    uint16_t beat_interval = 0;
    uint8_t start_point = 0;
    const char* label = "";
};

// The pieces. A ripper gives a piece the sound ID 0x80000000 and its place here.
inline constexpr auto kPikmin2Pieces = []
{
    using enum Pikmin2Piece::Kind;
    constexpr uint8_t kFindWaves = Pikmin2Piece::kFindWaves;
    constexpr Pikmin2Piece::SoundInfo kAreaInfo = Pikmin2Piece::kAreaInfo;

    // The sound information of the pieces that don't take initMainBgm's: initBossBgm's, initAdditionalBgm's and the
    // key's music's.
    constexpr Pikmin2Piece::SoundInfo kBossInfo = {0, 127u << 24 | 2u << 16, 0x3f800000, 40u << 24};
    constexpr Pikmin2Piece::SoundInfo kExtraInfo = {0, 127u << 24 | 1u << 16, 0x3f800000, 50u << 24};
    constexpr Pikmin2Piece::SoundInfo kKeyInfo = {0x1f00, 127u << 24 | 4u << 16, 0x3f800000, 35u << 24};

    return std::to_array<Pikmin2Piece>({
        {"2pbattle.bms", kBattle, kFindWaves},
        {"a_iron.bms", kAutoFree, kFindWaves, "iron.cnd"},
        {"a_zawinul01.bms", kAutoFree, kFindWaves, "iron.cnd"},
        {"battle_t.bms", kDirected, 6},
        {"book.bms", kPlain, 36},
        {"caveboss.bms", kAutoFree, kFindWaves, "caveboss.cnd"},
        {"caveconc.bms", kAuto, 26, "caveconc_00_0.cnd"},
        {"caveglass.bms", kAuto, 23, "caveglass_00_0.cnd"},
        {"cavekeyget.bms", kAuto, 5, "cavekeyget.cnd", kKeyInfo},
        {"cavemetal.bms", kAuto, 10, "cavemetal_00_0.cnd"},
        {"caverelax.bms", kAuto, 28, "caverelax.cnd"},
        {"cavesoil.bms", kAuto, 22, "cavesoil_00_0.cnd"},
        {"cavetile.bms", kAuto, 24, "cavetile_00_0.cnd"},
        {"cavetsumiki.bms", kAuto, 25, "cavetsumiki_00_0.cnd"},
        {"comp_result.bms", kPlain, 19},
        {"c_menu.bms", kPlain, 20},
        {"c_result.bms", kPlain, kFindWaves},
        {"ff_annihi.bms", kPlain, kFindWaves},
        {"ff_doped.bms", kPlain, kFindWaves},
        {"ff_doping.bms", kPlain, kFindWaves},
        {"ff_keyget.bms", kPlain, kFindWaves},
        {"ff_playerdown.bms", kPlain, kFindWaves},
        {"ff_treasureget.bms", kPlain, kFindWaves},
        {"forest.bms", kMain, 2},
        {"f_result.bms", kPlain, 19},
        {"hiscore.bms", kPlain, 21, "", kExtraInfo},
        {"id_test.bms", kFree, kFindWaves},
        {"kuro_post.bms", kPlain, 48, "", kExtraInfo},
        {"kuro_pre.bms", kPlain, 48},
        {"last.bms", kMain, 4, "", kAreaInfo, 30},
        {"l_boss.bms", kBigBoss, 35, "", kBossInfo, 60, 1},
        {"m_boss.bms", kDirected, 0, "", kBossInfo, 60, 9},
        {"new_00.bms", kAuto, 37, "new_00_0.cnd"},
        {"new_01.bms", kAuto, 38, "new_01_0.cnd"},
        {"new_02.bms", kAuto, 39, "new_02_0.cnd"},
        {"new_03.bms", kAuto, 40, "new_03_0.cnd"},
        {"new_04.bms", kAuto, 41, "new_04_0.cnd"},
        {"new_05.bms", kAuto, 42, "new_05_0.cnd"},
        {"new_06.bms", kAuto, 43, "new_06_0.cnd"},
        {"new_07.bms", kAuto, 44, "new_07_0.cnd"},
        {"new_08.bms", kAuto, 45, "new_08_0.cnd"},
        {"n_tutorial.bms", kMain, 15},
        {"n_tutorial_1stday.bms", kPlain, 15, "", kExtraInfo},
        {"result.bms", kPlain, kFindWaves},
        {"u_result.bms", kPlain, kFindWaves},
        {"worldmap.bms", kPlain, 16},
        {"worldmap_intro.bms", kPlain, 16},
        {"yakushima.bms", kMain, 3, "", kAreaInfo, 30},
        {"caveconc.bms", kAuto, 31, "caveconc_05_0.cnd", kAreaInfo, 0, 0, "caveconc_05_0"},
        {"caveconc.bms", kAuto, 32, "caveconc_10_0.cnd", kAreaInfo, 0, 0, "caveconc_10_0"},
        {"cavemetal.bms", kAuto, 29, "cavemetal_05_0.cnd", kAreaInfo, 0, 0, "cavemetal_05_0"},
        {"cavemetal.bms", kAuto, 29, "cavemetal_05_4.cnd", kAreaInfo, 0, 0, "cavemetal_05_4"},
        {"cavemetal.bms", kAuto, 30, "cavemetal_10_0.cnd", kAreaInfo, 0, 0, "cavemetal_10_0"},
        {"cavemetal.bms", kAuto, 30, "cavemetal_10_4.cnd", kAreaInfo, 0, 0, "cavemetal_10_4"},
        {"cavesoil.bms", kAuto, 33, "cavesoil_05_0.cnd", kAreaInfo, 0, 0, "cavesoil_05_0"},
        {"cavesoil.bms", kAuto, 34, "cavesoil_10_0.cnd", kAreaInfo, 0, 0, "cavesoil_10_0"},
        {"cavesoil.bms", kAuto, 34, "cavesoil_10_3.cnd", kAreaInfo, 0, 0, "cavesoil_10_3"},
        {"new_00.bms", kAuto, 37, "new_00_1.cnd", kAreaInfo, 0, 0, "new_00_1"},
        {"new_08.bms", kAuto, 45, "new_08_1.cnd", kAreaInfo, 0, 0, "new_08_1"},
        {"m_boss.bms", kDirected, 0, "", kBossInfo, 60, 0, "m_boss-0"},
        {"m_boss.bms", kDirected, 0, "", kBossInfo, 60, 1, "m_boss-1"},
        {"m_boss.bms", kDirected, 0, "", kBossInfo, 60, 2, "m_boss-2"},
        {"m_boss.bms", kDirected, 0, "", kBossInfo, 60, 3, "m_boss-3"},
        {"m_boss.bms", kDirected, 0, "", kBossInfo, 60, 4, "m_boss-4"},
        {"m_boss.bms", kDirected, 0, "", kBossInfo, 60, 5, "m_boss-5"},
        {"m_boss.bms", kDirected, 0, "", kBossInfo, 60, 6, "m_boss-6"},
        {"m_boss.bms", kDirected, 0, "", kBossInfo, 60, 7, "m_boss-7"},
        {"m_boss.bms", kDirected, 0, "", kBossInfo, 60, 8, "m_boss-8"},
        {"m_boss.bms", kDirected, 0, "", kBossInfo, 60, 10, "m_boss-10"},
        {"m_boss.bms", kDirected, 0, "", kBossInfo, 60, 11, "m_boss-11"},
        {"m_boss.bms", kDirected, 0, "", kBossInfo, 60, 12, "m_boss-12"},
        {"l_boss.bms", kBigBoss, 35, "", kBossInfo, 60, 2, "l_boss-2"},
        {"l_boss.bms", kBigBoss, 35, "", kBossInfo, 60, 3, "l_boss-3"},
        {"l_boss.bms", kBigBoss, 35, "", kBossInfo, 60, 4, "l_boss-4"},
        {"l_boss.bms", kBigBoss, 35, "", kBossInfo, 60, 5, "l_boss-5"},
        {"l_boss.bms", kBigBoss, 35, "", kBossInfo, 60, 6, "l_boss-6"},
        {"l_boss.bms", kBigBoss, 35, "", kBossInfo, 60, 7, "l_boss-7"},
        {"l_boss.bms", kBigBoss, 35, "", kBossInfo, 60, 8, "l_boss-8"},
        {"l_boss.bms", kBigBoss, 35, "", kBossInfo, 60, 9, "l_boss-9"},
        {"l_boss.bms", kBigBoss, 35, "", kBossInfo, 60, 10, "l_boss-10"},
        {"l_boss.bms", kBigBoss, 35, "", kBossInfo, 60, 11, "l_boss-11"},
        {"l_boss.bms", kBigBoss, 35, "", kBossInfo, 60, 12, "l_boss-12"},
        {"l_boss.bms", kBigBoss, 35, "", kBossInfo, 60, 13, "l_boss-13"},
        {"l_boss.bms", kBigBossNear, 35, "", kBossInfo, 60, 4, "l_boss-4-near"},
        {"l_boss.bms", kBigBossNear, 35, "", kBossInfo, 60, 11, "l_boss-11-near"},
    });
}();

// The most pieces that a ripper's driver holds in its table.
inline constexpr std::size_t kMaxPikmin2Pieces = 96;
static_assert(kPikmin2Pieces.size() <= kMaxPikmin2Pieces);

// The piece that sound `sound_id` plays, or null for a sound ID that isn't a piece's.
const Pikmin2Piece* Pikmin2PieceOf(uint32_t sound_id);

// Appends the driver's table of the pieces to `words`: the number of pieces, then for each piece a word with its kind,
// its start point from bit 8 and its beat interval from bit 16, its sound information, and its file's name and its
// conductor's name in 24 bytes each, with a 0 after each name.
void AppendPikmin2Pieces(std::vector<uint32_t>& words);

} // namespace ntdlib::jaudio
