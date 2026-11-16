// SPDX-License-Identifier: MIT

// The states of Skyward Sword HD's music that the game's code sets through the sound players of its sound archive.
// Every address is in the game's `main`, title 01002DA013484000 version 0.
//
// Underwater, the game moves the low-pass and the send to aux bus B of its two music players (0xf7c5b0). It moves each
// over 15 frames, through controls that write the SoundPlayer's low-pass and FX sends (see SoundPlayerSettings). It
// moves them back to 0 when the camera leaves the water (0xf76a94). A sound that plays in the state takes the values
// that the move ends at.

#pragma once

#include "ntdlib/rvl/snd/effect_setup.h"

namespace ntdlib::rvl::nx
{

// The music players: the archive's players 0 and 1 (PL_BGM and PL_BGM_BATTLE). The underwater state reaches the sounds
// of the players below this number.
constexpr int kMusicPlayers = 2;

// The settings of the music players underwater: a low-pass of -0.55 (0xbf0ccccd at 0xf7c5f4 and 0xf7c7f8) and a send of
// 0.2 to bus B (0x3e4ccccd at 0xf7c6f0 and 0xf7c8f4).
constexpr snd::SoundPlayerSettings kUnderwaterMusic = {.lpf = -0.55f, .fx_sends = {0.0f, 0.2f, 0.0f}};

} // namespace ntdlib::rvl::nx
