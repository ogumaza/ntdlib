// SPDX-License-Identifier: MIT

/*
 * The steps of a game-mode driver of Pikmin 2 that the GameCube release and the Wii release (New Play Control!) share:
 * an effect's random draws, the sound heap, the sound system, the game's objects that the sequences ask about, the
 * piece's object and scene, and the sound's start and frames. The releases differ in the steps between them: the
 * machine, the root heaps, the start-up archive's paths, the global scene's wave groups and the scene's wave groups. A
 * step that fails stops the driver with the step's number in the status word.
 *
 * Each step is inlined where the driver calls it. The driver's code is then the same as with the step written out in
 * the driver's main function.
 *
 * A driver includes this file after the definitions that the steps use:
 *   - `driver_params`, the parameter block. Its fields hold the header (sound_id, sound and status), the sizes, the
 *     game's functions and variables that the steps name, `layer` (pikmin2.h's SoundLayer), get_random and big_boss;
 *   - u8, u16 and u32, fn0 to fn7 (the types of the game's functions by their number of arguments), STATUS_PLAYING and
 *     STATUS_FINISHED;
 *   - word(address) and byte(address), which read the game's memory;
 *   - wait_frame(), one frame of the game, and fail(step), which stops the driver;
 *   - call_virtual(object, vtable, slot, argument), which calls a virtual function of the game;
 *   - load_scene(scene), which loads a scene's resources and returns 0 if they take too long;
 *   - play_data, rappas and RAPPA_WAIT: the game's play data and the captains' marching horns, as the release lays
 *     them out;
 *   - stand_in_boss: a zeroed object that stands in for a boss and for the final boss's sound object. A boss's music
 *     marks the boss with the byte at +0xff as it jumps into an attack. Without a boss, the game stops with an
 *     assertion, and the attack plays nothing.
 */

#ifndef NTDLIB_DRIVERS_JAUDIO_PIKMIN2_STEPS_H
#define NTDLIB_DRIVERS_JAUDIO_PIKMIN2_STEPS_H

/* Before an effect, the driver draws the sound library's random numbers that the effect's value names, and takes them
   out of the sound ID. A sequence's command draws a random number from the sound library's generator. From the state at
   the game's start, the first draw ends some effects at once. In the game, every earlier draw moves the generator on.
   The draws come before the set-up. The sound then starts at the same time as without them. */
__attribute__((always_inline)) static inline void pikmin2_draw_randoms(void)
{
    volatile struct Params* p = &driver_params;
    const u32 draws = p->sound_id >> DRAWS_SHIFT & DRAWS_MASK;
    for (u32 i = 0; i < draws; i++)
    {
        ((fn0)p->get_random)();
    }
    p->sound_id &= ~(DRAWS_MASK << DRAWS_SHIFT);
}

/* After JFWSystem::init's heaps and the ARAM manager: the main thread's JKRThread. System::initialize then makes the
   game's root heap, `root`, current again. The game's system object makes a heap for the sound system in it, which
   becomes the current heap. Returns that heap. */
__attribute__((always_inline)) static inline u32 pikmin2_sound_heap(u32 root)
{
    volatile struct Params* p = &driver_params;
    const u32 thread = ((fn1)p->alloc)(p->thread_size);
    if (thread)
    {
        ((fn3)p->thread_ctor)(thread, ((fn0)p->get_current_thread)(), 4);
    }
    ((fn1)p->become_current_heap)(root);
    const u32 free = ((fn1)p->get_free_size)(root);
    if (free <= p->heap_margin)
    {
        fail(2);
    }
    const u32 sound = ((fn3)p->create_exp_heap)(free - p->heap_margin, root, 1);
    if (!sound)
    {
        fail(2);
    }
    ((fn1)p->become_current_heap)(sound);
    return sound;
}

/* System::createSoundSystem once the start-up archive, `archive`, is in the heap `files`: the factory. The factory
   makes the sound system in the sound heap, `sound`. Then the global scene, in a solid heap. The start-up archive's
   heap goes. Returns the scene manager, and the sound system in *sys_if. */
__attribute__((always_inline)) static inline u32 pikmin2_sound_system(u32 archive, u32 sound, u32 files, u32* sys_if)
{
    volatile struct Params* p = &driver_params;
    const u32 factory = ((fn1)p->alloc)(p->factory_size);
    if (!factory)
    {
        fail(3);
    }
    ((fn1)p->layer.factory_ctor)(factory);
    *(volatile u32*)(factory + FACTORY_VTABLE) = p->layer.factory_vtable;
    *(volatile u32*)(factory + FACTORY_MAKE_SE) = p->layer.make_se;
    *(volatile u32*)(factory + FACTORY_HEAP) = sound;
    *(volatile u32*)(factory + FACTORY_HEAP_SIZE) = p->sound_memory;
    *(volatile u32*)(factory + FACTORY_ARCHIVE) = archive;
    ((fn1)p->layer.new_sound_system)(factory);
    const u32 manager = word(p->layer.scene_manager);
    const u32 system = word(p->layer.system_interface);
    if (!manager || !system)
    {
        fail(3);
    }
    const u32 solid = ((fn3)p->create_solid_heap)(((fn1)p->get_free_size)(sound), sound, 1);
    ((fn1)p->become_current_heap)(solid);
    call_virtual(manager, 0, MANAGER_NEW_GLOBAL, 0);
    ((fn1)p->adjust_size)(solid);
    ((fn1)p->become_current_heap)(sound);
    ((fn1)p->destroy_heap)(files);
    *sys_if = system;
    return manager;
}

/* The game's objects that the sequences ask about: the play data at the story's start and the captains' marching
   horns, each at the start of its tune with the shortest wait after a note. */
__attribute__((always_inline)) static inline void pikmin2_game_objects(void)
{
    volatile struct Params* p = &driver_params;
    *(volatile u32*)p->layer.play_data = (u32)play_data;
    for (u32 i = 0; i < 2; i++)
    {
        *(volatile u16*)((u32)rappas[i] + RAPPA_WAIT) = (u16)RAPPA_SHORTEST_WAIT;
        *(volatile u32*)(p->layer.rappas + 4 * i) = (u32)rappas[i];
    }
}

/* Makes the piece's object in the current heap, `heap`, the way the game makes it. Returns 0 if the game made none. */
__attribute__((always_inline)) static inline u32 pikmin2_make_piece(volatile struct Piece* piece, u32 manager, u32 info,
                                                                    u32 heap)
{
    volatile struct Params* p = &driver_params;
    const u32 sequence = (u32)piece->sequence;
    const u32 sound_info = (u32)piece->info;
    u32 seq = 0;
    switch (piece->kind & 0xff)
    {
    case PIECE_PLAIN:
        seq = ((fn3)p->layer.new_bgm_seq)(manager, sequence, sound_info);
        break;
    case PIECE_MAIN:
        seq = ((fn3)p->layer.new_main_bgm)(manager, sequence, sound_info);
        break;
    case PIECE_DIRECTED:
        seq = ((fn3)p->layer.new_directed_bgm)(manager, sequence, sound_info);
        break;
    case PIECE_BIG_BOSS:
    case PIECE_BIG_BOSS_NEAR:
        /* The game makes the final boss's music only while the boss's sound object exists (initBossBgm: 0x8033606c in
           the GameCube release's main.dol, 0x803b5740 in the Wii release's). The music's directors then take tracks 14
           and 15 in place of 10 and 11 (0x80470240 and 0x804f5d24). */
        *(volatile u32*)p->big_boss = (u32)stand_in_boss;
        seq = ((fn3)p->layer.new_directed_bgm)(manager, sequence, sound_info);
        break;
    case PIECE_AUTO:
        /* newAndSetCurrentScene gives the conductors the current heap first. */
        *(volatile u32*)p->layer.conductor_heap = heap;
        seq = ((fn7)p->layer.new_auto_bgm)(manager, (u32)piece->conductor, sequence, sound_info,
                                           byte(manager + MANAGER_ACCESS_MODE), info, 0);
        break;
    case PIECE_AUTO_FREE:
        /* newAutoBgm would look up the piece's directors in a track map, and the game has none for these pieces. */
        *(volatile u32*)p->layer.conductor_heap = heap;
        seq = ((fn1)p->alloc)(p->auto_bgm_size);
        if (!seq)
        {
            return 0;
        }
        ((fn6)p->layer.auto_bgm_ctor)(seq, (u32)piece->conductor, sequence, sound_info,
                                      byte(manager + MANAGER_ACCESS_MODE), 0);
        call_virtual(seq, SEQ_VTABLE, SEQ_INIT, 0);
        break;
    case PIECE_BATTLE:
    case PIECE_FREE:
        {
            u32 directors = 0;
            if ((piece->kind & 0xff) == PIECE_BATTLE)
            {
                directors = ((fn1)p->alloc)(p->battle_directors_size);
                if (!directors)
                {
                    return 0;
                }
                ((fn1)p->layer.battle_directors_ctor)(directors);
            }
            seq = ((fn1)p->alloc)(p->directed_bgm_size);
            if (!seq)
            {
                return 0;
            }
            ((fn4)p->layer.directed_bgm_ctor)(seq, sequence, sound_info, directors);
            call_virtual(seq, SEQ_VTABLE, SEQ_INIT, 0);
            if (directors)
            {
                ((fn2)p->layer.adapt_directors)(directors, seq);
            }
            break;
        }
    }

    /* The game slows the swing of some pieces' root tracks. */
    const u32 beat = piece->kind >> 16;
    if (seq && beat)
    {
        *(volatile u16*)(word(seq + BGM_ROOT_TRACK) + ROOT_BEAT_INTERVAL) = (u16)beat;
    }
    return seq;
}

/* A piece's scene, with a scene information of the profile's type, and the piece's object in it. The scene loads the
   wave groups `areas`, one in each of its two areas. The global scene, `global`, takes it as a child. The scene's
   resources load, then the piece starts. The game starts a boss's music when the boss appears, at the part of the music
   that the boss gives. The scene starts the other pieces. Returns the piece's object. */
__attribute__((always_inline)) static inline u32 pikmin2_start_piece(volatile struct Piece* piece, u32 manager,
                                                                     u32 global, u32 sound, const u32* areas)
{
    volatile struct Params* p = &driver_params;
    const u32 info = ((fn1)p->alloc)(p->scene_info_size);
    if (!info)
    {
        fail(5);
    }
    ((fn1)p->layer.scene_info_ctor)(info);
    *(volatile u8*)(info + INFO_SCENE_TYPE) = (u8)p->scene_type;
    const u32 scene = ((fn3)p->layer.new_game_scene)(manager, areas[0], info);
    if (!scene)
    {
        fail(5);
    }
    const u32 seq = pikmin2_make_piece(piece, manager, info, sound);
    if (!seq)
    {
        fail(5);
    }
    ((fn2)p->layer.append_seq)(scene, seq);
    if (areas[1] != NO_WAVE_SCENE)
    {
        const u32 loader = word(scene + SCENE_WAVE_LOADER);
        if (!loader)
        {
            fail(5);
        }
        *(volatile u8*)(loader + LOADER_SECOND_SCENE) = (u8)areas[1];
    }
    ((fn2)p->layer.adapt_child_scene)(global, scene);

    if (!load_scene(scene))
    {
        fail(6);
    }
    const u32 start_point = (piece->kind >> 8) & 0xff;
    if (start_point)
    {
        *(volatile u32*)(seq + BOSS_SEQ_BOSS) = (u32)stand_in_boss;
        ((fn2)p->layer.start_jump_bgm)(seq, start_point);
    }
    else
    {
        call_virtual(scene, 0, SCENE_START, 0);
    }
    return seq;
}

/* Starts the sound and runs the sound layer's frame work once a frame, for ever. An effect starts as the game's system
   effects start: in two dimensions, with no actor. The sound library keeps the effect's JAISound in `sound`, and
   clears it when the effect ends. A piece's object, `seq`, has started already. The driver copies its JAISound to
   `sound` once a frame. `sys_if` is the sound system. */
__attribute__((always_inline, noreturn)) static inline void pikmin2_play(int effect, volatile struct Piece* piece,
                                                                         u32 seq, u32 sys_if)
{
    volatile struct Params* p = &driver_params;
    u32 handle = (u32)&p->sound;
    u32 tension = 0;
    if (effect)
    {
        ((fn4)p->layer.play_system_se)(sys_if, p->sound_id, handle, 0);
    }
    else
    {
        handle = call_virtual(seq, SEQ_VTABLE, SEQ_HANDLE, 0);
        if ((piece->kind & 0xff) == PIECE_BIG_BOSS_NEAR)
        {
            tension = word(word(word(seq + BGM_DIRECTORS) + DIRECTORS_LIST) + 4 * TENSION_DIRECTOR);
        }
    }
    p->sound = word(handle);
    if (!p->sound)
    {
        fail(7);
    }
    p->status = STATUS_PLAYING;

    for (;;)
    {
        ((fn1)p->layer.main_loop)(sys_if);
        /* The game turns the tension director on while a captain stands within 300 units of the boss (0x8046cc70 in
           the GameCube release's main.dol, 0x804f2130 in the Wii release's). directOn does nothing until the sequence
           has linked the director's track and the frame work has powered the director on. It does nothing once the
           director is on. */
        if (tension)
        {
            call_virtual(tension, 0, DIRECTOR_ON, 0);
        }
        p->sound = word(handle);
        if (p->status == STATUS_PLAYING && p->sound == 0)
        {
            p->status = STATUS_FINISHED;
        }
        wait_frame();
    }
}

#endif
