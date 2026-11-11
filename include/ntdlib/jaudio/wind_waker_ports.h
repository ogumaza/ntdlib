// SPDX-License-Identifier: MIT

// The sounds of The Legend of Zelda: The Wind Waker that read a port that the game's code sets, and the values that a
// rip plays them with. The GameCube release and The Wind Waker HD on the Wii U share them.

#pragma once

#include <cstdint>
#include <vector>

namespace ntdlib::jaudio
{

// A sound that reads a port that the game's code sets when it starts the sound, and the values that the game puts
// there for it. Each value plays a variant of the sound. A piece of music can read a port that the game writes while
// the piece plays. A ripper's driver writes the value once the piece plays.
struct PortEffect
{
    uint32_t id = 0; // the sound's ID
    int port = 0;
    std::vector<int16_t> values;
};

// The Wind Waker's sounds that read a port.
// - The effects. An ID such as 0x2803 is the effect with index 3 in category 2 (se2-0003). Port 6 holds the low 16
//   bits of the map information that the game passes with an effect. For a step, a jump, a landing or a climb, that's
//   the material of the ground in the game's collision data, 1 to 27. For a hit, it's the material of the surface, or a
//   code from 32 up for the kind of thing hit. An effect plays nothing for 0. Some values play the same variant. A list
//   holds the lowest value that plays each variant. The game also gives every effect port 9 after the start: the reverb
//   of the room that the effect plays in. JA_SE_ST_WALK (0x3832), a Stalfos's step, plays nothing while port 9 holds 0.
//   Its list holds the reverbs of the rooms that place a Stalfos. Four more effects read port 9 and play nothing with
//   any value.
// - The music. Port 9 holds the status that the game's actors give a piece while it plays. changeBgmStatus writes it
//   on the main music (0x802a5a40 in the GameCube release's main.dol, 0x02027fd4 in HD's executable), and
//   changeSubBgmStatus on the music over the main music (0x802a5a78, HD 0x02027ff0). onEnemyDamage (0x802a57a4)
//   writes 1 to port 9 of JA_BGM_SEA_ENEMY, and to port 11 of JA_BGM_BATTLE_NORM, when Link hits an enemy. A list
//   holds the values that change what the piece plays. In HD, changeBgmStatus gets 1, 2 and 9 (0x021554ac,
//   0x0215beac, 0x02255dd0 and 0x02255df0), and changeSubBgmStatus 2 to 8 and 10 (0x02136258 to 0x02136efc). The
//   GameCube release's actors are in the game's modules (RELs), which weren't read. The GameCube release's sequences
//   of these six pieces, and of mboss and mboss_s, are HD's, byte for byte.
std::vector<PortEffect> WindWakerPortEffects();

} // namespace ntdlib::jaudio
