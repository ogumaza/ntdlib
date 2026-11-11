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

} // namespace ntdlib::jaudio
