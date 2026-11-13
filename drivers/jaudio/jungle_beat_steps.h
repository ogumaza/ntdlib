// SPDX-License-Identifier: MIT

/*
 * The step of a game-mode driver of Donkey Kong Jungle Beat that the GameCube release and the Wii release (New Play
 * Control!) share: the states of a variant of a piece of music (see jungle_beat.h).
 *
 * A driver includes this file after the definitions that the step uses:
 *   - `driver_params`, the parameter block. Its fields hold the game's functions and offsets that the step names;
 *   - u32 and s32, and fn2 and fn3, the types of the game's functions with two and three arguments;
 *   - word(address), which reads the game's memory, and fail(step), which stops the driver.
 */

#ifndef NTDLIB_DRIVERS_JAUDIO_JUNGLE_BEAT_STEPS_H
#define NTDLIB_DRIVERS_JAUDIO_JUNGLE_BEAT_STEPS_H

/* Sets the states of the variant `id` (STATE_BITS) on the music that `player` plays, at once. `audio` is the game's
   audio object that holds the stage object. The game's code sets the states during play, through the same functions.
   A Helibird sets the player's flags while it carries DK. The flags pick a state of the first table. A combo's count
   picks a state of the first table by the counts of the stage. The driver uses the count at which the combo reaches
   the layer in `combo_stage`. A state of the second table comes from the code of a boss, a stage or a scene. The game
   sets each state with a fade. The driver sets each one at once. Stops the driver at step 7 if the stage has no
   count for the layer. */
static void jungle_beat_play_states(u32 audio, u32 player, u32 id)
{
    volatile struct Params* p = &driver_params;
    if (id & HELIBIRD)
    {
        *(volatile u32*)(player + p->flags_offset) = HELIBIRD_FLAGS;
        ((fn2)p->flags_state)(player, 1);
    }
    const u32 layer = (id >> COMBO_SHIFT) & COMBO_MASK;
    if (layer)
    {
        const u32 stage = word(audio + p->stage_offset);
        if (!stage)
        {
            fail(7);
        }
        *(volatile u32*)(stage + STAGE_NUMBER) = p->combo_stage;
        const s32 count = (s32)((fn2)p->combo_threshold)(stage, layer);
        if (count <= 0)
        {
            fail(7);
        }
        ((fn3)p->combo_state)(player, (u32)count, 1);
    }
    const u32 state = (id >> TABLE_STATE_SHIFT) & TABLE_STATE_MASK;
    if (state)
    {
        ((fn3)p->set_music_state)(player, state, 1);
    }
}

#endif
