// SPDX-License-Identifier: MIT

/*
 * What a game-mode driver of Donkey Kong Jungle Beat reads of the game's objects, and the states of a variant of a
 * piece of music in its sound ID. The GameCube release and the Wii release (New Play Control!) share them. The ripper's
 * side of the states is in ntdlib/jaudio/jungle_beat_states.h.
 */

#ifndef NTDLIB_DRIVERS_JAUDIO_JUNGLE_BEAT_H
#define NTDLIB_DRIVERS_JAUDIO_JUNGLE_BEAT_H

/* A wave archive's state once it is in ARAM. */
#define WAVES_LOADED 2u

/* JKRDvdRipper's options: decode Yaz0, and allocate from a heap's head or tail. */
#define EXPAND 1u
#define FROM_HEAD 1u
#define FROM_TAIL 2u

/* The tracks of a BGM that its states set. */
#define MUSIC_TRACKS 16u

/* The chord effects. The chord database's table of records holds their count, then the records. A record holds its
   entry, then its effect's sound ID. The audio manager's chord effects hold the database at +8. The database holds the
   table at +0xc. The chord effects hold their players from +0xc. A player's byte at +0x18 is set from its start until
   the game stops it. Its byte at +0x19 is set while a step of its effect waits for a beat. Its sound object at +0x34
   plays the steps. The object holds its JAISoundHandles at +0 and their number at +4. */
#define NO_RECORD 0xffffffffu
#define RECORD_SIZE 16u
#define CHORD_DATABASE 0x8u
#define RECORD_TABLE 0xcu
#define CHORD_PLAYER_LIST 0xcu
#define CHORD_PLAYERS 64u
#define PLAYER_BUSY 0x18u
#define PLAYER_WAITING 0x19u
#define PLAYER_SOUND_OBJECT 0x34u

/* The stage object holds the stage's number at +4 and its row of the stage table at +0xc. */
#define STAGE_NUMBER 0x4u
#define STAGE_ROW 0xcu

/* The states of a variant of a piece of music, in bits 16 to 23 of its sound ID. Those bits are 0 in the ID of a piece.
   Bits 16 and 17 set the layer of a combo (1 to 3), bits 18 to 20 a state of the BGM player's second table (1 to 7),
   and bit 21 a flight with a Helibird. */
#define STATE_BITS 0xff0000u
#define COMBO_SHIFT 16u
#define COMBO_MASK 3u
#define TABLE_STATE_SHIFT 18u
#define TABLE_STATE_MASK 7u
#define HELIBIRD 0x200000u

/* The player's flags that a Helibird sets while it carries DK: bit 2 on and bit 0 off (0x801ed0a4 in the GameCube
   release's main.dol). Bit 1 follows another flag of DK. The states of the pieces that a flight changes leave the
   tracks that bit 1 picks as they are. */
#define HELIBIRD_FLAGS 4u

#endif
