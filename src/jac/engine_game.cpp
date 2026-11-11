// SPDX-License-Identifier: MIT

// The game's code above the C JAudio, on the model: the start-up, the starts of pieces, parts and effects, and
// the game's frames (see engine.cpp).

#include "ntdlib/jac/engine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "jac/engine_internal.h"

namespace ntdlib::jac
{

namespace
{

// Hagi's sub-frames in a second, and the game's frames that the model takes in a second.
constexpr uint64_t kSubframesPerSecond = 32000 / jaudio2::Dsp::kSubframeSamples;
constexpr uint64_t kGameFramesPerSecond = 30;

// The IDs that tracks of the start-up's sequences register (Jam_GetTrackHandle). The roots of handles 0, 1 and 2
// register 0x10000, 0x20000 and 0x30000.
constexpr uint32_t kEffectRoot = 0x10000;
constexpr uint32_t kEventRoot = 0x20000;
constexpr uint32_t kCutsceneRoot = 0x30000;
constexpr uint32_t kCrowdTrack = 0x10003;
constexpr uint32_t kFormationTrack = 0x10007;
constexpr uint32_t kWalkTrack = 0x10008;
constexpr uint32_t kSystemTrack = 0x10009;
constexpr uint32_t kCaptainTrack = 0x1000a;
constexpr uint32_t kDemoTrack = 0x1000f;

// A queue of values for a track's port (CmdQueue): Jal_SendCmdQueue_Noblock adds a value while the queue holds fewer
// than 16.
constexpr std::size_t kQueueSize = 16;

// Jac_UpdateCamera's scale of an event's distance by its type (0x5b7498), and the bounds of its angles.
constexpr std::array<float, 8> kEventDistanceScale = {1.0f, 1.0f, 2.0f, 0.8f, 1.2f, 1.0f, 1.2f, 2.0f};
constexpr float kHalfTurn = 3.1416f;
constexpr float kTurn = -6.2832f;
constexpr double kAngleOffset = 3.141592; // V3D_GetAngle's offset of the angle (0x27aa60 in the release's main)

// Jac_UpdateEventPosition's switches of an event's track: the volume and the pan (Jac_InitEventSystem).
constexpr uint16_t kEventSwitches = OuterParam::kVolume | OuterParam::kPan;

// Jac_Orima_Formation's volume of the formation's track while fn_2c1ef0 gives 0 (it always does).
constexpr float kFormationVolume = 1.2f;
constexpr int32_t kStickLimit = 127;

// Jac_UpdatePikiGaya: the scene of a course (Jac_GetCurrentScene), the frames without a sound of the captain before
// the crowd fades out and in, and the volume's step.
constexpr int32_t kCourseScene = 5;
constexpr uint32_t kCrowdFadeOut = 150;
constexpr uint32_t kCrowdFadeIn = 240;
constexpr float kCrowdStep = 0.05f;

// More of the captain's effects: the one that pauses the events (kPlayerDown), and the Pikmin's two effects of a throw.
constexpr uint16_t kPlayerDown = 0xc;
constexpr uint16_t kFlyReady = 0x8004;
constexpr uint16_t kFly = 0x8002;

// The values that kCmenuOff queues for port 1 of the system's track, in their order (0x266aac and 0x2667e0), before
// it queues value 7 for port 0.
constexpr std::array<uint16_t, 2> kCmenuOffPort1 = {0x23, 0x21};

constexpr std::size_t kTrackPorts = std::tuple_size_v<decltype(Track::ports)>;

// The game's frames of an effect's later steps: an object's action, two frames after its event's creation, and a
// cutscene's sound. The cutscene's finish comes the effect's `finish` frames after the sound.
constexpr uint32_t kEventActionFrame = 2;
constexpr uint32_t kDemoSoundFrame = 1;

} // namespace

namespace
{

// The game's code above the C JAudio, on the model. Each function names the game's function that it follows.
class Game
{
public:
    explicit Game(Engine::Model& m) : m_(m), t_(m.tracks)
    {
    }

    // Jaf_ReadySeq and Jaf_PlaySeq on a handle.
    bool ReadySeq(uint32_t handle, const Sequence& sequence)
    {
        const int32_t root = t_.SetSeqData(Tracks::kTracks + handle, sequence);
        m_.handles[handle] = root;
        return root >= 0;
    }

    void PlaySeq(uint32_t handle)
    {
        t_.StartSeq(m_.handles[handle]);
    }

    uint32_t HandleToSeq(uint32_t handle) const
    {
        return Tracks::kTracks + handle;
    }

    // Jal_CmdQueue_Init: a queue at the end of the list.
    uint32_t AddQueue(uint32_t track, uint32_t port)
    {
        m_.queues.push_back({track, port, {}});
        return static_cast<uint32_t>(m_.queues.size() - 1);
    }

    // Jal_SendCmdQueue_Noblock and Jal_SendCmdQueue_Force: the latter takes the queue's first value out to make room. A
    // cutscene's later events can go unheard (Engine::QueueDemoEvents).
    bool Send(uint32_t queue, uint16_t value, bool unheard = false)
    {
        if (queue == kNone)
        {
            return false;
        }

        std::deque<Engine::Model::QueuedValue>& values = m_.queues[queue].values;
        if (m_.recording && std::find(m_.sound_queues.begin(), m_.sound_queues.end(), queue) == m_.sound_queues.end())
        {
            m_.sound_queues.push_back(queue);
            Record(m_.queues[queue].track, m_.queues[queue].port);
        }
        if (values.size() >= kQueueSize)
        {
            return false;
        }

        values.push_back({value, unheard});

        return true;
    }

    void SendForce(uint32_t queue, uint16_t value)
    {
        if (queue != kNone && !Send(queue, value))
        {
            m_.queues[queue].values.pop_front();
            Send(queue, value);
        }
    }

    // Jal_FrameWork (0x279f70): each queue's first value goes to its port once the track has read the last.
    int32_t PortQueues()
    {
        for (Engine::Model::PortQueue& queue : m_.queues)
        {
            if (queue.track == kNone)
            {
                m_.unmodelled++;
                continue;
            }
            if (!t_.CheckPortAppDirect(queue.track, queue.port, 1) || queue.values.empty())
            {
                continue;
            }

            const Engine::Model::QueuedValue value = queue.values.front();
            queue.values.pop_front();
            t_.WritePortAppDirect(queue.track, queue.port, value.value);
            t_.SetUnheardOpens(queue.track, value.unheard);
        }

        return 0;
    }

    // The task list (0x277780): the port commands that wait, each once. __SetVolandPan gives an event's track the
    // event's volume and pan.
    int32_t PortCommands()
    {
        const std::vector<uint32_t> commands = std::move(m_.port_commands);
        m_.port_commands.clear();
        for (const uint32_t index : commands)
        {
            const Engine::Model::Event& event = m_.events[index];
            t_.SetExtParam(event.track, OuterParam::kVolume, event.volume);
            t_.SetExtParam(event.track, OuterParam::kPan, event.pan);
        }

        return 0;
    }

    // fn_263fe0: BgmFrameWork at every 8th call.
    int32_t BgmCallback()
    {
        m_.bgm_calls++;
        if ((m_.bgm_calls & 7) == 0)
        {
            BgmFrameWork();
        }

        return 0;
    }

    // BgmFrameWork (0x2645a0) for the music's first record. A record that needs its buffers gets them
    // (ChangeBgmTrackVol). The moves of the volumes start only from the game's changes of the music.
    void BgmFrameWork()
    {
        Engine::Model::Bgm& bgm = m_.bgm;
        if (!bgm.active || !bgm.needs_init)
        {
            return;
        }

        const uint32_t root = HandleToSeq(kMusicHandle);
        for (uint8_t i = 0; i < 16; i++)
        {
            if (bgm.buffers[i] == kNone)
            {
                bgm.buffers[i] = t_.AddOuter();
            }
            else
            {
                t_.InitExtBuffer(bgm.buffers[i]);
            }
            t_.AssignExtBufferP(root, i, bgm.buffers[i]);
            t_.SetExtParamP(root, i, OuterParam::kVolume, bgm.targets[i]);
            t_.OnExtSwitchP(root, i, OuterParam::kVolume);
        }
        bgm.needs_init = false;
    }

    // Jac_DemoCheck and Jac_PauseCheck.
    bool DemoCheck() const
    {
        return m_.demo != -1;
    }

    bool PauseCheck() const
    {
        return m_.paused || m_.cmenu;
    }

    // fn_264cc0: the options' levels. Ports 2 of the cutscenes' root, and 0 of the effects' and the events' roots.
    void SetLevels()
    {
        WritePort(t_.TrackHandle(kCutsceneRoot), 2, m_.startup.cutscene_level);
        WritePort(t_.TrackHandle(kEffectRoot), 0, m_.startup.effect_level);
        WritePort(t_.TrackHandle(kEventRoot), 0, m_.startup.effect_level);
    }

    void WritePort(uint32_t track, uint32_t port, uint16_t value)
    {
        if (track != kNone)
        {
            t_.WritePortAppDirect(track, port, value);
            Record(track, port);
        }
    }

    // A port that the sound's code writes, for Playing.
    void Record(uint32_t track, uint32_t port)
    {
        const std::pair<uint32_t, uint32_t> place{track, port};
        if (m_.recording && track != kNone && port < kTrackPorts &&
            std::find(m_.sound_ports.begin(), m_.sound_ports.end(), place) == m_.sound_ports.end())
        {
            m_.sound_ports.push_back(place);
        }
    }

    // Jac_InitEventSystem (0x262360) and Jac_InitDemoSystem (0x261510).
    void InitEvents()
    {
        const uint32_t root = t_.TrackHandle(kEventRoot);
        for (std::size_t i = 0; i < kEvents; i++)
        {
            Engine::Model::Event& event = m_.events[i];
            event = {};
            event.track = t_.At(root).children[i];
            event.outer = t_.AddOuter();
            t_.AssignExtBuffer(event.track, event.outer);
            t_.OnExtSwitch(event.track, kEventSwitches);
            event.queue = AddQueue(event.track, 0);
        }

        m_.demo_track = t_.TrackHandle(kDemoTrack);
        if (m_.demo_track != kNone)
        {
            m_.demo_queue = AddQueue(m_.demo_track, 2);
            m_.demo_queue_set = true;
        }
    }

    // Jac_EventFrameCheck (0x2624e0).
    void EventFrameCheck()
    {
        if (DemoCheck() || PauseCheck())
        {
            return;
        }

        for (Engine::Model::Event& event : m_.events)
        {
            if (event.type == 0 || event.frames == 0)
            {
                continue;
            }

            event.frames--;
            if (event.frames == 0)
            {
                SendForce(event.queue, kStopActions);
            }
        }
    }

    // Jac_Gsync (0x2611e0): the options' levels once they change, then the events' frames. The rest of its work waits
    // for states that the model doesn't reach.
    void Gsync()
    {
        if (m_.levels_pending)
        {
            m_.levels_pending = false;
            SetLevels();
        }
        EventFrameCheck();
        m_.game_frames++;
    }

    // Jac_PlayBgm (0x2642e0) for the music's first record, with fn_263fe0's count of calls back to 0.
    bool PlayBgm(const Piece& piece, const Sequence& sequence)
    {
        if (m_.bgm.active)
        {
            m_.unmodelled++;
            return false;
        }

        const uint16_t number = std::max<uint16_t>(piece.file, 2);
        if (!ReadySeq(kMusicHandle, sequence))
        {
            return false;
        }

        Engine::Model::Bgm& bgm = m_.bgm;
        bgm.active = true;
        bgm.needs_init = true;
        for (uint32_t i = 0; i < 16; i++)
        {
            bgm.targets[i] = (piece.playing_tracks >> i & 1) != 0 ? piece.volume : 0.0f;
        }
        m_.bgm_calls = 0;
        BgmFrameWork();

        const uint32_t root = HandleToSeq(kMusicHandle);
        t_.SetBankNumber(root, static_cast<uint8_t>(number));
        bgm.root_volume = piece.music_volume;
        t_.MuteTrack(root, false);
        t_.SetExtParam(root, OuterParam::kVolume, bgm.root_volume);
        t_.OnExtSwitch(root, OuterParam::kVolume);
        t_.OffExtSwitch(root, OuterParam::kTempo);
        t_.At(root).pause_status = Track::kMuteNoNotes | Track::kPauseChannels | Track::kPauseParser;
        PlaySeq(kMusicHandle);

        return true;
    }

    // Jac_PlaySystemSe (0x266530).
    void PlaySystemSe(uint16_t value)
    {
        if (!m_.system_queues)
        {
            const uint32_t track = t_.TrackHandle(kSystemTrack);
            if (track != kNone)
            {
                m_.system_queue = AddQueue(track, 0);
                m_.system_port1_queue = AddQueue(track, 1);
                m_.system_queues = true;
            }
        }

        switch (value)
        {
        case kPause:
            if (!m_.cmenu)
            {
                PauseEffects();
            }
            m_.paused = true;
            break;

        case kEveningAlert:
            break;

        case kCountdown:
            m_.countdowns++;
            // At kCountdownLimit, fn_264020 starts a fade of the music's records: neither plays.
            break;

        case kCmenuOn:
            m_.cmenu = true;
            Formation(0, 0);
            if (m_.captain != kNone)
            {
                t_.PauseTrack(m_.captain);
            }
            PauseEvents();
            break;

        case kCmenuOff:
            if (!m_.cmenu)
            {
                return;
            }

            m_.cmenu = false;
            UnpauseEffects();

            for (const uint16_t port1 : kCmenuOffPort1)
            {
                Send(m_.system_port1_queue, port1);
            }
            break;

        case kLifeDim:
        case kViewChange:
            if (DemoCheck() || PauseCheck() || m_.scene != kCourseScene)
            {
                return;
            }
            break;

        case kUnpause:
            if (!m_.paused)
            {
                m_.countdowns = 0;
                return;
            }
            m_.unmodelled++;
            return;

        case kContainerFilled:
            PlayOrimaSe(kContainerSound);
            return;

        case kShipPartEffect:
            m_.banks.SetScene(kPartSystem, kShipPartScene, true);
            WritePort(t_.TrackHandle(kCutsceneRoot), 0, kShipPart);
            return;

        case kPlayerDownPause:
            PauseEvents();
            Formation(0, 0);
            m_.paused = true;
            return;

        case kHighScore:
        case kPauseAll:
        case kResumeAll:
            m_.unmodelled++;
            return;

        default:
            break;
        }

        if (m_.system_queues)
        {
            Send(m_.system_queue, value);
        }
    }

    // The pauses of kPause: the events' track and its children, the captain's track (Jac_PauseOrimaSe), and the
    // formation's sound.
    void PauseEffects()
    {
        PauseEvents();
        if (m_.captain != kNone)
        {
            t_.PauseTrack(m_.captain);
        }
        Formation(0, 0);
    }

    void PauseEvents()
    {
        const uint32_t root = t_.TrackHandle(kEventRoot);
        if (root != kNone)
        {
            t_.PauseTrack(root);
        }
    }

    // The unpauses of kCmenuOff: the events' track and its children, and the captain's track (Jac_UnPauseOrimaSe).
    void UnpauseEffects()
    {
        const uint32_t root = t_.TrackHandle(kEventRoot);
        if (root != kNone)
        {
            t_.UnpauseTrack(root);
        }

        if (m_.captain != kNone)
        {
            t_.UnpauseTrack(m_.captain);
        }
    }

    // Jac_PlayOrimaSe (0x265250): an effect of the captain goes to the queue of port 0 of 0x1000a, one of the Pikmin
    // to port 1. The pluck draws its cry. The engine writes the cry that the effect gives.
    bool PlayOrimaSe(uint16_t value)
    {
        if (m_.captain == kNone)
        {
            m_.captain = t_.TrackHandle(kCaptainTrack);
            if (m_.captain == kNone)
            {
                return false;
            }
        }
        if (m_.captain_queue == kNone)
        {
            m_.captain_queue = AddQueue(m_.captain, 0);
        }

        if ((value & kPikminEffect) != 0)
        {
            if (DemoCheck() || (value == kFlyReady && m_.fly_ready))
            {
                return false;
            }
            WritePort(m_.captain, 1, value & 0x7fff);
            if (value == kFly || value == kFlyReady)
            {
                m_.fly_ready = value == kFlyReady;
            }
            return true;
        }

        Send(m_.captain_queue, value);
        if (value == kPluck)
        {
            m_.random.ULimit(4);
        }
        if (value == kPlayerDown)
        {
            // fn_264020 starts a fade of each of the music's two records (no music plays), then the system's
            // effect 40 pauses the events and stops the formation (0x265354).
            PlaySystemSe(kPlayerDownPause);
        }

        return true;
    }

    // Jac_Orima_Formation (0x2650a0): the stick's place across and its distance from the centre.
    void Formation(int32_t x, int32_t y)
    {
        if (m_.formation == kNone)
        {
            m_.formation = t_.TrackHandle(kFormationTrack);
            if (m_.formation == kNone)
            {
                return;
            }

            if (m_.formation_outer == kNone)
            {
                m_.formation_outer = t_.AddOuter();
            }
            else
            {
                t_.InitExtBuffer(m_.formation_outer);
            }
            m_.formation_assigned = false;
        }
        if (!m_.formation_assigned)
        {
            m_.formation_assigned = t_.AssignExtBuffer(m_.formation, m_.formation_outer);
            if (m_.formation_assigned)
            {
                t_.OnExtSwitchD(m_.formation_outer, OuterParam::kVolume);
            }
        }

        if (DemoCheck() || PauseCheck())
        {
            x = 0;
            y = 0;
        }
        t_.SetExtParamD(m_.formation_outer, OuterParam::kVolume, kFormationVolume);
        x = std::min(std::max(x, -kStickLimit), kStickLimit);
        y = std::min(std::max(y, -kStickLimit), kStickLimit);
        const auto distance = static_cast<int32_t>(std::sqrt(static_cast<float>(x * x + y * y)));
        WritePort(m_.formation, 2, static_cast<uint16_t>(x));
        WritePort(m_.formation, 3, static_cast<uint16_t>(distance));
        if ((x | distance) != 0)
        {
            if (!m_.formation_on)
            {
                WritePort(m_.formation, 0, 1);
                m_.formation_on = true;
            }
            m_.idle_frames = 0;
        }
        else if (m_.formation_on)
        {
            WritePort(m_.formation, 0, 0);
            m_.formation_on = false;
        }
    }

    // Jac_Orima_Walk (0x265010).
    void Walk(uint16_t value)
    {
        m_.idle_frames = 0;
        if (m_.walk == kNone)
        {
            m_.walk = t_.TrackHandle(kWalkTrack);
            if (m_.walk == kNone)
            {
                return;
            }
        }
        if (!DemoCheck())
        {
            WritePort(m_.walk, 0, value);
        }
    }

    // Jac_UpdatePikiGaya (0x265560).
    void UpdateCrowd()
    {
        if (m_.crowd_outer == kNone)
        {
            m_.crowd_outer = t_.AddOuter();
        }

        if (m_.scene != kCourseScene || DemoCheck())
        {
            t_.SetExtParamD(m_.crowd_outer, OuterParam::kVolume, 0.0f);
            return;
        }

        if (!m_.crowd_assigned)
        {
            m_.crowd = t_.TrackHandle(kCrowdTrack);
            t_.InitExtBuffer(m_.crowd_outer);
            m_.crowd_assigned = t_.AssignExtBuffer(m_.crowd, m_.crowd_outer);
            if (!m_.crowd_assigned)
            {
                return;
            }
            t_.OnExtSwitchD(m_.crowd_outer, OuterParam::kVolume);
        }

        if (m_.idle_frames >= kCrowdFadeOut && m_.crowd_size != 0)
        {
            if (m_.idle_frames >= kCrowdFadeIn)
            {
                WritePort(m_.crowd, 0, static_cast<uint16_t>(m_.crowd_size));
                m_.crowd_volume = std::fmin(m_.crowd_volume + kCrowdStep, 1.0f);
                t_.SetExtParamD(m_.crowd_outer, OuterParam::kVolume, m_.crowd_volume);
            }
        }
        else
        {
            t_.SetExtParamD(m_.crowd_outer, OuterParam::kVolume, m_.crowd_volume);
            m_.crowd_volume = std::fmax(m_.crowd_volume - kCrowdStep, 0.0f);
            if (m_.crowd_size == 0)
            {
                WritePort(m_.crowd, 0, 0);
            }
        }
        m_.idle_frames++;
    }

    // Jac_CreateEvent (0x2626d0) at the listener. An event of type 7 starts its first action as it's created. The
    // engine queues the first value that the effect gives.
    int32_t CreateEvent(uint8_t type, uint16_t first)
    {
        if (type == 0 || DemoCheck())
        {
            return -1;
        }

        for (std::size_t i = 0; i < kEvents; i++)
        {
            Engine::Model::Event& event = m_.events[i];
            if (event.type != 0)
            {
                continue;
            }

            event.type = type;
            event.frames = kEventFrames;
            if (first != Effect::kNone)
            {
                SendForce(event.queue, first);
            }
            return static_cast<int32_t>(i);
        }

        return -1;
    }

    // Jac_UpdateEventPosition (0x2635d0) at the listener.
    void UpdateEventPosition(int32_t index)
    {
        if (index >= 0 && m_.events[static_cast<std::size_t>(index)].type != 0)
        {
            m_.events[static_cast<std::size_t>(index)].frames = kEventFrames;
        }
    }

    // Jac_UpdateCamera (0x262570): each event's volume and pan from its place, as a port command for the task list.
    // An event at the listener has a distance of 0, and an angle of atan2(0, 0) plus V3D_GetAngle's offset. Then
    // Jac_UpdatePikiGaya.
    void UpdateCamera()
    {
        for (std::size_t i = 0; i < kEvents; i++)
        {
            Engine::Model::Event& event = m_.events[i];
            if (event.type == 0)
            {
                continue;
            }

            const float distance = std::fmin(0.0f * kEventDistanceScale[event.type & 7], 1.0f);
            event.volume = 1.0f - distance;
            float angle = static_cast<float>(static_cast<double>(std::atan2(0.0f, 0.0f)) + kAngleOffset);
            if (!(angle < kHalfTurn))
            {
                angle = angle + kTurn;
            }
            float pan = 0.5f - angle / kHalfTurn;
            if (pan > 1.0f)
            {
                pan = 2.0f - pan;
            }
            if (pan < 0.0f)
            {
                pan = -pan;
            }
            event.pan = pan;
            if (std::find(m_.port_commands.begin(), m_.port_commands.end(), i) == m_.port_commands.end())
            {
                m_.port_commands.push_back(static_cast<uint32_t>(i));
            }
        }
        UpdateCrowd();
    }

    // Jac_StartDemo (0x261560) as the engine plays a cutscene's sound: the cutscene's number to ports 0 of 0x1000f, and
    // to port 1 for a cutscene of type 1. Jac_StartDemo's other work depends on the cutscene's settings in the game's
    // table, and the model leaves it out.
    void StartDemo(uint16_t cutscene, bool port1)
    {
        if (!m_.demo_queue_set)
        {
            m_.demo_track = t_.TrackHandle(kDemoTrack);
            if (m_.demo_track == kNone)
            {
                return;
            }
            m_.demo_queue = AddQueue(m_.demo_track, 2);
            m_.demo_queue_set = true;
        }
        m_.demo = cutscene;
        WritePort(m_.demo_track, 0, cutscene);
        if (port1)
        {
            WritePort(m_.demo_track, 1, cutscene);
        }
        Formation(0, 0);
    }

    // Jac_DemoSound, from Jac_DemoFrame's timed event.
    void DemoSound(uint16_t value, bool unheard)
    {
        if (m_.demo != -1)
        {
            Send(m_.demo_queue, value, unheard);
        }
    }

    // Jac_FinishDemo (0x261f90), from Jac_DemoFrame's last timed event: __Jac_FinishDemo (0x262010) writes 0 to port 0
    // of 0x1000f, and the cutscene ends. Its other work depends on the cutscene's settings in the game's table: the
    // music's fade, the events' pause, the formation and the cutscene's stream. The model leaves it out.
    void FinishDemo()
    {
        if (m_.demo != -1)
        {
            WritePort(m_.demo_track, 0, 0);
            m_.demo = -1;
        }
    }

private:
    Engine::Model& m_;
    Tracks& t_;
};

// The tracks' opens and notes as a part or an effect starts.
void TakeStartCounts(Engine::Model& m)
{
    for (uint32_t i = 0; i < Tracks::kAllTracks; i++)
    {
        m.start_opens[i] = m.tracks.Opens(i);
        m.start_notes[i] = m.tracks.NoteCount(i);
    }
}

// The game's frames from a cutscene's sound to its last step: the cutscene's finish, or the last of its later events.
uint32_t LastDemoStep(const Engine::Model& m)
{
    uint32_t last = m.effect.finish;
    for (const DemoEvent& event : m.demo_events)
    {
        last = std::max<uint32_t>(last, event.frames);
    }

    return last;
}

// Whether a part's or an effect's code has a step still to take, or a value that its tracks haven't read: the action of
// an object's effect in frame 2, the sound of a cutscene in frame 1, its later events and the cutscene's finish, a
// value in a queue or on a queue's port, or a value on port 0 or 1. A value on port 0 or 1 starts an interrupt of the
// track. A track reads a queue's port as it polls, and its other ports only when its sequence asks.
//
// The formation's and the crowd's code runs in every frame for as long as the game holds the stick or keeps the crowd.
// Their sounds play for as long as that code runs.
bool SoundWaits(const Engine::Model& m)
{
    if (m.sound == Engine::Model::Sound::kEffect &&
        ((m.effect.kind == Effect::kEvent && m.effect_frame <= kEventActionFrame) ||
         m.effect.kind == Effect::kFormation || m.effect.kind == Effect::kCrowd ||
         (m.effect.kind == Effect::kCutscene && m.effect_frame <= kDemoSoundFrame + LastDemoStep(m))))
    {
        return true;
    }

    for (const uint32_t queue : m.sound_queues)
    {
        const Engine::Model::PortQueue& q = m.queues[queue];
        if (!q.values.empty() || (q.track != kNone && m.tracks.At(q.track).ports[q.port].import_flag == 1))
        {
            return true;
        }
    }

    const auto unread = [&m](const std::pair<uint32_t, uint32_t>& place)
    {
        return place.second <= 1 && m.tracks.At(place.first).ports[place.second].import_flag == 1;
    };

    return std::any_of(m.sound_ports.begin(), m.sound_ports.end(), unread);
}

} // namespace

bool Engine::Playing() const
{
    const Model& m = *model_;
    switch (m.sound)
    {
    case Model::Sound::kNone:
        return false;
    case Model::Sound::kPiece:
        return m.tracks.At(m.root).state != Track::kClosed;
    default:
        break;
    }

    // A part or an effect plays on tracks that keep running after it: it plays while a track of theirs has opened or
    // started a note since it started.
    if (SoundWaits(m))
    {
        return true;
    }

    const std::vector<uint32_t> order = TreeOrder();
    const auto changed = [&m](uint32_t index)
    {
        return m.tracks.Opens(index) != m.start_opens[index] || m.tracks.NoteCount(index) != m.start_notes[index];
    };
    return std::any_of(order.begin(), order.end(), changed);
}

std::optional<std::string> Engine::StartUp(const Startup& startup)
{
    Model& m = *model_;
    if (m.start != Model::Start::kNone)
    {
        return std::string("the start-up has run");
    }

    m.startup = startup;
    Game game(m);

    // Jac_InitBgm (0x263e80): the four sequences, each on its handle once Jaf_LoadSeq has read it. A file whose index
    // entry gives no data loads nothing.
    for (uint32_t handle = 0; handle < startup.files.size(); handle++)
    {
        const SoundFiles::SequenceFile* file = files_.FindSequence(startup.files[handle]);
        if (file == nullptr)
        {
            return "the sequence archive has no file " + std::to_string(startup.files[handle]);
        }
        if (file->sequence->Size() == 0)
        {
            continue;
        }
        if (!game.ReadySeq(handle, *file->sequence))
        {
            return std::string("no root track is free");
        }

        game.PlaySeq(handle);
    }
    m.tracks.RegisterPlayerCall([this]() { return Game(*model_).BgmCallback(); });

    // The scenes, then the task list and Jal's queues in the player's list.
    for (const SceneLoad& load : startup.loads)
    {
        m.banks.SetScene(load.scene.system, load.scene.scene, load.set);
    }
    m.tracks.RegisterPlayerCall([this]() { return Game(*model_).PortCommands(); });
    m.tracks.RegisterPlayerCall([this]() { return Game(*model_).PortQueues(); });

    m.levels_pending = startup.saved;
    m.start = Model::Start::kWaiting;

    return std::nullopt;
}

uint32_t Engine::GameFrames() const
{
    return model_->game_frames;
}

std::optional<std::string> Engine::StartPiece(const Piece& piece)
{
    Model& m = *model_;
    if (m.sound != Model::Sound::kNone)
    {
        return std::string("a sound has started");
    }

    const SoundFiles::SequenceFile* file = files_.FindSequence(piece.file);
    if (file == nullptr)
    {
        return "the sequence archive has no file " + std::to_string(piece.file);
    }

    m.banks.SetScene(piece.scene.system, piece.scene.scene, true);
    if (!Game(m).PlayBgm(piece, *file->sequence))
    {
        return std::string("no root track is free");
    }

    m.sound = Model::Sound::kPiece;
    m.root = Tracks::kTracks + kMusicHandle;

    return std::nullopt;
}

std::optional<std::string> Engine::StartPart(uint16_t number, const Scene& scene)
{
    Model& m = *model_;
    if (m.sound != Model::Sound::kNone)
    {
        return std::string("a sound has started");
    }

    m.banks.SetScene(scene.system, scene.scene, true);
    Game game(m);
    TakeStartCounts(m);
    m.recording = true;
    game.WritePort(m.tracks.TrackHandle(kCutsceneRoot), 0, number);
    m.recording = false;
    m.sound = Model::Sound::kPart;
    m.root = m.tracks.TrackHandle(kCutsceneRoot);

    return std::nullopt;
}

std::optional<std::string> Engine::StartEffect(const Effect& effect)
{
    Model& m = *model_;
    if (m.sound != Model::Sound::kNone)
    {
        return std::string("a sound has started");
    }

    m.sound = Model::Sound::kEffect;
    m.effect = effect;
    m.effect_frame = 0;
    TakeStartCounts(m);
    if (effect.kind == Effect::kSystem)
    {
        // A system's effect in a course. Jac_PlaySystemSe queues kLifeDim and kViewChange only there. kCmenuOff plays
        // as the C menu closes. The model opens the menu by its flag alone, without kCmenuOn's effect and pauses.
        m.scene = kCourseScene;
        m.cmenu = effect.value == kCmenuOff;
    }
    if (effect.kind == Effect::kCrowd)
    {
        // A crowd in a course, after 240 frames without a sound of the captain, at full volume.
        m.scene = kCourseScene;
        m.idle_frames = kCrowdFadeIn;
        m.crowd_volume = 1.0f;
        m.crowd_size = effect.value;
    }

    EffectFrame();

    return std::nullopt;
}

void Engine::QueueDemoEvents(std::vector<DemoEvent> events, bool unheard)
{
    model_->demo_events = std::move(events);
    model_->demo_events_unheard = unheard;
}

// The effect's work in its frame `m.effect_frame`.
void Engine::EffectFrame()
{
    Model& m = *model_;
    Game game(m);
    const Effect& e = m.effect;
    const uint32_t frame = m.effect_frame++;
    m.recording = true;
    switch (e.kind)
    {
    case Effect::kSystem:
        if (frame == 0)
        {
            game.PlaySystemSe(e.value);
        }
        break;

    case Effect::kCaptain:
        if (frame == 0 && game.PlayOrimaSe(e.value) && e.first != Effect::kNone)
        {
            game.WritePort(m.captain, 1, e.first & 0x7fff);
        }
        break;

    case Effect::kPikmin:
        if (frame == 0)
        {
            game.PlayOrimaSe(static_cast<uint16_t>(e.value | kPikminEffect));
        }
        break;

    case Effect::kEvent:
        // The event, then its place and the camera in each frame. The action starts two frames later, after the
        // frame's update of the place and the camera. An event of type 7 with no first value plays its value as it's
        // created: the effect is the action that Jac_CreateEvent starts, and no action follows.
        if (frame == 0)
        {
            m.event = game.CreateEvent(e.type, e.type == kShipEvent && e.first == Effect::kNone ? e.value : e.first);
        }
        game.UpdateEventPosition(m.event);
        game.UpdateCamera();
        if (frame == kEventActionFrame && m.event >= 0 && !(e.type == kShipEvent && e.first == Effect::kNone))
        {
            game.SendForce(m.events[static_cast<std::size_t>(m.event)].queue, e.value);
        }
        break;

    case Effect::kFormation:
        game.Formation(0, e.value);
        break;

    case Effect::kCrowd:
        game.UpdateCrowd();
        break;

    case Effect::kWalk:
        if (frame == 0)
        {
            game.Walk(e.value);
        }
        break;

    case Effect::kCutscene:
        if (frame == 0)
        {
            game.StartDemo(e.first, e.type == Effect::kCutscenePart);
        }
        if (frame == kDemoSoundFrame)
        {
            game.DemoSound(e.value, false);
        }
        for (const DemoEvent& event : m.demo_events)
        {
            if (frame == kDemoSoundFrame + event.frames)
            {
                game.DemoSound(event.value, m.demo_events_unheard);
            }
        }
        if (e.finish != 0 && frame == kDemoSoundFrame + e.finish)
        {
            game.FinishDemo();
        }
        break;

    default:
        m.unmodelled++;
        break;
    }
    m.recording = false;
}

void Engine::UpdateFrame()
{
    Model& m = *model_;
    UpdateDsp();
    m.subframes++;

    Game game(m);
    if (m.start == Model::Start::kWaiting && m.tracks.TrackHandle(kEventRoot) != kNone)
    {
        game.InitEvents();
        m.start = Model::Start::kDone;
    }
    if (m.start == Model::Start::kDone &&
        m.subframes * kGameFramesPerSecond >= (uint64_t{m.game_frames} + 1) * kSubframesPerSecond)
    {
        game.Gsync();
        if (m.sound == Model::Sound::kEffect)
        {
            EffectFrame();
        }
    }
}

} // namespace ntdlib::jac
