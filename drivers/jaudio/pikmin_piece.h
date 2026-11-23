// SPDX-License-Identifier: MIT

/*
 * The watch of a piece's tracks that a game-mode driver of Pikmin makes, and that the GameCube release and the Wii
 * release (New Play Control!) share (see pikmin.h): the flag of the disc's pass and the piece's track, the end of the
 * sound, the wait for ever, an event's place at the camera and the search for the track that plays.
 *
 * A driver includes this file after pikmin.h. The watch uses its TRACK_STATE, TRACK_FREE, TRACK_CHILDREN and
 * TRACK_FIRST_CHILD. The driver defines the rest of what the watch uses first:
 *   - `driver_params` and its `struct Params`, with `status` and `sound`;
 *   - u32, STATUS_PLAYING and STATUS_FINISHED;
 *   - word(address) and byte(address), which read the game's memory, and wait_frame(), one frame of the game.
 */

#ifndef NTDLIB_DRIVERS_JAUDIO_PIKMIN_PIECE_H
#define NTDLIB_DRIVERS_JAUDIO_PIKMIN_PIECE_H

/* The flag that the disc task's pass sets once the disc has read everything before it, and the sound's track: the
   piece's root track, or the track that plays the part. The length analysis reads that track through `sound`. */
static volatile u32 piece_read;
static volatile u32 piece_root;

/* The end of the sound: once its track is free again. */
static void check_end(volatile struct Params* p)
{
    if (p->status == STATUS_PLAYING && byte(piece_root + TRACK_STATE) == TRACK_FREE)
    {
        p->sound = 0;
        p->status = STATUS_FINISHED;
    }
}

/* The wait for ever. */
__attribute__((noreturn)) static void watch_piece(volatile struct Params* p)
{
    for (;;)
    {
        check_end(p);
        wait_frame();
    }
}

/* An event's place against the camera, as Jac_CreateEvent takes it: three floats. An event at the camera gets the full
   volume and the pan of the middle. */
static const u32 at_camera[3];

/* The first child of the `count` tracks at `parents` that plays, or 0. */
static u32 playing_child(const u32* parents, u32 count)
{
    for (u32 i = 0; i < count; i++)
    {
        for (u32 n = 0; n < TRACK_CHILDREN; n++)
        {
            const u32 child = word(parents[i] + TRACK_FIRST_CHILD + 4 * n);
            if (child && byte(child + TRACK_STATE) != TRACK_FREE)
            {
                return child;
            }
        }
    }

    return 0;
}

#endif
