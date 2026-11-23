// SPDX-License-Identifier: MIT

// The states in which Super Mario Galaxy and Super Mario Galaxy 2 play a piece of music once the piece has started. The
// game's BGM manager gives a piece a state through AudBgm::changeTrackMuteState. The state sets the children of the
// piece's track 0 from the piece's row of the state table. Some of the game's code also sets the piece's tempo, the
// pitch of its children or the volume of one child with the state. sequence_export/jaudio/game.h makes each state an
// arrangement of its piece.
//
// The addresses are in the main.dol of each game's USA release. AudSingleBgm::start (0x8002e694 in Galaxy, 0x80080fb0
// in Galaxy 2) gives a piece state 0. MR::setStageBGMState (0x803f97cc, 0x8001b1f0) gives the stage's music another
// state through the BGM's changeTrackMuteState (AudSingleBgm's at 0x8002e91c and 0x800814b0, AudMultiBgm's at
// 0x8002f0a8 and 0x80081e30). AudBgmSetting gives a piece the index of its rows: Galaxy's table at 0x8052e4c8 by the
// piece's place in its group, Galaxy 2's at 0x80645da0 by its ID. Galaxy's rows at 0x8052e810 hold 8 states each, and
// Galaxy 2's at 0x80645f8c hold 11. A state is 8 bytes of nibbles, a nibble for each child of track 0 from child 0 in
// the high nibble, then two volumes of the stream that the piece plays with. A nibble's low three bits pass to
// AudTrackController::setMuteState: 0 fades the child out, 1 fades it in, 2 mutes it, 3 unmutes it, and 7 leaves it.
// Bit 3 mutes the child once its volume reaches 0. A multi BGM (MBGM_*) plays a BGM_* sequence over a stream, and the
// sequence takes the states of its BGM_* row. Many states change only the stream's volumes.
//
// The table below lists each state whose part of the sequence sounds different from the piece and from every earlier
// state of the table: other children with notes, or another tempo, pitch or balance between the children. A state plays
// its children from the piece's start: the state of the start, then this state. The game fades them in over up to 180
// frames. A state that changes only the stream is left out. So is a state that changes the volume of the whole part
// alone, such as the Blimp Fruit's state 10 of a sequence whose only child with notes is child 15, and a state whose
// children play no note that sounds. BGM_GALAXY_01's state 3 plays one note at velocity 1, and state 4 of
// STM_SMG2_GALAXY_27's sequence plays child 1. That child holds no note. State 9 (the Dash Pepper) of
// BGM_SMG2_GALAXY_08, _20 and _HURRY adds children 5 and 6. Those children hold no notes. State 9 plays what state 8
// plays. A state's suffix names what sets it in the game's code. `stateN` is state N of a piece whose state comes from
// a stage's data (ChangeBgmCube, RestartCube, EarthenPipe and BgmChangeArea) or from code that wasn't found. A suffix
// starts with "_layer" when the piece plays over a stream: the state's arrangement plays the sequence's layer without
// the stream.

#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace ntdlib::jaudio
{

// A state of a piece of the Galaxy games: the piece's JAudio sound ID, the suffix of the state's name, the state's
// number in the piece's row, and the children of track 0 that play after the start's state and this one (bit n for
// child n). `tempo` is the tempo ratio that the game's code gives the piece's tempo manager with the state. `pitch` is
// the pitch ratio that it gives the children in `pitched_children`. `quiet_child` is a child that plays at
// `quiet_volume`, or -1 for none.
struct GalaxyState
{
    uint32_t sound = 0;
    std::string_view suffix;
    int32_t state = 0;
    uint16_t children = 0;
    float tempo = 1.0f;
    float pitch = 1.0f;
    uint16_t pitched_children = 0;
    int32_t quiet_child = -1;
    float quiet_volume = 1.0f;
};

// Super Mario Galaxy's states.
//
// - The Star Ball (AudTamakoroBgmCtrl::control, 0x801563a4) sets a state once the ball has rolled for 107 frames
//   (sFirstBarSteps, 0x8069d15c). The ball's speed, clamped to 2 to 22, picks it: under 7 state 1 (sStateNumLow), under
//   12 state 2 (sStateNumMid), and otherwise state 3 (sStateNumHigh). The tempo ratio goes from 0.55 to 1.22 with the
//   speed (modifyTempo, 0x80156534). Each state here plays at the middle of its band of speeds: 4.5, 9.5 and 17. While
//   the ball is out of the player's control, the state is 4 (sStateNumVeryHigh), the tempo ratio 1.478 and the pitch
//   ratio 1.189 on every child but child 14, the percussion (modifyPitch, 0x801565c8). The constants are at 0x8069d138
//   to 0x8069d170.
// - Bowser's fights (KoopaFunction): changeBgmStateNormal (0x8005fa2c) sets state 1, changeBgmStateEscape (0x8005fa38)
//   state 2, changeBgmStateDamageTail (0x8005fa44) state 3 and changeBgmStateFlyAway (0x8005fa50) state 4.
// - The galaxy map's sphere selector (SphereSelectorHandle) sets state 4 while the pointer rotates the map
//   (cBgmRotateState, 0x8069d318) and state 6 as the player confirms a galaxy (cBgmConfirmState, 0x8069d328). A nibble
//   of 7 leaves a child as the state before left it. State 6 leaves children 1 to 3 and plays 4 and 5. The variant
//   plays 4 and 5 alone, the state that the game settles in. setStateConfirmStartAtFirstStep (0x8019d944) zeroes the
//   rotation speeds (resetRotateParam, 0x8019d470) before it gives state 6. Within a frame, changeBgmRotateState
//   (0x8019d7c0, called each frame by control at 0x8019d100) gives state 3, whose row fades children 1 to 3 out. The
//   game plays children 1 to 5 together for at most 30 frames.
// - The file select (FileSelector::updateBgm, 0x8017c700) sets state 6 while the camera is at or moves to its near
//   point (sBgmNearState, 0x8069d230), and state 5 at the far point.
// - The conductor (AudBgmConductor::movement, 0x80155f94) sets BGM_GALAXY_03's and BGM_GALAXY_04's states from two
//   bits. MarioSwim::jet (0x802ff7dc) sets bit 4 while Mario jets through the water. AudCameraWatcher::playAtmosphereSE
//   (0x80154b4c) sets bit 2 while the camera is under water. Bit 4 alone gives state 4, bit 2 alone state 5, and both
//   state 6. States 5 and 6 change only the stream's volumes.
// - The runaway rabbits' chase (RunawayRabbitCollect::exeActive, 0x80286764) gives BGM_GALAXY_24 state 3 at 0x80286924
//   while a rabbit runs (`rabbit_chase`). The number is the word at 0x8069d7dc. The chase starts in state 2
//   (0x8069d7d8), a catch gives state 2 (0x8069d7e0) and the end state 1 (0x8069d7e4).
inline constexpr std::array kGalaxyStates{
    GalaxyState{0x0100000f, "_layer_swim_jet", 4, 0x0002},
    GalaxyState{0x01000025, "_layer_rotate", 4, 0x000e},
    GalaxyState{0x01000025, "_layer_confirm", 6, 0x0030},
    GalaxyState{0x01000038, "_layer_near", 6, 0x0002},
    GalaxyState{0x0100003b, "_layer_rotate", 4, 0x000e},
    GalaxyState{0x0100003b, "_layer_confirm", 6, 0x0030},
    GalaxyState{0x0100003c, "_layer_escape", 2, 0x0008},
    GalaxyState{0x0100003c, "_layer_damage_tail", 3, 0x003e},
    GalaxyState{0x0100003c, "_layer_fly_away", 4, 0x00c8},
    GalaxyState{0x0100004b, "_speed_low", 1, 0x1800, 0.63375f},
    GalaxyState{0x0100004b, "_speed_mid", 2, 0x07fe, 0.80125f},
    GalaxyState{0x0100004b, "_speed_high", 3, 0x47fe, 1.0525f},
    GalaxyState{0x0100004b, "_speed_very_high", 4, 0x47fe, 1.478f, 1.189f, 0xbfff},
    GalaxyState{0x0100004f, "_layer_rabbit_chase", 3, 0x0002},
    GalaxyState{0x01000060, "_layer_rotate", 4, 0x000e},
    GalaxyState{0x01000060, "_layer_confirm", 6, 0x0030},
    GalaxyState{0x01000063, "_speed_low", 1, 0x1800, 0.63375f},
    GalaxyState{0x01000063, "_speed_mid", 2, 0x07fe, 0.80125f},
    GalaxyState{0x01000063, "_speed_high", 3, 0x47fe, 1.0525f},
    GalaxyState{0x01000063, "_speed_very_high", 4, 0x47fe, 1.478f, 1.189f, 0xbfff},
};

// Super Mario Galaxy 2's states.
//
// - The Star Ball (AudTamakoroBgmCtrl::control, 0x802224e0) works as in Galaxy, with the same speeds and state numbers
//   (0x807cf9c8 to 0x807cf9e4). AudTamakoroBgmCtrl::init (0x802223d0) takes each piece's rates from the table at
//   0x80648978 (2 rows of 0x34 bytes): the first bar's frames, the tempo in BPM and the tempos at the slowest, the
//   fastest and the out-of-control speeds, the pitch in semitones out of control, and two children that keep their
//   pitch. BGM_TAMAKORO's tempos are 134, 73.7, 163.48 and 198.052 BPM, its pitch 2.997 semitones (2^(2.997/12) =
//   1.189), and child 14 keeps its pitch. BGM_TAMAKORO_2's tempos are 84, 80, 92 and 112 BPM, its pitch 4.997
//   semitones, and children 14 and 15 keep their pitch. Each state here plays at the middle of its band of speeds, as
//   in Galaxy. BGM_TAMAKORO_2's state 2 plays the children of state 0 at 1.006 times its tempo. The piece's file is
//   already a tempo of that band, and the table leaves the state out.
// - The conductor (AudBgmConductor::movement, 0x80220d80) picks a state for the stage's music by its sound ID from
//   bits that the game's code sets (MR::setStageBGMStateBit, 0x8001b250). For the pieces below, 0x80220b80 gives state
//   8 for bit 0x08, state 9 for bit 0x10 and state 10 for bit 0x200. With state 10, MR's function at 0x8001b2a0 moves
//   child 15 to a volume of 0.4 through the BGM's track controller (AudFader::set, 0x80084540). One of Yoshi's nerves
//   (its execute at 0x80413c30) sets bit 0x08 while Mario rides Yoshi (`yoshi`). Yoshi::control (0x8040f770, through
//   0x8040ca10) sets bit 0x10 while Yoshi runs on a Dash Pepper (`dash_pepper`). MarioYoshiBalloon::update (0x8041454c,
//   through 0x80414730) sets bit 0x200 while Yoshi floats on a Blimp Fruit (`blimp_fruit`). The pieces:
//   BGM_REPEAT_TIMER, BGM_GAMBLE, BGM_PINCH_01 and _02, BGM_SMG2_EV_PORTER, and the BGM_* sequences of these streams:
//   STM_SMG2_GALAXY_08, _10, _11, _20, _22, _32, _HURRY, GALAXY31, EV_MOKKU, MARIO_SHIP01 to 03 and STM_SMG_GALAXY_14
//   (AudioRes/Info/MultiBgmInfo.arc pairs each stream with its sequence). For STM_SMG2_GALAXY_22, the conductor first
//   compares the stage's name with the string at 0x806941d0 (0x80221160). On that stage it picks states 1 and 2 by bit
//   0x40 instead.
// - BGM_DASH_YOSHI (0x80221060): bit 0x10 gives state 9 with a tempo of 174 BPM over the piece's 154 (0x807dc210 and
//   0x807dc214), bit 0x08 state 8 at 154. The tempo goes through the BGM's virtual at +0x2c (0x80081280). That function
//   sets the tempo manager's ratio.
// - BGM_SMG2_BOSS_04A (0x802210e4): bit 0x20 gives state 2, at a tempo ratio of 1. MR::adjustSandyBossFireMorphBGM
//   (0x8001c2c0) sets the bit while Mario has the fire form in the fight (`fire`).
// - STM_SMG2_GALAXY_27's sequence (0x80220fdc): MarioSwim's code at 0x80402cd0 sets bit 0x04 while Mario jets through
//   the water, and AudCameraWatcher::playAtmosphereSE (0x80221750) sets bit 0x02 while the camera is under water. Bit
//   0x04 alone gives state 4, whose child holds no note. Bit 0x02 gives states 5 and 6. They play what states 0 and 4
//   play. Without either bit, 0x10 gives state 9 and 0x08 state 8. Those two play the same children.
// - BGM_TITLE's state 6 comes from code that wasn't found.
inline constexpr std::array kGalaxy2States{
    GalaxyState{0x01000008, "_yoshi", 8, 0x85fe},
    GalaxyState{0x01000008, "_blimp_fruit", 10, 0x85fe, 1.0f, 1.0f, 0, 15, 0.4f},
    GalaxyState{0x01000009, "_speed_low", 1, 0x0800, 0.63375f},
    GalaxyState{0x01000009, "_speed_mid", 2, 0x07fe, 0.80125f},
    GalaxyState{0x01000009, "_speed_high", 3, 0x47fe, 1.0525f},
    GalaxyState{0x01000009, "_speed_very_high", 4, 0x47fe, 1.478f, 1.189f, 0xbfff},
    GalaxyState{0x0100000a, "_speed_low", 1, 0x3800, 0.97023809f},
    GalaxyState{0x0100000a, "_speed_high", 3, 0xc73e, 1.05952382f},
    GalaxyState{0x0100000a, "_speed_very_high", 4, 0xc73e, 1.33333337f, 1.33460736f, 0x3fff},
    GalaxyState{0x01000017, "_yoshi", 8, 0x819e},
    GalaxyState{0x01000017, "_dash_pepper", 9, 0x81fe, 1.12987f},
    GalaxyState{0x0100001e, "_fire", 2, 0xfcfe},
    GalaxyState{0x01000023, "_layer_yoshi", 8, 0x8190},
    GalaxyState{0x01000025, "_layer_yoshi", 8, 0x8000},
    GalaxyState{0x01000029, "_layer_yoshi", 8, 0x8190},
    GalaxyState{0x0100002c, "_yoshi", 8, 0x9ffe},
    GalaxyState{0x0100002c, "_blimp_fruit", 10, 0x9ffe, 1.0f, 1.0f, 0, 15, 0.4f},
    GalaxyState{0x0100002d, "_yoshi", 8, 0x9c86},
    GalaxyState{0x0100002d, "_blimp_fruit", 10, 0x9c86, 1.0f, 1.0f, 0, 15, 0.4f},
    GalaxyState{0x01000035, "_layer_yoshi", 8, 0x8000},
    GalaxyState{0x01000039, "_yoshi", 8, 0x803e},
    GalaxyState{0x01000039, "_blimp_fruit", 10, 0x803e, 1.0f, 1.0f, 0, 15, 0.4f},
    GalaxyState{0x0100003a, "_layer_yoshi", 8, 0x8190},
    GalaxyState{0x01000045, "_layer_state6", 6, 0x0002},
    GalaxyState{0x01000047, "_layer_yoshi", 8, 0x8000},
    GalaxyState{0x01000058, "_layer_yoshi", 8, 0x8000},
    GalaxyState{0x0100005a, "_layer_yoshi", 8, 0x8000},
    GalaxyState{0x01000065, "_layer_yoshi", 8, 0x8000},
    GalaxyState{0x01000070, "_yoshi", 8, 0x803e},
    GalaxyState{0x01000070, "_blimp_fruit", 10, 0x803e, 1.0f, 1.0f, 0, 15, 0.4f},
    GalaxyState{0x01000072, "_layer_yoshi", 8, 0x8000},
    GalaxyState{0x01000073, "_layer_yoshi", 8, 0x8000},
    GalaxyState{0x01000076, "_layer_yoshi", 8, 0x8000},
    GalaxyState{0x01000077, "_layer_yoshi", 8, 0x8000},
    GalaxyState{0x01000078, "_layer_yoshi", 8, 0x8000},
};

} // namespace ntdlib::jaudio
