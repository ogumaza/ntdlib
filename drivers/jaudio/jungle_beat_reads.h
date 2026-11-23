// SPDX-License-Identifier: MIT

/*
 * The reads of the game's wave archives and chord effects that a game-mode driver of Donkey Kong Jungle Beat makes,
 * and that the GameCube release and the Wii release (New Play Control!) share (see jungle_beat.h).
 *
 * A driver includes this file after jungle_beat.h. The reads use its WAVES_LOADED, CHORD_PLAYER_LIST, CHORD_PLAYERS,
 * PLAYER_BUSY, PLAYER_WAITING and PLAYER_SOUND_OBJECT. The driver defines the rest of what the reads use first:
 *   - `struct Params` and `driver_params`, the parameter block. Its fields `section_heap`, `wave_banks_offset`,
 *     `wave_count`, `waves` and `wave_arc_state_offset` give the wave archives;
 *   - u32 and s32, and fn1 and fn2, the types of the game's functions with one and two arguments;
 *   - MAX_WAVES, the most wave groups that the block lists;
 *   - word(address) and byte(address), which read the game's memory, and virtual_function(object, slot).
 */

#ifndef NTDLIB_DRIVERS_JAUDIO_JUNGLE_BEAT_READS_H
#define NTDLIB_DRIVERS_JAUDIO_JUNGLE_BEAT_READS_H

/* A wave archive of the listed `wave`: (wave bank << 16) | group. A bank's vtable gives its archive at slot 0x10 and
   the number of its archives at slot 0x14. Returns 0 if the bank or the group doesn't exist. */
static u32 wave_arc(u32 wave)
{
    volatile struct Params* p = &driver_params;
    const u32 bank = word(word(p->section_heap) + p->wave_banks_offset + (wave >> 16) * 4);
    if (!bank || (wave & 0xffff) >= ((fn1)virtual_function(bank, 0x14))(bank))
    {
        return 0;
    }

    return ((fn2)virtual_function(bank, 0x10))(bank, wave & 0xffff);
}

/* Whether every listed wave archive is in ARAM. An archive without a file on the disc (entry number below 0) is never
   loaded. */
static int waves_loaded(void)
{
    volatile struct Params* p = &driver_params;
    for (u32 i = 0; i < p->wave_count && i < MAX_WAVES; i++)
    {
        const u32 arc = wave_arc(p->waves[i]);
        if (arc && (s32)word(arc + p->wave_arc_state_offset + 4) >= 0 &&
            word(arc + p->wave_arc_state_offset) != WAVES_LOADED)
        {
            return 0;
        }
    }

    return 1;
}

/* The first busy player of the chord effects, or 0. */
static u32 busy_player(u32 chords)
{
    const u32 players = word(chords + CHORD_PLAYER_LIST);
    for (u32 i = 0; i < CHORD_PLAYERS; i++)
    {
        const u32 player = word(players + i * 4);
        if (player && (byte(player + PLAYER_BUSY) || byte(player + PLAYER_WAITING)))
        {
            return player;
        }
    }

    return 0;
}

/* The JAISound of the step that a chord effect's `player` plays, or 0 between steps. */
static u32 step_sound(u32 player)
{
    const u32 object = word(player + PLAYER_SOUND_OBJECT);
    const u32 handles = word(object);
    for (u32 i = 0; i < word(object + 4); i++)
    {
        const u32 sound = word(handles + i * 4);
        if (sound)
        {
            return sound;
        }
    }

    return 0;
}

#endif
