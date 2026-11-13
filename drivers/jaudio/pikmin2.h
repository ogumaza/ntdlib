// SPDX-License-Identifier: MIT

/*
 * What a game-mode driver of Pikmin 2 reads of the sound ID that it plays, of the ripper's table of pieces and of the
 * game's objects. The GameCube release and the Wii release (New Play Control!) share them. The ripper's side of the
 * table is in ntdlib/jaudio/pikmin2_pieces.h.
 */

#ifndef NTDLIB_DRIVERS_JAUDIO_PIKMIN2_H
#define NTDLIB_DRIVERS_JAUDIO_PIKMIN2_H

/* The most pieces in the profile's table. */
#define MAX_PIECES 96u

/* An effect's value holds the random numbers to draw before the start in these bits, and its sound ID in the others.
   The game's effect IDs leave bits 16 to 30 clear. */
#define DRAWS_SHIFT 20
#define DRAWS_MASK 0xffu

/* A scene loads at most two groups of wave system 0, one in each of its areas. 255 names none. */
#define NO_WAVE_SCENE 255u

/* How the driver makes a piece's object. Each kind is a way in which the game makes a piece's object. */
#define PIECE_PLAIN 0u     /* PikSceneMgr::newBgmSeq: a BgmSeq that only plays */
#define PIECE_MAIN 1u      /* PSM::SceneMgr::newMainBgm: a JumpBgmSeq with the scene's directors, for the areas */
#define PIECE_DIRECTED 2u  /* PSM::SceneMgr::newDirectedBgm: the bosses' music and the music of 2-player battles */
#define PIECE_AUTO 3u      /* PSM::SceneMgr::newAutoBgm: an AutoBgm. Its conductor picks what the tracks play */
#define PIECE_BATTLE 4u    /* a DirectedBgm with the 2-player battle directors that newDirectedBgm gives battle_t */
#define PIECE_FREE 5u      /* a DirectedBgm without directors */
#define PIECE_AUTO_FREE 6u /* an AutoBgm without directors or the arrangement of its melody */
#define PIECE_BIG_BOSS 7u  /* newDirectedBgm on the final boss's floor, with the final boss's sound object in place */
#define PIECE_BIG_BOSS_NEAR 8u /* PIECE_BIG_BOSS with the tension director on: a captain stands near the boss */

/* One piece of the profile's table. */
struct Piece
{
    unsigned int kind;    /* PIECE_*, a boss's start point from bit 8, and the root track's beat interval from bit 16 (0
                             keeps the default) */
    unsigned int info[4]; /* the JAInter::SoundInfo that the game gives the piece's object */
    char sequence[24];    /* the sequence's file in the sequence archive */
    char conductor[24];   /* an AutoBgm's conductor in the conductor archive */
};

/* The functions and variables of the game's sound layer that a driver uses, in the order of both releases' parameter
   blocks. The ripper fills them in. */
struct SoundLayer
{
    unsigned int factory_ctor;     /* PSGame::SysFactory::SysFactory() */
    unsigned int factory_vtable;   /* PSM::Factory's vtable */
    unsigned int make_se;          /* PSM::SeSound::makeSeSound, the factory's maker of effects */
    unsigned int new_sound_system; /* void PSGame::SysFactory::newSoundSystem() */
    unsigned int scene_manager;    /* the variable that holds the scene manager, a PSM::SceneMgr */
    unsigned int system_interface; /* the variable that holds the sound system, a PSSystem::SysIF */
    unsigned int main_loop;        /* void PSSystem::SysIF::mainLoop(): the sound layer's frame work */
    unsigned int new_bgm_seq;      /* BgmSeq* PikSceneMgr::newBgmSeq(const char* bms, SoundInfo&) */
    unsigned int new_main_bgm;     /* BgmSeq* PSM::SceneMgr::newMainBgm(const char* bms, SoundInfo&) */
    unsigned int new_directed_bgm; /* BgmSeq* PSM::SceneMgr::newDirectedBgm(const char* bms, SoundInfo&) */
    unsigned int new_auto_bgm; /* BgmSeq* PSM::SceneMgr::newAutoBgm(cnd, bms, SoundInfo&, AccessMode, SceneInfo&, ..) */
    unsigned int new_game_scene;    /* Scene* PSM::SceneMgr::newGameScene(u8 wave_scene, SceneInfo*) */
    unsigned int append_seq;        /* void PSSystem::Scene::appendSeq(SeqBase*) */
    unsigned int adapt_child_scene; /* void PSSystem::Scene::adaptChildScene(Scene*) */
    unsigned int scene_info_ctor;   /* PSGame::SceneInfo::SceneInfo() */
    unsigned int directed_bgm_ctor; /* DirectedBgm::DirectedBgm(const char* bms, const SoundInfo&, DirectorMgrBase*) */
    unsigned int
        auto_bgm_ctor; /* AutoBgm::AutoBgm(const char* cnd, const char* bms, const SoundInfo&, AccessMode, ..) */
    unsigned int battle_directors_ctor; /* PSM::DirectorMgr_2PBattle::DirectorMgr_2PBattle() */
    unsigned int adapt_directors;       /* void PSSystem::DirectorMgrBase::initAndAdaptToBgm(DirectedBgm&) */
    unsigned int start_jump_bgm;        /* void PSSystem::JumpBgmSeq::startSeq(u16 start_point) */
    unsigned int conductor_heap;        /* the variable that holds the heap of the conductors' objects */
    unsigned int play_system_se;        /* void PSSystem::SysIF::playSystemSe(u32 id, JAISound** handle, u32 flag) */
    unsigned int play_data;             /* the variable that holds the game's play data, Game::PlayData */
    unsigned int rappas;                /* PSGame::Rappa::sRappa: the two captains' marching horns */
};

/* The offsets of the game's objects that the driver reads and writes. */
#define FACTORY_HEAP 0x00         /* SysFactory: the heap for the sound system */
#define FACTORY_HEAP_SIZE 0x04    /* the sound library's ARAM */
#define FACTORY_ARCHIVE 0x08      /* the start-up archive */
#define FACTORY_MAKE_SE 0x0c      /* the maker of effects */
#define FACTORY_VTABLE 0x10       /* its vtable pointer, after the members above */
#define MANAGER_GLOBAL_SCENE 0x04 /* PSSystem::SceneMgr: the global scene */
#define MANAGER_ACCESS_MODE 0x0c  /* the access mode of the conductors' data (a byte) */
#define MANAGER_NEW_GLOBAL 0x10   /* its vtable's newAndSetGlobalScene */
#define SCENE_WAVE_LOADER 0x08    /* PSSystem::Scene: its WaveLoader, or 0 */
#define SCENE_LOAD 0x10           /* its vtable's scene1st(TaskChecker*) */
#define SCENE_START 0x1c          /* its vtable's startMainSeq */
#define LOADER_SECOND_SCENE 0x05  /* WaveLoader: the group of wave system 0 in its second area (a byte) */
#define INFO_SCENE_TYPE 0x06      /* PSGame::SceneInfo: the scene's type (a byte) */
#define SEQ_VTABLE 0x10           /* PSSystem::SeqBase: its vtable pointer, after its JSULink */
#define SEQ_INIT 0x0c             /* its vtable's init */
#define SEQ_HANDLE 0x3c           /* its vtable's getHandleP */
#define BGM_DIRECTORS 0x6c        /* DirectedBgm: its DirectorMgrBase */
#define BGM_ROOT_TRACK 0x70       /* DirectedBgm: its SeqTrackRoot */
#define DIRECTORS_LIST 0x04       /* DirectorMgrBase: its array of directors */
#define DIRECTOR_ON 0x10          /* a director's vtable's directOn */
#define TENSION_DIRECTOR 1u       /* tentionD's place among the directors of a boss's music */
#define ROOT_BEAT_INTERVAL 0x3e   /* SeqTrackRoot: the beat interval (a halfword) */
#define TASK_COUNT 0x18           /* TaskChecker: the loads in flight (a byte), after its OSMutex */
#define BOSS_SEQ_BOSS 0x134       /* PSM::MiddleBossSeq and BigBossSeq: the boss whose actions pick the parts */

/* The shortest wait after each note of a captain's marching horn that the game sets: the stick tilted all the way. */
#define RAPPA_SHORTEST_WAIT 3u

#endif
