// SPDX-License-Identifier: MIT

#include "ntdlib/cafe/snd/sequence_starts.h"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

namespace ntdlib::cafe::snd
{

namespace
{

// The SHA-1s of the sound archives in the lists. Animal Crossing: amiibo Festival keeps its sounds in two archives.
constexpr std::string_view kAmiiboFestivalSha1 = "5784acb45299369f8388246f0ba4311610c709bc";
constexpr std::string_view kAmiiboFestivalArc1Sha1 = "724d9942fa0ea684839a3de269878e11b206a135";
constexpr std::string_view kMarioParty10Sha1 = "00bdc6c4610830576a44b7198d25894fe87f06b9";
constexpr std::string_view kNintendoLandSha1 = "1379ab0c17a80e0c38e0c9e90aaf92e971f2c1a4";
constexpr std::string_view kSuperMario3dWorldSha1 = "96b005771143ce5508eb0136143bde832e059ef8";
constexpr std::string_view kNewSuperMarioBrosUSha1 = "e0da277f9eed3c9fbd6dcbce09230811932d14dc";
constexpr std::string_view kWiiPartyUSha1 = "528580bb03f3fbdea2815d7fa7b355233d90d28d";
constexpr std::string_view kRainbowCurseSha1 = "813a15124d13b1978e7e3cb49f6648b9cb50d705";

constexpr SequenceVariable Var(uint8_t index, int16_t value)
{
    return {.index = index, .value = value};
}

// The tracks in `playing`, a bit for each.
constexpr uint16_t Playing(std::initializer_list<int> playing)
{
    uint16_t bits = 0;
    for (const int t : playing)
    {
        bits = static_cast<uint16_t>(bits | 1u << t);
    }

    return bits;
}

// A sequence that a game's code starts with some of its variables set, or with some tracks turned down, and its
// arrangements. A sequence whose values the code doesn't show starts as nw::snd starts it, with `start` empty. Its
// arrangements then hold the values on which its branches play something else, from the sequence's comparisons.
// `tracks` holds the tracks that play at the start. SequenceArrangement::tracks has the same meaning.
struct Sound
{
    std::string_view name;
    SequenceVariables start = {};
    std::span<const SequenceArrangement> arrangements = {};
    uint16_t tracks = 0xffff;
};

// Arrangements that several sequences share.
constexpr std::array<SequenceArrangement, 1> k0Is1 = {{{"_0_1", {Var(0, 1)}}}};
constexpr std::array<SequenceArrangement, 1> k0Is4 = {{{"_0_4", {Var(0, 4)}}}};
constexpr std::array<SequenceArrangement, 1> k0Is5 = {{{"_0_5", {Var(0, 5)}}}};
constexpr std::array<SequenceArrangement, 1> k8Is0 = {{{"_8_0", {Var(8, 0)}}}};
constexpr std::array<SequenceArrangement, 2> k8Is1Or2 = {{{"_8_1", {Var(8, 1)}}, {"_8_2", {Var(8, 2)}}}};
constexpr std::array<SequenceArrangement, 1> k15Is0 = {{{"_15_0", {Var(15, 0)}}}};
constexpr std::array<SequenceArrangement, 1> k15Is1 = {{{"_15_1", {Var(15, 1)}}}};
constexpr std::array<SequenceArrangement, 2> k15Is1Or2 = {{{"_15_1", {Var(15, 1)}}, {"_15_2", {Var(15, 2)}}}};
constexpr std::array<SequenceArrangement, 2> k15Is0Or1 = {{{"_15_0", {Var(15, 0)}}, {"_15_1", {Var(15, 1)}}}};
constexpr std::array<SequenceArrangement, 3> k15Is0To2 = {
    {{"_15_0", {Var(15, 0)}}, {"_15_1", {Var(15, 1)}}, {"_15_2", {Var(15, 2)}}}};
constexpr std::array<SequenceArrangement, 2> k22Is0Or1 = {{{"_22_0", {Var(22, 0)}}, {"_22_1", {Var(22, 1)}}}};
constexpr std::array<SequenceArrangement, 3> kLiftSpeeds = {
    {{"_0_20", {Var(0, 20)}}, {"_0_100", {Var(0, 100)}}, {"_0_200", {Var(0, 200)}}}};
constexpr std::array<SequenceArrangement, 2> kSpinAttack = {{{"_0_4", {Var(0, 4)}}, {"_2_-2", {Var(2, -2)}}}};

// Animal Crossing: amiibo Festival (USA), bbq.rpx. Each scene starts with resetScene (0x020F13A8), which sets every
// global variable to -1 (0x020F1574 to 0x020F158C). The scene's entry in content/sound/common/settingpreset.bin then
// sets some. The game's reader of that file (0x0210D320) calls writeGlobalVariable, the game's only caller. Its op 0x35
// adds a track's variable for one sequence (addLocalVariableSet, 0x0210D37C). setLocalVariableSet writes it once the
// sequence starts (writeTrackVariable, 0x020F26C4).
//
// - The board's roulettes give their spinning sequences to timing 1 (UIRoulette::setSELabel, 0x0241AA88, called at
//   0x0258DF78 and 0x0258EADC). UIRoulette::playSE writes variable 15 = 0 after timing 1's start (0x02419044). stop
//   writes 15 = 1 once the player stops the roulette (0x02419A84). The player picks the moment. The profile plays the
//   spin.
// - The escape game's and the habitat card's scenes set global 2 = 0 (variable 18): settingpreset.bin's scenes escape
//   and card_hab.
// - bbq::audioSetCharacter (0x023468D4) writes variable 11 from a table at 0x101C293C: 0, then 1. The overload for a
//   villager gives index 1 to villager 0x3E8 (0x02346944), whose steps ring the bell.
// - No code sets global 3 (variable 19) or global 8 (variable 24), and no scene's preset does. Only the game's
//   sequences set them. Those sequences start with -1, and the values become arrangements.
// - The game's code doesn't name SQ_SE_SYS_RLT_ROT, SQ_SE_ESCAPE_RLT_MAN_RLT or SQ_SE_INSECT_HORNET. Their variable 15
//   comes from data that hasn't been read. They start with -1, with the value from the sequences' comparisons as an
//   arrangement.
constexpr std::array<SequenceArrangement, 1> kLandingOne = {{{"_19_3", {Var(19, 3)}}}};
constexpr std::array<SequenceArrangement, 2> kWings = {{{"_24_1", {Var(24, 1)}}, {"_24_2", {Var(24, 2)}}}};
constexpr std::array<Sound, 17> kAmiiboFestivalSounds = {{
    {"SQ_SE_BOARD_RLT_MAN_RLT", {Var(15, 0)}},
    {"SQ_SE_BOARD_RLT_AUT_RLT", {Var(15, 0)}},
    {"SQ_SE_ESCAPE_ENV_SHAPER", {Var(18, 0)}},
    {"SQ_SE_HAB_ENV", {Var(18, 0)}},
    {"SQ_SE_PLY_FSTEP_BELL", {Var(11, 1)}},
    {"SQ_SE_PLY_FSTEP_BELL_SUB", {Var(11, 1)}},
    {"SQ_SE_BOARD_LND_ONE_H_01_4", {}, kLandingOne},
    {"SQ_SE_BOARD_LND_ONE_H_01_5", {}, kLandingOne},
    {"SQ_SE_BOARD_LND_ONE_M_01_4", {}, kLandingOne},
    {"SQ_SE_BOARD_LND_ONE_M_01_5", {}, kLandingOne},
    {"SQ_SE_BOARD_LND_ONE_R_01_4", {}, kLandingOne},
    {"SQ_SE_BOARD_LND_ONE_R_01_5", {}, kLandingOne},
    {"SQ_SE_ESCAPE_MFA_FLY_WING_LP", {}, kWings},
    {"SQ_SE_SYS_RLT_ROT", {}, k15Is0},
    {"SQ_SE_ESCAPE_RLT_MAN_RLT", {}, k15Is0},
    {"SQ_SE_INSECT_HORNET", {}, k15Is0},
    // The scene's preset gives track 0's variable 15 the value 1 (scenes board, event_place, event_viewer and
    // villager_test). The sequence is in BBQ_SOUND_ARC1.bfsar.
    {"SQ_SE_ACT_HUMMING", {SequenceVariable{.track = 0, .index = 15, .value = 1}}},
}};

// Mario Party 10 (USA), tenst.rpx. resetScene (0x020E48B8) sets every global variable to -1 (0x020E4A0C to
// 0x020E4A24). nw::snd's WriteGlobalVariable (0x022793F0) has one other caller, the game's writeGlobalVariable
// (0x020EDD28). Its only caller is the reader of content/sound/common/settingpreset.bin (0x020FEFC4, op 0x34).
//
// - Scenes BD001 to BD005 set global 13 (variable 29) to 0. Scene MG0B05, the tank game, sets global 8 (variable 24)
//   to 0.
// - No preset of their scenes sets the globals of SQ_SE_MG0306_KNK_HIT_A_RM, SQ_SE_MG0K04_KOP_TCH_DRC or
//   SQ_SE_MG0K06_BAL_ROL_DRC. Only the game's sequences set them.
// - The sound's variables 0, 8 and 15 come from the boards' and minigames' modules (the .rpl files in code/).
// - mg0307.rpl starts SQ_SE_MG0307_TRP with play3D. One path then writes variable 15 = 0 (0x0200ED2C), and another
//   writes 15 = 1 (0x0200EE18). The sequence starts at 0, with 1 as an arrangement.
// - mg0206.rpl sets the player's variable 15 through TaskCharaModelBase::setSEVariable: 0 on the ground (0x0200B678
//   and 0x0200B99C), 1 when the character's state is 2 (0x0200A7E4 and 0x0200AF9C) and 2 at a high jump (0x0200BD8C).
//   mp::CharaSound::setup (0x02E36480) writes the stored variables to each sound that the character plays (0x02E36590
//   to 0x02E365B0). No module names SQ_SE_MG0206_JUMP or SQ_SE_MG0206_ONP_LND. The character's data plays them. Both
//   start at 0, with the other values as arrangements.
// - The game computes the other sequences' values from its state. They start with -1, with the values from their
//   comparisons as arrangements. mg0428.rpl gives SQ_SE_MG0428_BLK_TUR_1P to 4P a count less 1, from 0 to 2 (0x02006D00
//   to 0x02006E44). mg0b05.rpl gives the small tanks' sounds a flag of the tank, 0 or 1 (0x020206F8 to 0x02020774), and
//   the large tank's sounds a byte of its state (0x02043830 to 0x02043AA4). mg0k05.rpl computes SQ_SE_MG0K05_DIC_LND's
//   variable from a ratio (0x0200F2A4). sbd001.rpl gives SQ_SE_PLY_AMI_BOUND_HAVOK a halfword of the object
//   (0x020A36DC), and SQ_SE_SBD001_COIN_GET_APPEND's variable 0 another (0x020317A4). bd001.rpl looks up the Koopa
//   dice's variable 8 from fields of the object (0x020A1208). mg0k04.rpl, mg0k06.rpl and mg0306.rpl also compute their
//   sequences' variable 15 from the physics (0x02005B94, 0x02008F9C and 0x02006430). No module names
//   SQ_SE_PLY_GEN_DMG_SEL_DMG_FIR, SQ_SE_PLY_FSTEP_RUN_MG0410 or SQ_SE_PLY_FSTEP_LAND_MG0410.
constexpr std::array<SequenceArrangement, 5> kKoopaTouch = {{
    {"_24_0", {Var(24, 0)}},
    {"_24_1", {Var(24, 1)}},
    {"_25_1", {Var(25, 1)}},
    {"_24_0__25_0", {Var(24, 0), Var(25, 0)}},
    {"_24_1__25_1", {Var(24, 1), Var(25, 1)}},
}};
constexpr std::array<SequenceArrangement, 4> kBalloonRoll = {{
    {"_22_0", {Var(22, 0)}},
    {"_22_1", {Var(22, 1)}},
    {"_15_0__22_0", {Var(15, 0), Var(22, 0)}},
    {"_15_1__22_1", {Var(15, 1), Var(22, 1)}},
}};
constexpr std::array<SequenceArrangement, 1> kHavokBound = {{{"_15_21", {Var(15, 21)}}}};
constexpr std::array<Sound, 48> kMarioParty10Sounds = {{
    {"SQ_SE_BD001_VCL_LOP", {Var(29, 0)}},
    {"SQ_SE_BD001_VCL_LOP_WOOD", {Var(29, 0)}},
    {"SQ_SE_BD002_VCL_LOP", {Var(29, 0)}},
    {"SQ_SE_BD003_VCL_LOP", {Var(29, 0)}},
    {"SQ_SE_BD004_VCL_LOP", {Var(29, 0)}},
    {"SQ_SE_BD004_VCL_LOP_JET", {Var(29, 0)}},
    {"SQ_SE_BD005_VCL_LOP", {Var(29, 0)}},
    {"SQ_SE_NPC000_TANK_READY00_2", {Var(24, 0)}},
    {"SQ_SE_MG0306_KNK_HIT_A_RM", {}, k22Is0Or1},
    {"SQ_SE_MG0K04_KOP_TCH_DRC", {}, kKoopaTouch},
    {"SQ_SE_MG0K06_BAL_ROL_DRC", {}, kBalloonRoll},
    {"SQ_SE_PLY_GEN_DMG_SEL_DMG_FIR", {}, k15Is0Or1},
    {"SQ_SE_BD000_KOOPA_DICE_BOUND", {}, k8Is0},
    {"SQ_SE_BD000_KOOPA_DICE_DRC_BOUND_DRC", {}, k8Is0},
    {"SQ_SE_BD005_KOOPA_DICE_DRC_BOUND_MTF", {}, k8Is0},
    {"SQ_SE_BD005_KOOPA_DICE_DRC_BOUND_MTF_DRC", {}, k8Is0},
    {"SQ_SE_SBD001_COIN_GET_APPEND", {}, k0Is5},
    {"SQ_SE_SBD001_COIN_GET_APPEND_DRC", {}, k0Is5},
    {"SQ_SE_PLY_AMI_BOUND_HAVOK", {}, kHavokBound},
    {"SQ_SE_MG0206_JUMP", {Var(15, 0)}, k15Is1Or2},
    {"SQ_SE_MG0206_ONP_LND", {Var(15, 0)}, k15Is1},
    {"SQ_SE_MG0307_TRP", {Var(15, 0)}, k15Is1},
    {"SQ_SE_PLY_FSTEP_RUN_MG0410", {}, k8Is1Or2},
    {"SQ_SE_PLY_FSTEP_LAND_MG0410", {}, k8Is1Or2},
    {"SQ_SE_MG0428_BLK_TUR_1P", {}, k15Is0To2},
    {"SQ_SE_MG0428_BLK_TUR_2P", {}, k15Is0To2},
    {"SQ_SE_MG0428_BLK_TUR_3P", {}, k15Is0To2},
    {"SQ_SE_MG0428_BLK_TUR_4P", {}, k15Is0To2},
    {"SQ_SE_MG0B05_TANK_S_IDLING_1P", {}, k15Is0Or1},
    {"SQ_SE_MG0B05_TANK_S_IDLING_2P", {}, k15Is0Or1},
    {"SQ_SE_MG0B05_TANK_S_IDLING_3P", {}, k15Is0Or1},
    {"SQ_SE_MG0B05_TANK_S_IDLING_4P", {}, k15Is0Or1},
    {"SQ_SE_MG0B05_TANK_S_AXEL_1P", {}, k15Is0Or1},
    {"SQ_SE_MG0B05_TANK_S_AXEL_2P", {}, k15Is0Or1},
    {"SQ_SE_MG0B05_TANK_S_AXEL_3P", {}, k15Is0Or1},
    {"SQ_SE_MG0B05_TANK_S_AXEL_4P", {}, k15Is0Or1},
    {"SQ_SE_MG0B05_TANK_S_CATERPILLAR_1P", {}, k15Is0Or1},
    {"SQ_SE_MG0B05_TANK_S_CATERPILLAR_2P", {}, k15Is0Or1},
    {"SQ_SE_MG0B05_TANK_S_CATERPILLAR_3P", {}, k15Is0Or1},
    {"SQ_SE_MG0B05_TANK_S_CATERPILLAR_4P", {}, k15Is0Or1},
    {"SQ_SE_MG0B05_TANK_S_GEAR_1P", {}, k15Is0Or1},
    {"SQ_SE_MG0B05_TANK_S_GEAR_2P", {}, k15Is0Or1},
    {"SQ_SE_MG0B05_TANK_S_GEAR_3P", {}, k15Is0Or1},
    {"SQ_SE_MG0B05_TANK_S_GEAR_4P", {}, k15Is0Or1},
    {"SQ_SE_MG0B05_TANK_L_AXEL", {}, k15Is0Or1},
    {"SQ_SE_MG0B05_TANK_L_CATERPILLAR", {}, k15Is0Or1},
    {"SQ_SE_MG0B05_TANK_L_AXEL_OFF", {}, k15Is0Or1},
    {"SQ_SE_MG0K05_DIC_LND", {}, k15Is0},
}};

// Nintendo Land (USA), Lunch.rpx. Four wrappers start the game's sounds (0x02024F0C, 0x02024FD8, 0x02025058 and
// 0x02025124). After the start, each writes the sound's variable 15 from a halfword of the object that plays it, unless
// the halfword holds 0xFFFF. The object's constructor writes 0xFFFF (0x02024DB4). The code doesn't name
// SE_PKM_PLAYER_FOOTSTEP, and nobody has traced the value that its object holds.
//
// SE_LIG_OBAKE_LIFE_DOWN rises from the key that variable 4 gives to the top of its range. The game writes 4 from a
// halfword of the object (0x028C1BE4). The constructor (0x028C1470) and a reset (0x028C1C5C) set the halfword to 0.
// After each start, the game reads variable 5 into it (0x028C1BFC). The sound thread hasn't set the new sound up by
// then: BasicSound's flag at +0xB9 stays 0 until the player's set-up command (0x02ACF5A4) has run. ReadVariable
// (0x02AD6EF8) therefore gives -1. The first play after a reset starts at 0, and each later play at -1.
//
// The train's two sounds take variable 0 as a level from -10 to -6. At -1 they sound as at -6. The game gives
// trunc(5 * y - 10) (0x028D984C to 0x028D98BC). y is a step curve of the train's speed, capped at 1 (0x028D9118 to
// 0x028D915C): 0, 0.05, 0.35 and 0.6 from 0, 0.2, 0.6 and 0.85 (0x101D9D88, read by 0x029434BC). Each of the train's
// states sets the speed to 0.5 (0x02741BE4, 0x027422F0, 0x02742AF0 and 0x02742DA0), and the speed then moves toward
// 0.5, 1.4 or 1.5 (0x02742730). The game therefore gives -9 at the start, then -8 and -7, and never -10 or -6.
//
// As each attraction's scene starts, the scene puts a reverb of the game's code on the TV's aux A and the GamePad's
// (constructor 0x02018784, set-up 0x0201F918). The files can't hold the reverb. Effect send A becomes CC91, and the
// synth's reverb stands in for the game's. Every scene's reverb has an all-pass coefficient of 0.6, a damping of 0.4
// and a gain of 1. The other settings, and the sequences that send to aux A, go by the attraction's prefix:
// - GWR: a pre-delay of 20 ms, a decay of 1 s, early reflection 1, length set 3, an early level of 0.1 and a level of
//   0.9. 11 sequences.
// - LIG, NZM and ZLD: as GWR, with early reflection 3. 3, 3 and 2 sequences.
// - MTR: as GWR, with early reflection 5 and set 4. 8 sequences.
// - PKM: as GWR, with early reflection 3 and set 5. 12 sequences.
// - DBT, FZR and YSI: other settings. None of their sequences sends to aux A.
// - The plaza (TTL, 0x028D4D2C), on the TV only: a delay of one sample without feedback, at a level of 0.5 and a
//   damping of 0.3. Then a reverb without pre-delay or early reflection, with a decay of 3 s, a level of 1 and the
//   48 kHz rate. One sequence, SE_TTL_STATUE_Gwr_Seaweed.
constexpr std::array<SequenceArrangement, 2> kFootstep = {{{"_15_-2", {Var(15, -2)}}, {"_15_0", {Var(15, 0)}}}};
constexpr std::array<SequenceArrangement, 1> k4IsMinus1 = {{{"_4_-1", {Var(4, -1)}}}};
constexpr std::array<SequenceArrangement, 2> kTrainFaster = {{{"_0_-8", {Var(0, -8)}}, {"_0_-7", {Var(0, -7)}}}};
constexpr std::array<Sound, 4> kNintendoLandSounds = {{
    {"SE_PKM_PLAYER_FOOTSTEP", {}, kFootstep},
    {"SE_LIG_OBAKE_LIFE_DOWN", {Var(4, 0)}, k4IsMinus1},
    {"SE_TTL_TRAIN_MOVE", {Var(0, -9)}, kTrainFaster},
    {"SE_TTL_TRAIN_MOVE_STEAM", {Var(0, -9)}, kTrainFaster},
}};

// Super Mario 3D World (USA), RedCarpet.rpx. The lantern's code gives variable 0 the swing as a share of 120: arg /
// [0x104E4350] * 120 (0x021E8CFC to 0x021E8D94). The word at 0x104E4350 holds a value that the game sets as it runs.
// The code doesn't name the other effects. The values of all nine are the sequences' comparisons. SeEvRunnerSqueak
// transposes its note by variable 0. Its instrument plays keys up to 32. The note sounds only with variable 0 at -32 or
// less. The game's SoundData/SeDataBase.szs plays the spin attacks (actors Rosetta and GhostPlayer), Captain Toad's
// three effects (KinopioBrigade) and the squeak (Runner) from actions or from code, and gives none of them a variable
// (LocalVarNo). The sound layer writes a sound's variables only from its list of 12 parameter commands (0x024D4980):
// command 4 writes one of the sound's (0x024D4AF0 to 0x024D4B18), and command 5 a global one (0x024D4B2C to
// 0x024D4B50), each from two floats. The code that fills the lists hasn't been traced.
constexpr std::array<SequenceArrangement, 1> kLantern = {{{"_0_41", {Var(0, 41)}}}};
constexpr std::array<SequenceArrangement, 3> kSqueak = {
    {{"_0_-64", {Var(0, -64)}}, {"_0_-48", {Var(0, -48)}}, {"_0_-32", {Var(0, -32)}}}};
constexpr std::array<Sound, 9> kSuperMario3dWorldSounds = {{
    {"SeOjLanternSwing", {}, kLantern},
    {"SePvSpinAttack", {}, kSpinAttack},
    {"SePvSpinAttackAir", {}, k0Is4},
    {"SePvKinotanJumpFail", {}, k0Is5},
    {"SePvKinotanAppearStage", {}, k0Is5},
    {"SePvKinotanAppearStageHaunt", {}, k0Is5},
    {"SeGvSpinAttack", {}, kSpinAttack},
    {"SeGvSpinAttackAir", {}, k0Is4},
    {"SeEvRunnerSqueak", {}, kSqueak},
}};

// New Super Mario Bros. U (USA), red-pro2.rpx. The lifts' and the water wheel's code start a held sound through
// 0x02972CD8 with variable 0 in r7, every frame. The line lift gives its speed * 100 (0x02799CD0 to 0x02799D34), the
// rising lift too (0x02793D4C to 0x02793DA4), and the water wheel its step of rotation (0x027B7D10 to 0x027B7D70). The
// map gives its touch scroll * 100 (0x021B57C8 to 0x021B57FC). The speeds come from each level's data, and the scroll
// from the player's touch. The code doesn't name SE_OBJ_FREE_FALL or SE_OBJ_RC_FENCE_ROLL. Each sequence follows its
// value above a least value up to a limit. The arrangements hold values from its comparisons. The executable holds
// neither SE_OBJ_FREE_FALL's nor SE_OBJ_RC_FENCE_ROLL's label, and loads neither archive index (0x5BD and 0x5C1) as an
// immediate.
constexpr std::array<SequenceArrangement, 3> kWaterWheel = {
    {{"_0_5", {Var(0, 5)}}, {"_0_50", {Var(0, 50)}}, {"_0_300", {Var(0, 300)}}}};
constexpr std::array<SequenceArrangement, 4> kFence = {
    {{"_0_1", {Var(0, 1)}}, {"_0_97", {Var(0, 97)}}, {"_0_193", {Var(0, 193)}}, {"_0_289", {Var(0, 289)}}}};
constexpr std::array<SequenceArrangement, 4> kMapScroll = {{{"_0_200", {Var(0, 200)}},
                                                            {"_0_1000", {Var(0, 1000)}},
                                                            {"_0_4800", {Var(0, 4800)}},
                                                            {"_0_10000", {Var(0, 10000)}}}};
constexpr std::array<Sound, 6> kNewSuperMarioBrosUSounds = {{
    {"SE_OBJ_RC_LIFTLINE_MOVE", {}, kLiftSpeeds},
    {"SE_OBJ_RC_UPLIFT_MOVE", {}, kLiftSpeeds},
    {"SE_OBJ_SUISHA_ROLL", {}, kWaterWheel},
    {"SE_OBJ_FREE_FALL", {}, k0Is1},
    {"SE_OBJ_RC_FENCE_ROLL", {}, kFence},
    {"SE_SYS_MAP_TOUCH_SCROLL_LV", {}, kMapScroll},
}};

// Wii Party U (USA, Rev 1), nexpy.rpx. The animations play the Miis' sounds through np::CharaSound::playSE (0x031AEC64,
// called from 0x031E380C and 0x031F68C4). Each start writes 16 halfwords of the CharaSound to the sound's variables 0
// to 15, all but 11 (0x031AE85C). The constructor sets them to 0 (0x031AEAFC). Only three minigames change one,
// variable 15: nmg411.rpl to 1 in the opening and to 0 at the touchdown (0x02008F84 and 0x02009B3C), nmg418.rpl to 0
// or 1 (0x0200FAC4 and 0x0200FAE0), and nmg426.rpl to 0, 1, 2 or a halfword (0x0200D504 to 0x0200D5C8).
//
// Variable 11 is the footstep. CharaSound writes the one that it holds, or else the scene's (0x02293138).
// NdSysAudio::play writes the scene's to the four sounds that addFootStepSound lists (0x022903D0, with the list from
// 0x031BDFDC). The scene's footstep starts at 0 (0x0228E59C). Each scene's entry in SETTING_PACK.bin can set it by name
// (0x02296AD8, with the names at 0x1065A29C): 1 earth, 2 sand, 3 stone, 4 iron, 5 wood, 6 lawn, 7 grass, 8 water, 9
// ice, 10 carpet, 11 barefoot and 12 barefoot on wood.
//
// npg009.rpl gives the ball's hits a speed from 1 to 127 in variable 15 (0x02000588 and 0x02000310). npg001.rpl writes
// the trace's variable 15 as the finger moves (0x0204369C). Those three start at -1 and play nothing there. Each
// frame, npg001.rpl gives the balloon's SEQ_SE_TVP_SDICE_03_AIR_B a value from 0 to 127 in variable 10 (0x02009A04 to
// 0x02009A40, through 0x02009674). The sequence bends its pitch by the value.
//
// npg006.rpl raises SEQ_BGM_NPG006_THEMA's variable 0 from -1 to 1, 2 and 3 as the player's score passes each of
// three thresholds (0x02002F58, through 0x02000960). The theme fades in its third track at 1 and its fourth at 2, and
// plays its ending at 3.
//
// In the archive, SEQ_SE_NMG431_TARU_HIT_TARU draws a transpose from 0 to 2 that differs from the last one and keeps it
// in global variable 28. SEQ_SE_NMG431_TARU_HIT_TARU_RM plays at that transpose and sets no variable. The hits on wood
// do the same with globals 25 and 27. The copies get an arrangement at the lowest transpose. 64 sequences check that
// the variable lies in a range, 0 to 127 for all but one, and map it onto their volume, pitch bend or tempo. Outside
// the range, as at -1, each plays its defaults. No value in the range plays those. Each starts at -1 and gets
// arrangements at the least value that sounds, the middle and the top. The game's code computes the value as it plays.
// The game's code for those values hasn't been read, apart from the two balls of npg001.rpl's dice (0 to 127,
// 0x02017720 to 0x020177A0, started at 0x020174B8). Every other sequence that reads a variable that it doesn't set
// plays at -1 as at a value in its range.
constexpr std::array<SequenceArrangement, 3> k15From0 = {
    {{"_15_0", {Var(15, 0)}}, {"_15_64", {Var(15, 64)}}, {"_15_127", {Var(15, 127)}}}};
constexpr std::array<SequenceArrangement, 3> k15From1 = {
    {{"_15_1", {Var(15, 1)}}, {"_15_64", {Var(15, 64)}}, {"_15_127", {Var(15, 127)}}}};
constexpr std::array<SequenceArrangement, 3> k15From2 = {
    {{"_15_2", {Var(15, 2)}}, {"_15_64", {Var(15, 64)}}, {"_15_127", {Var(15, 127)}}}};
constexpr std::array<SequenceArrangement, 3> k10From0 = {
    {{"_10_0", {Var(10, 0)}}, {"_10_64", {Var(10, 64)}}, {"_10_127", {Var(10, 127)}}}};
constexpr std::array<SequenceArrangement, 3> k10From1 = {
    {{"_10_1", {Var(10, 1)}}, {"_10_64", {Var(10, 64)}}, {"_10_127", {Var(10, 127)}}}};
constexpr std::array<SequenceArrangement, 3> kRoulette = {
    {{"_0_2", {Var(0, 2)}}, {"_0_50", {Var(0, 50)}}, {"_0_100", {Var(0, 100)}}}};
constexpr std::array<SequenceArrangement, 1> kBarrelCopy = {{{"_28_0", {Var(28, 0)}}}};
constexpr std::array<SequenceArrangement, 1> kWoodCopy = {{{"_25_0__27_0", {Var(25, 0), Var(27, 0)}}}};
constexpr std::array<SequenceArrangement, 3> kThemeLevels = {
    {{"_0_1", {Var(0, 1)}}, {"_0_2", {Var(0, 2)}}, {"_0_3", {Var(0, 3)}}}};
constexpr std::array<SequenceArrangement, 12> kSurfaces = {{
    {"_11_1", {Var(11, 1)}},
    {"_11_2", {Var(11, 2)}},
    {"_11_3", {Var(11, 3)}},
    {"_11_4", {Var(11, 4)}},
    {"_11_5", {Var(11, 5)}},
    {"_11_6", {Var(11, 6)}},
    {"_11_7", {Var(11, 7)}},
    {"_11_8", {Var(11, 8)}},
    {"_11_9", {Var(11, 9)}},
    {"_11_10", {Var(11, 10)}},
    {"_11_11", {Var(11, 11)}},
    {"_11_12", {Var(11, 12)}},
}};
constexpr std::array<Sound, 101> kWiiPartyUSounds = {{
    {"SEQ_SE_PLY_FUTTOBI_DAMAGE", {Var(15, 0)}, k15Is1Or2},
    {"SEQ_SE_PLY_FUTTOBI_DAMAGE_DRC", {Var(15, 0)}, k15Is1Or2},
    {"SEQ_SE_PLY_LAND_REAR_1", {Var(15, 0)}, k15Is1},
    {"SEQ_SE_PLY_LAND_REAR_1_DRC", {Var(15, 0)}, k15Is1},
    {"SEQ_SE_PLY_LAND_REAR_2", {Var(15, 0)}},
    {"SEQ_SE_PLY_LAND_REAR_2_DRC", {Var(15, 0)}},
    {"SEQ_SE_PLY_DAMAGE_BOUND", {Var(15, 0)}, k15Is1Or2},
    {"SEQ_SE_PLY_DAMAGE_BOUND_DRC", {Var(15, 0)}, k15Is1Or2},
    {"SEQ_SE_PLY_DAMAGE06_1", {Var(15, 0)}, k15Is1},
    {"SEQ_SE_PLY_DAMAGE06_1_DRC", {Var(15, 0)}, k15Is1},
    {"SEQ_SE_PLY_DAMAGE06_2", {Var(15, 0)}},
    {"SEQ_SE_PLY_DAMAGE06_2_DRC", {Var(15, 0)}},
    {"SEQ_SE_PLY_GROGGY02A", {Var(15, 0)}, k15Is1},
    {"SEQ_SE_PLY_GROGGY02A_DRC", {Var(15, 0)}, k15Is1},
    {"SEQ_SE_PLY_NMG411_NIN_JUMP00", {Var(15, 0)}, k15Is1},
    {"SEQ_SE_NPG009_BALL_HIT_BALL", {}, k15From1},
    {"SEQ_SE_NPG009_BALL_HIT_WALL_FLOOR", {}, k15From2},
    {"SEQ_SE_NPG001_EVE_CHALL_3JYUMARU_NAZORI_DRC", {}, k15From2},
    {"SEQ_SE_TVP_SDICE_03_AIR_B", {}, k10From0},
    {"SEQ_BGM_NPG006_THEMA", {}, kThemeLevels},
    {"SEQ_SE_NMG431_TARU_HIT_TARU_RM", {}, kBarrelCopy},
    {"SEQ_SE_NMG431_TARU_HIT_KI_RM", {}, kWoodCopy},
    {"SEQ_SE_NMG805_CAR_DRIVE_1P_DRC", {}, k10From0},
    {"SEQ_SE_NMG805_CAR_DRIVE_2P_DRC", {}, k10From0},
    {"SEQ_SE_TVP_SDICE_06_BALL", {}, k10From1},
    {"SEQ_SE_TVP_SDICE_06_BALL_OUTSIDE", {}, k10From1},
    {"SEQ_SE_DICE020_BOW_STRETCH", {}, k15From0},
    {"SEQ_SE_DICE023_AIR_DRC", {}, k15From0},
    {"SEQ_SE_DICE023_FUE_DRC", {}, k15From2},
    {"SEQ_SE_DICE029_BANE_NOBI_CHIDIMI", {}, k15From0},
    {"SEQ_SE_NMG308_WALL_HIT", {}, k15From1},
    {"SEQ_SE_NMG308_WALL_HIT_DRC", {}, k15From1},
    {"SEQ_SE_NMG313_TANK_L_AXEL", {}, k15From0},
    {"SEQ_SE_NMG313_TANK_L_AXEL_DRC", {}, k15From0},
    {"SEQ_SE_NMG313_TANK_L_HIT_WALL", {}, k15From0},
    {"SEQ_SE_NMG313_TANK_L_HIT_WALL_DRC", {}, k15From0},
    {"SEQ_SE_NMG408_BALL_HIT_WALL", {}, k15From0},
    {"SEQ_SE_NMG412_HIT_POLE", {}, k15From0},
    {"SEQ_SE_NMG412_HIT_RC", {}, k15From1},
    {"SEQ_SE_NMG412_HIT_WALL", {}, k15From1},
    {"SEQ_SE_NMG413_SLIDE_CONCRETE_1P", {}, k15From0},
    {"SEQ_SE_NMG413_SLIDE_CONCRETE_2P", {}, k15From0},
    {"SEQ_SE_NMG413_SLIDE_CONCRETE_3P", {}, k15From0},
    {"SEQ_SE_NMG413_SLIDE_CONCRETE_4P", {}, k15From0},
    {"SEQ_SE_NMG425_MII_SLIDE_1P", {}, k15From0},
    {"SEQ_SE_NMG425_MII_SLIDE_2P", {}, k15From0},
    {"SEQ_SE_NMG425_MII_SLIDE_3P", {}, k15From0},
    {"SEQ_SE_NMG425_MII_SLIDE_4P", {}, k15From0},
    {"SEQ_SE_NMG425_TUBE_SLIDE_1P", {}, k15From2},
    {"SEQ_SE_NMG425_TUBE_SLIDE_2P", {}, k15From2},
    {"SEQ_SE_NMG425_TUBE_SLIDE_3P", {}, k15From2},
    {"SEQ_SE_NMG425_TUBE_SLIDE_4P", {}, k15From2},
    {"SEQ_SE_NMG426_HIT_WALL", {}, k15From2},
    {"SEQ_SE_NMG429_CENTER_SLOT_VAR", {}, k15From0},
    {"SEQ_SE_NMG431_TARU_ROLL_1P", {}, k15From2},
    {"SEQ_SE_NMG431_TARU_ROLL_2P", {}, k15From2},
    {"SEQ_SE_NMG431_TARU_ROLL_3P", {}, k15From2},
    {"SEQ_SE_NMG431_TARU_ROLL_4P", {}, k15From2},
    {"SEQ_SE_NMG442_AMU", {}, k15From0},
    {"SEQ_SE_NMG601_KEZURU_DRC", {}, k15From2},
    {"SEQ_SE_NMG803_BALL_L_ROLL_LINE_1_DRC", {}, k15From0},
    {"SEQ_SE_NMG803_BALL_L_ROLL_LINE_2_DRC", {}, k15From0},
    {"SEQ_SE_NMG803_BALL_L_ROLL_LINE_3_DRC", {}, k15From0},
    {"SEQ_SE_NMG803_BALL_M_ROLL_LINE_1_DRC", {}, k15From0},
    {"SEQ_SE_NMG803_BALL_M_ROLL_LINE_2_DRC", {}, k15From0},
    {"SEQ_SE_NMG803_BALL_M_ROLL_LINE_3_DRC", {}, k15From0},
    {"SEQ_SE_NMG804_SCRATCH_DRC", {}, k15From2},
    {"SEQ_SE_NMG851_CONVEYOR", {}, k15From2},
    {"SEQ_SE_NMG854_CRANK_1P_DRC", {}, k15From2},
    {"SEQ_SE_NMG854_CRANK_2P_DRC", {}, k15From2},
    {"SEQ_SE_NPG002_DICE_BOUND", {}, k15From2},
    {"SEQ_SE_NPG002_DICE_FURU", {}, k15From0},
    {"SEQ_SE_NPG002_DICE_FURU_RM", {}, k15From0},
    {"SEQ_SE_NPG002_DICE_ROTA", {}, k15From0},
    {"SEQ_SE_NPG010_DRAW_DRC", {}, k15From0},
    {"SEQ_SE_NPG012_BALL_HIT_PENDULUM_DRC", {}, k15From0},
    {"SEQ_SE_NPG012_BALL_HIT_PENDULUM_SHINKOU_DRC", {}, k15From0},
    {"SEQ_SE_NPG012_BALL_HIT_WALL_DRC", {}, k15From0},
    {"SEQ_SE_NPG012_BALL_HIT_WALL_MAZE_DRC", {}, k15From0},
    {"SEQ_SE_NPG012_BALL_HIT_WALL_PLASTIC_DRC", {}, k15From0},
    {"SEQ_SE_NPG012_BALL_SHOT_DRC", {}, k15From0},
    {"SEQ_SE_NPG027_DICE_BOUND", {}, k15From2},
    {"SEQ_SE_NPG027_DICE_FURU", {}, k15From0},
    {"SEQ_SE_NPG027_DICE_FURU_RM", {}, k15From0},
    {"SEQ_SE_NPG027_DICE_ROTA", {}, k15From0},
    {"SEQ_SE_NPG001_DROUL_ROTA", {}, kRoulette},
    {"SEQ_SE_PLY_FOOT_WALK", {Var(11, 0)}, kSurfaces},
    {"SEQ_SE_PLY_FOOT_WALK_DRC", {Var(11, 0)}, kSurfaces},
    {"SEQ_SE_PLY_FOOT_WALK_V1", {Var(11, 0)}, kSurfaces},
    {"SEQ_SE_PLY_FOOT_WALK_V1_DRC", {Var(11, 0)}, kSurfaces},
    {"SEQ_SE_PLY_FOOT_RUN", {Var(11, 0)}, kSurfaces},
    {"SEQ_SE_PLY_FOOT_RUN_DRC", {Var(11, 0)}, kSurfaces},
    {"SEQ_SE_PLY_FOOT_RUN_V1", {Var(11, 0)}, kSurfaces},
    {"SEQ_SE_PLY_FOOT_RUN_V1_DRC", {Var(11, 0)}, kSurfaces},
    {"SEQ_SE_PLY_JUMP", {Var(11, 0)}, kSurfaces},
    {"SEQ_SE_PLY_JUMP_DRC", {Var(11, 0)}, kSurfaces},
    {"SEQ_SE_PLY_LANDING", {Var(11, 0)}, kSurfaces},
    {"SEQ_SE_LANDING", {Var(11, 0)}, kSurfaces},
    {"SEQ_SE_PLY_LANDING_DRC", {Var(11, 0)}, kSurfaces},
    {"SEQ_SE_PLY_LANDING_V1", {Var(11, 0)}, kSurfaces},
    {"SEQ_SE_PLY_LANDING_V1_DRC", {Var(11, 0)}, kSurfaces},
}};

// Kirby and the Rainbow Curse (USA), seven.rpx. The stage select plays BGM_SEQ_MENU_INTERACTIVE (item 0x0100030B)
// through areastageselect::BgmController. Its changeType (0x023EC428) takes an area type and gives the sequence's
// tracks that type's row of a table at 0x10058ABC: 1 for each track that plays and 0 for the rest. The first call
// starts the sound with the row already applied (forceCompleteFade, 0x023EC404). Later calls fade each track to its new
// volume by 1/30 a frame. The area view passes type 1 (0x023F4948). A world's list of stages passes the world's number
// and 1, and its boss stage the world's number and 8 (0x023FA074). The table's row 0 is the same as row 1, and no code
// passes type 0. The sequence has no track 9.
constexpr std::array<SequenceArrangement, 14> kStageSelect = {{
    {" world 1", {}, Playing({0, 10})},
    {" world 2", {}, Playing({1})},
    {" world 3", {}, Playing({2, 12})},
    {" world 4", {}, Playing({3})},
    {" world 5", {}, Playing({4})},
    {" world 6", {}, Playing({5})},
    {" world 7", {}, Playing({6})},
    {" world 1 boss", {}, Playing({0, 8, 10})},
    {" world 2 boss", {}, Playing({1, 11})},
    {" world 3 boss", {}, Playing({2, 8, 12})},
    {" world 4 boss", {}, Playing({3, 8})},
    {" world 5 boss", {}, Playing({4, 11})},
    {" world 6 boss", {}, Playing({5, 11})},
    {" world 7 boss", {}, Playing({6, 8})},
}};
constexpr std::array<Sound, 1> kRainbowCurseSounds = {{
    {"BGM_SEQ_MENU_INTERACTIVE", {}, kStageSelect, Playing({7})},
}};

// The sequences that each archive lists, by its SHA-1. amiibo Festival's second archive holds the last of its list.
struct ArchiveSounds
{
    std::string_view sha1;
    std::span<const Sound> sounds;
};

constexpr std::array<ArchiveSounds, 8> kArchives = {{
    {kAmiiboFestivalSha1, std::span(kAmiiboFestivalSounds).first(kAmiiboFestivalSounds.size() - 1)},
    {kAmiiboFestivalArc1Sha1, std::span(kAmiiboFestivalSounds).last(1)},
    {kMarioParty10Sha1, kMarioParty10Sounds},
    {kNintendoLandSha1, kNintendoLandSounds},
    {kSuperMario3dWorldSha1, kSuperMario3dWorldSounds},
    {kNewSuperMarioBrosUSha1, kNewSuperMarioBrosUSounds},
    {kWiiPartyUSha1, kWiiPartyUSounds},
    {kRainbowCurseSha1, kRainbowCurseSounds},
}};

// The entry for the sequence labelled `name` in the archive whose SHA-1 is `archive_sha1`, or nullptr.
const Sound* Find(std::string_view archive_sha1, std::string_view name)
{
    for (const ArchiveSounds& archive : kArchives)
    {
        if (archive.sha1 != archive_sha1)
        {
            continue;
        }

        for (const Sound& sound : archive.sounds)
        {
            if (sound.name == name)
            {
                return &sound;
            }
        }
    }

    return nullptr;
}

} // namespace

SequenceStart StartOfSequence(std::string_view archive_sha1, std::string_view sound)
{
    const Sound* found = Find(archive_sha1, sound);
    return found ? SequenceStart{found->start, found->tracks} : SequenceStart{};
}

std::span<const SequenceArrangement> SequenceArrangements(std::string_view archive_sha1, std::string_view sound)
{
    const Sound* found = Find(archive_sha1, sound);
    return found ? found->arrangements : std::span<const SequenceArrangement>();
}

std::vector<std::string_view> ListedSequences(std::string_view archive_sha1)
{
    std::vector<std::string_view> labels;
    for (const ArchiveSounds& archive : kArchives)
    {
        if (archive.sha1 == archive_sha1)
        {
            for (const Sound& sound : archive.sounds)
            {
                labels.push_back(sound.name);
            }
        }
    }

    return labels;
}

} // namespace ntdlib::cafe::snd
