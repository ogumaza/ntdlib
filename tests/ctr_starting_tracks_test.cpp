// SPDX-License-Identifier: MIT

// Tests of how known 3DS games start a sequence (ntdlib/ctr/snd/starting_tracks.h): the tracks that they quiet, the
// variables that they set, the random generator's state, the volume and the banks. Each game's code, or its sound
// archive, gives the values, at the addresses that src/ctr/snd/starting_tracks.cpp cites. The stream sounds' masks are
// tested in ctr_snd_test.cpp.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "xsflib/testing/check.h"

#include "ntdlib/ctr/snd/starting_tracks.h"

namespace
{

using ntdlib::ctr::snd::BankArrangements;
using ntdlib::ctr::snd::kNoBank;
using ntdlib::ctr::snd::SequenceArrangement;
using ntdlib::ctr::snd::SequenceArrangements;
using ntdlib::ctr::snd::SequenceStart;
using ntdlib::ctr::snd::SequenceVariable;
using ntdlib::ctr::snd::StartingSequenceTracks;
using xsflib::testing::Check;

// The SHA-1s of the sound archives that the table lists.
constexpr std::string_view kOcarinaOfTime3D = "c3fdb98e65e6143d76e6d971cfaaac058455300d";
constexpr std::string_view kIslandTour = "e9d5d40aa7b034360905ecb171a5825b37688c6c";
constexpr std::string_view kWarioWareGold = "6b471abb246fcaa2cf13ffbf16345e5d0c02e7e1";
constexpr std::string_view kNewLeaf = "54f0fbc801c85ea2035219923aa4cd955e6f4cc7";
constexpr std::string_view kTomodachiLife = "200b87d7d1152f5035b29a092f2e20e726695264";
constexpr std::string_view kUltimateAngler = "a98dd3c22da8e4bed8327e4fc98196e102ce53ea";
constexpr std::string_view kFlowerTown = "331b572b09819830436fe8a09673b22fc6f7bdae";
constexpr std::string_view kMiiPlaza = "33b514ee80f6a6df3034bc8774235e40dfa52d1c";
constexpr std::string_view kMiiPlazaSystemTitle = "c2550ce04d4c8c44e92c2d72682038616fbec658";
constexpr std::string_view kNinjaLauncher = "ddb93158672258c7d8ea2b5edb8662bda34234e6";
constexpr std::string_view kMiiForce = "e566c28135170a936f1f6e4538a15d745f3b2a9e";
constexpr std::string_view kSystemSettings = "ff5231c919ad81331321e6e29e24d84bbf2127cc";
constexpr std::string_view kSystemSettings2 = "fd187b38953c162695779f26c66140859a63e74b";
constexpr std::string_view kActivityLog = "ea55099f2b92479898b4eda7956d645021dac2d3";
constexpr std::string_view kMiiMaker = "0cc99a6732a7dbc0d6e8887449233ab25396672c";
constexpr std::string_view kMarioPartyStarRush = "90fa0978d0f4e3af259a48b48141d3e58ab5e66e";
constexpr std::string_view kSuperMario3DLand = "bfb2742fc5e8f2869c9ce781710ba393f675fbc1";
constexpr std::string_view kMarioKart7 = "cfd8fb5750069922dc631708d883fcc7374f8fa5";
constexpr std::string_view kHappyHomeDesigner = "8c07814daf28befb015cc97dd2c66699254035be";
constexpr std::string_view kHappyHomeDesignerOld = "30c98d66881296eda5e676964918e7003b403c75";

void CheckStart(std::string_view archive, std::string_view sound, SequenceStart expected)
{
    const SequenceStart start = StartingSequenceTracks(archive, sound);
    Check(start == expected, std::string(sound) + " starts with the game's masks");
}

// Near Malon, NA_BGM_RONRON's track 13 plays at a volume of 0. WarioWare Gold silences tracks 0 to 11 of the three
// Tricky jingles at flag 0. Flower Town's atelier fades out every track but 0 to 5. Each of these tracks takes its
// voices.
void TestSilencedTracks()
{
    CheckStart(kOcarinaOfTime3D, "NA_BGM_RONRON", {.silenced = 0x2000});
    for (const std::string_view jingle :
         {"SEQ_M_TSUNAGI_TRICKY_NEXT1", "SEQ_M_TSUNAGI_TRICKY_NG1", "SEQ_M_TSUNAGI_TRICKY_OK1"})
    {
        CheckStart(kWarioWareGold, jingle, {.silenced = 0x0fff});
    }
    CheckStart(kFlowerTown, "BGM_FLOWER_ATELIER", {.silenced = 0xffc0});
}

// Island Tour's main menu plays tracks 0 to 3 of SEQ_BGM_MENU. New Leaf's museum entrance plays tracks 0 to 3 and 10 to
// 12. Tomodachi Life's apartment plays tracks 0 to 2 of BGM_MANSION_AND_ROOM, and tracks 4 to 7 of BGM_AKICHI_NIGHT.
void TestMutedTracks()
{
    CheckStart(kIslandTour, "SEQ_BGM_MENU", {.muted = 0xfff0});
    CheckStart(kNewLeaf, "SEQ_BGM_IND_MUSEUM", {.muted = 0xe3f0});
    CheckStart(kTomodachiLife, "BGM_MANSION_AND_ROOM", {.muted = 0xfff8});
    CheckStart(kTomodachiLife, "BGM_AKICHI_NIGHT", {.muted = 0xff0f});
}

// The system titles mute the tracks that a piece's user parameter leaves out. System Settings' three pieces of one
// sequence keep tracks 0 to 6, 7 to 15 and 3, in both of its archives. The Activity Log's three keep tracks 0 to 9,
// 0 to 11, and 0 to 9 with 12 and 13. A piece whose parameter is 0 keeps every track. Mii Maker's first screen sets
// tracks 8 to 15 of its music to a volume of 0.
void TestSystemTitles()
{
    for (const std::string_view archive : {kSystemSettings, kSystemSettings2})
    {
        CheckStart(archive, "BGM_CTR_MSET", {.muted = 0xff80});
        CheckStart(archive, "BGM_CTR_MSET_NET", {.muted = 0x007f});
        CheckStart(archive, "BGM_CTR_MSET_FIRST", {.muted = 0xfff7});
        CheckStart(archive, "BGM_CTR_MSET_TWL", {});
    }
    CheckStart(kActivityLog, "BGM_CTR_PLOG_MAIN", {.muted = 0xfc00});
    CheckStart(kActivityLog, "BGM_CTR_PLOG_MAIN_CLOCK", {.muted = 0xf000});
    CheckStart(kActivityLog, "BGM_CTR_PLOG_MAIN_STEP", {.muted = 0xcc00});
    CheckStart(kActivityLog, "BGM_CTR_PLOG_BOOK", {});
    CheckStart(kMiiPlaza, "BGM_CTR_MSET", {});
    CheckStart(kMiiMaker, "MEDIT_BGM_FACE", {.silenced = 0xff00});

    // Mii Maker's later mixes: the sound at 0.7, tracks 3, 4 and 7 out, tracks 8 and 9 at 0.8, and in the second, track
    // 10 at 0.75. A start's tracks default to a volume of 1.
    const auto mixes = SequenceArrangements(kMiiMaker, "MEDIT_BGM_FACE");
    std::array<float, ntdlib::ctr::snd::kSequenceTracks> volumes = ntdlib::ctr::snd::kFullTrackVolumes;
    volumes[8] = 0.8f;
    volumes[9] = 0.8f;
    Check(mixes.size() == 2 && mixes[0].suffix == "_tracks_8_9" &&
              mixes[0].start == SequenceStart{.silenced = 0xfc98, .track_volumes = volumes, .volume = 0.7f},
          "Mii Maker's first later mix adds tracks 8 and 9 at 0.8");
    volumes[10] = 0.75f;
    Check(mixes.size() == 2 && mixes[1].suffix == "_tracks_8_to_10" &&
              mixes[1].start == SequenceStart{.silenced = 0xf898, .track_volumes = volumes, .volume = 0.7f},
          "Mii Maker's second later mix adds track 10 at 0.75");
    Check(SequenceStart{}.track_volumes == ntdlib::ctr::snd::kFullTrackVolumes, "a start's tracks play at 1");
}

// Ultimate Angler mutes track 15 of every piece on its main BGM player, by the start of the piece's name.
// SEQ_BGM_SPOT_00_A plays on a second player with every track. SEQ_BGM_CROSS also starts with tracks 10 to 13 at a
// volume of 0. The archive's other sounds play every track.
void TestNamePrefix()
{
    CheckStart(kUltimateAngler, "SEQ_BGM_TITLE", {.muted = 0x8000});
    CheckStart(kUltimateAngler, "SEQ_BGM_CROSS", {.muted = 0x8000, .silenced = 0x3c00});
    CheckStart(kUltimateAngler, "SEQ_BGM_SPOT_00_A", {});
    CheckStart(kUltimateAngler, "SE_ENV_TITLE", {});
    CheckStart(kUltimateAngler, "SEQ_BGM", {});
}

// Ultimate Angler's reeling pieces start with their layer for tension level 2 at a volume of 0: track 5, 8, 6 or 12.
// The ending starts with tracks 2 to 13 muted, and the home ambience with tracks 2 to 15 muted.
void TestStartingStates()
{
    CheckStart(kUltimateAngler, "SEQ_BGM_REELING_NORMAL", {.muted = 0x8000, .silenced = 0x0020});
    CheckStart(kUltimateAngler, "SEQ_BGM_REELING_BIG", {.muted = 0x8000, .silenced = 0x0100});
    CheckStart(kUltimateAngler, "SEQ_BGM_REELING_KAIGYO", {.muted = 0x8000, .silenced = 0x0040});
    CheckStart(kUltimateAngler, "SEQ_BGM_REELING_LASTBOSS", {.muted = 0x8000, .silenced = 0x1000});
    CheckStart(kUltimateAngler, "SEQ_BGM_ED2_DEMO", {.muted = 0xbffc});
    CheckStart(kUltimateAngler, "SE_ENV_HOME_CROSS", {.muted = 0xfffc});
    CheckStart(kMiiPlaza, "SEQ_USAGI", {.muted = 0xf102});
}

// The Plaza's From_New_Mii pieces start once global variable 16 changes, at frame 1, in both of its archives. The
// system title's archive has no SEQ_USAGI entry. Ultimate Angler's jump-ins start at the count 0, and Flower Town's
// second light at the value that SE_COLOR_WHEEL_LIGHT sets. Angler's fever
// starts at the pull count 0, and its crowd at the fullest crowd. Two of Angler's effects get a state of the random
// generator that plays their sound.
void TestVariablesAndSeeds()
{
    constexpr SequenceVariable kBeat = {.index = 16, .value = 1, .frame = 1};
    CheckStart(kMiiPlaza, "BGM_MAIN_Mii_MIN_From_New_Mii", {.variables = {{kBeat}}});
    CheckStart(kMiiPlaza, "BGM_MAIN_Mii_No7_From_New_Mii", {.variables = {{kBeat}}});
    CheckStart(kMiiPlaza, "BGM_MAIN_Mii_MIN", {});
    CheckStart(kMiiPlazaSystemTitle, "BGM_MAIN_Mii_Only_One_From_New_Mii", {.variables = {{kBeat}}});
    CheckStart(kMiiPlazaSystemTitle, "BGM_MAIN_Mii_No7_From_New_Mii", {.variables = {{kBeat}}});
    CheckStart(kMiiPlazaSystemTitle, "BGM_MAIN_Mii_MIN", {});
    CheckStart(kMiiPlazaSystemTitle, "SEQ_USAGI", {});
    CheckStart(kUltimateAngler, "SE_ANIM_CRUISER_JUMP_IN", {.variables = {{{.index = 31, .value = 0}}}});
    CheckStart(kFlowerTown, "SE_COLOR_WHEEL_LIGHT_2", {.variables = {{{.index = 19, .value = 0}}}});
    CheckStart(kUltimateAngler, "SE_FEVER_PULL_2", {.variables = {{{.index = 0, .value = 0}}}});
    CheckStart(kUltimateAngler, "SE_FEVER_PULL", {});
    CheckStart(kUltimateAngler, "SE_MIIS_VOICE_START_FEVER", {.variables = {{{.index = 0, .value = 0x1f}}}});
    CheckStart(kUltimateAngler, "SE_CATCHED_092", {.seed = 0x8d9333a4});
    CheckStart(kUltimateAngler, "SE_CATCHED_120", {.seed = 0x995e6351});

    const SequenceStart unlisted = StartingSequenceTracks(kUltimateAngler, "SE_CATCHED_142");
    Check(!unlisted.seed && unlisted.variables[0].index == SequenceVariable::kNone,
          "an unlisted effect keeps the player's generator and sets no variable");
}

// Super Mario 3D Land's note melodies play with track variable 2 (34) at 1 on every track. The melody's object sets
// that value when the player collects a note in time. Each melody then plays every beat in time. The BGM player gives
// each melody and each gift fanfare the volume of its entry in the game's BGM table. The note sounds that aren't
// melodies keep their variables and their volume.
void TestNoteMelodies()
{
    constexpr SequenceVariable kInTime = {.index = 34, .value = 1};
    for (const std::string_view melody :
         {"SS_NOTE_OBJ_CHIKA", "SS_NOTE_OBJ_MWORLD", "SS_NOTE_OBJ_M3INTRO", "SS_NOTE_OBJ_MWORLD2", "SsNoteObjTest"})
    {
        CheckStart(kSuperMario3DLand, melody, {.variables = {{kInTime}}, .volume = 0.55f});
    }
    CheckStart(kSuperMario3DLand, "SS_NOTE_OBJ_SEA", {.variables = {{kInTime}}, .volume = 0.6f});
    CheckStart(kSuperMario3DLand, "SsNoteObjTestLong", {.variables = {{kInTime}}, .volume = 0.7f});
    CheckStart(kSuperMario3DLand, "SS_KINO_GIFT_SILVER", {.volume = 0.5f});
    CheckStart(kSuperMario3DLand, "SS_KINO_GIFT_GOLD", {.volume = 0.4f});
    CheckStart(kSuperMario3DLand, "SeSyNoteObjGet", {});
    CheckStart(kSuperMario3DLand, "SeSyNoteObjComplete", {});
}

// Super Mario 3D Land starts Mario's voice sequences with Luigi's banks for Luigi. A sequence whose slot 0 holds
// BANK_PLAYER_VOICE_MR gets a state with BANK_PLAYER_VOICE_LU, and one with BANK_PLAYER_VOICE_DEMO gets one with
// BANK_PLAYER_VOICE_DEMO_LU. The game's StartInfo leaves the other slots empty. The banks of the last cutscene's lines
// and other banks have no such state, and neither has another archive.
void TestBankArrangements()
{
    const auto voice = BankArrangements(kSuperMario3DLand, 0x03000000);
    Check(voice.size() == 1 && voice[0].suffix == "_Luigi" &&
              voice[0].start == SequenceStart{.banks = {{0x03000001, kNoBank, kNoBank, kNoBank}}},
          "Mario's voice plays with Luigi's bank in Luigi's state");

    const auto cutscene = BankArrangements(kSuperMario3DLand, 0x0300000C);
    Check(cutscene.size() == 1 && cutscene[0].suffix == "_Luigi" &&
              cutscene[0].start == SequenceStart{.banks = {{0x0300000D, kNoBank, kNoBank, kNoBank}}},
          "Mario's cutscene voice plays with Luigi's cutscene bank in Luigi's state");

    Check(BankArrangements(kSuperMario3DLand, 0x03000017).empty(), "the last cutscene's lines have Luigi's sequences");
    Check(BankArrangements(kSuperMario3DLand, 0x03000016).empty(), "Bowser's cutscene bank stays the same");
    Check(BankArrangements(kSuperMario3DLand, 0x03000002).empty(), "another bank has no Luigi state");
    Check(BankArrangements(kNewLeaf, 0x03000000).empty(), "another archive's bank 0 has no Luigi state");
    Check(SequenceArrangements(kSuperMario3DLand, "SePvJumpS").empty(), "a voice has no state by its name");
}

// Super Mario 3D Land's effects that transpose by a player variable start at the value of their first play. The last
// red coin of a set plays at 4, and the world intro's counter takes variable 10. The roulette's effect keeps -1. Its
// sequence never reads the variable. The beat blocks' effects and the warp lift's step keep -1. The game starts them
// without a variable.
void TestEffectsFirstPlay()
{
    constexpr SequenceStart kFirst = {.variables = {{{.index = 0, .value = 0}}}};
    for (const std::string_view effect :
         {"SeSyRedCoin", "SeSyGetCollectCoin", "SeSyBeatEnemySequentially", "SeBmBunbunFireDamage",
          "SeBmPunpunFireDamage", "SeSyNoteBlockJumpNormalNote", "SeSyNoteBlockJumpHighNote"})
    {
        CheckStart(kSuperMario3DLand, effect, kFirst);
    }
    CheckStart(kSuperMario3DLand, "SeSyRedCoinStarterComplete", {.variables = {{{.index = 0, .value = 4}}}});
    CheckStart(kSuperMario3DLand, "SeSyDmWldInt05MotionCounter", {.variables = {{{.index = 10, .value = 0}}}});
    constexpr std::array<std::string_view, 4> kUnset = {"SeOjRouletteShuffle", "SeSyBeatBlockTrgSub",
                                                        "SeSyBeatBlockTrgSubBack", "SeOjTrickTeresaWarpStepMove"};
    for (const std::string_view effect : kUnset)
    {
        CheckStart(kSuperMario3DLand, effect, {});
    }
}

// Each reeling piece has one later state, with its layer at full volume. The ending has four steps, and the home
// ambience six more groups of tracks. Ultimate Angler's fever has 20 more pull counts, and its crowd 26 more states.
// Ninja Launcher's battle music speeds up at the end of a battle, and the Plaza's
// music plays faster and higher in its quick mode. Happy Home Designer's music has a state for each of two rooms.
// Other sounds and archives have none.
void TestArrangements()
{
    const auto reeling = SequenceArrangements(kUltimateAngler, "SEQ_BGM_REELING_KAIGYO");
    Check(reeling.size() == 1 && reeling[0].suffix == "_tension_2", "a reeling piece has its state at tension 2");
    Check(reeling.size() == 1 && reeling[0].start == SequenceStart{.muted = 0x8000},
          "a reeling piece plays its layer at tension 2");

    const auto ending = SequenceArrangements(kUltimateAngler, "SEQ_BGM_ED2_DEMO");
    Check(ending.size() == 4 && ending[0].suffix == "_step_1" && ending[3].suffix == "_step_4",
          "the ending has four steps");
    Check(ending.size() == 4 && ending[1].start == SequenceStart{.muted = 0x8000, .silenced = 0x7f90},
          "the ending's second step plays tracks 0 to 3, 5 and 6");

    const auto ambience = SequenceArrangements(kUltimateAngler, "SE_ENV_HOME_CROSS");
    Check(ambience.size() == 6 && ambience[0].start == SequenceStart{.muted = 0xffe3},
          "the home ambience's first other group plays tracks 2 to 4");

    const auto usagi = SequenceArrangements(kMiiPlaza, "SEQ_USAGI");
    Check(usagi.size() == 4 && usagi[3].suffix == "_happy" && usagi[3].start == SequenceStart{.muted = 0x0100},
          "SEQ_USAGI's last state plays every track but 8");

    const auto jump_ins = SequenceArrangements(kUltimateAngler, "SE_ANIM_CRUISER_JUMP_IN_MY_MII");
    Check(jump_ins.size() == 9 && jump_ins[0].suffix == "_jump_2" && jump_ins[8].suffix == "_jump_10",
          "the jump-ins have nine later counts");
    Check(jump_ins.size() == 9 && jump_ins[8].start == SequenceStart{.variables = {{{.index = 31, .value = 9}}}},
          "the tenth jump-in plays at the count 9");

    const auto pulls = SequenceArrangements(kUltimateAngler, "SE_FEVER_PULL_2");
    Check(pulls.size() == 20 && pulls[0].suffix == "_pull_2" && pulls[19].suffix == "_pull_21" &&
              pulls[19].start == SequenceStart{.variables = {{{.index = 0, .value = 20}}}},
          "the fever's pulls have 20 later counts, up to 20");

    const auto crowds = SequenceArrangements(kUltimateAngler, "SE_MIIS_VOICE_HARD");
    Check(crowds.size() == 26, "the crowd has 26 other states");
    bool kinds_named = true;
    for (const SequenceArrangement& crowd : crowds)
    {
        const int16_t state = crowd.start.variables[0].value;
        std::string name = "_crowd_a" + std::to_string(state & 3);
        if ((state & 0x0c) != 0)
        {
            name += ((state & 0x10) != 0 ? "_c" : "_b") + std::to_string((state >> 2) & 3);
        }
        kinds_named = kinds_named && crowd.suffix == name && crowd.start.variables[0].index == 0 && state != 0 &&
                      state != 0x1f && (state & 0x1c) != 0x10;
    }
    Check(kinds_named, "each other crowd is named by its levels, and none is empty, the fullest or of kind C at 0");

    const auto light = SequenceArrangements(kFlowerTown, "SE_COLOR_WHEEL_LIGHT_2");
    Check(light.size() == 1 && light[0].start == SequenceStart{.variables = {{{.index = 19, .value = 1}}}},
          "the second light plays at the value 1");

    const auto hurry = SequenceArrangements(kNinjaLauncher, "SEQ_BGM_BATTLE_BOSS");
    Check(hurry.size() == 1 && hurry[0].suffix == "_hurry" && hurry[0].start == SequenceStart{.tempo_ratio = 1.3f},
          "a battle piece plays at a tempo ratio of 1.3 in its last 10 seconds");

    const auto endings = SequenceArrangements(kMiiForce, "SD_BGM_D01");
    Check(endings.size() == 2 && endings[0].suffix == "_ending" &&
              endings[0].start == SequenceStart{.variables = {{{.index = 0, .value = 1, .frame = 4400}}}} &&
              endings[1].suffix == "_ending_early" &&
              endings[1].start == SequenceStart{.variables = {{{.index = 0, .value = 1, .frame = 2200}}}},
          "Mii Force's game-over piece ends when player variable 0 changes to 1");
    Check(StartingSequenceTracks(kMiiForce, "SD_BGM_D01") == SequenceStart{},
          "the game's other scenes play Mii Force's piece as a loop");

    const auto quick = SequenceArrangements(kMiiPlaza, "BGM_MAIN_Mii_MAX");
    Check(quick.size() == 1 && quick[0].suffix == "_quick" &&
              quick[0].start == SequenceStart{.tempo_ratio = 1.1625f, .pitch = 1.25f},
          "the Plaza's music plays at a pitch of 1.25 and a tempo ratio of 1.1625 in quick mode");
    constexpr SequenceVariable kBeat = {.index = 16, .value = 1, .frame = 1};
    const auto quick_after_jingle = SequenceArrangements(kMiiPlaza, "BGM_MAIN_Mii_No6_From_New_Mii");
    Check(quick_after_jingle.size() == 1 &&
              quick_after_jingle[0].start ==
                  SequenceStart{.variables = {{kBeat}}, .tempo_ratio = 1.1625f, .pitch = 1.25f},
          "a piece after the jingle keeps its change of global 16 in quick mode");
    Check(SequenceArrangements(kMiiPlaza, "BGM_NEW_Mii").empty(), "the short jingle has no quick state");

    // Happy Home Designer's music players send 0.38 to the room's reverb or delay on bus A.
    constexpr SequenceStart kRinging = {.fx_send = {0.38f, 0.0f}, .game_effect = 0};
    constexpr SequenceStart kEchoes = {.fx_send = {0.38f, 0.0f}, .game_effect = 1};
    for (const std::string_view archive : {kHappyHomeDesigner, kHappyHomeDesignerOld})
    {
        for (const std::string_view piece :
             {"SEQ_BGM_MU01_SANBIKA", "SEQ_BGM_MUSS91_BIRTHDAYSONG_SCORE", "SEQ_BGM_RM90_DISCO"})
        {
            const auto room = SequenceArrangements(archive, piece);
            Check(room.size() == 2 && room[0].suffix == "_ringing" && room[0].start == kRinging &&
                      room[1].suffix == "_echoes" && room[1].start == kEchoes,
                  "a room's music has a state with the reverb and one with the delay");
        }
        Check(SequenceArrangements(archive, "SE_MU_CTRL_GRID_NORMAL").empty(),
              "the session's control sequences have no room states");
    }
    Check(SequenceArrangements(kNewLeaf, "SEQ_BGM_MU01_SANBIKA").empty(), "New Leaf's music has no room states");

    const auto menu = SequenceArrangements(kIslandTour, "SEQ_BGM_MENU");
    Check(menu.size() == 3 && menu[0].suffix == "_game_select" && menu[0].start == SequenceStart{.muted = 0xff80} &&
              menu[1].suffix == "_character_select" && menu[1].start == SequenceStart{.muted = 0xfc00} &&
              menu[2].suffix == "_minigame_select" && menu[2].start == SequenceStart{},
          "Island Tour's menu screens play tracks 0 to 6, 0 to 9 and all 16 of the menu's music");

    Check(SequenceArrangements(kUltimateAngler, "SEQ_BGM_TITLE").empty(), "an unlisted piece has no later state");
    Check(SequenceArrangements(kFlowerTown, "SEQ_BGM_REELING_NORMAL").empty(),
          "a listed name in another archive has no later state");
}

// Mario Party: Star Rush starts SQ_BGM_MGN708 with variable 9 at 0. The other sounds of minigame 708 set their
// variables as they start. SQ_BGM_MGN708 plays the A set's patterns in the order of an Azahar run, with variable 9 at 0
// and variable 10 at the pattern from the start. The B set's other patterns follow SQ_BGM_MGN708_B_FIRST, with
// variable 10 at the pattern from frame 1.
void TestMinigame708()
{
    CheckStart(kMarioPartyStarRush, "SQ_BGM_MGN708", {.variables = {{{.index = 9, .value = 0}}}});
    CheckStart(kMarioPartyStarRush, "SQ_BGM_MGN708_A_FIRST", {});
    CheckStart(kMarioPartyStarRush, "SQ_BGM_MGN708_B_INTERVAL_START", {});

    const auto a = SequenceArrangements(kMarioPartyStarRush, "SQ_BGM_MGN708");
    Check(a.size() == 4 && a[0].suffix == "_interval_loop" && a[1].suffix == "_otehon" &&
              a[2].suffix == "_rhythm_loop" && a[3].suffix == "_kaitou",
          "the A set's four patterns follow the bare sequence in the run's order");
    Check(a.size() == 4 &&
              a[0].start == SequenceStart{.variables = {{{.index = 9, .value = 0}, {.index = 10, .value = 3}}}},
          "the A set's interval pattern starts with variable 9 at 0 and variable 10 at 3");
    const auto b = SequenceArrangements(kMarioPartyStarRush, "SQ_BGM_MGN708_B_FIRST");
    Check(b.size() == 3 && b[0].suffix == "_otehon" &&
              b[0].start == SequenceStart{.variables = {{{.index = 10, .value = 0, .frame = 1}}}},
          "the B set's model pattern sets variable 10 to 0 at frame 1");
    Check(SequenceArrangements(kMarioPartyStarRush, "SQ_BGM_MGN708_A_FIRST").empty() &&
              SequenceArrangements(kMarioPartyStarRush, "SQ_BGM_MGN708_A_INTERVAL_START").empty(),
          "the A set's first sound and the interval starts have no other pattern");
}

// Mario Party: Island Tour's seven tempo pieces start at the variable 0 of an Azahar run. MG3801 and MNG_012 play the
// run's rows of the game's track table. Each piece also has its later states.
void TestIslandTourTempoPieces()
{
    CheckStart(kIslandTour, "SEQ_BGM_MNG_MG3801", {.muted = 0x98b4, .variables = {{{.index = 0, .value = 100}}}});
    CheckStart(kIslandTour, "SEQ_BGM_MNG_MG3802", {.variables = {{{.index = 0, .value = 99}}}});
    CheckStart(kIslandTour, "SEQ_BGM_MNG_012", {.muted = 0xbff0, .variables = {{{.index = 0, .value = 96}}}});
    for (const std::string_view boss :
         {"SEQ_BGM_BOSS_04_A", "SEQ_BGM_BOSS_04_B", "SEQ_BGM_BOSS_04_B2", "SEQ_BGM_BOSS_04_B3"})
    {
        CheckStart(kIslandTour, boss, {.variables = {{{.index = 0, .value = 20}}}});
        const auto hurry = SequenceArrangements(kIslandTour, boss);
        Check(hurry.size() == 1 && hurry[0].suffix == "_hurry" &&
                  hurry[0].start == SequenceStart{.variables = {{{.index = 0, .value = 10}}}},
              "a boss piece's later state has 10 seconds left");
    }

    const auto mg3801 = SequenceArrangements(kIslandTour, "SEQ_BGM_MNG_MG3801");
    Check(mg3801.size() == 1 && mg3801[0].suffix == "_hurry" &&
              mg3801[0].start == SequenceStart{.muted = 0x654a, .variables = {{{.index = 0, .value = 10}}}},
          "MG3801's later state plays row 11 of the track table with 10 seconds left");
    const auto mng012 = SequenceArrangements(kIslandTour, "SEQ_BGM_MNG_012");
    Check(mng012.size() == 2 && mng012[0].suffix == "_second" && mng012[1].suffix == "_third" &&
              mng012[0].start == SequenceStart{.muted = 0xbe0f, .variables = {{{.index = 0, .value = 96}}}} &&
              mng012[1].start == SequenceStart{.muted = 0x01ff, .variables = {{{.index = 0, .value = 96}}}},
          "MNG_012's later states play rows 8 and 9 of the track table at the ball's full speed");
    CheckStart(kIslandTour, "SEQ_BGM_MNG_MG3801_NORMAL", {});
}

// Mario Kart 7's beat sequences play at their stream's BPM over 120. Music Park's starts with tracks 0 to 3 muted. Its
// other states unmute one of them, or come from the final lap, with track variable 0 at 1 and the faster stream. Killer
// Ship's and Waluigi Pinball's have a final lap too. SEQ_SYNC_BEAT_120 starts at Toad Circuit's BPM, and its states
// follow each other course's normal and final-lap streams.
void TestMarioKart7Beats()
{
    constexpr float kMusicPark = 139.0f / 120.0f;
    constexpr float kMusicParkFinalLap = 166.0f / 120.0f;
    CheckStart(kMarioKart7, "SEQ_SYNC_BEAT_MUSIC_PARK", {.muted = 0x000f, .tempo_ratio = kMusicPark});
    CheckStart(kMarioKart7, "SEQ_SYNC_BEAT_KILLER_SHIP", {.tempo_ratio = 148.0f / 120.0f});
    CheckStart(kMarioKart7, "SEQ_SYNC_BEAT_WALUIGI_PINBALL", {.tempo_ratio = 135.0f / 120.0f});
    CheckStart(kMarioKart7, "SEQ_SYNC_BEAT_120", {.tempo_ratio = 140.0f / 120.0f});

    const auto park = SequenceArrangements(kMarioKart7, "SEQ_SYNC_BEAT_MUSIC_PARK");
    Check(park.size() == 8 && park[0].suffix == "_track_0" && park[3].suffix == "_track_3" &&
              park[7].suffix == "_final_lap_track_2",
          "Music Park's beat has four tracks of its own and the final lap's four states");
    Check(park.size() == 8 && park[1].start == SequenceStart{.muted = 0x000d, .tempo_ratio = kMusicPark},
          "Music Park's second road plays track 1");
    constexpr SequenceVariable kTranspose = {.index = 32, .value = 1};
    Check(park.size() == 8 &&
              park[4].start ==
                  SequenceStart{.muted = 0x000f, .variables = {{kTranspose}}, .tempo_ratio = kMusicParkFinalLap},
          "Music Park's final lap starts with every part muted, track variable 0 at 1 and the faster tempo");
    Check(park.size() == 8 &&
              park[5].start ==
                  SequenceStart{.muted = 0x000e, .variables = {{kTranspose}}, .tempo_ratio = kMusicParkFinalLap},
          "Music Park's first road on the final lap plays track 0 a semitone higher");

    const auto ship = SequenceArrangements(kMarioKart7, "SEQ_SYNC_BEAT_KILLER_SHIP");
    Check(ship.size() == 1 && ship[0].suffix == "_final_lap" &&
              ship[0].start == SequenceStart{.tempo_ratio = 178.0f / 120.0f},
          "Killer Ship's beat follows the final lap's stream");
    const auto pinball = SequenceArrangements(kMarioKart7, "SEQ_SYNC_BEAT_WALUIGI_PINBALL");
    Check(pinball.size() == 1 && pinball[0].start == SequenceStart{.tempo_ratio = 166.0f / 120.0f},
          "Waluigi Pinball's beat follows the final lap's stream");
    const auto beat = SequenceArrangements(kMarioKart7, "SEQ_SYNC_BEAT_120");
    Check(beat.size() == 57 && beat[0].suffix == "_toad_circuit_final_lap" &&
              beat[0].start == SequenceStart{.tempo_ratio = 168.0f / 120.0f},
          "the first course's beat follows its final lap's stream");
    Check(beat.size() == 57 && beat[5].suffix == "_sand_town" &&
              beat[5].start == SequenceStart{.tempo_ratio = 112.0f / 120.0f},
          "Sand Town's beat plays at 112 BPM, the slowest course's");
    Check(beat.size() == 57 && beat[12].suffix == "_rally_cource_final_lap" &&
              beat[12].start == SequenceStart{.tempo_ratio = 207.0f / 120.0f},
          "the fastest final lap's beat plays at 207 BPM");
    Check(beat.size() == 57 && beat[56].suffix == "_sfc_rainbow_road_final_lap" &&
              beat[56].start == SequenceStart{.tempo_ratio = 174.0f / 120.0f},
          "the last retro course's beat follows its final lap's stream");
}

// The player's kart starts Mario Kart 7's falls and air current with variable 0 at 1. The glider's flutter always
// takes 1. The rise in rank starts at 0, the rise to 7th. The horns start at body 0. The fall into wind, the drop in
// rank, the coins and the button sound keep -1.
void TestMarioKart7Effects()
{
    constexpr SequenceStart kOne = {.variables = {{{.index = 0, .value = 1}}}};
    for (const std::string_view effect :
         {"SE_KT_FALL_WATER", "SE_KT_FALL_LAVA", "SE_KT_GLIDER_AIR_CURRENT", "SE_KT_GLIDER_FLUTTER"})
    {
        CheckStart(kMarioKart7, effect, kOne);
    }
    constexpr SequenceStart kZero = {.variables = {{{.index = 0, .value = 0}}}};
    CheckStart(kMarioKart7, "SE_RC_RANK_UP", kZero);
    for (const std::string_view horn : {"SE_KT_HORN_KOOPA", "SE_KT_HORN_MARIO", "SE_KT_HORN_YOSHI"})
    {
        CheckStart(kMarioKart7, horn, kZero);
    }
    for (const std::string_view effect :
         {"SE_KT_FALL_WIND", "SE_RC_RANK_DOWN", "SE_RC_RANK_UP_TOP", "SE_SYS_BTN_OK", "SE_OBJ_COIN_GET"})
    {
        CheckStart(kMarioKart7, effect, {});
    }
}

// New Leaf's DJ K.K. starts his songs with variable 0 at 1, and the birthday and event songs and the interludes at 0.
// Each plays with the club's effect 0 and a send of 0.4 on bus A. The two interludes that play the town's tune also
// take the game's first default tune in key 0 in track variables 0 to 15 (indices 32 to 47). A Unicorn run of the DJ's
// writer (0x28B08C) gave the 16 values. The museum's entry for its whole name stays. The café's piece starts with
// track 7 muted.
void TestNewLeafDj()
{
    constexpr SequenceStart kOne = {
        .variables = {{{.index = 0, .value = 1}}}, .fx_send = {0.4f, 0.0f}, .game_effect = 0};
    constexpr SequenceStart kZero = {
        .variables = {{{.index = 0, .value = 0}}}, .fx_send = {0.4f, 0.0f}, .game_effect = 0};
    for (const std::string_view song : {"SEQ_BGM_DJ01_HS_SANBIKA", "SEQ_BGM_DJ05_FC_JAZZ", "SEQ_BGM_DJ90_HS_DISCO"})
    {
        CheckStart(kNewLeaf, song, kOne);
    }
    for (const std::string_view song :
         {"SEQ_BGM_DJ91_HS_BIRTHDAYSONG", "SEQ_BGM_DJ95_FC_EVENT", "SEQ_BGM_DJ_HS_HANIWA", "SEQ_BGM_DJ_FC_HANIWA"})
    {
        CheckStart(kNewLeaf, song, kZero);
    }
    constexpr std::array<int16_t, 16> kTune = {12, 10000, 7, 4,      5, 5,     5,     10000,
                                               7,  7,     5, -10000, 0, 10000, 10000, 10000};
    SequenceStart tune = kZero;
    for (std::size_t step = 0; step < kTune.size(); ++step)
    {
        tune.variables[step + 1] = {.index = static_cast<uint8_t>(32 + step), .value = kTune[step]};
    }
    for (const std::string_view interlude : {"SEQ_BGM_DJ_HS_MURAMELO", "SEQ_BGM_DJ_FC_MURAMELO"})
    {
        CheckStart(kNewLeaf, interlude, tune);
    }
    Check(ntdlib::ctr::snd::kStartVariables == 17, "a start holds the interludes' 17 variables");
    CheckStart(kNewLeaf, "SEQ_BGM_IND_MUSEUM", {.muted = 0xe3f0});
    CheckStart(kTomodachiLife, "SEQ_BGM_DJ05_FC_JAZZ", {});
    CheckStart(kNewLeaf, "SEQ_BGM_IND_CAFEARBEIT", {.muted = 0x0080});
    const auto cafe = SequenceArrangements(kNewLeaf, "SEQ_BGM_IND_CAFEARBEIT");
    Check(cafe.size() == 1 && cafe[0].suffix == "_track_7" && cafe[0].start == SequenceStart{},
          "the café's piece plays every track after its fade");
}

// Tomodachi Life's apartment music plays five more sets of tracks, and its night version one more.
void TestTomodachiApartment()
{
    const auto room = SequenceArrangements(kTomodachiLife, "BGM_MANSION_AND_ROOM");
    Check(room.size() == 5 && room[0].suffix == "_tracks_3_4_10" && room[0].start == SequenceStart{.muted = 0xfbe7} &&
              room[3].suffix == "_track_15" && room[3].start == SequenceStart{.muted = 0x7fff},
          "the apartment's music has the chooser's other sets of tracks");
    const auto night = SequenceArrangements(kTomodachiLife, "BGM_AKICHI_NIGHT");
    Check(night.size() == 1 && night[0].start == SequenceStart{.muted = 0xf0ff},
          "the night version plays tracks 8 to 11");
    CheckStart(kTomodachiLife, "BGM_MANSION_AND_ROOM", {.muted = 0xfff8});
}

// The tune player of New Leaf and Happy Home Designer starts each of its sounds at the tune's C, transposed by the
// sound's scale and offset. The train's announcement plays the first note of its tune. The sounds that play the whole
// tune, and the tune player's sounds in other games, keep -1.
void TestTunePlayer()
{
    const auto transposed = [](int16_t transpose)
    {
        return SequenceStart{.variables = {{{.index = 14, .value = 3}, {.index = 12, .value = transpose}}}};
    };

    for (const std::string_view archive : {kNewLeaf, kHappyHomeDesigner, kHappyHomeDesignerOld})
    {
        CheckStart(archive, "SE_MELODY_DOOR_HOME", transposed(0));
        CheckStart(archive, "SE_MELODY_NPCNRM_ANT00", transposed(0));
        CheckStart(archive, "SE_MELODY_NPCNRM_BEA06", transposed(-7));
        CheckStart(archive, "SE_MELODY_NPCNRM_BRD15", transposed(-17));
        CheckStart(archive, "SE_MELODY_NPCNRM_CAT20", transposed(4));
        CheckStart(archive, "SE_MELODY_NPCSP_SZA0", transposed(12));
        CheckStart(archive, "SE_MELODY_DOOR_GARDEN", transposed(24));
        CheckStart(archive, "SE_MELODY_HUMMING", transposed(12));
        CheckStart(archive, "SE_MELODY_LETTER", transposed(0));
        CheckStart(archive, "SE_MELODY_TRAIN_ANNOUNCE",
                   {.variables = {{{.index = 14, .value = 9}, {.index = 12, .value = 11}}}});
        CheckStart(archive, "SE_MELODY_ONESEQ_DOOR_HOME", {});
        CheckStart(archive, "SE_MELODY_ONESEQ_CHIME_TIME", {});
    }
    CheckStart(kTomodachiLife, "SE_MELODY_DOOR_HOME", {});
}

// The TV's run starts at the count of ticks left in its part, in New Leaf and Happy Home Designer. The jump keeps -1.
// Tomodachi Life's guitars play in the apartment music's first chord.
void TestCountdownAndChord()
{
    for (const std::string_view archive : {kNewLeaf, kHappyHomeDesigner, kHappyHomeDesignerOld})
    {
        CheckStart(archive, "SE_TV_12_RUN", {.variables = {{{.index = 0, .value = 928}}}});
        CheckStart(archive, "SE_TV_12_JUMP_START", {});
    }
    for (const std::string_view guitar : {"SE_MII_PLAY_GUITAR_S", "SE_MII_PLAY_GUITAR_M", "SE_MII_PLAY_GUITAR_L"})
    {
        CheckStart(kTomodachiLife, guitar, {.variables = {{{.index = 26, .value = 0}}}});
    }
    CheckStart(kNewLeaf, "SE_MII_PLAY_GUITAR_S", {});
}

// A sound that the table leaves out, a listed sound of another archive, an unknown archive and a name in other letters
// play every track.
void TestUnlisted()
{
    CheckStart(kNewLeaf, "SEQ_BGM_PROLOGUE_12", {});
    CheckStart(kTomodachiLife, "NA_BGM_RONRON", {});
    CheckStart(kNewLeaf, "SEQ_BGM_TITLE", {});
    CheckStart("0000000000000000000000000000000000000000", "SEQ_BGM_IND_MUSEUM", {});
    CheckStart(kNewLeaf, "seq_bgm_ind_museum", {});
    CheckStart("C3FDB98E65E6143D76E6D971CFAAAC058455300D", "NA_BGM_RONRON", {});
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("ctr_starting_tracks",
                                    []
                                    {
                                        TestSilencedTracks();
                                        TestMutedTracks();
                                        TestSystemTitles();
                                        TestNamePrefix();
                                        TestStartingStates();
                                        TestVariablesAndSeeds();
                                        TestNoteMelodies();
                                        TestEffectsFirstPlay();
                                        TestArrangements();
                                        TestMinigame708();
                                        TestIslandTourTempoPieces();
                                        TestMarioKart7Beats();
                                        TestMarioKart7Effects();
                                        TestNewLeafDj();
                                        TestTomodachiApartment();
                                        TestTunePlayer();
                                        TestCountdownAndChord();
                                        TestBankArrangements();
                                        TestUnlisted();
                                    });
}
