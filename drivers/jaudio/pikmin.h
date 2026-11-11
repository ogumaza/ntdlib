// SPDX-License-Identifier: MIT

/*
 * What a game-mode driver of Pikmin reads of the sound ID that it plays, and of the game's sound library, Jac. The
 * GameCube release and the Wii release (New Play Control!) share them. The ripper's side of the effects' IDs is in
 * ntdlib/jaudio/jac_effects.h.
 */

#ifndef NTDLIB_DRIVERS_JAUDIO_PIKMIN_H
#define NTDLIB_DRIVERS_JAUDIO_PIKMIN_H

/* The fields of a piece's sound ID: 0x80000000 | part << 8 | piece. The piece field holds the piece's number in the
   sequence index. Part 0 plays the piece in the game's slot for music. Any other part goes to the piece that the sound
   library plays from its start-up. */
#define SOUND_PIECE(id) ((id) & 0xffu)
#define SOUND_PART(id) (((id) >> 8) & 0xffu)

/* An effect's ID: 0x80000000 | kind << 24 | number << 8 | sequence. The sequence field holds 0 or 1, the effects'
   sequence or the events'. Pieces start at FIRST_PIECE. The kind gives the game's function that starts the effect. An
   event's number holds the event's type and the action (EVENT_TYPE). */
#define FIRST_PIECE 2u
#define EFFECT_KIND(id) (((id) >> 24) & 0x7fu)
#define EFFECT_NUMBER(id) (((id) >> 8) & 0xffffu)
#define EFFECT_SYSTEM 1u
#define EFFECT_PLAYER 2u
#define EFFECT_EVENT 3u
#define EFFECT_FORMATION 4u
#define EVENT_TYPE(number) ((number) >> 12)
#define EVENT_ACTION(number) ((number) & 0xfffu)

/* The DVD task's passes for a piece's files and for the effects' wave groups. The game's scene set-up makes them the
   same way. */
#define PASS_OF_PIECE 0x10000u
#define PASS_OF_EFFECTS 0x20000u

/* The wave system of the effects. The game's set-up of its first scene of play (Jac_SceneSetup, 0x80019be0 in the
   GameCube release's main.dol, from 0x8001d5a0 in the Wii release's) loads groups 1 to 3 of that system without making
   them the system's scene. It makes group 0 of systems 14, 15 and 2 their systems' scenes. The Wii release's set-up
   also loads group 0 of wave system 22, which the bank file lacks. */
#define EFFECTS_SYSTEM 0u

/* The game's scene of a stage's play, and the system's effect that closes the menu. Jac_PlaySystemSe plays that effect
   only while the menu's flag is set. The effect that opens the menu sets it. */
#define SCENE_OF_PLAY 5u
#define EFFECT_CLOSES_MENU 7u

/* The port that a cutscene writes its part to, and the word of a track that holds the track's first child. The root
   track opens the track that plays the part as its first child. */
#define PART_PORT 0u
#define TRACK_FIRST_CHILD 0x44u

/* A track's state, a byte: 0 while the track is free. A track has 16 children, in words from TRACK_FIRST_CHILD. */
#define TRACK_STATE 0x3cu
#define TRACK_FREE 0u
#define TRACK_CHILDREN 16u

#endif
