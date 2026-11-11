// SPDX-License-Identifier: MIT

// Definitions that the parts of jac's engine share (engine.cpp and engine_game.cpp): the DSP's memory, the game's
// constants and the model of one piece. The two files build as one translation unit. The definitions in the
// unnamed namespace therefore keep internal linkage. Only those files include this header.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

#include "ntdlib/jac/engine.h"
#include "ntdlib/jaudio2/tables.h"

namespace ntdlib::jac
{
namespace
{

// The game's sequence player (Jaf): 16 handles, each with a root track (0x7e8240 in main). The model's tracks 256 to
// 271. Jac_PlayBgm plays the music's first piece on handle 4.
constexpr uint32_t kHandles = 16;
constexpr uint32_t kMusicHandle = 4;

// The game's events (EVENT, 16 of 0x238 bytes at the pointer at 0x73cfd8): an event's type, its track and the queue
// of its track's port 0. Jac_CreateEvent gives a new event 100 frames (+0x1f0). Jac_UpdateEventPosition gives it 100
// again. Jac_EventFrameCheck counts them down, and stops the event's actions at 0.
constexpr std::size_t kEvents = 16;
constexpr int32_t kEventFrames = 100;
constexpr uint16_t kStopActions = 0xffff;

// More of the system's effects that Jac_PlaySystemSe handles on their own, with their names in the game's table of
// effects. kLifeDim and kViewChange queue only in a course, and kCmenuOff only while the C menu is open. The model
// leaves out kHighScore, kPauseAll and kResumeAll.
constexpr uint16_t kPause = 3;        // SYSSE_PAUSE
constexpr uint16_t kEveningAlert = 4; // SYSSE_EVENING_ALERT
constexpr uint16_t kCountdown = 5;    // SYSSE_COUNTDOWN
constexpr uint16_t kCmenuOn = 6;      // SYSSE_CMENU_ON
constexpr uint16_t kCmenuOff = 7;     // SYSSE_CMENU_OFF
constexpr uint16_t kLifeDim = 10;     // SYSSE_ORIMA_LIFEDIM
constexpr uint16_t kViewChange = 12;  // SYSSE_VIEW_CHANGE
constexpr uint16_t kUnpause = 15;     // SYSSE_UNPAUSE

// The scene of wave system kPartSystem that kShipPartEffect sets, and the count of kCountdown at which the game fades
// the music.
constexpr uint16_t kShipPartScene = 6;
constexpr uint16_t kCountdownLimit = 10;

} // namespace

// The game's state above the C JAudio, by the game's variables.
struct Engine::Model
{
    Model(const SoundFiles& files, jaudio2::DspMemory& memory)
        : banks(files.FileBanks(), files.FileSystems(), files.Waves(), memory, kAram, kAramSize),
          channels(memory, kBlocks, banks, random),
          tracks(channels, banks, random),
          dsp(memory, jaudio2::DspTables())
    {
    }

    Random random;
    Banks banks;
    Channels channels;
    Tracks tracks;
    jaudio2::Dsp dsp;

    // The start-up: not yet run, waiting for the track of 0x20000, or done.
    enum class Start : uint8_t
    {
        kNone,
        kWaiting,
        kDone,
    };
    Start start = Start::kNone;
    Startup startup;
    bool levels_pending = false; // the options' flag at 0x761df4

    // The sub-frames run, and the game's frames started.
    uint64_t subframes = 0;
    uint32_t game_frames = 0;

    // Jaf's handles: the root's handle that Jaq_SetSeqData gave, or -1 (0x7ed748).
    std::array<int32_t, kHandles> handles{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

    // The music's first record (0x7642d0): whether it plays, whether its child tracks still need their buffers, its
    // child tracks' buffers and volumes, and the root track's volume. The calls of fn_263fe0, the player's callback
    // that runs BgmFrameWork at every 8th call (0x7642b8).
    struct Bgm
    {
        bool active = false;
        bool needs_init = false;
        std::array<uint32_t, 16> buffers{kNone, kNone, kNone, kNone, kNone, kNone, kNone, kNone,
                                         kNone, kNone, kNone, kNone, kNone, kNone, kNone, kNone};
        std::array<float, 16> targets{};
        float root_volume = 0.0f;
    };
    Bgm bgm;
    uint32_t bgm_calls = 0;

    // The queues of values for the tracks' ports (Jal's list from 0x7e8238), in the order of their set-up. For a length
    // analysis, a value can be unheard: the tracks that the port's track opens once it has the value are unheard
    // (Tracks::SetUnheardOpens).
    struct QueuedValue
    {
        uint16_t value = 0;
        bool unheard = false;
    };

    struct PortQueue
    {
        uint32_t track = kNone;
        uint32_t port = 0;
        std::deque<QueuedValue> values;
    };
    std::vector<PortQueue> queues;

    // The port commands that wait for the task list (Add_PortcmdOnce's list at 0x7e5600): the events whose levels go
    // to their tracks.
    std::vector<uint32_t> port_commands;

    // The events.
    struct Event
    {
        uint32_t type = 0;
        uint32_t track = kNone;
        uint32_t outer = kNone;
        uint32_t queue = kNone;
        int32_t frames = 0;
        float volume = 0.0f;
        float pan = 0.0f;
    };
    std::array<Event, kEvents> events{};

    // The game's state that the effects read: the current scene (0x6af460), the flag that kPause and kPlayerDownPause
    // set (0x7652e8), the flag that kCmenuOn sets (0x7652ec), and the current cutscene (0x6af28c).
    int32_t scene = -1;
    bool paused = false;
    bool cmenu = false;
    int32_t demo = -1;

    // The system's effects: whether their two queues are set up (0x7652f0), the queues for ports 0 and 1 of their
    // track, and the count of kCountdown.
    bool system_queues = false;
    uint32_t system_queue = kNone;
    uint32_t system_port1_queue = kNone;
    uint32_t countdowns = 0;

    // The captain's track (0x765040) and its queue, and the flag of a Pikmin ready to fly (0x765014).
    uint32_t captain = kNone;
    uint32_t captain_queue = kNone;
    bool fly_ready = false;

    // The formation's track (0x765038), its buffer (0x765058) and whether the track has it, and whether its sound
    // plays (0x76500c).
    uint32_t formation = kNone;
    uint32_t formation_outer = kNone;
    bool formation_assigned = false;
    bool formation_on = false;

    // The track of the footsteps (0x765030), and the frames since the captain's last sound (0x76501c).
    uint32_t walk = kNone;
    uint32_t idle_frames = 0;

    // The crowd: whether its track has its buffer (0x765028), the track (0x765050), the buffer (0x765098), the
    // crowd's size (0x765024) and its volume (0x76502c).
    bool crowd_assigned = false;
    uint32_t crowd = kNone;
    uint32_t crowd_outer = kNone;
    uint32_t crowd_size = 0;
    float crowd_volume = 0.0f;

    // The cutscenes' effects: whether their queue is set up, the track (0x761e48) and the queue.
    bool demo_queue_set = false;
    uint32_t demo_track = kNone;
    uint32_t demo_queue = kNone;

    // The sound: its kind, the root of a piece, the effect and its event.
    enum class Sound : uint8_t
    {
        kNone,
        kPiece,
        kPart,
        kEffect,
    };
    Sound sound = Sound::kNone;
    uint32_t root = kNone;
    Effect effect;
    uint32_t effect_frame = 0;
    int32_t event = -1;

    // A cutscene's later events, from Engine::QueueDemoEvents, and whether they're unheard.
    std::vector<DemoEvent> demo_events;
    bool demo_events_unheard = false;

    // For Playing: the ports that a part's or an effect's code writes and the queues that it sends to, each once, and
    // the tracks' opens and notes as the sound started (Tracks::Opens and Tracks::NoteCount). The code records them
    // while `recording` is set.
    bool recording = false;
    std::vector<std::pair<uint32_t, uint32_t>> sound_ports;
    std::vector<uint32_t> sound_queues;
    std::array<uint32_t, Tracks::kAllTracks> start_opens{};
    std::array<uint32_t, Tracks::kAllTracks> start_notes{};

    uint32_t unmodelled = 0;
};

} // namespace ntdlib::jac
