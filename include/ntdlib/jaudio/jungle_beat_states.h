// SPDX-License-Identifier: MIT

// The states that Donkey Kong Jungle Beat's code sets for a piece of music during play. The BGM player sets a piece's
// states from two tables. Each state below plays tracks that the piece's start leaves off, or leaves off tracks that
// the start plays. A state whose tracks are those of the start or of a state before it is left out. The addresses are
// in the main.dol of the GameCube release (USA).
// - The first table contains the layers of a combo. A combo builds a count (0x8004043c). Once the count reaches the
//   first, second or third count of the stage's row, the BGM player plays state 11, 12 or 13 (0x802540d8). The stage
//   table (0x803b3978) picks each stage's counts from the sets at 0x803b3810: 10, 45 and 80 in most stages, and up to
//   300, 500 and 1,000 in AboveTheClouds. State 13 plays the tracks of state 12 in every piece and is left out.
//   `combo1` and `combo2` are states 11 and 12.
// - The first table also contains states for the player's flags. A Helibird sets bit 2 while it carries DK
//   (0x801ed0a4). The BGM player then plays state 6 (0x80253da8). Three pieces play more tracks then: `helibird`. Two
//   of them also have the state with the layers of a combo.
// - The second table contains the states that the code of a boss, a stage or a scene sets for its piece through
//   0x802565b8 (72 callers) or 0x802569e8. Each state is named after what sets it.
// - The fights against the four Kongs set four states: Dread Kong (JA_BGM_MATSURI_A and _B), Konfu, Miyamoto and
//   Akebono. The B piece starts with the boxing (0x800c9d7c). The boss's face sets state 1 once the boss has 75 % of
//   its health or less (`below_75`, 0x80062e9c). The health is 100 times the boss's points over its most points
//   (0x80063e68). The face sets state 4 while the boss changes between the rounds (`change`, the face's ChangeDemo). It
//   sets state 2 in the second round (`round2`), and state 3 once the boss has 30 % or less there (`round2_below_30`).
//   The faces are at 0x80044590, 0x8005038c, 0x8005dd48 and 0x800618d0.
// - The fights against GoroPig (0x800551e8 to 0x800554a8, 0x800cbddc and 0x800cc014), ElephantCannon (0x80047fd4 to
//   0x80049860) and SlumpBird (0x80072178 to 0x80072454 and 0x800ce318) have two rounds. In the first round, the boss's
//   code sets state 1 as the piece starts. It sets state 2 while DK hits or holds the boss there (`hit`), and state 5
//   while the boss changes between the rounds (`change`). It sets state 3 in the second round (`round2`), and state 4
//   while DK hits or holds the boss there (`round2_hit`). ElephantCannon's fight never sets state 5.
// - The final fight sets a state by the boss's health over 500 (0x80068904): 0 above 60 %, 2 above 30 % (`below_60`)
//   and 4 at 30 % or less (`below_30`). Each is one higher while DK holds on to the boss (`hold`, `below_60_hold` and
//   `below_30_hold`, from 0x8006829c on).
// - TropicalCoast's frame sets JA_BGM_MAIN_05 state 1 while DK is below a depth of 12,000 (`underwater`, 0x80250c04).
//   Its state 2 plays only in a test stage, Y_TestSea, and is left out.
// - VsFinalKong's frame sets JA_BGM_MAIN_22 states 1 and 2 by DK's place (0x80250c80). State 1 plays the tracks of the
//   start. A step of the lead-in to the final fight sets state 3 as it starts (`fall`, 0x800cf3dc). In that step DK
//   falls from the Helibird. The next step lands him. The Helibird then leaves (0x800cf4f0).
// - `stateN` is state N of the other pieces: JA_BGM_MAIN_22 state 2, and the states below. EndingCeremony's frame sets
//   JA_BGM_ENDING_CELEMONY state 2 by DK's place (0x80250d70). Its state 4 turns every track off and is left out. The
//   bind's music, JA_BGM_BIND_01, gets state 1 from 0x800fd2c0 and 0x800fe738.
//
// The Wii release, New Play Control! Donkey Kong Jungle Beat, keeps the GameCube release's tables of states and stages.
// Its code sets the same states. In its main.dol, the combo's count is at 0x80052a90, the BGM player plays states 11 to
// 13 at 0x80279cf0, and the stage table is at 0x803aafb0 with the sets of counts at 0x803aae48. The BGM player plays
// the Helibird's state 6 at 0x802799d0. The second table's states go through 0x8027bc60 (75 callers). JA_BGM_MAIN_22's
// `fall` comes from 0x800e7a80. The Wii release adds a row for JA_BGM_COURSE_SELECT. Its state 1 turns tracks 2 and 3
// off. The menus' code sets it (0x801e3ab8 to 0x801e433c).

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <span>
#include <vector>

namespace ntdlib::jaudio
{

// A ripper gives a piece in a state the piece's sound ID with the state's bits: a combo's layer in bits 16 and 17, a
// state of the BGM player's second table in bits 18 to 20, and a flight with a Helibird in bit 21. The bits are 0 in
// the ID of a piece. The ripper's driver sets the states once it has started the piece.
inline constexpr uint32_t kComboLayerShift = 16;
inline constexpr uint32_t kTableStateShift = 18;
inline constexpr uint32_t kHelibird = 1u << 21;

// The bits of layer `layer` of a combo, 1 to 3.
constexpr uint32_t Combo(uint32_t layer)
{
    return layer << kComboLayerShift;
}

// The bits of state `state` of the BGM player's second table, 1 to 7.
constexpr uint32_t TableState(uint32_t state)
{
    return state << kTableStateShift;
}

// A state of a piece of music: the piece's JAudio sound ID, the state's bits, the suffix of the state's name, and the
// children of the piece's track 0 that play in the state (bit n for child n). The BGM player's tables give the
// children.
struct JungleBeatState
{
    uint32_t piece = 0;
    uint32_t bits = 0;
    const char* suffix = "";
    uint16_t children = 0;
};

// The GameCube release's states, in the order of the pieces.
inline constexpr JungleBeatState kJungleBeatStates[] = {
    {0x01000003, Combo(1), "combo1", 0xb7fe}, // JA_BGM_MAIN_04
    {0x01000003, Combo(2), "combo2", 0xf6fe},
    {0x01000003, kHelibird, "helibird", 0x3ffe},
    {0x01000003, kHelibird | Combo(1), "helibird_combo1", 0xbffe},
    {0x01000003, kHelibird | Combo(2), "helibird_combo2", 0xfefe},
    {0x01000004, Combo(1), "combo1", 0x8ffe}, // JA_BGM_UG_01
    {0x01000004, Combo(2), "combo2", 0xfffe},
    {0x01000005, Combo(1), "combo1", 0x8ffe}, // JA_BGM_MAIN_01
    {0x01000005, Combo(2), "combo2", 0xfefe},
    {0x0100000e, TableState(1), "below_75", 0x01fe}, // JA_BGM_MATSURI_A
    {0x0100000f, TableState(1), "below_75", 0x001e}, // JA_BGM_MATSURI_B
    {0x0100000f, TableState(2), "round2", 0x003e},
    {0x0100000f, TableState(3), "round2_below_30", 0x005e},
    {0x0100000f, TableState(4), "change", 0x0004},
    {0x01000010, Combo(1), "combo1", 0xbffe}, // JA_BGM_MAIN_02
    {0x01000010, Combo(2), "combo2", 0xfefe},
    {0x01000013, TableState(1), "state1", 0xfffe},     // JA_BGM_BIND_01
    {0x01000016, TableState(1), "underwater", 0xfb08}, // JA_BGM_MAIN_05
    {0x01000017, Combo(1), "combo1", 0x8ffe},          // JA_BGM_MAIN_03
    {0x01000017, Combo(2), "combo2", 0xffde},
    {0x01000018, Combo(1), "combo1", 0x8ffe}, // JA_BGM_MAIN_06
    {0x01000018, Combo(2), "combo2", 0xbdfe},
    {0x01000019, Combo(1), "combo1", 0x8ffe}, // JA_BGM_MAIN_07
    {0x01000019, Combo(2), "combo2", 0xfdfe},
    {0x0100001a, Combo(1), "combo1", 0x9dc0}, // JA_BGM_BIRDRACE_01
    {0x0100001a, Combo(2), "combo2", 0xfdc0},
    {0x0100001a, kHelibird, "helibird", 0x1ffe},
    {0x0100001a, kHelibird | Combo(1), "helibird_combo1", 0x9ffe},
    {0x0100001a, kHelibird | Combo(2), "helibird_combo2", 0xfffe},
    {0x0100001d, Combo(1), "combo1", 0x9ffe}, // JA_BGM_BIRDRACE_02
    {0x0100001d, Combo(2), "combo2", 0xfffe},
    {0x0100001e, Combo(1), "combo1", 0x9ffe}, // JA_BGM_VOLCANO
    {0x0100001e, Combo(2), "combo2", 0xfffe},
    {0x01000020, Combo(1), "combo1", 0x9ffe}, // JA_BGM_MAIN_08
    {0x01000020, Combo(2), "combo2", 0xfffe},
    {0x01000021, Combo(1), "combo1", 0x9ffe}, // JA_BGM_MAIN_09
    {0x01000021, Combo(2), "combo2", 0xfffe},
    {0x01000022, Combo(1), "combo1", 0xbffe}, // JA_BGM_MAIN_10
    {0x01000022, Combo(2), "combo2", 0xfffe},
    {0x01000023, Combo(1), "combo1", 0x9ffe}, // JA_BGM_MAIN_11
    {0x01000023, Combo(2), "combo2", 0xfffe},
    {0x01000024, Combo(1), "combo1", 0x9ffe}, // JA_BGM_MAIN_12
    {0x01000024, Combo(2), "combo2", 0xfdfe},
    {0x01000025, Combo(1), "combo1", 0x9ffe}, // JA_BGM_MAIN_13
    {0x01000025, Combo(2), "combo2", 0xfffe},
    {0x01000027, Combo(1), "combo1", 0xbffe}, // JA_BGM_MAIN_15
    {0x01000027, Combo(2), "combo2", 0xfffe},
    {0x01000028, Combo(1), "combo1", 0x9ffe}, // JA_BGM_MAIN_16
    {0x01000028, Combo(2), "combo2", 0xfbfe},
    {0x01000029, Combo(1), "combo1", 0xbffe}, // JA_BGM_MAIN_17
    {0x01000029, Combo(2), "combo2", 0xfffe},
    {0x0100002a, Combo(1), "combo1", 0x9ffe}, // JA_BGM_MAIN_18
    {0x0100002a, Combo(2), "combo2", 0xfffe},
    {0x0100002f, Combo(1), "combo1", 0xbffe}, // JA_BGM_MAIN_19
    {0x0100002f, Combo(2), "combo2", 0xfffe},
    {0x01000030, Combo(1), "combo1", 0xfffe},        // JA_BGM_MAIN_20
    {0x01000031, TableState(1), "below_75", 0xfffe}, // JA_BGM_BOSS_KONFU_A
    {0x01000032, TableState(1), "below_75", 0xfe1e}, // JA_BGM_BOSS_KONFU_B
    {0x01000032, TableState(2), "round2", 0xfe7e},
    {0x01000032, TableState(3), "round2_below_30", 0xfffe},
    {0x01000032, TableState(4), "change", 0xe006},
    {0x01000033, TableState(1), "below_75", 0xfffe}, // JA_BGM_BOSS_MIYAMOTO_A
    {0x01000034, TableState(1), "below_75", 0xff9e}, // JA_BGM_BOSS_MIYAMOTO_B
    {0x01000034, TableState(2), "round2", 0xffbe},
    {0x01000034, TableState(3), "round2_below_30", 0xfffe},
    {0x01000034, TableState(4), "change", 0xff8c},
    {0x01000035, TableState(1), "below_75", 0xfffe}, // JA_BGM_BOSS_AKEBONO_A
    {0x01000036, TableState(1), "below_75", 0xfc3e}, // JA_BGM_BOSS_AKEBONO_B
    {0x01000036, TableState(2), "round2", 0xf8de},
    {0x01000036, TableState(3), "round2_below_30", 0xfb5e},
    {0x01000036, TableState(4), "change", 0xf80e},
    {0x01000039, Combo(1), "combo1", 0x9ffe}, // JA_BGM_SEARACE_01
    {0x01000039, Combo(2), "combo2", 0xfffe},
    {0x0100003a, Combo(1), "combo1", 0x9ffe}, // JA_BGM_SEARACE_02
    {0x0100003a, Combo(2), "combo2", 0xfbfe},
    {0x0100003f, Combo(1), "combo1", 0xbffe}, // JA_BGM_MAIN_21
    {0x0100003f, Combo(2), "combo2", 0xfffe},
    {0x01000041, Combo(1), "combo1", 0x9ffe}, // JA_BGM_NINRACE_01
    {0x01000041, Combo(2), "combo2", 0xfffe},
    {0x01000042, Combo(1), "combo1", 0x9ffe}, // JA_BGM_NINRACE_02
    {0x01000042, Combo(2), "combo2", 0xfffe},
    {0x01000044, TableState(2), "hit", 0xe7fe}, // JA_BGM_BOSS_GOROPY_B
    {0x01000044, TableState(3), "round2", 0x1ffe},
    {0x01000044, TableState(4), "round2_hit", 0xfffe},
    {0x01000044, TableState(5), "change", 0x0144},
    {0x01000046, TableState(2), "hit", 0xcffe}, // JA_BGM_BOSS_IUZOU_B
    {0x01000046, TableState(3), "round2", 0x3ffe},
    {0x01000046, TableState(4), "round2_hit", 0xfffe},
    {0x01000048, TableState(2), "hit", 0xc7fe}, // JA_BGM_BOSS_BIRD_B
    {0x01000048, TableState(3), "round2", 0x1ffe},
    {0x01000048, TableState(4), "round2_hit", 0xdffe},
    {0x01000048, TableState(5), "change", 0x0396},
    {0x01000049, TableState(2), "state2", 0x7ffe}, // JA_BGM_ENDING_CELEMONY
    {0x0100004a, kHelibird, "helibird", 0xa03e},   // JA_BGM_MAIN_22
    {0x0100004a, TableState(2), "state2", 0x9fc6},
    {0x0100004a, TableState(3), "fall", 0xc000},
    {0x0100004b, TableState(1), "hold", 0x1ffe}, // JA_BGM_BOSS_FINAL
    {0x0100004b, TableState(2), "below_60", 0x4ffe},
    {0x0100004b, TableState(3), "below_60_hold", 0x7ffe},
    {0x0100004b, TableState(4), "below_30", 0xcffe},
    {0x0100004b, TableState(5), "below_30_hold", 0xfffe},
};

// The Wii release's states: the GameCube release's, then JA_BGM_COURSE_SELECT's.
inline constexpr auto kJungleBeatWiiStates = []
{
    std::array<JungleBeatState, std::size(kJungleBeatStates) + 1> states{};
    std::ranges::copy(kJungleBeatStates, states.begin());
    states.back() = {0x0100002c, TableState(1), "state1", 0xfff2};
    return states;
}();

// A chord layer: the chord effects that an object of the game plays beside a stage's music while the object is near
// DK, such as a sleeping hog's. The effects follow the chords of the music and repeat until the object stops them. Most
// objects ask for their effect's record of the chord database once a frame (0x80256fcc in the GameCube release's
// main.dol, through 0x802301b4 and the helpers at 0x80230264 to 0x802302f4). Objects that share an effect start it
// once and stop it later, through the audio manager's list at +0x1040 (0x8025751c, from 0x802570f0): the coconut pigs'
// battle and the attack chance of a stunned enemy, which plays two records. A ripper gives a piece with a layer the
// piece's sound ID with kChordLayer and bits 0 to 6 of the layer's index in bits 16 to 22. Bit 15 holds bit 7 of the
// index. A piece's ID holds 0 there, and the bits of the other variants' states stay as they were. The driver starts
// the music of the stage's row and the effects.
//
// The layers come from the objects that each stage places (the objinfo of its world/*.arc) and the records that each
// object's class plays. The Wii release's main.dol names the classes of its nerves (RTTI). The calls of the helpers
// were attributed to the class whose nerves call them, or else to the class whose nerves' code comes just before them.
// A piece that several stages play gets each layer once, at the first stage by number that places such an object. The
// castle stages (0x43 and 0x44) play only DK_WAIT_ATTACK: startChordSe refuses the other loops there (0x8024c590).
// The driver's table has room for 256 layers.
struct JungleBeatChordLayer
{
    uint32_t piece = 0;  // the music of the stage's row
    uint32_t stage = 0;  // the stage's number, its row of the stage table
    uint32_t record = 0; // the record of the chord database
    uint32_t second = 0; // a second record that starts with the first, or 0
    bool started_once = false;
    const char* suffix = "";
};

inline constexpr uint32_t kChordLayer = 1u << 23;
inline constexpr uint32_t kChordLayerShift = 16;
inline constexpr uint32_t kChordLayerHigh = 1u << 15;
inline constexpr std::size_t kMaxChordLayers = 256;

// The sound ID of layer `index` of the profile's list, on `piece`: bits 0 to 6 of the index in bits 16 to 22, and bit 7
// in bit 15.
constexpr uint32_t ChordLayerId(uint32_t piece, uint32_t index)
{
    return piece | kChordLayer | (index & 0x7f) << kChordLayerShift | ((index & 0x80) != 0 ? kChordLayerHigh : 0);
}

// The words of the driver's table of chord layers: kMaxChordLayers words, 0 past the list's end. A word holds the
// record in bits 0 to 7, the second record in bits 8 to 15, the stage in bits 16 to 23, and bit 24 for effects that
// the objects start once.
inline void AppendChordLayers(std::span<const JungleBeatChordLayer> layers, std::vector<uint32_t>& words)
{
    for (std::size_t i = 0; i < kMaxChordLayers; i++)
    {
        const JungleBeatChordLayer* layer = i < layers.size() ? &layers[i] : nullptr;
        words.push_back(
            layer ? (layer->started_once ? 1u << 24 : 0) | layer->stage << 16 | layer->second << 8 | layer->record : 0);
    }
}

// The GameCube release's chord layers, in the order of the pieces. The comment gives the stage and the objects there
// that play the layer.
inline constexpr JungleBeatChordLayer kJungleBeatChordLayers[] = {
    {0x01000003, 0x2d, 135, 0, false, "enemy_angry"},          // JA_BGM_MAIN_04 at BigGodTree: PigPoppoKiba
    {0x01000003, 0x2d, 136, 0, false, "dk_wait_attack"},       // JA_BGM_MAIN_04 at BigGodTree: PigPoppoKiba
    {0x01000003, 0x2d, 138, 0, false, "dk_has_wait"},          // JA_BGM_MAIN_04 at BigGodTree: MiniIga
    {0x01000003, 0x2d, 143, 0, false, "dk_has_weight_damage"}, // JA_BGM_MAIN_04 at BigGodTree: MiniIga
    {0x01000004, 0x4c, 135, 0, false, "enemy_angry"},          // JA_BGM_UG_01 at LavaValley: NoMouth, RollingFrog
    {0x01000004, 0x4c, 136, 0, false, "dk_wait_attack"},       // JA_BGM_UG_01 at LavaValley: FireBakky, RollingFrog
    {0x01000004, 0x4c, 138, 0, false, "dk_has_wait"},          // JA_BGM_UG_01 at LavaValley: FireMiniIga
    {0x01000004, 0x4c, 143, 0, false, "dk_has_weight_damage"}, // JA_BGM_UG_01 at LavaValley: FireMiniIga
    {0x01000004, 0x4c, 157, 158, true, "attack_chance"},       // JA_BGM_UG_01 at LavaValley: RollingFrog
    {0x01000004, 0x4c, 46, 0, false, "only_head_move"},        // JA_BGM_UG_01 at LavaValley: OnlyHead
    {0x01000005, 0x1e, 134, 0, false, "enemy_sleeping"},       // JA_BGM_MAIN_01 at MorningField: PigPoppoSleepWithHint
    // JA_BGM_MAIN_01 at MorningField: MiniPigPoppo, PigPoppoSleepWithHint
    {0x01000005, 0x1e, 136, 0, false, "dk_wait_attack"},
    {0x01000005, 0x1e, 132, 0, false, "lect_kikki_run"},       // JA_BGM_MAIN_01 at MorningField: LectureKikki
    {0x01000010, 0x30, 135, 0, false, "enemy_angry"},          // JA_BGM_MAIN_02 at SteepValley: Garigari
    {0x01000010, 0x30, 136, 0, false, "dk_wait_attack"},       // JA_BGM_MAIN_02 at SteepValley: Bakky, PigPoppo
    {0x01000010, 0x30, 140, 0, false, "enemy_wait_pig"},       // JA_BGM_MAIN_02 at SteepValley: PigPoppo
    {0x01000010, 0x30, 141, 0, false, "enemy_blow_wind"},      // JA_BGM_MAIN_02 at SteepValley: PigPoppo
    {0x01000010, 0x30, 157, 158, true, "attack_chance"},       // JA_BGM_MAIN_02 at SteepValley: Garigari, PigPoppo
    {0x01000010, 0x30, 34, 0, false, "garigari_rush"},         // JA_BGM_MAIN_02 at SteepValley: Garigari
    {0x01000010, 0x30, 37, 0, false, "garigari_spin"},         // JA_BGM_MAIN_02 at SteepValley: Garigari
    {0x01000010, 0x30, 44, 0, false, "garigari_ball_roll"},    // JA_BGM_MAIN_02 at SteepValley: Garigari
    {0x01000016, 0x2c, 136, 0, false, "dk_wait_attack"},       // JA_BGM_MAIN_05 at TropicalCoast: NeedlePoppo
    {0x01000016, 0x2c, 157, 158, true, "attack_chance"},       // JA_BGM_MAIN_05 at TropicalCoast: NeedlePoppo
    {0x01000016, 0x2c, 159, 0, false, "enemy_battle"},         // JA_BGM_MAIN_05 at TropicalCoast: NeedlePoppo
    {0x01000017, 0x1f, 136, 0, false, "dk_wait_attack"},       // JA_BGM_MAIN_03 at DeepJungle: PigPoppo
    {0x01000017, 0x1f, 138, 0, false, "dk_has_wait"},          // JA_BGM_MAIN_03 at DeepJungle: MiniIga
    {0x01000017, 0x1f, 140, 0, false, "enemy_wait_pig"},       // JA_BGM_MAIN_03 at DeepJungle: PigPoppo
    {0x01000017, 0x1f, 141, 0, false, "enemy_blow_wind"},      // JA_BGM_MAIN_03 at DeepJungle: PigPoppo
    {0x01000017, 0x1f, 143, 0, false, "dk_has_weight_damage"}, // JA_BGM_MAIN_03 at DeepJungle: MiniIga
    {0x01000017, 0x1f, 157, 158, true, "attack_chance"},       // JA_BGM_MAIN_03 at DeepJungle: PigPoppo
    {0x01000017, 0x1f, 132, 0, false, "lect_kikki_run"},       // JA_BGM_MAIN_03 at DeepJungle: LectureKikki
    {0x01000018, 0x22, 134, 0, false, "enemy_sleeping"},       // JA_BGM_MAIN_06 at SkyGarden01: KobuPakkun
    {0x01000018, 0x22, 135, 0, false, "enemy_angry"},          // JA_BGM_MAIN_06 at SkyGarden01: KobuPakkun
    {0x01000018, 0x22, 136, 0, false, "dk_wait_attack"},      // JA_BGM_MAIN_06 at SkyGarden01: KobuPakkun, MiniPigPoppo
    {0x01000018, 0x24, 141, 0, false, "enemy_blow_wind"},     // JA_BGM_MAIN_06 at TheNestOfHeliBird: BigBee
    {0x01000018, 0x22, 157, 158, true, "attack_chance"},      // JA_BGM_MAIN_06 at SkyGarden01: KobuPakkun
    {0x01000018, 0x24, 159, 0, false, "enemy_battle"},        // JA_BGM_MAIN_06 at TheNestOfHeliBird: BigBee
    {0x01000018, 0x22, 132, 0, false, "lect_kikki_run"},      // JA_BGM_MAIN_06 at SkyGarden01: LectureKikki
    {0x01000019, 0x27, 136, 0, false, "dk_wait_attack"},      // JA_BGM_MAIN_07 at GoUpTheWaterFall: PigPoppo
    {0x01000019, 0x27, 140, 0, false, "enemy_wait_pig"},      // JA_BGM_MAIN_07 at GoUpTheWaterFall: PigPoppo
    {0x01000019, 0x27, 141, 0, false, "enemy_blow_wind"},     // JA_BGM_MAIN_07 at GoUpTheWaterFall: PigPoppo
    {0x01000019, 0x27, 157, 158, true, "attack_chance"},      // JA_BGM_MAIN_07 at GoUpTheWaterFall: PigPoppo
    {0x01000019, 0x27, 47, 0, false, "bubble_flower_rotate"}, // JA_BGM_MAIN_07 at GoUpTheWaterFall: BubbleFlower
    {0x0100001e, 0x28, 135, 0, false, "enemy_angry"},         // JA_BGM_VOLCANO at VolcanoBottom: NoMouth
    {0x0100001e, 0x28, 136, 0, false, "dk_wait_attack"},      // JA_BGM_VOLCANO at VolcanoBottom: FireBakky
    {0x0100001e, 0x28, 138, 0, false, "dk_has_wait"},         // JA_BGM_VOLCANO at VolcanoBottom: FireMiniIga
    {0x0100001e, 0x28, 143, 0, false, "dk_has_weight_damage"}, // JA_BGM_VOLCANO at VolcanoBottom: FireMiniIga
    {0x0100001e, 0x28, 46, 0, false, "only_head_move"},        // JA_BGM_VOLCANO at VolcanoBottom: OnlyHead
    {0x01000020, 0x33, 135, 0, false, "enemy_angry"},          // JA_BGM_MAIN_08 at EscapeFromRelic: RollingFrog
    {0x01000020, 0x33, 136, 0, false, "dk_wait_attack"},       // JA_BGM_MAIN_08 at EscapeFromRelic: Bakky, RollingFrog
    {0x01000020, 0x33, 157, 158, true, "attack_chance"},       // JA_BGM_MAIN_08 at EscapeFromRelic: RollingFrog
    {0x01000020, 0x33, 46, 0, false, "only_head_move"},        // JA_BGM_MAIN_08 at EscapeFromRelic: OnlyHead
    {0x01000020, 0x33, 132, 0, false, "lect_kikki_run"},       // JA_BGM_MAIN_08 at EscapeFromRelic: LectureKikki
    {0x01000021, 0x32, 138, 0, false, "dk_has_wait"},          // JA_BGM_MAIN_09 at KikkiFestival: MiniIga
    {0x01000021, 0x32, 143, 0, false, "dk_has_weight_damage"}, // JA_BGM_MAIN_09 at KikkiFestival: MiniIga
    {0x01000021, 0x32, 132, 0, false, "lect_kikki_run"},       // JA_BGM_MAIN_09 at KikkiFestival: LectureKikki
    {0x01000022, 0x48, 134, 0, false, "enemy_sleeping"},       // JA_BGM_MAIN_10 at FactoryRelic: PigPoppoSleep
    {0x01000022, 0x48, 135, 0, false, "enemy_angry"}, // JA_BGM_MAIN_10 at FactoryRelic: BigBee, Garigari, NoMouth
    // JA_BGM_MAIN_10 at FactoryRelic: BigBee, MiniPigPoppo, PigPoppoSleep
    {0x01000022, 0x48, 136, 0, false, "dk_wait_attack"},
    {0x01000022, 0x48, 138, 0, false, "dk_has_wait"},          // JA_BGM_MAIN_10 at FactoryRelic: FireMiniIga
    {0x01000022, 0x52, 140, 0, false, "enemy_wait_pig"},       // JA_BGM_MAIN_10 at UnderGroundMaze: PigPoppo
    {0x01000022, 0x48, 141, 0, false, "enemy_blow_wind"},      // JA_BGM_MAIN_10 at FactoryRelic: BigBee
    {0x01000022, 0x48, 143, 0, false, "dk_has_weight_damage"}, // JA_BGM_MAIN_10 at FactoryRelic: FireMiniIga
    {0x01000022, 0x48, 157, 158, true, "attack_chance"},       // JA_BGM_MAIN_10 at FactoryRelic: BigBee, Garigari
    {0x01000022, 0x48, 159, 0, false, "enemy_battle"},         // JA_BGM_MAIN_10 at FactoryRelic: BigBee
    {0x01000022, 0x48, 34, 0, false, "garigari_rush"},         // JA_BGM_MAIN_10 at FactoryRelic: Garigari
    {0x01000022, 0x48, 37, 0, false, "garigari_spin"},         // JA_BGM_MAIN_10 at FactoryRelic: Garigari
    {0x01000022, 0x48, 44, 0, false, "garigari_ball_roll"},    // JA_BGM_MAIN_10 at FactoryRelic: Garigari
    {0x01000022, 0x48, 46, 0, false, "only_head_move"},        // JA_BGM_MAIN_10 at FactoryRelic: OnlyHead
    {0x01000023, 0x3b, 135, 0, false, "enemy_angry"},          // JA_BGM_MAIN_11 at DesertRelic: KobuPakkunAngry
    // JA_BGM_MAIN_11 at DesertRelic: Bakky, DanSpider, KobuPakkunAngry, MiniPigPoppo, PigPoppo
    {0x01000023, 0x3b, 136, 0, false, "dk_wait_attack"},
    {0x01000023, 0x3b, 140, 0, false, "enemy_wait_pig"},  // JA_BGM_MAIN_11 at DesertRelic: PigPoppo
    {0x01000023, 0x3b, 141, 0, false, "enemy_blow_wind"}, // JA_BGM_MAIN_11 at DesertRelic: PigPoppo
    {0x01000023, 0x3b, 157, 158, true, "attack_chance"},  // JA_BGM_MAIN_11 at DesertRelic: KobuPakkunAngry, PigPoppo
    {0x01000023, 0x3b, 159, 0, true, "enemy_battle"},     // JA_BGM_MAIN_11 at DesertRelic: CocoPig
    {0x01000024, 0x2f, 134, 0, false, "enemy_sleeping"},  // JA_BGM_MAIN_12 at IceCave: KobuPakkun
    {0x01000024, 0x2f, 135, 0, false, "enemy_angry"},     // JA_BGM_MAIN_12 at IceCave: KobuPakkun
    {0x01000024, 0x2f, 136, 0, false, "dk_wait_attack"},  // JA_BGM_MAIN_12 at IceCave: IceBakky, KobuPakkun
    {0x01000024, 0x2f, 138, 0, false, "dk_has_wait"},     // JA_BGM_MAIN_12 at IceCave: IceMiniIga
    {0x01000024, 0x36, 140, 0, false, "enemy_wait_pig"},  // JA_BGM_MAIN_12 at IceTunnel: PigPoppo
    {0x01000024, 0x36, 141, 0, false, "enemy_blow_wind"}, // JA_BGM_MAIN_12 at IceTunnel: PigPoppo
    {0x01000024, 0x2f, 143, 0, false, "dk_has_weight_damage"}, // JA_BGM_MAIN_12 at IceCave: IceMiniIga
    {0x01000024, 0x2f, 157, 158, true, "attack_chance"},       // JA_BGM_MAIN_12 at IceCave: KobuPakkun
    {0x01000024, 0x36, 159, 0, true, "enemy_battle"},          // JA_BGM_MAIN_12 at IceTunnel: CocoPigSnow
    {0x01000024, 0x2f, 46, 0, false, "only_head_move"},        // JA_BGM_MAIN_12 at IceCave: OnlyHead
    {0x01000025, 0x4f, 134, 0, false, "enemy_sleeping"},       // JA_BGM_MAIN_13 at InfernalZoo: PigPoppoSleep
    // JA_BGM_MAIN_13 at InfernalZoo: Garigari, KobuPakkunAngry, NoMouth, RollingFrog
    {0x01000025, 0x4f, 135, 0, false, "enemy_angry"},
    // JA_BGM_MAIN_13 at InfernalZoo: FireBakky, KobuPakkunAngry, MiniPigPoppo, PigPoppo, PigPoppoSleep, RollingFrog
    {0x01000025, 0x4f, 136, 0, false, "dk_wait_attack"},
    {0x01000025, 0x4f, 138, 0, false, "dk_has_wait"},          // JA_BGM_MAIN_13 at InfernalZoo: FireMiniIga
    {0x01000025, 0x4f, 140, 0, false, "enemy_wait_pig"},       // JA_BGM_MAIN_13 at InfernalZoo: PigPoppo
    {0x01000025, 0x4f, 141, 0, false, "enemy_blow_wind"},      // JA_BGM_MAIN_13 at InfernalZoo: PigPoppo
    {0x01000025, 0x4f, 143, 0, false, "dk_has_weight_damage"}, // JA_BGM_MAIN_13 at InfernalZoo: FireMiniIga
    // JA_BGM_MAIN_13 at InfernalZoo: Garigari, KobuPakkunAngry, PigPoppo, RollingFrog
    {0x01000025, 0x4f, 157, 158, true, "attack_chance"},
    {0x01000025, 0x4f, 159, 0, true, "enemy_battle"},          // JA_BGM_MAIN_13 at InfernalZoo: CocoPig
    {0x01000025, 0x4f, 34, 0, false, "garigari_rush"},         // JA_BGM_MAIN_13 at InfernalZoo: Garigari
    {0x01000025, 0x4f, 37, 0, false, "garigari_spin"},         // JA_BGM_MAIN_13 at InfernalZoo: Garigari
    {0x01000025, 0x4f, 44, 0, false, "garigari_ball_roll"},    // JA_BGM_MAIN_13 at InfernalZoo: Garigari
    {0x01000026, 0x41, 135, 0, false, "enemy_angry"},          // JA_BGM_MAIN_14 at Iceberg: NoFin, RollingFrog
    {0x01000026, 0x41, 136, 0, false, "dk_wait_attack"},       // JA_BGM_MAIN_14 at Iceberg: NeedlePoppo, RollingFrog
    {0x01000026, 0x41, 157, 158, true, "attack_chance"},       // JA_BGM_MAIN_14 at Iceberg: NeedlePoppo, RollingFrog
    {0x01000026, 0x41, 159, 0, false, "enemy_battle"},         // JA_BGM_MAIN_14 at Iceberg: NeedlePoppo
    {0x01000027, 0x3e, 136, 0, false, "dk_wait_attack"},       // JA_BGM_MAIN_15 at CloudHeaven: MiniPigPoppo
    {0x01000027, 0x3e, 138, 0, false, "dk_has_wait"},          // JA_BGM_MAIN_15 at CloudHeaven: MiniIga
    {0x01000027, 0x3e, 143, 0, false, "dk_has_weight_damage"}, // JA_BGM_MAIN_15 at CloudHeaven: MiniIga
    {0x01000028, 0x4e, 138, 0, false, "dk_has_wait"},          // JA_BGM_MAIN_16 at AboveTheClouds: MiniIga
    {0x01000028, 0x4e, 143, 0, false, "dk_has_weight_damage"}, // JA_BGM_MAIN_16 at AboveTheClouds: MiniIga
    {0x01000028, 0x4e, 46, 0, false, "only_head_move"},        // JA_BGM_MAIN_16 at AboveTheClouds: OnlyHead
    {0x01000028, 0x4e, 47, 0, false, "bubble_flower_rotate"},  // JA_BGM_MAIN_16 at AboveTheClouds: BubbleFlower
    {0x01000029, 0x51, 135, 0, false, "enemy_angry"},          // JA_BGM_MAIN_17 at ClockTower: BigBee
    {0x01000029, 0x51, 136, 0, false, "dk_wait_attack"},       // JA_BGM_MAIN_17 at ClockTower: BigBee
    {0x01000029, 0x51, 141, 0, false, "enemy_blow_wind"},      // JA_BGM_MAIN_17 at ClockTower: BigBee
    {0x01000029, 0x51, 157, 158, true, "attack_chance"},       // JA_BGM_MAIN_17 at ClockTower: BigBee
    {0x01000029, 0x51, 159, 0, false, "enemy_battle"},         // JA_BGM_MAIN_17 at ClockTower: BigBee
    {0x0100002a, 0x25, 136, 0, false, "dk_wait_attack"},       // JA_BGM_MAIN_18 at FairyWood: MiniPigPoppo, PigPoppo
    {0x0100002a, 0x25, 140, 0, false, "enemy_wait_pig"},       // JA_BGM_MAIN_18 at FairyWood: PigPoppo
    {0x0100002a, 0x25, 141, 0, false, "enemy_blow_wind"},      // JA_BGM_MAIN_18 at FairyWood: PigPoppo
    {0x0100002a, 0x25, 157, 158, true, "attack_chance"},       // JA_BGM_MAIN_18 at FairyWood: PigPoppo
    {0x0100002a, 0x25, 159, 0, true, "enemy_battle"},          // JA_BGM_MAIN_18 at FairyWood: CocoPig
    {0x0100002a, 0x25, 132, 0, false, "lect_kikki_run"},       // JA_BGM_MAIN_18 at FairyWood: LectureKikki
    // JA_BGM_MAIN_19 at RoadToCastle: DanSpider, NinKnifeMonkey, PigPoppoFire, PigPoppoKiba, SlumpBirdMini
    {0x0100002f, 0x43, 136, 0, false, "dk_wait_attack"},
    // JA_BGM_MAIN_20 at InTheCastle: NeedlePoppo, NinKnifeMonkey, PigPoppoFire, SlumpBirdMini, SlumpBirdMiniOnRail
    {0x01000030, 0x44, 136, 0, false, "dk_wait_attack"},
    {0x0100003f, 0x49, 135, 0, false, "enemy_angry"}, // JA_BGM_MAIN_21 at DemonFireMarsh: PigPoppoFire
    // JA_BGM_MAIN_21 at DemonFireMarsh: PigPoppoFire, SlumpBirdMini
    {0x0100003f, 0x49, 136, 0, false, "dk_wait_attack"},
    {0x0100003f, 0x49, 138, 0, false, "dk_has_wait"},          // JA_BGM_MAIN_21 at DemonFireMarsh: IceMiniIga
    {0x0100003f, 0x49, 141, 0, false, "enemy_blow_wind"},      // JA_BGM_MAIN_21 at DemonFireMarsh: PigPoppoFire
    {0x0100003f, 0x49, 143, 0, false, "dk_has_weight_damage"}, // JA_BGM_MAIN_21 at DemonFireMarsh: IceMiniIga
    {0x0100004a, 0x58, 135, 0, false, "enemy_angry"},          // JA_BGM_MAIN_22 at VsFinalKong2: PigPoppoKiba
    {0x0100004a, 0x56, 136, 0, false, "dk_wait_attack"},       // JA_BGM_MAIN_22 at VsFinalKong: NinKnifeMonkey
    // The opening ceremony's layer comes last. The other layers keep the indices that they had in a table of 128.
    {0x01000006, 0x57, 132, 0, false, "lect_kikki_run"}, // JA_BGM_OP_CEMEMONY at OpeningCeremony: LectureKikki
};

static_assert(std::size(kJungleBeatChordLayers) <= kMaxChordLayers);

// The Wii release's chord layers. Its stage table gives the same stages the same rows and music, and its startChordSe
// refuses the same records at the castle stages (0x80273940). Its stages place other objects, but the objects that play
// these effects stay, with one exception: KikkiFestival (0x32) places no MiniIga. The Wii release's main.dol starts a
// record through 0x8027c050 and keeps it playing through 0x8027c0b0.
inline constexpr auto kJungleBeatWiiChordLayers = []
{
    std::array<JungleBeatChordLayer, std::size(kJungleBeatChordLayers) - 2> layers{};
    std::size_t n = 0;
    for (const JungleBeatChordLayer& layer : kJungleBeatChordLayers)
    {
        if (layer.stage != 0x32 || (layer.record != 138 && layer.record != 143))
        {
            layers[n++] = layer;
        }
    }
    return layers;
}();

// The chord file of a piece of music (chord_files.h): its name in JaiChord.arc. The game takes it from a table by the
// low 16 bits of the piece's sound ID (0x80256008, table 0x80351db0, 83 entries; in the Wii release 0x8027bab0 and
// 0x803ab600). The table gives the file's ID in the archive, and the two releases' archives keep the files in different
// orders. The IDs name the same files. A piece without a chord file starts no chords (0x8024a808).
struct JungleBeatChordFile
{
    uint32_t piece = 0;
    const char* file = "";
};

inline constexpr JungleBeatChordFile kJungleBeatChordFiles[] = {
    {0x01000003, "chord_s_main04.cit"},     {0x01000004, "chord_ug1.cit"},
    {0x01000005, "chord_s_main01.cit"},     {0x01000006, "chord_scene00.cit"},
    {0x01000008, "chord_bind00.cit"},       {0x0100000a, "chord_s_escape.cit"},
    {0x0100000b, "chord_goal01_loop.cit"},  {0x01000010, "chord_s_main02.cit"},
    {0x01000013, "chord_bind01.cit"},       {0x01000014, "chord_bind03.cit"},
    {0x01000015, "chord_bind04.cit"},       {0x01000016, "chord_s_main05.cit"},
    {0x01000017, "chord_s_main03.cit"},     {0x01000018, "chord_s_main06.cit"},
    {0x01000019, "chord_s_main07.cit"},     {0x0100001a, "chord_s_birdrace01.cit"},
    {0x0100001d, "chord_s_birdrace02.cit"}, {0x0100001e, "chord_s_volcano.cit"},
    {0x0100001f, "chord_bind05.cit"},       {0x01000020, "chord_s_main08.cit"},
    {0x01000021, "chord_s_main09.cit"},     {0x01000022, "chord_s_main10.cit"},
    {0x01000023, "chord_s_main11.cit"},     {0x01000024, "chord_s_main12.cit"},
    {0x01000025, "chord_s_main13.cit"},     {0x01000026, "chord_s_main14.cit"},
    {0x01000027, "chord_s_main15.cit"},     {0x01000028, "chord_s_main16.cit"},
    {0x01000029, "chord_s_main17.cit"},     {0x0100002a, "chord_s_main18.cit"},
    {0x0100002b, "chord_goal01_loop.cit"},  {0x0100002f, "chord_s_main19.cit"},
    {0x01000030, "chord_s_main20.cit"},     {0x01000031, "chord_boss_confu.cit"},
    {0x01000032, "chord_boss_confu.cit"},   {0x01000036, "chord_boss_akebo.cit"},
    {0x01000037, "chord_s_cameo.cit"},      {0x01000038, "chord_s_escape.cit"},
    {0x01000039, "chord_s_searace01.cit"},  {0x0100003a, "chord_s_searace02.cit"},
    {0x0100003f, "chord_s_main21.cit"},     {0x01000041, "chord_s_ninrace01.cit"},
    {0x01000042, "chord_s_ninrace02.cit"},  {0x01000043, "chord_boss_goropi.cit"},
    {0x01000044, "chord_boss_goropi.cit"},  {0x01000045, "chord_boss_iuzo.cit"},
    {0x01000046, "chord_boss_iuzo.cit"},    {0x01000047, "chord_boss_bird.cit"},
    {0x01000048, "chord_boss_bird.cit"},    {0x01000049, "chord_scene03.cit"},
    {0x0100004a, "chord_s_main22.cit"},     {0x0100004b, "chord_boss_final.cit"},
    {0x0100004c, "chord_bind01b.cit"},
};

// The name of the chord file of `piece`, or nullptr for a piece without one.
constexpr const char* JungleBeatChordFileOf(uint32_t piece)
{
    for (const JungleBeatChordFile& entry : kJungleBeatChordFiles)
    {
        if (entry.piece == piece)
        {
            return entry.file;
        }
    }
    return nullptr;
}

} // namespace ntdlib::jaudio
