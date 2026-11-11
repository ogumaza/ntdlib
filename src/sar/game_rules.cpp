// SPDX-License-Identifier: MIT

#include "ntdlib/sar/game_rules.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "xsflib/sha1.h"

namespace ntdlib::sar
{

namespace
{

// The tracks from `first` to `last`, a bit for each.
constexpr uint16_t Tracks(int first, int last)
{
    uint16_t bits = 0;
    for (int t = first; t <= last; ++t)
    {
        bits = static_cast<uint16_t>(bits | 1u << t);
    }

    return bits;
}

// One track's bit.
constexpr uint16_t Track(int t)
{
    return Tracks(t, t);
}

// The microgames' sounds of WarioWare: Get It Together!'s Sound_Common.bfsar, by the reverb that each microgame puts on
// aux bus B. Each record of the game's PetitList.elf (in Archive/Data~GameData.nx.bea) is a microgame. It names the
// reverb at +0xe0 and the microgame's theme and jingles at +0x70 to +0xc0. A name there that starts with FF_ is the
// archive's label with BGM_ in place of FF_. A microgame whose code starts its music names BGM_Silence there. Its
// music's labels start with BGM_P_ and the record's name (BGM_P_Guitar for the record Guitar). The lists hold the
// labels of the archive's sequences that either way gives and that send to aux bus B on some path through their
// commands (the commands of the effect sends, 0xd9, 0xda and 0xde).
constexpr std::array<std::string_view, 35> kHallSounds = {
    "BGM_P_Angel",  "BGM_P_Animation",     "BGM_P_Apple",    "BGM_P_Bag",    "BGM_P_Balloon", "BGM_P_Building",
    "BGM_P_Buster", "BGM_P_Candy",         "BGM_P_Cleaner",  "BGM_P_Clock",  "BGM_P_Cockpit", "BGM_P_Cockpit_Success",
    "BGM_P_Corn",   "BGM_P_Crayfish",      "BGM_P_Dark",     "BGM_P_Drop",   "BGM_P_Duck",    "BGM_P_Face",
    "BGM_P_Fan",    "BGM_P_Fly",           "BGM_P_Hypnosis", "BGM_P_Jump",   "BGM_P_Lifting", "BGM_P_Maze",
    "BGM_P_Ninja",  "BGM_P_Ninja_Success", "BGM_P_Popup",    "BGM_P_Poster", "BGM_P_Slime",   "BGM_P_Snack",
    "BGM_P_Spring", "BGM_P_Sunshine",      "BGM_P_Tooth",    "BGM_P_Train",  "BGM_P_Turtle",
};
constexpr std::array<std::string_view, 226> kDefaultSounds = {
    "BGM_B_BonusGame",
    "BGM_BossBouldering",
    "BGM_BossBouldering_Miss",
    "BGM_BossBouldering_Success",
    "BGM_BossRobot",
    "BGM_BossRobot_Miss",
    "BGM_BossSafari",
    "BGM_P_Alms",
    "BGM_P_Alms_Miss",
    "BGM_P_Alms_Success",
    "BGM_P_Animal",
    "BGM_P_Archery",
    "BGM_P_Archery_Miss",
    "BGM_P_Archery_Success",
    "BGM_P_Avoidance",
    "BGM_P_Avoidance_Miss",
    "BGM_P_Avoidance_Success",
    "BGM_P_Badge",
    "BGM_P_Balance",
    "BGM_P_Bamboo",
    "BGM_P_Bamboo_Success",
    "BGM_P_Beaker",
    "BGM_P_Bear",
    "BGM_P_Behind",
    "BGM_P_Behind_Miss",
    "BGM_P_Behind_Success",
    "BGM_P_Bike",
    "BGM_P_Bike_Success",
    "BGM_P_Blimp",
    "BGM_P_Blimp_Success",
    "BGM_P_Block",
    "BGM_P_Block_Miss",
    "BGM_P_Block_Success",
    "BGM_P_Bottle",
    "BGM_P_Bottle_SnareRoll",
    "BGM_P_Bottle_Success",
    "BGM_P_Box",
    "BGM_P_Box_Success",
    "BGM_P_Buffalo",
    "BGM_P_Buffalo_Miss",
    "BGM_P_Buffalo_Success",
    "BGM_P_Carry",
    "BGM_P_Cat",
    "BGM_P_Cat_Success",
    "BGM_P_Check",
    "BGM_P_Check_Miss",
    "BGM_P_Check_Success",
    "BGM_P_Chick",
    "BGM_P_Chick_Success",
    "BGM_P_Circus",
    "BGM_P_Climber",
    "BGM_P_Climber_Success",
    "BGM_P_Constellation",
    "BGM_P_Count",
    "BGM_P_Cracker",
    "BGM_P_Crown",
    "BGM_P_Curtain",
    "BGM_P_Dance",
    "BGM_P_Devil_Lv1",
    "BGM_P_Devil_Lv2",
    "BGM_P_Dig",
    "BGM_P_Dig_Success",
    "BGM_P_Dinner",
    "BGM_P_Diy",
    "BGM_P_Down_Success",
    "BGM_P_Dust",
    "BGM_P_Dust_Success",
    "BGM_P_Egg",
    "BGM_P_Feast",
    "BGM_P_Fire",
    "BGM_P_Fire_Miss",
    "BGM_P_Flower",
    "BGM_P_Garlic",
    "BGM_P_Garlic_Success",
    "BGM_P_Ghost",
    "BGM_P_Glasses",
    "BGM_P_Gold",
    "BGM_P_Golf",
    "BGM_P_Golf_Success",
    "BGM_P_Handcar",
    "BGM_P_Head",
    "BGM_P_Head_Success_Lv1",
    "BGM_P_Head_Success_Lv2",
    "BGM_P_Head_Success_Lv3",
    "BGM_P_Hockey",
    "BGM_P_Hockey_Failure",
    "BGM_P_Hockey_Miss",
    "BGM_P_Hockey_Success",
    "BGM_P_Horn",
    "BGM_P_Horn_Success",
    "BGM_P_Icecream",
    "BGM_P_Ink_Miss",
    "BGM_P_Ink_Success",
    "BGM_P_Intercom",
    "BGM_P_Intercom_Success",
    "BGM_P_Invader",
    "BGM_P_Janken",
    "BGM_P_Janken_Lv2",
    "BGM_P_Janken_Lv3",
    "BGM_P_Janken_Miss",
    "BGM_P_Janken_Miss_Lv2",
    "BGM_P_Janken_Miss_Lv3",
    "BGM_P_Janken_Success",
    "BGM_P_Janken_Success_Lv2",
    "BGM_P_Janken_Success_Lv3",
    "BGM_P_Kickover",
    "BGM_P_Kickover_Success",
    "BGM_P_Koopa",
    "BGM_P_Like",
    "BGM_P_Matryoshka",
    "BGM_P_Mawaru_Beard",
    "BGM_P_Mawaru_Candle",
    "BGM_P_Mawaru_Dustbin",
    "BGM_P_Mawaru_Face",
    "BGM_P_Mawaru_Hukkin",
    "BGM_P_Mawaru_Tonight",
    "BGM_P_Mirror",
    "BGM_P_Mirror_Clear",
    "BGM_P_Mirror_Success",
    "BGM_P_Monochrome",
    "BGM_P_Monochrome_Miss",
    "BGM_P_Monochrome_Success",
    "BGM_P_Moon",
    "BGM_P_Moon_Success",
    "BGM_P_Morph_Lv1",
    "BGM_P_Morph_Lv3",
    "BGM_P_Nightmare",
    "BGM_P_Nose",
    "BGM_P_Nose_Breath1",
    "BGM_P_Nose_Breath2",
    "BGM_P_Nose_Success",
    "BGM_P_Nursery",
    "BGM_P_Nursery_Miss",
    "BGM_P_Pasta",
    "BGM_P_Percussion_1",
    "BGM_P_Percussion_1_Success",
    "BGM_P_Percussion_2",
    "BGM_P_Percussion_2_Success",
    "BGM_P_Percussion_3",
    "BGM_P_Percussion_3_Success",
    "BGM_P_Pig",
    "BGM_P_Polo",
    "BGM_P_Polo_Success",
    "BGM_P_Poseidon",
    "BGM_P_Poseidon_Success",
    "BGM_P_Punch",
    "BGM_P_Push",
    "BGM_P_Raven",
    "BGM_P_Restraint",
    "BGM_P_Restraint_Success",
    "BGM_P_Rock",
    "BGM_P_Rocket",
    "BGM_P_Rocket_Miss",
    "BGM_P_Safety",
    "BGM_P_Sand",
    "BGM_P_Sand_Success",
    "BGM_P_Scale",
    "BGM_P_Scale_Miss",
    "BGM_P_Scale_Success",
    "BGM_P_Scuba",
    "BGM_P_Separation",
    "BGM_P_Separation_Failure",
    "BGM_P_Set_Success_Lv1_1",
    "BGM_P_Shoot",
    "BGM_P_Shoot_Miss",
    "BGM_P_Shuffle",
    "BGM_P_Shutter",
    "BGM_P_Shutter_Miss",
    "BGM_P_Shutter_Success",
    "BGM_P_Signboard",
    "BGM_P_Signboard_Miss",
    "BGM_P_Signboard_Success",
    "BGM_P_Skydiving",
    "BGM_P_Sleep",
    "BGM_P_Snatch",
    "BGM_P_Snatch_Miss",
    "BGM_P_Snatch_Success",
    "BGM_P_Snowman",
    "BGM_P_Snowman_Miss",
    "BGM_P_Snowman_Success",
    "BGM_P_Spider",
    "BGM_P_Spot",
    "BGM_P_Stasis",
    "BGM_P_Stasis_Success",
    "BGM_P_Step",
    "BGM_P_Stopper",
    "BGM_P_Storm",
    "BGM_P_Swimming",
    "BGM_P_Swimming_Success",
    "BGM_P_Swing",
    "BGM_P_Swing_Success",
    "BGM_P_Talk",
    "BGM_P_Tanker",
    "BGM_P_Tanuki",
    "BGM_P_Tanuki_Success1",
    "BGM_P_Tanuki_Success2",
    "BGM_P_Tear",
    "BGM_P_Tear_Clear",
    "BGM_P_Toad",
    "BGM_P_Toad_Success",
    "BGM_P_Treasure",
    "BGM_P_Treasure_Success",
    "BGM_P_Tube",
    "BGM_P_Tube_Success",
    "BGM_P_Wall",
    "BGM_P_Wanted",
    "BGM_P_Wanted_Clear",
    "BGM_P_Warp",
    "BGM_P_Watering",
    "BGM_P_What",
    "BGM_P_What_Miss",
    "BGM_P_Wheel",
    "BGM_P_Wheel_Lv3",
    "BGM_P_Wheel_Success",
    "BGM_P_Wheel_Success_Lv3",
    "BGM_P_Who",
    "BGM_P_Who_Miss",
    "BGM_P_Who_Success",
    "BGM_P_Who_Timeover",
    "BGM_P_Windmill",
    "BGM_P_Windmill_Success",
    "BGM_P_Xmas",
    "BGM_P_Xmas_Clear",
    "BGM_T_BigPyoro_Game_1",
    "BGM_V_PartyPetitPaint",
    "BGM_V_PartyPetitRocket_1",
};

// The sequences of WarioWare: Get It Together!'s Sound_Common.bfsar that no microgame plays, and that send to aux bus B
// on some path through their commands (the commands of the effect sends, 0xd9, 0xda and 0xde).
constexpr std::array<std::string_view, 183> kOutsideSounds = {
    "BGM_B_Result",
    "BGM_B_Stand",
    "BGM_BossRobot_Success",
    "BGM_BossSafari_Miss",
    "BGM_BossWorld_Clear",
    "BGM_BossWorld_Lv1",
    "BGM_BossWorld_Lv3",
    "BGM_BossWorld_PSwitch_lp",
    "BGM_P_Hatena1",
    "BGM_P_Hatena2",
    "BGM_P_Success1",
    "BGM_P_Success2",
    "BGM_P_Success3",
    "BGM_T_Penny_Clear",
    "BGM_T_Penny_Continue_10",
    "BGM_T_Penny_Continue_10_EN",
    "BGM_T_Penny_Continue_11",
    "BGM_T_Penny_Continue_11_EN",
    "BGM_T_Penny_Continue_12",
    "BGM_T_Penny_Continue_12_EN",
    "BGM_T_Penny_Continue_13",
    "BGM_T_Penny_Continue_13_EN",
    "BGM_T_Penny_Continue_14",
    "BGM_T_Penny_Continue_14_EN",
    "BGM_T_Penny_Continue_15",
    "BGM_T_Penny_Continue_4",
    "BGM_T_Penny_Continue_5",
    "BGM_T_Penny_Continue_6",
    "BGM_T_Penny_Continue_7",
    "BGM_T_Penny_Continue_8",
    "BGM_T_Penny_Continue_9",
    "BGM_T_Penny_Continue_9_EN",
    "BGM_T_Penny_Failure_1",
    "BGM_T_Penny_Failure_10",
    "BGM_T_Penny_Failure_10_EN",
    "BGM_T_Penny_Failure_11",
    "BGM_T_Penny_Failure_11_EN",
    "BGM_T_Penny_Failure_12",
    "BGM_T_Penny_Failure_12_EN",
    "BGM_T_Penny_Failure_13",
    "BGM_T_Penny_Failure_13_EN",
    "BGM_T_Penny_Failure_14",
    "BGM_T_Penny_Failure_14_EN",
    "BGM_T_Penny_Failure_15",
    "BGM_T_Penny_Failure_15_EN",
    "BGM_T_Penny_Failure_1_EN",
    "BGM_T_Penny_Failure_2",
    "BGM_T_Penny_Failure_2_EN",
    "BGM_T_Penny_Failure_3",
    "BGM_T_Penny_Failure_4",
    "BGM_T_Penny_Failure_4_EN",
    "BGM_T_Penny_Failure_5",
    "BGM_T_Penny_Failure_5_EN",
    "BGM_T_Penny_Failure_6",
    "BGM_T_Penny_Failure_6_EN",
    "BGM_T_Penny_Failure_7",
    "BGM_T_Penny_Failure_7_EN",
    "BGM_T_Penny_Failure_8",
    "BGM_T_Penny_Failure_8_EN",
    "BGM_T_Penny_Failure_9",
    "BGM_T_Penny_Failure_9_EN",
    "BGM_T_Penny_GameOver",
    "BGM_T_Penny_Intro",
    "BGM_T_Penny_Intro_EN",
    "BGM_T_Penny_LevelUp_15",
    "BGM_T_Penny_LevelUp_15_EN",
    "BGM_T_Penny_Play_1",
    "BGM_T_Penny_Play_10",
    "BGM_T_Penny_Play_10_EN",
    "BGM_T_Penny_Play_11",
    "BGM_T_Penny_Play_11_EN",
    "BGM_T_Penny_Play_12",
    "BGM_T_Penny_Play_12_EN",
    "BGM_T_Penny_Play_13",
    "BGM_T_Penny_Play_13_EN",
    "BGM_T_Penny_Play_14",
    "BGM_T_Penny_Play_14_EN",
    "BGM_T_Penny_Play_15",
    "BGM_T_Penny_Play_15_EN",
    "BGM_T_Penny_Play_1_EN",
    "BGM_T_Penny_Play_2",
    "BGM_T_Penny_Play_2_EN",
    "BGM_T_Penny_Play_3",
    "BGM_T_Penny_Play_3_EN",
    "BGM_T_Penny_Play_4",
    "BGM_T_Penny_Play_4_EN",
    "BGM_T_Penny_Play_5",
    "BGM_T_Penny_Play_5_EN",
    "BGM_T_Penny_Play_6",
    "BGM_T_Penny_Play_6_EN",
    "BGM_T_Penny_Play_7",
    "BGM_T_Penny_Play_7_EN",
    "BGM_T_Penny_Play_8",
    "BGM_T_Penny_Play_8_EN",
    "BGM_T_Penny_Play_9",
    "BGM_T_Penny_Play_9_EN",
    "BGM_T_Penny_Result",
    "BGM_T_Penny_SpeedUp_12",
    "BGM_T_Penny_SpeedUp_12_EN",
    "BGM_T_Penny_SpeedUp_6",
    "BGM_T_Penny_SpeedUp_6_EN",
    "BGM_T_Penny_Stand_1",
    "BGM_T_Penny_Stand_10",
    "BGM_T_Penny_Stand_10_EN",
    "BGM_T_Penny_Stand_11",
    "BGM_T_Penny_Stand_11_EN",
    "BGM_T_Penny_Stand_12",
    "BGM_T_Penny_Stand_12_EN",
    "BGM_T_Penny_Stand_13",
    "BGM_T_Penny_Stand_13_EN",
    "BGM_T_Penny_Stand_14",
    "BGM_T_Penny_Stand_14_EN",
    "BGM_T_Penny_Stand_15",
    "BGM_T_Penny_Stand_15_EN",
    "BGM_T_Penny_Stand_1_EN",
    "BGM_T_Penny_Stand_2",
    "BGM_T_Penny_Stand_2_EN",
    "BGM_T_Penny_Stand_3",
    "BGM_T_Penny_Stand_3_EN",
    "BGM_T_Penny_Stand_4",
    "BGM_T_Penny_Stand_4_EN",
    "BGM_T_Penny_Stand_5",
    "BGM_T_Penny_Stand_5_EN",
    "BGM_T_Penny_Stand_6",
    "BGM_T_Penny_Stand_6_EN",
    "BGM_T_Penny_Stand_7",
    "BGM_T_Penny_Stand_7_EN",
    "BGM_T_Penny_Stand_8",
    "BGM_T_Penny_Stand_8_EN",
    "BGM_T_Penny_Stand_9",
    "BGM_T_Penny_Stand_9_EN",
    "BGM_T_Penny_Success_1",
    "BGM_T_Penny_Success_10",
    "BGM_T_Penny_Success_10_EN",
    "BGM_T_Penny_Success_11",
    "BGM_T_Penny_Success_11_EN",
    "BGM_T_Penny_Success_12",
    "BGM_T_Penny_Success_12_EN",
    "BGM_T_Penny_Success_13",
    "BGM_T_Penny_Success_13_EN",
    "BGM_T_Penny_Success_14",
    "BGM_T_Penny_Success_14_EN",
    "BGM_T_Penny_Success_15",
    "BGM_T_Penny_Success_15_EN",
    "BGM_T_Penny_Success_1_EN",
    "BGM_T_Penny_Success_2",
    "BGM_T_Penny_Success_2_EN",
    "BGM_T_Penny_Success_3",
    "BGM_T_Penny_Success_3_EN",
    "BGM_T_Penny_Success_4",
    "BGM_T_Penny_Success_4_EN",
    "BGM_T_Penny_Success_5",
    "BGM_T_Penny_Success_5_EN",
    "BGM_T_Penny_Success_6",
    "BGM_T_Penny_Success_6_EN",
    "BGM_T_Penny_Success_7",
    "BGM_T_Penny_Success_7_EN",
    "BGM_T_Penny_Success_8",
    "BGM_T_Penny_Success_8_EN",
    "BGM_T_Penny_Success_9",
    "BGM_T_Penny_Success_9_EN",
    "BGM_T_Sport_Boss",
    "BGM_T_Sport_Clear",
    "BGM_T_Sport_Failure",
    "BGM_T_Sport_GameOver",
    "BGM_T_Sport_Intro",
    "BGM_T_Sport_LevelUp",
    "BGM_T_Sport_Result",
    "BGM_T_Sport_SpeedUp",
    "BGM_T_Sport_Stand",
    "BGM_T_Sport_Success",
    "BGM_V_PartyPetitAlarm",
    "BGM_V_PartyPetitClimbing",
    "BGM_V_PartyPetitCyclopes",
    "BGM_V_PartyPetitEel",
    "BGM_V_PartyPetitLottery",
    "BGM_V_PartyPetitRocket_2",
    "BGM_V_PartyPetitRocket_3",
    "BGM_V_PartyPetitTweezers",
    "BGM_V_SuperWarioCo_Office",
    "BGM_V_SuperWarioCo_OfficeEvening",
    "BGM_V_SuperWarioCo_OfficeNight",
    "BGM_V_SuperWarioCo_Space",
};

// WarioWare: Get It Together!'s Sound_Common.bfsar. The game's code (its main, build
// e6784f640b1cdff90c8718a11294dbf32271cee9) sets the tracks' volumes through the BGM player's handles (0x7f49c0, which
// queues nn::atk's SetTrackVolume, command 0x1e, without a fade). A microgame has three levels, 1 to 3 in the game. Its
// code reads its level from 0x7354a0. The game sets the start state's volumes before the sequence's first tick.
ArchiveRules WarioWare()
{
    ArchiveRules a;

    // The guitar microgame (0x3df590 for level 3, 0x3df860 for levels 1 and 2) starts with track 0 alone. Each strum
    // runs 0x3dfa50. When the count of strums reaches a fifth of the microgame's total, rounded down, track 0 stops and
    // tracks 1 and 2 play. At three fifths, track 1 stops and track 3 plays. At the total, tracks 4 to 7 play too.
    a.layered.push_back({"BGM_P_Guitar",
                         {{"", Tracks(1, 7)},
                          {"20 percent", static_cast<uint16_t>(Track(0) | Tracks(3, 7))},
                          {"60 percent", static_cast<uint16_t>(Tracks(0, 1) | Tracks(4, 7))},
                          {"100 percent", Tracks(0, 1)}}});

    // The spotlight microgame (0x55c2b0) plays one piece for each level and starts it with tracks 0 to 3. Its 0x55c600
    // gives tracks 4 to 11 the volume of the success, 0 at the start. On levels 1 and 3 it gives track 12 that volume
    // too. The pieces have no track 12. At the success (0x55bfe0), 0x55ba30 turns that volume up by 1/12 a frame, to 1.
    a.layered.push_back({"BGM_P_Spotlight_Male", {{"", Tracks(4, 12)}, {"success", 0}}});
    a.layered.push_back({"BGM_P_Spotlight_Ballerina", {{"", Tracks(4, 11)}, {"success", 0}}});
    a.layered.push_back({"BGM_P_Spotlight_Female", {{"", Tracks(4, 12)}, {"success", 0}}});

    // The windmill microgame (0x5fe690) starts BGM_P_Windmill. Each frame, 0x5fe6b0 gives tracks 13 to 15 the share of
    // the turning done so far, from 0 at the start to 1 at the end. Level 1 turns up only track 15, and level 2 tracks
    // 14 and 15. At the end, the game starts the sound FF_P_Windmill_Success, which plays BGM_P_Windmill_Success. It
    // gives that piece's tracks 13 to 15 the levels' volumes at once: track 15 alone on level 1, tracks 14 and 15 on
    // level 2, and all three on level 3.
    a.layered.push_back({"BGM_P_Windmill",
                         {{"", Tracks(13, 15)},
                          {"level 1 turned", Tracks(13, 14)},
                          {"level 2 turned", Track(13)},
                          {"level 3 turned", 0}}});
    a.layered.push_back({"BGM_P_Windmill_Success", {{"", Tracks(13, 14)}, {"level 2", Track(13)}, {"level 3", 0}}});

    // The dance microgame's success (0x37bc20) stops tracks 5 to 7.
    a.layered.push_back({"BGM_P_Dance", {{"", 0}, {"success", Tracks(5, 7)}}});

    // Super Wario Co's results play on the BGM player's second slot. The result's Next button (0xaec920) runs 0xabfa00,
    // which stops tracks 3, 7, 11 and 13.
    const auto next = static_cast<uint16_t>(Track(3) | Track(7) | Track(11) | Track(13));
    a.layered.push_back({"BGM_V_SuperWarioCo_GoalResult", {{"", 0}, {"next", next}}});
    a.layered.push_back({"BGM_V_SuperWarioCo_MissResult", {{"", 0}, {"next", next}}});

    // Each microgame puts the reverb that its record names on aux bus B (0x768830, through 0x7ece30 with bus 1). Only
    // 0xb504b0 takes it off again, as a stage ends. The other sounds of a stage therefore start with bus B empty and
    // play with the last microgame's reverb after it. The presets' values are those of the game's SoundEffectList
    // sheet, as the game converts them (0x7f10f0): times in seconds, and the ratio and the dry gain as fractions.
    a.bus_b_presets.push_back(
        {"I3DL2_Default",
         {-1000.0f, -1000.0f, 2.0f, 0.8f, -700.0f, 0.005f, -500.0f, 0.04f, 50.0f, 3.0f, 6000.0f, 0.25f}});
    a.bus_b_presets.push_back(
        {"I3DL2_Hall", {-2400.0f, -2400.0f, 3.0f, 0.009f, -200.0f, 0.0f, 550.0f, 0.05f, 100.0f, 50.0f, 6000.0f, 0.0f}});
    for (const std::string_view label : kDefaultSounds)
    {
        a.bus_b.emplace(label, "I3DL2_Default");
    }
    for (const std::string_view label : kHallSounds)
    {
        a.bus_b.emplace(label, "I3DL2_Hall");
    }
    a.bus_b_later.assign(kOutsideSounds.begin(), kOutsideSounds.end());

    return a;
}

// WarioWare: Move It!'s Sound.bfsar. As it starts, the game puts the delay Delay_Waltz on aux bus A and the reverb
// I3DL2_Waltz on aux bus B (0xc7c1ac in its main), and leaves them there. The values are those of the game's
// SoundEffectList sheet. The reverb passes on none of its input unchanged.
ArchiveRules MoveIt()
{
    ArchiveRules a;
    a.bus_a_delay = true;
    a.bus_b_presets.push_back(
        {"I3DL2_Waltz", {-1000.0f, -1000.0f, 2.0f, 0.8f, -700.0f, 0.005f, -500.0f, 0.04f, 50.0f, 3.0f, 6000.0f, 0.0f}});
    a.bus_b_always = "I3DL2_Waltz";

    return a;
}

// The renderer's reverbs of Clubhouse Games: 51 Worldwide Classics, from the effect list of the game's
// audio/settingpreset/sound_settingpreset.bspp. At start-up, the game makes an nn::atk EffectReverb of each (0x600c00,
// on 4 channels) and passes it the list's values (0x35c910). The decay time goes in whole milliseconds (0x600ee0), and
// the pre-delay in whole microseconds (0x600dc0). Neither reverb passes on any of its input unchanged. A game's preset
// puts the reverb on aux bus B, and the game appends it there and enables it (0x606d10).
ReverbPreset ClubhouseReverb(std::string name, int early_mode, float early_gain, float predelay, int late_mode,
                             float late_gain, float decay, float ratio, float coloration, float reverb_gain)
{
    return {std::move(name),
            {static_cast<float>(early_mode), early_gain, predelay, static_cast<float>(late_mode), late_gain, decay,
             ratio, coloration, reverb_gain, 1.0f, 0.0f, 2.0f},
            ReverbType::kReverb};
}

// Clubhouse Games' AddonAudioProject.bfsar, in Archive/Resident.nx.bea. The piano game's preset ("gam022") puts
// EFFECT_GAM022 on aux bus B, and the 49 notes of the game's piano send there. The other sounds that send to bus B play
// in games whose presets leave the bus empty.
ArchiveRules Clubhouse()
{
    ArchiveRules a;
    a.bus_b_presets.push_back(
        ClubhouseReverb("EFFECT_GAM022", 2, 0.7f, 15'000.0f, 1, 0.7f, 2'500'000.0f, 0.8f, 0.7f, 0.4f));
    constexpr std::array<std::string_view, 12> kNotes = {"CN", "CS", "DN", "DS", "EN", "FN",
                                                         "FS", "GN", "GS", "AN", "AS", "BN"};
    for (int octave = 1; octave <= 4; ++octave)
    {
        for (const std::string_view note : kNotes)
        {
            a.bus_b.emplace("SQ_SE_GAM022_PIANO_" + std::string(note) + std::to_string(octave), "EFFECT_GAM022");
        }
    }
    a.bus_b.emplace("SQ_SE_GAM022_PIANO_CN5", "EFFECT_GAM022");

    return a;
}

// Super Mario 3D World + Bowser's Fury's SeData.bfsar. The voices of the player characters call a routine of the
// sequence file that turns local variable 0, the character, from -1 into 0 (INIT_VAR). It then picks the character's
// program from the variable (CHECK_CHARA: 0 to 7 give programs from 0, 10, 20, 30, 40, 70, 50 and 60 on). Only
// Rosalina's actor lists the spin attack's voices (x_Rosetta.byml in AudioEffectDataBase), and the ghosts' actors list
// their copies. Only character 4's program holds a sample at those voices' key. Only Captain Toad's actor lists his
// voices (x_KinopioBrigade.byml), and only character 6's program holds a sample at their key. The sequences also give
// character 6 alone a random pitch bend.
//
// A lantern plays its swing with local variable 0 as the volume, and lowers it by 20 a swing until it falls to 40. The
// lantern's code (0x277e60 in main) starts the sound with the variable at 120 times the swing's angle over 180 degrees
// (0x277f30). Its table of swings (0x1017b8c) holds angles of 120 and 180 degrees. The variable is therefore 80 or 120.
ArchiveRules SuperMario3DWorld()
{
    ArchiveRules a;
    const std::vector<VariableValues> rosalina = {{{0, 4}}};
    for (const char* label : {"SePvSpinAttack", "SePvSpinAttackAir", "SeGvSpinAttack", "SeGvSpinAttackAir"})
    {
        a.starts.push_back({label, rosalina});
    }
    const std::vector<VariableValues> captain_toad = {{{0, 6}}};
    for (const char* label : {"SePvKinotanJumpFail", "SePvKinotanAppearStage", "SePvKinotanAppearStageHaunt"})
    {
        a.starts.push_back({label, captain_toad});
    }
    a.starts.push_back({"SeOjLanternSwing", {{{0, 120}}, {{0, 80}}}});

    return a;
}

// The bowling game's sub-archive, subarc_gam005.bfsar in sound~subarc_gam005.nx.bea. Its sounds play only in that
// game, whose preset ("gam005") puts EFFECT_COMMON_ROOM_00 on aux bus B.
ArchiveRules ClubhouseBowling()
{
    ArchiveRules a;
    a.bus_b_presets.push_back(
        ClubhouseReverb("EFFECT_COMMON_ROOM_00", 2, 0.4f, 40'000.0f, 4, 0.5f, 2'000'000.0f, 0.7f, 0.1f, 0.6f));
    a.bus_b_always = "EFFECT_COMMON_ROOM_00";

    return a;
}

// Super Mario Party Jamboree's footsteps. The game writes local variables 6 to 10 of a 3D sound from the surface under
// it (0xf1708 in main, from 0xfb548): one variable from 7 to 10 for each of up to four surfaces, the surface's code in
// bits 7 to 13 and the square root of its weight, times 127, in bits 0 to 6. Variable 6 repeats the first surface. A
// footstep compares the code with 0 to 20 and 64 to 81. One surface under the whole foot gives a weight of 1.
std::vector<VariableValues> JamboreeSurfaces()
{
    std::vector<VariableValues> states;
    const auto add = [&states](int first, int last)
    {
        for (int code = first; code <= last; ++code)
        {
            const auto value = static_cast<int16_t>(code << 7 | 127);
            states.push_back({{6, value}, {7, value}});
        }
    };
    add(0, 20);
    add(64, 81);

    return states;
}

ArchiveRules JamboreeFootsteps()
{
    ArchiveRules a;
    a.starts.push_back({"SQ_SE_FS_*", JamboreeSurfaces()});

    return a;
}

// Super Mario Party Jamboree's subarc_bd01_bd03, a board's sounds. The roulette's train loop sounds only once local
// variable 15 is set. The board's code (ItemCastleRoulette::PlayRouletteAnim, 0x235e10 in bd01.nro) raises the
// variable from 0 to 127 over the roulette's animation and then holds it at 127 (0x235fa8).
ArchiveRules JamboreeBd03()
{
    ArchiveRules a;
    a.starts.push_back({"SQ_SE_BD03_ITEM00_TRN_LOP", {{{15, 127}}}});

    return a;
}

// Super Mario Party Jamboree's subarc_se_cmn_wtr_bike_1, the sounds of the water bike (mg0703) and the boat (mg1703).
// Each move loop compares local variable 15 with 0 to 9. The water bike's code writes 5 while it moves forward, 4 once
// it has stood still for half a second, 6 while it moves back and 7 in another state
// (RideBaseComponent::UpdateMoveSound, 0x3ccf4 in mg0703.nro), and 8 at the goal (0x3ecf8). The boat's code starts its
// loop at 0 (Player::SetEnableBoatSound, 0x37dc8 in mg1703.nro), writes 4 as it moves (Player::PlayBoatMove,
// 0x37464), 2 and 1 as the player steers (0x31980, 0x321c8) and 3 for a surface (0x33b04). The states start with the
// one that moves. The ambient copies of the two loops (SQ_AMB_*) are played by neither module.
ArchiveRules JamboreeWaterRides()
{
    ArchiveRules a;
    a.starts.push_back({"SQ_SE_MG0703_WTRBIKE_MOV_LOP", {{{15, 5}}, {{15, 4}}, {{15, 6}}, {{15, 7}}, {{15, 8}}}});
    a.starts.push_back({"SQ_SE_MG1703_BOAT_MOV_LOP", {{{15, 4}}, {{15, 0}}, {{15, 2}}, {{15, 1}}, {{15, 3}}}});

    return a;
}

// Super Mario Party Jamboree's subarc_mg0503. The two steam loops play at a volume from local variable 15. The
// minigame's code starts them at 1 (GameMgr::OnGameStartAfter_Mgr, 0x1fce8 and 0x1fd98 in mg0503.nro). It then
// writes 127 times a count of the minigame's state over 20, at most 127 (GameMgr::UpdateStreamEffect, 0x1c404 to
// 0x1c52c). The sequences pass the value through user procedure 5 first. The model doesn't run that procedure.
ArchiveRules JamboreeSteam()
{
    ArchiveRules a;
    for (const char* label : {"SQ_SE_MG0503_STEAM00_L_LOP", "SQ_SE_MG0503_STEAM00_R_LOP"})
    {
        a.starts.push_back({label, {{{15, 127}}}});
    }

    return a;
}

// Super Mario Party Jamboree's subarc_mg0912. The rolling rock's loop plays at a volume from local variable 15. The
// minigame's code writes the rock's distance a frame over 0.3, times 127, within 0 to 127 (RollOverRock::Update,
// 0x3c868 to 0x3c8b8 in mg0912.nro). The velocity that the code gives the rock (0x3ca3c), 15 down and 18.6 forward
// a second, moves it 0.4 a frame. The variable then reaches 127.
ArchiveRules JamboreeRock()
{
    ArchiveRules a;
    a.starts.push_back({"SQ_SE_MG0912_OBJ_ROCK00_ROL_LOP", {{{15, 127}}}});

    return a;
}

// A sound archive that has rules: its size, its SHA-1 and its rules.
struct RuledArchive
{
    std::size_t size = 0;
    std::string_view sha1;
    ArchiveRules rules;
};

const std::vector<RuledArchive>& Archives()
{
    static const std::vector<RuledArchive> kArchives = {
        {169'540'000, "de37204c45671bbb532b238669b459ab57c70a95", WarioWare()},
        {195'310'032, "971e60148f9ab6793a091803a2e649fea6b91fbb", MoveIt()},
        {56'574'072, "cd337a1d0041f9fed8eb6f2c9c74c419e87905bb", Clubhouse()},
        {3'600'192, "2f6b6c44031995054eaf70aa903d68d4faf4317b", ClubhouseBowling()},
        {929'216, "c628822613bf210ed412463c1f47de952be1a907", SuperMario3DWorld()},
        {64'338'704, "a4ede9f67dab3a17300a7a22d97b58f95552bfde", JamboreeFootsteps()},
        {341'968, "5405e2fdef8b729c852dd4cdde1f0e27551172ad", JamboreeFootsteps()},
        {1'842'144, "e6d62e59da1c2e15ed63d9dad14f17404b78901c", JamboreeBd03()},
        {650'360, "1fee777a145e7f00084e37c676adbcb59490c872", JamboreeWaterRides()},
        {1'731'768, "320813bb7292721bcae21d544fcde3288ac8feb9", JamboreeSteam()},
        {433'024, "ad47ccf12af80a9d1052c4b5c2a87191fd7c9a04", JamboreeRock()},
    };

    return kArchives;
}

} // namespace

const ReverbPreset* ArchiveRules::FindBusBPreset(std::string_view name) const
{
    const auto found = std::ranges::find(bus_b_presets, name, &ReverbPreset::name);

    return found == bus_b_presets.end() ? nullptr : &*found;
}

std::vector<SoundState> StatesOf(const ArchiveRules& rules, std::string_view label)
{
    const auto listed = rules.bus_b.find(label);
    const std::string reverb = listed != rules.bus_b.end() ? listed->second : rules.bus_b_always;

    std::vector<SoundState> states;
    const auto layered = std::ranges::find(rules.layered, label, &LayeredSound::label);
    if (layered != rules.layered.end())
    {
        for (const TrackState& state : layered->states)
        {
            states.push_back({state.name, state.muted, reverb});
        }

        return states;
    }

    constexpr std::string_view kI3dl2Prefix = "I3DL2_";
    states.push_back({"", 0, reverb});
    if (std::ranges::find(rules.bus_b_later, label) != rules.bus_b_later.end())
    {
        for (const ReverbPreset& preset : rules.bus_b_presets)
        {
            std::string_view name = preset.name;
            if (name.starts_with(kI3dl2Prefix))
            {
                name.remove_prefix(kI3dl2Prefix.size());
            }
            states.push_back({std::string(name) + " reverb", 0, preset.name});
        }
    }

    return states;
}

std::span<const VariableValues> StartsOf(const ArchiveRules& rules, std::string_view label)
{
    for (const VariableStarts& starts : rules.starts)
    {
        const std::string_view pattern = starts.label;
        const bool matches =
            pattern.ends_with('*') ? label.starts_with(pattern.substr(0, pattern.size() - 1)) : label == pattern;
        if (matches)
        {
            return starts.states;
        }
    }

    return {};
}

const ArchiveRules& RulesOf(std::span<const uint8_t> archive)
{
    const bool sized = std::ranges::any_of(Archives(), [&](const RuledArchive& a) { return a.size == archive.size(); });

    return sized ? RulesOf(archive.size(), xsflib::Sha1::Hex(xsflib::Sha1::Of(archive))) : RulesOf(archive.size(), "");
}

const ArchiveRules& RulesOf(std::size_t size, std::string_view sha1)
{
    static const ArchiveRules kNone;
    for (const RuledArchive& a : Archives())
    {
        if (a.size == size && a.sha1 == sha1)
        {
            return a.rules;
        }
    }

    return kNone;
}

} // namespace ntdlib::sar
