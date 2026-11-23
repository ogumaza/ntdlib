// SPDX-License-Identifier: MIT

/*
 * What the game-mode drivers of the GameCube and the Wii share: the C types, the values of a parameter block's status
 * word, the frames that a driver waits for the game, the most wave groups that a block lists, the types of the game's
 * functions that a driver calls, and the helpers that read the game's memory. A driver includes this file before the
 * other headers here. The magic and the version of a parameter block belong to its format, and the driver defines
 * them.
 *
 * A step that fails stops the driver with STATUS_ERROR and the step's number in the status word. Step 1 is the
 * driver's check of its block.
 */

#ifndef NTDLIB_DRIVERS_GUEST_H
#define NTDLIB_DRIVERS_GUEST_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef short s16;
typedef int s32;

/* Values of the status word. They're macros because an enumerator can't hold 0x80000000 before C23. */
#define STATUS_BOOTING 0u
#define STATUS_PLAYING 1u
#define STATUS_FINISHED 2u
#define STATUS_ERROR 0x80000000u /* | step number */

/* The frames that a driver waits for the game before it gives up on a step. */
#define SETUP_FRAMES 600u

/* The most wave groups that a block's wave list names (ntdlib/flipper/wave_list.h). */
#define MAX_WAVES 64u

/* The game's functions, by their number of arguments. A driver passes each argument and takes each result as a u32. */
typedef u32 (*fn0)(void);
typedef u32 (*fn1)(u32);
typedef u32 (*fn2)(u32, u32);
typedef u32 (*fn3)(u32, u32, u32);
typedef u32 (*fn4)(u32, u32, u32, u32);
typedef u32 (*fn5)(u32, u32, u32, u32, u32);
typedef u32 (*fn6)(u32, u32, u32, u32, u32, u32);
typedef u32 (*fn7)(u32, u32, u32, u32, u32, u32, u32);
typedef u32 (*fn9)(u32, u32, u32, u32, u32, u32, u32, u32, u32);

/* The word and the byte at `address`. */
static inline u32 word(u32 address)
{
    return *(volatile u32*)address;
}

static inline u8 byte(u32 address)
{
    return *(volatile u8*)address;
}

/* The big-endian halfword and word at `p`. */
static inline u32 be16(const u8* p)
{
    return (u32)p[0] << 8 | p[1];
}

static inline u32 be32(const u8* p)
{
    return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

/* A virtual function of `object`. The vtable is the object's first word. */
static inline u32 virtual_function(u32 object, u32 slot)
{
    return word(word(object) + slot);
}

#endif
