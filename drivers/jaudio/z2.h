// SPDX-License-Identifier: MIT

/*
 * The start-up of Z2, the sound layer that Twilight Princess (2006) puts on JAudio2, for a game-mode driver on the
 * GameCube or the Wii. A driver includes this file and calls z2_main() once it has checked its parameter block's
 * header. z2_main() calls the game's functions to do what the game's start-up does for sound, without its graphics.
 * The block's Z2Params give their addresses. The ripper fills them in. The steps:
 *   2. JSystem's framework: the root heap, the system heap and the ARAM manager;
 *   3. the game's machine: the heaps that its disc thread and audio set-up use, and the ARAM transfer buffer. The
 *      thread switch manager, a JKRThread for the main thread and the game's disc thread follow. The disc thread
 *      changes the current heap when it starts. The switch manager gives each thread back its current heap;
 *   4. the game's audio heap;
 *   5. the game's audio set-up, once a frame until the audio manager is initialised. The set-up loads the start-up
 *      archive and mounts the sequence archive through the disc thread;
 *   6. the wave groups that the block lists, each as the game's scene manager loads them. If the profile names the
 *      first scene's groups, the scene manager first erases those that the list doesn't hold. If the profile names
 *      start-up groups for room and the wave heap then has no room for a group, the scene manager erases the list's
 *      groups and those start-up groups, and loads the list's groups again;
 *   7. the sound: the audio manager starts it by its JAudio sound ID.
 * Then the audio manager's frame process runs once a frame, forever. A step that fails stops the driver with its number
 * in the status word (step 1 is the driver's check of its header).
 * The game's audio process calls the set-up and then the frame process. z2_main() calls them directly.
 *
 * The consoles differ in a few steps. z2_main() calls these functions, and the driver defines them:
 *   z2_init_machine(p)        before the heaps: the machine (VIInit on the GameCube, the reset data on the Wii);
 *   z2_wait_frame(p)          one frame of the game (video retraces on the GameCube, a sleep on the Wii);
 *   z2_create_aram(p)         after the system heap: the ARAM manager (JKRAram::create on the GameCube);
 *   z2_set_aram_buffer(p, s)  after the game's heaps: the ARAM transfer buffer, in the system heap `s`.
 * Each gets the block's Z2Params. A driver may add fields of its own after them.
 */

#ifndef NTDLIB_DRIVERS_JAUDIO_Z2_H
#define NTDLIB_DRIVERS_JAUDIO_Z2_H

typedef unsigned int u32;
typedef int s32;

/* Values of the status word. They're macros because an enumerator can't hold 0x80000000 before C23. */
#define Z2_STATUS_PLAYING 1u
#define Z2_STATUS_FINISHED 2u
#define Z2_STATUS_ERROR 0x80000000u /* | step number */

/* The frames that the driver waits for the audio set-up and for the wave groups before it gives up. */
#define Z2_SETUP_FRAMES 600u

/* The most wave groups that a block lists. */
#define Z2_MAX_WAVES 64u

/* The most first-scene groups that a profile lists. */
#define Z2_MAX_FIRST_SCENE_WAVES 4u

/* The most other start-up groups that a profile lists for room in the wave heap. */
#define Z2_MAX_ROOM_WAVES 4u

/* A wave archive's status (JASWaveArc): not loaded, loading or loaded. The status stays Z2_WAVE_NONE after a load if
   the wave heap has no room for the archive. */
#define Z2_WAVE_NONE 0
#define Z2_WAVE_LOADING 1
#define Z2_WAVE_LOADED 2

/* A main thread's JKRThread, allocated with room to spare. */
#define Z2_THREAD_SIZE 0x100u

/* What the driver's parameter block holds after its header. The ripper writes the profile, from frame_retraces on.
   Each mini file patches the sound and its wave groups. */
struct Z2Params
{
    u32 sound_id;            /* the JAudio sound ID to play */
    u32 sound;               /* the sound's JAISoundHandle (written by the game): its JAISound while it plays, then 0 */
    u32 wave_count;          /* the wave groups to load */
    u32 waves[Z2_MAX_WAVES]; /* each the wave bank in the high half and the group in the low half */
    u32 frame_retraces;      /* video retraces between the audio process's calls */
    /* the sizes that the game's start-up gives */
    u32 max_heaps; /* the root heap's maximum of standard heaps */
    u32 system_heap_size;
    u32 aram_audio_size;  /* ARAM for the sound library */
    u32 aram_graph_size;  /* ARAM for graphics */
    u32 aram_priorities;  /* the ARAM manager's three thread priorities, a byte each, from bit 16 down */
    u32 aram_buffer_size; /* the ARAM transfer buffer */
    u32 command_heap_size;
    u32 archive_heap_size;
    u32 zelda_heap_margin; /* what the zelda heap leaves free in the system heap */
    u32 audio_heap_size;
    /* the game profile: the addresses of the game's functions and variables */
    u32 vi_init;              /* void VIInit(void) */
    u32 vi_wait_for_retrace;  /* void VIWaitForRetrace(void) */
    u32 get_current_thread;   /* OSThread* OSGetCurrentThread(void) */
    u32 get_thread_priority;  /* s32 OSGetThreadPriority(OSThread*) */
    u32 create_root_heap;     /* JKRExpHeap* JKRExpHeap::createRoot(int max_heaps, bool) */
    u32 create_exp_heap;      /* JKRExpHeap* JKRExpHeap::create(u32 size, JKRHeap* parent, bool) */
    u32 create_solid_heap;    /* JKRSolidHeap* JKRSolidHeap::create(u32 size, JKRHeap* parent, bool) */
    u32 create_aram;          /* JKRAram* JKRAram::create(u32 audio, u32 graph, s32, s32, s32) */
    u32 set_aram_buffer;      /* void JKRAramStream::setTransBuffer(u8*, u32 size, JKRHeap*) */
    u32 create_thread_switch; /* JKRThreadSwitch* JKRThreadSwitch::createManager(JKRHeap*) */
    u32 create_command_heap;  /* JKRExpHeap* mDoExt_createCommandHeap(u32 size, JKRHeap* parent) */
    u32 create_archive_heap;  /* JKRExpHeap* mDoExt_createArchiveHeap(u32 size, JKRHeap* parent) */
    u32 archive_heap_parent;  /* the variable that holds the archive heap's parent, or 0 for the root heap */
    u32 create_assert_heap;   /* JKRHeap* mDoExt_createAssertHeap(JKRHeap* parent) */
    u32 create_zelda_heap;    /* JKRExpHeap* mDoExt_createZeldaHeap(u32 size, JKRHeap* parent) */
    u32 get_free_size;        /* s32 JKRHeap::getFreeSize() */
    u32 become_current_heap;  /* JKRHeap* JKRHeap::becomeCurrentHeap() */
    u32 system_heap;          /* the variable that holds JSystem's system heap */
    u32 alloc;                /* void* operator new(u32 size) from the current heap */
    u32 thread_ctor;          /* JKRThread::JKRThread(OSThread*, int message_count) */
    u32 create_disc_thread;   /* void mDoDvdThd::create(s32 priority) */
    u32 audio_heap;           /* the variable that holds the game's audio heap */
    u32 audio_setup;          /* void mDoAud_Create(void) */
    u32 init_flag;            /* the byte that the audio set-up sets when it's done */
    u32 audio_frame;          /* void Z2AudioMgr::gframeProcess(void) */
    u32 load_se_wave;         /* bool Z2SceneMgr::loadSeWave(u32 group): a group of wave bank 0 */
    u32 load_bgm_wave;        /* bool Z2SceneMgr::loadBgmWave(u32 group): a group of wave bank 1 */
    u32 wave_status;          /* s32 Z2SceneMgr::getWaveLoadStatus(u32 group, u32 bank) */
    u32 audio_manager;        /* the game's audio manager (Z2AudioMgr) */
    u32 start_sound;          /* bool Z2AudioMgr::startSound(JAISoundID, JAISoundHandle*, const TVec3*) */
    /* the first scene's wave groups: the audio set-up loads them, and the game's first scene change erases them.
       Before it loads the sound's groups, the driver erases those that the sound doesn't use. That leaves room in the
       game's wave heap. A console that erases none leaves these at 0. */
    u32 erase_bgm_wave;         /* Z2SceneMgr::eraseBgmWave(u32 group): a group of wave bank 1, or 0 */
    u32 erase_se_wave;          /* Z2SceneMgr::eraseSeWave(u32 group): a group of wave bank 0, or 0 */
    u32 first_scene_wave_count; /* the first scene's groups that follow */
    u32 first_scene_waves[Z2_MAX_FIRST_SCENE_WAVES]; /* each (bank << 16) | group */
    /* more of the start-up's wave groups. The driver erases them only when the wave heap has no room for one of the
       sound's groups. The heap puts each group that loads below the lowest one. Space freed above that group stays
       unused. The driver therefore waits for the sound's other groups to load, erases them and these, and loads the
       sound's groups again. A console that erases none leaves these at 0. */
    u32 room_wave_count;               /* the groups that follow */
    u32 room_waves[Z2_MAX_ROOM_WAVES]; /* each (bank << 16) | group */
};

typedef u32 (*z2_fn0)(void);
typedef u32 (*z2_fn1)(u32);
typedef u32 (*z2_fn2)(u32, u32);
typedef u32 (*z2_fn3)(u32, u32, u32);
typedef u32 (*z2_fn4)(u32, u32, u32, u32);
typedef u32 (*z2_fn5)(u32, u32, u32, u32, u32);

/* The console's steps. The driver defines them. */
static void z2_init_machine(volatile struct Z2Params* p);
static void z2_wait_frame(volatile struct Z2Params* p);
static void z2_create_aram(volatile struct Z2Params* p);
static void z2_set_aram_buffer(volatile struct Z2Params* p, u32 system_heap);

static u32 z2_word(u32 address)
{
    return *(volatile u32*)address;
}

/* Stops the driver at `step`: the status word reports it, and the driver waits for ever. */
__attribute__((noreturn)) static void z2_fail(volatile struct Z2Params* p, volatile u32* status, u32 step)
{
    *status = Z2_STATUS_ERROR | step;
    for (;;)
    {
        z2_wait_frame(p);
    }
}

/* Whether every listed wave group is loaded. The scene manager's functions don't use their object. */
static int z2_waves_loaded(volatile struct Z2Params* p)
{
    for (u32 i = 0; i < p->wave_count && i < Z2_MAX_WAVES; i++)
    {
        const u32 bank = p->waves[i] >> 16;
        const u32 group = p->waves[i] & 0xffff;
        if ((s32)((z2_fn3)p->wave_status)(0, group, bank) != Z2_WAVE_LOADED)
        {
            return 0;
        }
    }
    return 1;
}

/* Whether a listed wave group has the status `wave_status`. */
static int z2_any_wave(volatile struct Z2Params* p, s32 wave_status)
{
    for (u32 i = 0; i < p->wave_count && i < Z2_MAX_WAVES; i++)
    {
        const u32 bank = p->waves[i] >> 16;
        const u32 group = p->waves[i] & 0xffff;
        if ((s32)((z2_fn3)p->wave_status)(0, group, bank) == wave_status)
        {
            return 1;
        }
    }
    return 0;
}

/* Whether the block's list of wave groups holds `wave`. */
static int z2_lists_wave(volatile struct Z2Params* p, u32 wave)
{
    for (u32 i = 0; i < p->wave_count && i < Z2_MAX_WAVES; i++)
    {
        if (p->waves[i] == wave)
        {
            return 1;
        }
    }
    return 0;
}

/* Loads the listed wave groups as the scene manager loads them. */
static void z2_load_waves(volatile struct Z2Params* p)
{
    for (u32 i = 0; i < p->wave_count && i < Z2_MAX_WAVES; i++)
    {
        const u32 bank = p->waves[i] >> 16;
        const u32 group = p->waves[i] & 0xffff;
        ((z2_fn2)(bank == 1 ? p->load_bgm_wave : p->load_se_wave))(0, group);
    }
}

/* The function that erases a group of wave bank `bank`, or 0 for a bank that keeps its groups. */
static u32 z2_erase_fn(volatile struct Z2Params* p, u32 bank)
{
    return bank == 1 ? p->erase_bgm_wave : bank == 0 ? p->erase_se_wave : 0;
}

/* Erases each of the `count` wave groups at `waves` that has loaded. A bank whose function is 0 keeps its groups. */
static void z2_erase_loaded(volatile struct Z2Params* p, volatile u32* waves, u32 count)
{
    for (u32 i = 0; i < count; i++)
    {
        const u32 bank = waves[i] >> 16;
        const u32 group = waves[i] & 0xffff;
        const u32 erase = z2_erase_fn(p, bank);
        if (erase && (s32)((z2_fn3)p->wave_status)(0, group, bank) == Z2_WAVE_LOADED)
        {
            ((z2_fn2)erase)(0, group);
        }
    }
}

/* Starts the sound and runs the audio frame process for ever. `status` is the block's status word. */
__attribute__((noreturn)) static void z2_main(volatile struct Z2Params* p, volatile u32* status)
{
    z2_init_machine(p);

    /* JSystem's framework: the root heap, the system heap and the ARAM manager. */
    const u32 root = ((z2_fn2)p->create_root_heap)(p->max_heaps, 0);
    if (!root || !((z2_fn3)p->create_exp_heap)(p->system_heap_size, root, 0))
    {
        z2_fail(p, status, 2);
    }
    const u32 system = z2_word(p->system_heap);
    z2_create_aram(p);

    /* The game's machine: the heaps that its disc thread and audio set-up use, then the ARAM transfer buffer. The zelda
       heap becomes the current heap. */
    const u32 command = ((z2_fn2)p->create_command_heap)(p->command_heap_size, root);
    const u32 archive_parent = p->archive_heap_parent ? z2_word(p->archive_heap_parent) : root;
    const u32 archive = ((z2_fn2)p->create_archive_heap)(p->archive_heap_size, archive_parent);
    const u32 assert_heap = ((z2_fn1)p->create_assert_heap)(root);
    const s32 free = (s32)((z2_fn1)p->get_free_size)(system) - (s32)p->zelda_heap_margin;
    const u32 zelda = free > 0 ? ((z2_fn2)p->create_zelda_heap)((u32)free, system) : 0;
    if (!command || !archive || !assert_heap || !zelda)
    {
        z2_fail(p, status, 3);
    }
    ((z2_fn1)p->become_current_heap)(zelda);
    z2_set_aram_buffer(p, system);

    /* The thread switch manager, the main thread's JKRThread, and the game's disc thread. The disc thread's priority
       number is 2 less than that of the main thread: the disc thread runs first. */
    ((z2_fn1)p->create_thread_switch)(0);
    const u32 thread = ((z2_fn1)p->alloc)(Z2_THREAD_SIZE);
    if (thread)
    {
        ((z2_fn3)p->thread_ctor)(thread, ((z2_fn0)p->get_current_thread)(), 0);
    }
    const u32 priority = ((z2_fn1)p->get_thread_priority)(((z2_fn0)p->get_current_thread)());
    ((z2_fn1)p->create_disc_thread)(priority - 2);

    /* The game's audio heap, then its audio set-up once a frame until it's done. */
    const u32 audio_heap = ((z2_fn3)p->create_solid_heap)(p->audio_heap_size, zelda, 0);
    if (!audio_heap)
    {
        z2_fail(p, status, 4);
    }
    *(volatile u32*)p->audio_heap = audio_heap;
    u32 frames = 0;
    while (!*(volatile unsigned char*)p->init_flag)
    {
        ((z2_fn0)p->audio_setup)();
        if (++frames > Z2_SETUP_FRAMES)
        {
            z2_fail(p, status, 5);
        }
        z2_wait_frame(p);
    }

    /* The game's first scene change erases the first scene's groups. Here the driver erases those that the sound
       doesn't use. A bank whose function is 0 keeps its groups. */
    for (u32 i = 0; i < p->first_scene_wave_count && i < Z2_MAX_FIRST_SCENE_WAVES; i++)
    {
        const u32 erase = z2_erase_fn(p, p->first_scene_waves[i] >> 16);
        if (erase && !z2_lists_wave(p, p->first_scene_waves[i]))
        {
            ((z2_fn2)erase)(0, p->first_scene_waves[i] & 0xffff);
        }
    }

    /* The sound's wave groups. The frame process runs while they load. */
    z2_load_waves(p);
    if (p->room_wave_count && z2_any_wave(p, Z2_WAVE_NONE))
    {
        /* The wave heap has no room for a group. Once the others have loaded, the driver erases them and the profile's
           other start-up groups, then loads the sound's groups again. */
        for (frames = 0; z2_any_wave(p, Z2_WAVE_LOADING); frames++)
        {
            if (frames > Z2_SETUP_FRAMES)
            {
                z2_fail(p, status, 6);
            }
            ((z2_fn1)p->audio_frame)(p->audio_manager);
            z2_wait_frame(p);
        }
        z2_erase_loaded(p, p->waves, p->wave_count < Z2_MAX_WAVES ? p->wave_count : Z2_MAX_WAVES);
        z2_erase_loaded(p, p->room_waves,
                        p->room_wave_count < Z2_MAX_ROOM_WAVES ? p->room_wave_count : Z2_MAX_ROOM_WAVES);
        z2_load_waves(p);
    }
    for (frames = 0; !z2_waves_loaded(p); frames++)
    {
        if (frames > Z2_SETUP_FRAMES)
        {
            z2_fail(p, status, 6);
        }
        ((z2_fn1)p->audio_frame)(p->audio_manager);
        z2_wait_frame(p);
    }

    /* The sound. A JAISoundID passes by reference. */
    const u32 id = p->sound_id;
    if (!(((z2_fn4)p->start_sound)(p->audio_manager, (u32)&id, (u32)&p->sound, 0) & 0xff))
    {
        z2_fail(p, status, 7);
    }
    *status = Z2_STATUS_PLAYING;

    for (;;)
    {
        ((z2_fn1)p->audio_frame)(p->audio_manager);
        if (*status == Z2_STATUS_PLAYING && p->sound == 0)
        {
            *status = Z2_STATUS_FINISHED;
        }
        z2_wait_frame(p);
    }
}

#endif
