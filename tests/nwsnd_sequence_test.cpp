// SPDX-License-Identifier: MIT

// Tests of the sequence model, played on stand-in channels with each console's policy: a short looping sequence, its
// hooks, the parse limit, the Wii's clock, parser and call stack, and the commands and note starts that differ by
// console and build.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/envelope.h"
#include "ntdlib/nwsnd/sequence.h"
#include "ntdlib/nwsnd/sequence_impl.h"
#include "ntdlib/nwsnd/util.h"
#include "xsflib/testing/check.h"

// The test's types are in a named namespace. Templates explicitly instantiated on a type in an unnamed namespace have
// internal linkage, and GCC then warns about each of their members that no test calls.
namespace nwsnd_sequence_test
{

// A channel's envelope, with a record of the last value each setter took. -1 marks a setter that wasn't called.
template <typename Console>
struct RecordedEnvelope : ntdlib::nwsnd::CurveAdshr<Console>
{
    using Base = ntdlib::nwsnd::CurveAdshr<Console>;

    void SetAttack(int value)
    {
        attack = value;
        Base::SetAttack(value);
    }

    void SetDecay(int value)
    {
        decay = value;
        Base::SetDecay(value);
    }

    void SetSustain(int value)
    {
        sustain = value;
        Base::SetSustain(value);
    }

    void SetRelease(int value)
    {
        release = value;
        Base::SetRelease(value);
    }

    void SetHold(int value)
    {
        hold = value;
        Base::SetHold(value);
    }

    int attack = -1;
    int decay = -1;
    int sustain = -1;
    int release = -1;
    int hold = -1;
};

// A policy with stand-ins for the channels and the engine that the model's user supplies. With `kHooks`, the engine
// follows the sequence through the model's hooks and logs each call.
template <typename Console, bool kHooks = false>
struct TestPolicy : Console
{
    struct Engine;

    struct Channel
    {
        enum class CallbackStatus : uint32_t
        {
            kStopped = 0,
        };

        void Stop(Engine&)
        {
            active = false;
        }

        void Release(Engine&)
        {
            released = true;
        }

        void NoteOff(Engine&)
        {
            released = true;
        }

        // The engine passes the callback on to the owner's player.
        void CallChannelCallback(Engine& engine, CallbackStatus)
        {
            if (owner_track >= 0)
            {
                engine.player->ChannelEvent(engine, owner_track, *this);
            }
        }

        void Detach()
        {
            owner_track = -1;
        }

        bool WaveLoops() const
        {
            return false;
        }

        void SetSweepParam(float pitch, int32_t ticks, bool auto_update)
        {
            sweep_pitch = pitch;
            sweep_length = ticks;
            auto_sweep = auto_update;
        }

        void UpdateSweep(int32_t)
        {
        }

        RecordedEnvelope<Console> envelope;
        std::array<ntdlib::nwsnd::CurveLfo<Console>, 4> lfos;
        std::array<uint8_t, 4> lfo_types{};
        bool active = true;
        bool released = false;
        bool auto_sweep = true;
        bool release_priority_fix = false;
        uint8_t biquad_type = 0;
        float track_volume = 1.0f;
        float user_pitch = 1.0f;
        float lpf = 0.0f;
        float biquad_value = 0.0f;
        uint32_t output_line = 1;
        float bend = 0.0f;
        float sweep_pitch = 0.0f;
        int32_t sweep_length = 0;
        uint8_t silence_from = 255;
        uint8_t silence_to = 255;
        uint16_t silence_duration = 0;
        uint16_t silence_counter = 0;
        int32_t length = 0;
        uint32_t pan_mode = 0;
        uint32_t pan_curve = 0;
        uint8_t key = 60;
        uint8_t key_group = 0;
        float velocity_volume = 1.0f;
        float pan = 0.0f;
        float span = 0.0f;
        int owner_track = -1;
        ntdlib::nwsnd::ChannelId place = ntdlib::nwsnd::kNoChannel;
        ntdlib::nwsnd::ChannelId next_in_track = ntdlib::nwsnd::kNoChannel;
    };

    struct NoteOnInfo
    {
        uint32_t prg_no = 0;
        int key = 60;
        int velocity = 127;
        int32_t length = 0;
        int init_pan = 0;
        uint32_t priority = 0;
        int owner_player = -1;
        int owner_track = -1;
    };

    // The channels, each at the place it was made in.
    struct ChannelPool
    {
        Channel& At(ntdlib::nwsnd::ChannelId id)
        {
            return channels[static_cast<std::size_t>(id)];
        }

        const Channel& At(ntdlib::nwsnd::ChannelId id) const
        {
            return channels[static_cast<std::size_t>(id)];
        }

        void Free(Channel& channel)
        {
            channel.active = false;
        }

        std::deque<Channel> channels;
    };

    // The engine makes a channel for each note and records the notes it gets. A channel's callback goes on to the
    // player that the engine holds. While `follow_loops` is true, the engine follows the sequence's loops and records
    // them. The counted loops in `endless` loop for ever.
    struct Engine
    {
        using Player = ntdlib::nwsnd::SequenceSoundPlayer<TestPolicy>;

        bool FollowsLoops() const
        {
            return follow_loops;
        }

        void OnLoop(const ntdlib::nwsnd::SequenceSoundPlayer<TestPolicy>& looping, int track, uint32_t target)
        {
            loops.push_back(static_cast<uint32_t>(track) << 24 | target);
            quiet_at_loop.push_back(looping.Track(track) && looping.Track(track)->IsQuiet());
        }

        void OnTrackLoop(const ntdlib::nwsnd::SequenceSoundPlayer<TestPolicy>& looping, int track, uint32_t target)
        {
            track_loops.push_back(static_cast<uint32_t>(track) << 24 | target);
            idle_at_track_loop.push_back(looping.Track(track) && looping.Track(track)->IsIdle());
        }

        bool LoopsForEver(const ntdlib::nwsnd::SequenceSoundPlayer<TestPolicy>&, int track, uint32_t address) const
        {
            const uint32_t loop = static_cast<uint32_t>(track) << 24 | address;
            return std::find(endless.begin(), endless.end(), loop) != endless.end();
        }

        void OnTickStart(const Player&, uint64_t units_left)
            requires kHooks
        {
            log.push_back(units_left == Console::kFrameUnits ? "tick at a frame's start" : "tick");
            tick_times.push_back(units_left);
        }

        void OnTickEnd(Player&)
            requires kHooks
        {
            log.push_back("tick end");
        }

        void OnCommand(Player& from, int track, uint32_t command, int32_t, int32_t)
            requires kHooks
        {
            log.push_back("command " + std::to_string(track) + " " + std::to_string(command));
            if (on_command)
            {
                on_command(from, command);
            }
        }

        void OnCommandStart(Player&, int track, uint32_t offset, uint32_t command, bool conditional)
            requires kHooks
        {
            analysis.push_back("start " + std::to_string(track) + " " + std::to_string(offset) + " " +
                               std::to_string(command) + (conditional ? " if" : ""));
        }

        void OnExtendedCommand(Player&, int track, uint32_t command, int32_t arg1, int32_t arg2)
            requires kHooks
        {
            analysis.push_back("extended " + std::to_string(track) + " " + std::to_string(command) + " " +
                               std::to_string(arg1) + " " + std::to_string(arg2));
        }

        void OnRandomDraw(Player&, int track)
            requires kHooks
        {
            analysis.push_back("draw " + std::to_string(track));
        }

        void OnVariableRead(Player&, int track, int index)
            requires kHooks
        {
            analysis.push_back("read " + std::to_string(track) + " " + std::to_string(index));
        }

        bool StopsTick(Player&, int)
            requires kHooks
        {
            return stop_ticks;
        }

        void OnNoteCommand(Player&, int track, int key, int velocity, int32_t length)
            requires kHooks
        {
            analysis.push_back("note " + std::to_string(track) + " " + std::to_string(key) + " " +
                               std::to_string(velocity) + " " + std::to_string(length));
        }

        void OnNoteStart(Player&, int)
            requires kHooks
        {
            log.push_back("note start");
        }

        void OnNoteOn(Player&, int, Channel& channel)
            requires kHooks
        {
            log.push_back("note on " + std::to_string(channel.key));
        }

        void OnLegato(Player&, int, Channel&)
            requires kHooks
        {
            log.push_back("legato");
        }

        void OnRelease(Player&, int, Channel&)
            requires kHooks
        {
            log.push_back("release");
        }

        void OnLengthUpdated(Player&, int, Channel& channel)
            requires kHooks
        {
            log.push_back("length " + std::to_string(channel.length));
        }

        bool OnParseLimit(Player&, int track)
            requires kHooks
        {
            log.push_back("parse limit " + std::to_string(track));

            return stop_at_parse_limit;
        }

        void OnParseLimitEnd(Player&, int track)
            requires kHooks
        {
            log.push_back("parse limit end " + std::to_string(track));
        }

        ChannelPool& Channels()
        {
            return pool;
        }

        const ChannelPool& Channels() const
        {
            return pool;
        }

        ntdlib::nwsnd::Random& Rng()
        {
            return random;
        }

        std::array<int16_t, 16>& GlobalVariables()
        {
            return globals;
        }

        Channel* NoteOn(const std::vector<uint32_t>&, int, const NoteOnInfo& info)
        {
            Channel& channel = pool.channels.emplace_back();
            channel.place = static_cast<ntdlib::nwsnd::ChannelId>(pool.channels.size() - 1);
            channel.key = static_cast<uint8_t>(info.key);
            channel.key_group = key_group;
            channel.length = info.length;
            channel.owner_track = info.owner_track;
            notes.push_back(info.key);
            lengths.push_back(info.length);
            init_pans.push_back(info.init_pan);

            return &channel;
        }

        std::vector<std::string> log;
        std::vector<uint64_t> tick_times;  // each tick's units left in its frame (OnTickStart)
        std::vector<std::string> analysis; // the calls of the analysis hooks, from OnCommandStart to OnNoteCommand
        std::function<void(Player&, uint32_t)> on_command; // called with each command that OnCommand reports
        bool stop_ticks = false;                           // what StopsTick returns
        bool stop_at_parse_limit = false;

        // The key group that a bank's region would give every new channel.
        uint8_t key_group = 0;
        std::vector<int> notes;
        std::vector<int32_t> lengths;
        std::vector<int> init_pans;
        std::vector<uint32_t> loops;
        std::vector<bool> quiet_at_loop; // whether the looping track was quiet when the loop was reported
        std::vector<uint32_t> track_loops;    // each track's passes, as (track << 24) | the loop's start
        std::vector<bool> idle_at_track_loop; // whether each of those passes was idle
        bool follow_loops = true;
        std::vector<uint32_t> endless; // each (track << 24) | the loop's body
        ChannelPool pool;
        ntdlib::nwsnd::SequenceSoundPlayer<TestPolicy>* player = nullptr;
        ntdlib::nwsnd::Random random;
        std::array<int16_t, 16> globals{};
    };

    struct SoundInfo
    {
        uint8_t volume = 127;
        uint8_t pan_mode = 0;
        uint8_t pan_curve = 0;
        uint8_t channel_priority = 64;
        bool release_priority_fix = false;
        std::vector<uint32_t> banks;
        uint32_t allocate_track_flags = 1;
        uint32_t start_offset = 0;
    };

    struct Outputs
    {
        float pan = 0.0f;
        float span = 0.0f;
    };

    using Receiver = void;

    static Outputs CombineOutputs(const Outputs& player, const Outputs& track, float pan, float span, float,
                                  const std::array<float, 3>&)
    {
        return {(player.pan + pan) + track.pan, span};
    }

    static void SetOutputs(Channel& channel, const Outputs& outputs)
    {
        channel.pan = outputs.pan;
        channel.span = outputs.span;
    }

    static void SetFrontBypass(Engine&, Channel&, bool)
    {
    }

    static void SetReceiver(NoteOnInfo&, const Receiver*)
    {
    }
};

} // namespace nwsnd_sequence_test

using nwsnd_sequence_test::TestPolicy;

namespace
{

// Track 0 takes its allocation of tracks, plays C4 for 24 ticks, and jumps back to the note. The jump's offset is in
// the console's byte order.
std::vector<uint8_t> LoopingSequence(bool little_endian)
{
    std::vector<uint8_t> bytes = {0xFE, 0x01, 0x00, 0x3C, 0x64, 0x18, 0x89};
    const std::array<uint8_t, 3> offset =
        little_endian ? std::array<uint8_t, 3>{0x03, 0x00, 0x00} : std::array<uint8_t, 3>{0x00, 0x00, 0x03};
    bytes.insert(bytes.end(), offset.begin(), offset.end());
    bytes.push_back(0xFF);

    return bytes;
}

template <typename Console>
void TestLoop(const std::string& name)
{
    using Policy = TestPolicy<Console>;
    using Player = ntdlib::nwsnd::SequenceSoundPlayer<Policy>;
    // The player and its tracks copy as values. Their channels are linked by place.
    static_assert(std::is_copy_constructible_v<Player> && std::is_copy_assignable_v<Player>);
    typename Policy::Engine engine;
    const typename Policy::SoundInfo kInfo;
    const std::vector<uint8_t> data = LoopingSequence(Console::kLittleEndianArguments);
    Player player(0, kInfo, data);
    engine.player = &player;

    // A game can set the player's and the global variables before the start. The engine holds the global ones.
    player.SetVariable(engine, 3, 7);
    player.SetVariable(engine, 17, -2);
    player.SetVariable(engine, 48, 5);

    xsflib::testing::Check(engine.globals[1] == -2, name + ": a global variable set before the start");

    // 24 ticks at 120 beats a minute and 48 ticks a beat take 250 ms. A second covers four passes.
    player.Start();
    const int frames = static_cast<int>(1000 * 65536 / Console::kFrameUnits);
    for (int i = 0; i < frames; ++i)
    {
        player.Update(engine);
    }

    const std::vector<uint32_t>& loops = engine.loops;
    xsflib::testing::Check(engine.notes.size() == 4 || engine.notes.size() == 5,
                           name + ": four or five notes in a second, " + std::to_string(engine.notes.size()));
    xsflib::testing::Check(!engine.notes.empty() && engine.notes.front() == 60, name + ": the note is C4");
    xsflib::testing::Check(loops.size() + 1 == engine.notes.size(), name + ": a loop before each note after the first");
    xsflib::testing::Check(!loops.empty() && loops.front() == 3, name + ": track 0 loops back to offset 3");
    xsflib::testing::Check(player.IsActive(), name + ": the sequence plays on");

    // A copy of the engine and the player plays on as the original does. The copy holds no pointer into the original.
    const std::vector<int> first_second = engine.notes;
    typename Policy::Engine copied_engine = engine;
    Player copy = player;
    copied_engine.player = &copy;

    for (int i = 0; i < frames; ++i)
    {
        player.Update(engine);
        copy.Update(copied_engine);
    }

    xsflib::testing::Check(copied_engine.notes == engine.notes && copied_engine.loops == engine.loops &&
                               engine.notes.size() > first_second.size(),
                           name + ": a copy plays on as the original does");

    // An engine that doesn't follow the loops gets none, and the notes are the same.
    typename Policy::Engine quiet_engine;
    quiet_engine.follow_loops = false;
    Player quiet(0, kInfo, data);
    quiet_engine.player = &quiet;

    quiet.Start();
    for (int i = 0; i < frames; ++i)
    {
        quiet.Update(quiet_engine);
    }

    xsflib::testing::Check(quiet_engine.loops.empty() && quiet_engine.notes == first_second,
                           name + ": no loops while the engine doesn't follow them");
}

} // namespace

// Every member of the player and its tracks, built once for each test console. A member that no test calls still
// compiles.
template class ntdlib::nwsnd::SequenceTrack<TestPolicy<ntdlib::nwsnd::CafePolicy>>;
template class ntdlib::nwsnd::SequenceSoundPlayer<TestPolicy<ntdlib::nwsnd::CafePolicy>>;
template class ntdlib::nwsnd::SequenceTrack<TestPolicy<ntdlib::nwsnd::NxPolicy>>;
template class ntdlib::nwsnd::SequenceSoundPlayer<TestPolicy<ntdlib::nwsnd::NxPolicy>>;
template class ntdlib::nwsnd::SequenceTrack<TestPolicy<ntdlib::nwsnd::CafePolicy, true>>;
template class ntdlib::nwsnd::SequenceSoundPlayer<TestPolicy<ntdlib::nwsnd::CafePolicy, true>>;
template class ntdlib::nwsnd::SequenceTrack<TestPolicy<ntdlib::nwsnd::CtrPolicy>>;
template class ntdlib::nwsnd::SequenceSoundPlayer<TestPolicy<ntdlib::nwsnd::CtrPolicy>>;
template class ntdlib::nwsnd::SequenceTrack<TestPolicy<ntdlib::nwsnd::RevolutionPolicy>>;
template class ntdlib::nwsnd::SequenceSoundPlayer<TestPolicy<ntdlib::nwsnd::RevolutionPolicy>>;
template class ntdlib::nwsnd::SequenceTrack<TestPolicy<ntdlib::nwsnd::RevolutionPolicy, true>>;
template class ntdlib::nwsnd::SequenceSoundPlayer<TestPolicy<ntdlib::nwsnd::RevolutionPolicy, true>>;

// The hooks on the looping sequence: each tick starts and ends, the first at its frame's start. The note command starts
// a note. The ticks count the note down and release it. The jump back is a command.
template <typename Console>
void TestHooks(const std::string& name)
{
    using Policy = TestPolicy<Console, true>;
    using Player = ntdlib::nwsnd::SequenceSoundPlayer<Policy>;
    typename Policy::Engine engine;
    const typename Policy::SoundInfo kInfo;
    const std::vector<uint8_t> data = LoopingSequence(Console::kLittleEndianArguments);
    Player player(0, kInfo, data);
    engine.player = &player;

    player.Start();
    const int frames = static_cast<int>(300 * 65536 / Console::kFrameUnits);
    for (int i = 0; i < frames; ++i)
    {
        player.Update(engine);
    }

    const std::vector<std::string>& log = engine.log;
    const auto count = [&log](const std::string& entry)
    {
        return std::count(log.begin(), log.end(), entry);
    };

    xsflib::testing::Check(log.size() > 4 && log[0] == "tick at a frame's start" && log[1] == "note start" &&
                               log[2] == "note on 60" && log[3] == "tick end",
                           name + ": the first tick starts the note");
    xsflib::testing::Check(count("tick at a frame's start") + count("tick") == count("tick end") &&
                               count("tick end") == static_cast<std::ptrdiff_t>(player.TickCount()),
                           name + ": every tick starts and ends");
    xsflib::testing::Check(
        count("note on 60") == static_cast<std::ptrdiff_t>(engine.notes.size()) && count("note on 60") == 2,
        name + ": a hook for each of the two notes");
    xsflib::testing::Check(count("length 0") >= 1 && count("release") >= 1, name + ": the note runs out and releases");
    xsflib::testing::Check(count("command 0 137") == 1, name + ": the jump back is a command");
    xsflib::testing::Check(count("legato") == 0, name + ": no legato");
}

// A player that an engine steps a tick at a time (BeginTick, RunTrackTick, EndTick and UpdateTrackParams) plays as one
// that Update runs. The stepped player runs as many ticks in each frame as the other's clock does. Only the clock's
// OnTickStart is missing from its log.
template <typename Console>
void TestSteps(const std::string& name)
{
    using Policy = TestPolicy<Console, true>;
    using Player = ntdlib::nwsnd::SequenceSoundPlayer<Policy>;
    const typename Policy::SoundInfo kInfo;
    const std::vector<uint8_t> data = LoopingSequence(Console::kLittleEndianArguments);
    typename Policy::Engine clocked_engine;
    typename Policy::Engine stepped_engine;
    Player clocked(0, kInfo, data);
    Player stepped(0, kInfo, data);
    clocked_engine.player = &clocked;
    stepped_engine.player = &stepped;
    clocked.Start();
    stepped.Start();

    const int frames = static_cast<int>(300 * 65536 / Console::kFrameUnits);
    for (int frame = 0; frame < frames; ++frame)
    {
        const uint32_t before = clocked.TickCount();
        clocked.Update(clocked_engine);
        for (uint32_t tick = before; tick < clocked.TickCount(); ++tick)
        {
            stepped.BeginTick();
            bool any_open = false;
            for (int track = 0; track < 16; ++track)
            {
                any_open = stepped.RunTrackTick(stepped_engine, track) || any_open;
            }
            stepped.EndTick(stepped_engine, any_open);
        }

        stepped.UpdateTrackParams(stepped_engine);
    }

    std::vector<std::string> clocked_log;
    for (const std::string& entry : clocked_engine.log)
    {
        if (entry != "tick" && entry != "tick at a frame's start")
        {
            clocked_log.push_back(entry);
        }
    }

    xsflib::testing::Check(clocked.TickCount() > 0 && stepped.TickCount() == clocked.TickCount(),
                           name + ": the same ticks");
    xsflib::testing::Check(stepped_engine.log == clocked_log, name + ": the same hooks, in the same order");
    xsflib::testing::Check(
        stepped_engine.notes == clocked_engine.notes && stepped_engine.lengths == clocked_engine.lengths,
        name + ": the same notes");
    xsflib::testing::Check(stepped_engine.loops == clocked_engine.loops && !stepped_engine.loops.empty(),
                           name + ": the same loops");
}

// Track 0 jumps to the jump for ever, and never waits. The offset is in the console's byte order.
std::vector<uint8_t> EndlessSequence(bool little_endian)
{
    std::vector<uint8_t> bytes = {0xFE, 0x01, 0x00, 0x89};
    const std::array<uint8_t, 3> offset =
        little_endian ? std::array<uint8_t, 3>{0x03, 0x00, 0x00} : std::array<uint8_t, 3>{0x00, 0x00, 0x03};
    bytes.insert(bytes.end(), offset.begin(), offset.end());
    bytes.push_back(0xFF);

    return bytes;
}

// A track that never waits. Where the limit ends the track (`ends_track`, the Wii), the track first calls the engine's
// OnParseLimitEnd. The sequence then ends with the track. Elsewhere each tick ends at the limit, and the track runs on
// in the next tick. The engine's OnParseLimit can end the track instead.
template <typename Console>
void TestParseLimit(const std::string& name, bool ends_track)
{
    using Policy = TestPolicy<Console, true>;
    using Player = ntdlib::nwsnd::SequenceSoundPlayer<Policy>;
    const typename Policy::SoundInfo kInfo;
    const std::vector<uint8_t> data = EndlessSequence(Console::kLittleEndianArguments);
    for (const bool stop : {false, true})
    {
        typename Policy::Engine engine;
        engine.follow_loops = false;
        engine.stop_at_parse_limit = stop;
        Player player(0, kInfo, data);
        engine.player = &player;

        player.Start();
        for (int i = 0; i < 40; ++i)
        {
            player.Update(engine);
        }

        const std::ptrdiff_t limits = std::count(engine.log.begin(), engine.log.end(), "parse limit 0");
        const std::ptrdiff_t ends = std::count(engine.log.begin(), engine.log.end(), "parse limit end 0");
        if (ends_track)
        {
            xsflib::testing::Check(player.IsFinished() && player.TickCount() == 0 && limits == 0 && ends == 1,
                                   name + ": the track ends at the parse limit, and the sequence with it");
        }
        else if (stop)
        {
            xsflib::testing::Check(player.IsFinished() && limits == 1,
                                   name + ": the engine ends the track at the parse limit");
        }
        else
        {
            xsflib::testing::Check(player.IsActive() && player.TickCount() >= 2 &&
                                       limits == static_cast<std::ptrdiff_t>(player.TickCount()),
                                   name + ": each tick ends at the parse limit, and the track runs on");
        }
    }
}

// Track 0 sets the timebase and the tempo, then waits a tick at a time for ever. The arguments are big-endian, or
// little-endian with `little_endian`.
std::vector<uint8_t> ClockSequence(uint8_t timebase, uint16_t tempo, bool little_endian = false)
{
    const auto high = static_cast<uint8_t>(tempo >> 8);
    const auto low = static_cast<uint8_t>(tempo);
    if (little_endian)
    {
        return {0xFE, 0x01, 0x00, 0xB0, timebase, 0xE1, low, high, 0x80, 0x01, 0x89, 0x08, 0x00, 0x00, 0xFF};
    }

    return {0xFE, 0x00, 0x01, 0xB0, timebase, 0xE1, high, low, 0x80, 0x01, 0x89, 0x00, 0x00, 0x08, 0xFF};
}

// A player that an engine runs with the clock in steps (StartClock and NextTickTime) plays as one that Update runs: the
// same ticks in each frame, at the same units left in the frame. The first tick sets the sequence's timebase and tempo.
// The clock then runs at the new rate. Both players play as the library's build `build` does.
template <typename Console>
void TestClockSteps(const std::string& name, const typename Console::Build& build = {})
{
    using Policy = TestPolicy<Console, true>;
    using Player = ntdlib::nwsnd::SequenceSoundPlayer<Policy>;
    const std::array<std::pair<uint8_t, uint16_t>, 5> kClocks = {{{48, 120}, {96, 137}, {48, 61}, {120, 255}, {48, 0}}};
    for (const auto& [timebase, tempo] : kClocks)
    {
        const typename Policy::SoundInfo kInfo;
        const std::vector<uint8_t> data = ClockSequence(timebase, tempo, Console::kLittleEndianArguments);
        typename Policy::Engine clocked_engine;
        typename Policy::Engine stepped_engine;
        Player clocked(0, kInfo, data);
        Player stepped(0, kInfo, data);
        clocked_engine.player = &clocked;
        stepped_engine.player = &stepped;
        clocked.SetBuild(build);
        stepped.SetBuild(build);
        clocked.Start();
        stepped.Start();

        bool same_frames = true;
        for (int frame = 0; frame < 1000; ++frame)
        {
            clocked.Update(clocked_engine);
            stepped.StartClock(1);
            while (const std::optional<uint64_t> left = stepped.NextTickTime())
            {
                stepped_engine.tick_times.push_back(*left);
                stepped.BeginTick();
                bool any_open = false;
                for (int track = 0; track < 16; ++track)
                {
                    any_open = stepped.RunTrackTick(stepped_engine, track) || any_open;
                }
                stepped.EndTick(stepped_engine, any_open);
            }

            stepped.UpdateTrackParams(stepped_engine);
            same_frames = same_frames && stepped.TickCount() == clocked.TickCount();
        }

        const std::string clock =
            name + ", timebase " + std::to_string(timebase) + " and tempo " + std::to_string(tempo) + ": ";
        xsflib::testing::Check(same_frames && clocked.TickCount() > 0, clock + "the same ticks in each frame");
        xsflib::testing::Check(stepped_engine.tick_times == clocked_engine.tick_times, clock + "the same tick times");
        xsflib::testing::Check(stepped.ClockStopped() == (tempo == 0) && (tempo != 0 || clocked.TickCount() == 1),
                               clock + "a tempo of 0 stops the clock after the first tick");
    }
}

// Track 0 sets variable 0 to 1, waits 1 or 2 ticks at random, waits as many ticks as variable 0 holds, and plays key 60
// for 24 ticks. It then jumps back to the random wait if the last comparison held. The arguments are big-endian.
std::vector<uint8_t> AnalysisSequence()
{
    return {0xF0, 0x80, 0x00, 0x00, 0x01, 0xA0, 0x80, 0x00, 0x01, 0x00, 0x02, 0xA1,
            0x80, 0x00, 0x3C, 0x64, 0x18, 0xA2, 0x89, 0x00, 0x00, 0x05, 0xFF};
}

// The analysis hooks report, in order, each command's start at its offset, the extended command, the random draw, the
// variable read and the note command. A copy of the player made in the jump's OnCommand runs the rest of the tick
// (ContinueTrackTick) as the original does.
void TestAnalysisHooks()
{
    using Policy = TestPolicy<ntdlib::nwsnd::CafePolicy, true>;
    using Player = ntdlib::nwsnd::SequenceSoundPlayer<Policy>;
    typename Policy::Engine engine;
    const typename Policy::SoundInfo kInfo;
    const std::vector<uint8_t> data = AnalysisSequence();
    Player player(0, kInfo, data);
    engine.player = &player;

    std::optional<Player> copy;
    typename Policy::Engine copy_engine;
    std::size_t copied_at = 0;
    engine.on_command = [&](Player& from, uint32_t command)
    {
        if (command == 0x89 && !copy)
        {
            copy.emplace(from);
            copy_engine = engine;
            copy_engine.player = &*copy;
            copy_engine.analysis.clear();
            copy_engine.on_command = nullptr;
            copied_at = engine.analysis.size();
        }
    };

    player.Start();
    for (int tick = 0; tick < 200 && !copy; ++tick)
    {
        player.BeginTick();
        const bool open = player.RunTrackTick(engine, 0);
        player.EndTick(engine, open);
    }

    const bool copy_open = copy && copy->ContinueTrackTick(copy_engine, 0);

    const std::vector<std::string> kFirst = {"start 0 0 240", "extended 0 61568 0 1", "start 0 5 128",
                                             "draw 0",        "start 0 11 128",       "read 0 0",
                                             "start 0 14 60", "note 0 60 100 24",     "start 0 17 137 if"};
    const std::vector<std::string>& log = engine.analysis;
    xsflib::testing::Check(log.size() >= kFirst.size() && std::equal(kFirst.begin(), kFirst.end(), log.begin()),
                           "the analysis hooks, in order");
    xsflib::testing::Check(copy_open && copy->Track(0)->Current() == player.Track(0)->Current() &&
                               copy->Track(0)->Wait() == player.Track(0)->Wait(),
                           "a copy made at the jump ends the tick where the original does");
    xsflib::testing::Check(copy_engine.analysis == std::vector<std::string>(log.begin() + copied_at, log.end()) &&
                               copy_engine.analysis == std::vector<std::string>{"start 0 5 128", "draw 0"},
                           "the copy's hooks are the original's after the jump");
}

// An engine that stops the tick after the jump (StopsTick) leaves the track at the jump's target, with no wait. The
// rest of the tick then runs through ContinueTrackTick. The track ends the tick where one that the engine doesn't stop
// ends it.
void TestStopsTick()
{
    using Policy = TestPolicy<ntdlib::nwsnd::CafePolicy, true>;
    using Player = ntdlib::nwsnd::SequenceSoundPlayer<Policy>;
    const typename Policy::SoundInfo kInfo;
    const std::vector<uint8_t> data = AnalysisSequence();
    typename Policy::Engine engine;
    typename Policy::Engine stopping_engine;
    Player player(0, kInfo, data);
    Player stopped(0, kInfo, data);
    engine.player = &player;
    stopping_engine.player = &stopped;

    stopping_engine.on_command = [&stopping_engine](Player&, uint32_t command)
    {
        stopping_engine.stop_ticks = stopping_engine.stop_ticks || command == 0x89;
    };

    player.Start();
    stopped.Start();
    for (int tick = 0; tick < 200 && !stopping_engine.stop_ticks; ++tick)
    {
        player.BeginTick();
        player.EndTick(engine, player.RunTrackTick(engine, 0));
        stopped.BeginTick();
        stopped.EndTick(stopping_engine, stopped.RunTrackTick(stopping_engine, 0));
    }

    const uint32_t stop_current = stopped.Track(0)->Current();
    const int32_t stop_wait = stopped.Track(0)->Wait();
    stopping_engine.stop_ticks = false;
    const bool open = stopped.ContinueTrackTick(stopping_engine, 0);

    xsflib::testing::Check(stop_current == 5 && stop_wait == 0, "a stopped tick leaves the track at the jump's target");
    xsflib::testing::Check(open && stopped.Track(0)->Current() == player.Track(0)->Current() &&
                               stopped.Track(0)->Wait() == player.Track(0)->Wait(),
                           "the stopped track runs the rest of its tick as the other runs it");
}

// The Wii's two clocks: the ticks after 1, 4, 100, 1,000 and 10,000 frames, in float milliseconds and in the 416ths of
// a tick of the build of October 2006. The expected counts are fixed. In 416ths, the first frame adds the default tempo
// of 120: the first tick sets the sequence's tempo after the add.
void TestWiiClock()
{
    using Policy = TestPolicy<ntdlib::nwsnd::RevolutionPolicy>;
    using Player = ntdlib::nwsnd::SequenceSoundPlayer<Policy>;
    struct Case
    {
        bool october_2006;
        uint8_t timebase;
        uint16_t tempo;
        std::array<uint32_t, 5> ticks;
    };
    const Case kCases[] = {
        {false, 48, 120, {1, 2, 29, 288, 2880}}, {false, 96, 137, {1, 3, 66, 658, 6577}},
        {false, 48, 61, {1, 1, 15, 147, 1464}},  {false, 120, 255, {2, 7, 154, 1531, 15301}},
        {true, 48, 120, {1, 1, 29, 289, 2885}},  {true, 96, 137, {1, 1, 33, 329, 3293}},
        {true, 48, 255, {1, 2, 61, 613, 6129}},  {true, 48, 500, {1, 3, 119, 1200, 12018}},
    };
    const std::array<int, 5> kAt = {1, 4, 100, 1000, 10000};
    for (const Case& c : kCases)
    {
        typename Policy::Engine engine;
        const typename Policy::SoundInfo kInfo;
        const std::vector<uint8_t> data = ClockSequence(c.timebase, c.tempo);
        Player player(0, kInfo, data);
        engine.player = &player;
        ntdlib::SndBuild build;
        build.october_2006 = c.october_2006;
        player.SetBuild(build);

        player.Start();
        int frames = 0;
        for (std::size_t i = 0; i < kAt.size(); ++i)
        {
            for (; frames < kAt[i]; ++frames)
            {
                player.Update(engine);
            }

            xsflib::testing::Check(player.TickCount() == c.ticks[i],
                                   std::string("the Wii's clock") + (c.october_2006 ? " of October 2006" : "") +
                                       ", timebase " + std::to_string(c.timebase) + " and tempo " +
                                       std::to_string(c.tempo) + ": " + std::to_string(player.TickCount()) +
                                       " ticks after " + std::to_string(frames) + " frames, not " +
                                       std::to_string(c.ticks[i]));
        }
    }
}

// What a short sequence did: its notes' keys and lengths, whether a volume moved at the third frame, and whether the
// sequence ended. With the loops followed: the loops reported, and whether each looping track was quiet then, and every
// track's passes, and whether each was idle. At the end: whether track 0 had a note that ends, and whether the player
// was stuck and moving.
struct Played
{
    std::vector<int> keys;
    std::vector<int32_t> lengths;
    bool moving = false;
    bool finished = false;
    std::vector<uint32_t> loops;
    std::vector<bool> quiet_at_loop;
    std::vector<uint32_t> track_loops;
    std::vector<bool> idle_at_track_loop;
    bool ending_note = false;
    bool stuck = false;
    bool moving_at_end = false;
};

// Plays `data` for 30 frames on `Console`'s library of `build`, with the player's variable 0 at 48, and the tracks that
// `tracks` names a bit each. The engine follows the loops if `follow_loops` says so, and takes the counted loops in
// `endless` as endless.
template <typename Console>
Played Play(const std::vector<uint8_t>& data, const typename Console::Build& build = {}, uint32_t tracks = 1,
            bool follow_loops = false, const std::vector<uint32_t>& endless = {})
{
    using Policy = TestPolicy<Console>;
    using Player = ntdlib::nwsnd::SequenceSoundPlayer<Policy>;
    typename Policy::Engine engine;
    engine.follow_loops = follow_loops;
    engine.endless = endless;
    typename Policy::SoundInfo info;
    info.allocate_track_flags = tracks;
    Player player(0, info, data);
    engine.player = &player;
    player.SetBuild(build);
    player.SetVariable(engine, 0, 48);

    player.Start();
    Played played;
    for (int i = 0; i < 30; ++i)
    {
        player.Update(engine);
        if (i == 2)
        {
            played.moving = player.IsMoving();
        }
    }

    played.keys = engine.notes;
    played.lengths = engine.lengths;
    played.finished = player.IsFinished();
    played.loops = engine.loops;
    played.quiet_at_loop = engine.quiet_at_loop;
    played.track_loops = engine.track_loops;
    played.idle_at_track_loop = engine.idle_at_track_loop;
    played.ending_note = player.Track(0) && player.Track(0)->HasEndingNote(engine);
    played.stuck = player.IsStuck(engine);
    played.moving_at_end = player.IsMoving();

    return played;
}

// A loop of two passes plays C4 for a tick in each, and the sequence ends. The loop counts no loop of the sequence.
// Taken as endless (the engine's LoopsForEver), it plays on, and each pass counts as a loop at the loop's body.
template <typename Console>
void TestEndlessCountedLoop(const std::string& name)
{
    using xsflib::testing::Check;
    const std::vector<uint8_t> kCounted = {0xFE, 0x00, 0x01, 0xD4, 0x02, 0x3C, 0x64, 0x01, 0xFC, 0xFF};

    const Played twice = Play<Console>(kCounted, {}, 1, true);
    const Played endless = Play<Console>(kCounted, {}, 1, true, {5});

    Check(twice.keys == std::vector<int>{60, 60} && twice.loops.empty() && twice.finished,
          name + ": a counted loop plays its passes and counts no loop");
    Check(endless.keys.size() > 2 && !endless.finished, name + ": a counted loop taken as endless plays on");
    Check(!endless.loops.empty() && endless.loops.size() + 1 == endless.keys.size() &&
              std::all_of(endless.loops.begin(), endless.loops.end(), [](uint32_t loop) { return loop == 5; }),
          name + ": each pass of an endless counted loop counts as a loop at its body");
}

// Track 0 opens tracks 1 and 2, plays C4 for two ticks and loops. Track 1 plays E4 for three ticks and loops. Track 2
// waits a tick and loops, which changes nothing. The offsets are in the console's byte order.
std::vector<uint8_t> TrackLoopsSequence(bool little_endian)
{
    // An offset below 256, in three bytes.
    const auto offset = [little_endian](uint8_t value)
    {
        return little_endian ? std::vector<uint8_t>{value, 0, 0} : std::vector<uint8_t>{0, 0, value};
    };

    // Track 0 opens the others. Tracks 0, 1 and 2 then start at offsets 10, 17 and 24.
    const std::vector<std::vector<uint8_t>> parts = {{0x88, 0x01},
                                                     offset(17),
                                                     {0x88, 0x02},
                                                     offset(24),
                                                     {0x3C, 0x64, 0x02, 0x89},
                                                     offset(10),
                                                     {0x40, 0x64, 0x03, 0x89},
                                                     offset(17),
                                                     {0x80, 0x01, 0x89},
                                                     offset(24)};
    std::vector<uint8_t> bytes;
    for (const std::vector<uint8_t>& part : parts)
    {
        bytes.insert(bytes.end(), part.begin(), part.end());
    }

    return bytes;
}

// The engine's OnLoop gets only track 0's loops of TrackLoopsSequence. Its OnTrackLoop gets the passes of all three
// tracks, each at the start of its track's loop, and only track 2's passes are idle.
template <typename Console>
void TestTrackLoops(const std::string& name)
{
    using xsflib::testing::Check;

    const Played played = Play<Console>(TrackLoopsSequence(Console::kLittleEndianArguments), {}, 0x7, true);

    // Each track's passes, the idle ones among them, and the passes that went back elsewhere.
    constexpr std::array<uint32_t, 3> kStarts = {10, 17, 24};
    std::array<int, 3> passes{};
    std::array<int, 3> idle{};
    int elsewhere = 0;
    for (std::size_t i = 0; i < played.track_loops.size() && i < played.idle_at_track_loop.size(); ++i)
    {
        const uint32_t track = played.track_loops[i] >> 24;
        if (track >= kStarts.size() || (played.track_loops[i] & 0xFFFFFF) != kStarts[track])
        {
            ++elsewhere;
            continue;
        }

        ++passes[track];
        idle[track] += played.idle_at_track_loop[i] ? 1 : 0;
    }

    const auto track_0 = [](uint32_t loop)
    {
        return loop == 10;
    };
    Check(!played.loops.empty() && std::all_of(played.loops.begin(), played.loops.end(), track_0),
          name + ": the loops of the sequence are track 0's");
    Check(passes[0] == static_cast<int>(played.loops.size()) && passes[1] >= 2 && passes[2] >= 2 && elsewhere == 0 &&
              played.track_loops.size() == played.idle_at_track_loop.size(),
          name + ": every track's passes reach OnTrackLoop");
    Check(idle[0] == 0 && idle[1] == 0 && idle[2] == passes[2],
          name + ": only the passes that change nothing are idle");
}

// The Wii's parser against the Wii U's parser, on sequences in big-endian order.
void TestWiiParser()
{
    using ntdlib::nwsnd::CafePolicy;
    using ntdlib::nwsnd::RevolutionPolicy;
    using xsflib::testing::Check;
    ntdlib::SndBuild wii_2007;
    ntdlib::SndBuild wii_2008;
    wii_2008.date = 20080308;

    // A random prefix, then a variable prefix, before a note of key 60 whose length is variable 0. On the Wii both
    // prefixes apply. On the Wii U the variable prefix is the command, and the note's length is the byte 0.
    const std::vector<uint8_t> kChained = {0xFE, 0x00, 0x01, 0xA0, 0xA1, 0x3C, 0x64, 0x00, 0x80, 0x7F, 0xFF};

    const Played wii_chained = Play<RevolutionPolicy>(kChained, wii_2007);
    Check(wii_chained.keys == std::vector<int>{60} && wii_chained.lengths == std::vector<int32_t>{48},
          "the Wii: a variable prefix after a random one gives the note's length");
    const Played cafe_chained = Play<CafePolicy>(kChained);
    Check(cafe_chained.keys == std::vector<int>{60} && cafe_chained.lengths == std::vector<int32_t>{-1},
          "the Wii U: a variable prefix after a random one is a command");

    // A time prefix of 16 ticks before a volume of 100, then 00 10 80 7F. The Wii's builds from 2008 and the Wii U move
    // the volume. The Wii's older builds run the prefix as a command that does nothing, set the volume at once, and
    // play 00 10 80 7F as a note of key 0 and length 127.
    const std::vector<uint8_t> kTimed = {0xFE, 0x00, 0x01, 0xA3, 0xC1, 0x64, 0x00, 0x10, 0x80, 0x7F, 0xFF};

    const Played wii_2008_timed = Play<RevolutionPolicy>(kTimed, wii_2008);
    Check(wii_2008_timed.moving && wii_2008_timed.keys.empty(), "the Wii of 2008: a time prefix moves the volume");
    const Played wii_2007_timed = Play<RevolutionPolicy>(kTimed, wii_2007);
    Check(!wii_2007_timed.moving && wii_2007_timed.keys == std::vector<int>{0} &&
              wii_2007_timed.lengths == std::vector<int32_t>{127},
          "the Wii of 2007: a time prefix is a command that does nothing");
    const Played cafe_timed = Play<CafePolicy>(kTimed);
    Check(cafe_timed.moving && cafe_timed.keys.empty(), "the Wii U: a time prefix moves the volume");

    // Variable 0 isn't 1. The condition of the end is then false. The Wii ends the track there anyway. The Wii U reads
    // on to the note.
    const std::vector<uint8_t> kEnding = {0xFE, 0x00, 0x01, 0xF0, 0x90, 0x00, 0x00,
                                          0x01, 0xA2, 0xFF, 0x3C, 0x64, 0x18, 0xFF};

    const Played wii_ending = Play<RevolutionPolicy>(kEnding, wii_2007);
    Check(wii_ending.finished && wii_ending.keys.empty(), "the Wii: the end ends the track after a false condition");
    const Played cafe_ending = Play<CafePolicy>(kEnding);
    Check(cafe_ending.keys == std::vector<int>{60}, "the Wii U: a false condition skips the end");

    // F0 B5 is no command. The Wii reads no byte after it, and plays key 60. The Wii U reads 3C as its byte.
    const std::vector<uint8_t> kExtended = {0xFE, 0x00, 0x01, 0xF0, 0xB5, 0x3C, 0x64, 0x18, 0xFF};

    Check(Play<RevolutionPolicy>(kExtended, wii_2008).keys == std::vector<int>{60},
          "the Wii: no byte after F0 with 0xA0 to 0xBF");
    Check(Play<CafePolicy>(kExtended).keys == std::vector<int>{100}, "the Wii U: a byte after F0 with 0xA0 to 0xBF");
}

// The Wii's call stack against the Wii U's call stack, on sequences in big-endian order.
void TestWiiCallStack()
{
    using ntdlib::nwsnd::CafePolicy;
    using ntdlib::nwsnd::RevolutionPolicy;
    using xsflib::testing::Check;

    // A call to a subroutine that starts a loop, plays key 64 and returns, before key 60 and the end. The notes last a
    // tick. On the Wii, the return takes the loop's entry first, and key 64 plays again. The Wii U returns past the
    // loop at once.
    const std::vector<uint8_t> kReturning = {0xFE, 0x00, 0x01, 0x8A, 0x00, 0x00, 0x0B, 0x3C, 0x64,
                                             0x01, 0xFF, 0xD4, 0x02, 0x40, 0x64, 0x01, 0xFD};

    Check(Play<RevolutionPolicy>(kReturning).keys == std::vector<int>{64, 64, 60},
          "the Wii: a return takes a loop's entry");
    Check(Play<CafePolicy>(kReturning).keys == std::vector<int>{64, 60}, "the Wii U: a return skips a loop's entry");

    // A loop of one pass leaves its count of 1 at level 0. A call to a subroutine follows, and the subroutine ends a
    // loop, plays key 64 and returns, before key 60. On the Wii, the loop end takes the call's entry with that count,
    // ends it, and the return then finds nothing. The Wii U's loop end ignores the call's entry.
    const std::vector<uint8_t> kEnding = {0xFE, 0x00, 0x01, 0xD4, 0x01, 0xFC, 0x8A, 0x00, 0x00, 0x0E,
                                          0x3C, 0x64, 0x01, 0xFF, 0xFC, 0x40, 0x64, 0x01, 0xFD, 0xFF};

    Check(Play<RevolutionPolicy>(kEnding).keys == std::vector<int>{64}, "the Wii: a loop end takes a call's entry");
    Check(Play<CafePolicy>(kEnding).keys == std::vector<int>{64, 60}, "the Wii U: a loop end ignores a call's entry");

    // Track 0 opens track 1 at a wait of 48 ticks, and opens it again 5 ticks later at a note. On the Wii, track 1
    // keeps its wait, and the note comes after the 30 frames. The Wii U clears the wait, and the note plays at once.
    const std::vector<uint8_t> kOpening = {0xFE, 0x03, 0x00, 0x88, 0x01, 0x00, 0x00, 0x14, 0x80, 0x05,
                                           0x88, 0x01, 0x00, 0x00, 0x18, 0x80, 0x7F, 0xFF, 0xFF, 0xFF,
                                           0x80, 0x30, 0xFF, 0xFF, 0x3C, 0x64, 0x18, 0xFF};

    Check(Play<RevolutionPolicy>(kOpening, {}, 3).keys.empty(), "the Wii: a track that opens again keeps its wait");
    Check(Play<CafePolicy>(kOpening, {}, 3).keys == std::vector<int>{60},
          "the Wii U: a track that opens again clears its wait");
}

// The rules of the length analysis on the Wii against the Wii U, on sequences in big-endian order.
void TestWiiLengthRules()
{
    using ntdlib::nwsnd::CafePolicy;
    using ntdlib::nwsnd::RevolutionPolicy;
    using xsflib::testing::Check;

    // A subroutine plays a note and jumps back to it, with no conditional command and no held note. The Wii U counts
    // the jump as a loop, and the Wii doesn't.
    const std::vector<uint8_t> kJumping = {0xFE, 0x00, 0x01, 0x8A, 0x00, 0x00, 0x08, 0xFF,
                                           0x3C, 0x64, 0x01, 0x89, 0x00, 0x00, 0x08};

    Check(Play<RevolutionPolicy>(kJumping, {}, 1, true).loops.empty(),
          "the Wii: a jump back inside a subroutine is no loop without a held note");
    Check(!Play<CafePolicy>(kJumping, {}, 1, true).loops.empty(),
          "the Wii U: a jump back inside a subroutine with no conditional command is a loop");

    // The damper is on (0xDF), and a note of 96 ticks plays. On the Wii, the note ends when its length runs out. On the
    // Wii U, it doesn't.
    const std::vector<uint8_t> kDamped = {0xFE, 0x00, 0x01, 0xDF, 0x7F, 0x3C, 0x64, 0x60, 0xFF};

    Check(Play<RevolutionPolicy>(kDamped).ending_note, "the Wii: a note under the damper ends");
    Check(!Play<CafePolicy>(kDamped).ending_note, "the Wii U: a note under the damper doesn't end");

    // A timebase of 0 stops the clock while the track waits. It stops the sequence on the Wii U, not on the Wii.
    const std::vector<uint8_t> kTimeless = {0xFE, 0x00, 0x01, 0xB0, 0x00, 0x80, 0x01, 0x89, 0x00, 0x00, 0x05};

    Check(!Play<RevolutionPolicy>(kTimeless).stuck, "the Wii: a timebase of 0 doesn't stop the sequence");
    Check(Play<CafePolicy>(kTimeless).stuck, "the Wii U: a timebase of 0 stops the sequence");

    // The sequence ends at once. With no track open, the Wii's sequence isn't stuck and moves. The Wii U's sequence is
    // stuck and still.
    const std::vector<uint8_t> kEmpty = {0xFE, 0x00, 0x01, 0xFF};

    const Played wii_empty = Play<RevolutionPolicy>(kEmpty);
    Check(wii_empty.finished && !wii_empty.stuck && wii_empty.moving_at_end,
          "the Wii: with no track open, the sequence isn't stuck and moves");
    const Played cafe_empty = Play<CafePolicy>(kEmpty);
    Check(cafe_empty.finished && cafe_empty.stuck && !cafe_empty.moving_at_end,
          "the Wii U: with no track open, the sequence is stuck and still");

    // A note, then a loop that changes the volume and plays no note. On the Wii, the pass is already marked quiet when
    // the loop is reported. On the Wii U, the mark comes after the report.
    const std::vector<uint8_t> kQuiet = {0xFE, 0x00, 0x01, 0x3C, 0x64, 0x01, 0xD4, 0x00,
                                         0xC1, 0x64, 0xC1, 0x32, 0x80, 0x01, 0xFC, 0xFF};

    const Played wii_quiet = Play<RevolutionPolicy>(kQuiet, {}, 1, true);
    Check(!wii_quiet.quiet_at_loop.empty() && wii_quiet.quiet_at_loop.front(),
          "the Wii: the pass is marked quiet when the loop is reported");
    const Played cafe_quiet = Play<CafePolicy>(kQuiet, {}, 1, true);
    Check(!cafe_quiet.quiet_at_loop.empty() && !cafe_quiet.quiet_at_loop.front(),
          "the Wii U: a pass is marked quiet after the loop's report");
}

// Track 0 sets its initial pan, pan and surround pan to `value`, sets the sweep's pitch to -40, and plays C4 for 24
// ticks. The sweep's pitch is big-endian.
std::vector<uint8_t> PanSweepSequence(uint8_t value)
{
    return {0xFE, 0x00, 0x01, 0xDC, value, 0xC0, value, 0xD7, value,
            0xE3, 0xFF, 0xD8, 0x3C, 0x64,  0x18, 0x80,  0x18, 0xFF};
}

// The Wii's pans and sweep by build. The build of October 2006 raises a pan, an initial pan or a surround pan of 0 or 1
// by one. The sweep's pitch is in 32nds of a semitone before 2008, and in 64ths from 2008. The expected values are
// fixed.
void TestWiiPansAndSweep()
{
    using Wii = ntdlib::nwsnd::RevolutionPolicy;
    using Policy = TestPolicy<Wii>;
    using Player = ntdlib::nwsnd::SequenceSoundPlayer<Policy>;
    struct Case
    {
        const char* build;
        bool october_2006;
        uint32_t date;
        uint8_t value;
        int pan;
        int span;
        float sweep;
    };
    const Case kCases[] = {
        {"October 2006", true, 0, 1, -62, 2, -1.25f},        {"October 2006", true, 0, 2, -62, 2, -1.25f},
        {"June 2007", false, 20070608, 1, -63, 1, -1.25f},   {"June 2007", false, 20070608, 2, -62, 2, -1.25f},
        {"March 2008", false, 20080308, 1, -63, 1, -0.625f}, {"June 2008", false, 20080620, 1, -63, 1, -0.625f},
    };
    for (const Case& c : kCases)
    {
        typename Policy::Engine engine;
        const typename Policy::SoundInfo kInfo;
        const std::vector<uint8_t> data = PanSweepSequence(c.value);
        Player player(0, kInfo, data);
        engine.player = &player;
        ntdlib::SndBuild build;
        build.date = c.date;
        build.october_2006 = c.october_2006;
        player.SetBuild(build);

        player.Start();
        player.Update(engine);

        const std::string name = std::string("the Wii's build of ") + c.build + ", value " + std::to_string(c.value);
        const auto& channels = engine.pool.channels;
        const bool one_note = channels.size() == 1 && engine.init_pans.size() == 1;
        xsflib::testing::Check(one_note, name + ": one note");
        if (one_note)
        {
            xsflib::testing::Check(engine.init_pans[0] == c.pan, name + ": the initial pan");
            xsflib::testing::Check(channels[0].pan == Wii::PanOfValue(c.pan), name + ": the pan");
            xsflib::testing::Check(channels[0].span == Wii::SpanOfValue(c.span), name + ": the surround pan");
            xsflib::testing::Check(channels[0].sweep_pitch == c.sweep, name + ": the sweep's pitch");
        }
    }
}

// The Wii's tempo by build. Builds from 2008 hold it to 0-1023, and older builds keep it whole. A tempo of 2000 then
// runs at 1023's speed from 2008, and faster before.
void TestWiiTempo()
{
    using Policy = TestPolicy<ntdlib::nwsnd::RevolutionPolicy>;
    using Player = ntdlib::nwsnd::SequenceSoundPlayer<Policy>;

    const auto ticks = [](uint32_t date, uint16_t tempo)
    {
        typename Policy::Engine engine;
        const typename Policy::SoundInfo kInfo;
        const std::vector<uint8_t> data = ClockSequence(48, tempo);
        Player player(0, kInfo, data);
        engine.player = &player;
        ntdlib::SndBuild build;
        build.date = date;
        player.SetBuild(build);

        player.Start();
        for (int i = 0; i < 100; ++i)
        {
            player.Update(engine);
        }

        return player.TickCount();
    };

    xsflib::testing::Check(ticks(20080308, 2000) == ticks(20080308, 1023),
                           "the Wii's builds from 2008: a tempo of 2000 runs at 1023's speed");
    xsflib::testing::Check(ticks(20070608, 2000) > ticks(20070608, 1023),
                           "the Wii's builds before 2008: a tempo of 2000 runs faster than 1023");
}

// The tempo ratio on the Wii's two clocks. In float milliseconds, the ratio multiplies the ticks in a millisecond
// before the division by 60,000 (SeqPlayer::UpdateTick, 0x8003eec4 in Wii Sports' main.dol). A ratio of 2 then ticks as
// twice the tempo does, and a ratio of 0.5 as half the tempo. In 416ths of a tick, each frame adds the tempo times the
// ratio, truncated (0x800e6618 to 0x800e664c in WarioWare: Smooth Moves' main.dol). The first frame adds the default
// tempo of 120 times the ratio, and the first tick sets the sequence's tempo T. After N frames, for N of 2 or more,
// the ticks are (416 + int(120 r) + (N - 2) int(T r)) / 416.
void TestWiiTempoRatio()
{
    using Policy = TestPolicy<ntdlib::nwsnd::RevolutionPolicy>;
    using Player = ntdlib::nwsnd::SequenceSoundPlayer<Policy>;

    const auto ticks = [](bool october_2006, uint16_t tempo, float ratio, int frames)
    {
        typename Policy::Engine engine;
        const typename Policy::SoundInfo kInfo;
        const std::vector<uint8_t> data = ClockSequence(48, tempo);
        Player player(0, kInfo, data);
        engine.player = &player;
        ntdlib::SndBuild build;
        build.october_2006 = october_2006;
        player.SetBuild(build);
        player.SetTempoRatio(ratio);

        player.Start();
        for (int i = 0; i < frames; ++i)
        {
            player.Update(engine);
        }

        return player.TickCount();
    };

    xsflib::testing::Check(ticks(false, 120, 2.0f, 1000) == ticks(false, 240, 1.0f, 1000),
                           "the Wii's clock: a tempo ratio of 2 ticks as twice the tempo does");
    xsflib::testing::Check(ticks(false, 136, 0.5f, 1000) == ticks(false, 68, 1.0f, 1000),
                           "the Wii's clock: a tempo ratio of 0.5 ticks as half the tempo does");

    struct Case
    {
        uint16_t tempo;
        float ratio;
    };
    const Case kCases[] = {{120, 2.0f}, {137, 1.1f}, {255, 0.5f}, {500, 0.25f}};
    for (const Case& c : kCases)
    {
        const int first = static_cast<int>(120.0f * c.ratio);
        const int each = static_cast<int>(static_cast<float>(c.tempo) * c.ratio);
        for (const int frames : {4, 100, 1000})
        {
            const auto expected = static_cast<uint32_t>((416 + first + (frames - 2) * each) / 416);
            const uint32_t got = ticks(true, c.tempo, c.ratio, frames);
            xsflib::testing::Check(got == expected, "the Wii's clock of October 2006, tempo " +
                                                        std::to_string(c.tempo) + " and tempo ratio " +
                                                        std::to_string(c.ratio) + ": " + std::to_string(got) +
                                                        " ticks after " + std::to_string(frames) + " frames, not " +
                                                        std::to_string(expected));
        }
    }

    // A step that truncates to 0 stops the clock of October 2006 once less than a tick is left: 3 times 0.25 adds
    // nothing, and 4 times 0.25 adds 1.
    const auto stopped = [](uint16_t tempo, float ratio)
    {
        typename Policy::Engine engine;
        const typename Policy::SoundInfo kInfo;
        const std::vector<uint8_t> data = ClockSequence(48, tempo);
        Player player(0, kInfo, data);
        engine.player = &player;
        ntdlib::SndBuild build;
        build.october_2006 = true;
        player.SetBuild(build);
        player.SetTempoRatio(ratio);

        player.Start();
        for (int i = 0; i < 4; ++i)
        {
            player.Update(engine);
        }

        return player.ClockStopped();
    };

    xsflib::testing::Check(stopped(3, 0.25f) && !stopped(4, 0.25f),
                           "the Wii's clock of October 2006: a step that truncates to 0 stops it");
}

// Track 0 turns the note wait off, plays C4 and D4 for 96 ticks each from the same tick, and waits. With `envelope`, it
// first sets the attack to 0x80, the decay to 0x10 and the release to 0xFE. The allocation mask is in the console's
// byte order.
std::vector<uint8_t> KeyGroupSequence(bool little_endian, bool envelope)
{
    std::vector<uint8_t> bytes = {0xFE, 0x00, 0x01};
    if (little_endian)
    {
        std::swap(bytes[1], bytes[2]);
    }

    if (envelope)
    {
        bytes.insert(bytes.end(), {0xD0, 0x80, 0xD1, 0x10, 0xD3, 0xFE});
    }

    bytes.insert(bytes.end(), {0xC7, 0x00, 0x3C, 0x64, 0x60, 0x3E, 0x64, 0x60, 0x80, 0x60, 0xFF});

    return bytes;
}

// Two notes in a key group: the second releases the first. The Wii U and the Switch first set the first note's release
// rate to 0x7E. The Wii keeps its rate. A track's envelope value replaces the bank's value from 0 to 127, and in the
// Wii's builds before 2008 at any value but 0xFF. The expected values are fixed.
template <typename Console>
void TestNoteOn(const std::string& name)
{
    using Policy = TestPolicy<Console>;
    using Player = ntdlib::nwsnd::SequenceSoundPlayer<Policy>;
    constexpr bool kWii = std::is_same_v<Console, ntdlib::nwsnd::RevolutionPolicy>;
    const typename Policy::SoundInfo kInfo;
    for (const bool envelope : {false, true})
    {
        typename Policy::Engine engine;
        engine.key_group = 1;
        const std::vector<uint8_t> data = KeyGroupSequence(Console::kLittleEndianArguments, envelope);
        Player player(0, kInfo, data);
        engine.player = &player;

        player.Start();
        player.Update(engine);

        const std::string with = name + (envelope ? ", with the track's envelope" : "");
        const auto& channels = engine.pool.channels;
        const bool two_notes = channels.size() == 2;
        xsflib::testing::Check(two_notes && channels[0].released && !channels[1].released,
                               with + ": the second note releases the first");
        if (!two_notes)
        {
            continue;
        }

        const int first_release = kWii ? (envelope ? 0xFE : -1) : 0x7E;
        xsflib::testing::Check(channels[0].envelope.release == first_release, with + ": the first note's release rate");
        xsflib::testing::Check(channels[1].envelope.release == (envelope && kWii ? 0xFE : -1),
                               with + ": the second note's release rate");
        const int attack = envelope && kWii ? 0x80 : -1;
        const int decay = envelope ? 0x10 : -1;
        for (const auto& channel : channels)
        {
            xsflib::testing::Check(channel.envelope.attack == attack && channel.envelope.decay == decay &&
                                       channel.envelope.sustain == -1 && channel.envelope.hold == -1,
                                   with + ": the track's envelope values that apply");
        }
    }
}

// Track 0 sets the hold to `hold` and plays C4 for 96 ticks. The allocation mask is in the console's byte order.
std::vector<uint8_t> HoldSequence(bool little_endian, uint8_t hold)
{
    std::vector<uint8_t> bytes = {0xFE, 0x00, 0x01, 0xB1, hold, 0x3C, 0x64, 0x60, 0xFF};
    if (little_endian)
    {
        std::swap(bytes[1], bytes[2]);
    }

    return bytes;
}

// A track's hold replaces the bank's hold from 0 to 127 on every console. On the Wii, the hold comes in 2008. The
// expected values are fixed.
template <typename Console>
void TestHold(const std::string& name)
{
    using Policy = TestPolicy<Console>;
    using Player = ntdlib::nwsnd::SequenceSoundPlayer<Policy>;
    const typename Policy::SoundInfo kInfo;
    for (const int hold : {0x00, 0x7F, 0x80, 0xFE})
    {
        typename Policy::Engine engine;
        const std::vector<uint8_t> data = HoldSequence(Console::kLittleEndianArguments, static_cast<uint8_t>(hold));
        Player player(0, kInfo, data);
        if constexpr (std::is_same_v<Console, ntdlib::nwsnd::RevolutionPolicy>)
        {
            ntdlib::SndBuild build;
            build.date = 20080308;
            player.SetBuild(build);
        }

        engine.player = &player;
        player.Start();
        player.Update(engine);

        const auto& channels = engine.pool.channels;
        const int expected = hold <= 127 ? hold : -1;
        xsflib::testing::Check(channels.size() == 1 && channels[0].envelope.hold == expected,
                               name + ": the note's hold after a track's hold of " + std::to_string(hold));
    }
}

// The Wii's builds before 2008 take a track's envelope value at any value but 0xFF, and have no hold. Builds from 2008
// take the envelope values and the hold from 0 to 127. The expected values are fixed.
void TestWiiEnvelopeRange()
{
    using Policy = TestPolicy<ntdlib::nwsnd::RevolutionPolicy>;
    using Player = ntdlib::nwsnd::SequenceSoundPlayer<Policy>;

    // Track 0 sets the attack to 0x80, the decay to 0x10, the sustain to 0xFE, the release to 0x7F and the hold to
    // 0x40, then plays C4 for 96 ticks.
    const std::vector<uint8_t> kData = {0xFE, 0x00, 0x01, 0xD0, 0x80, 0xD1, 0x10, 0xD2, 0xFE,
                                        0xD3, 0x7F, 0xB1, 0x40, 0x3C, 0x64, 0x60, 0xFF};

    const Policy::SoundInfo kInfo;
    for (const bool since_2008 : {false, true})
    {
        Policy::Engine engine;
        Player player(0, kInfo, kData);
        ntdlib::SndBuild build;
        build.date = since_2008 ? 20080308 : 20070608;
        player.SetBuild(build);
        engine.player = &player;

        player.Start();
        player.Update(engine);

        const std::string with = since_2008 ? "the Wii's build of 2008" : "the Wii's build of 2007";
        const auto& channels = engine.pool.channels;
        xsflib::testing::Check(channels.size() == 1, with + ": one note");
        if (channels.size() != 1)
        {
            continue;
        }

        const auto& envelope = channels[0].envelope;
        xsflib::testing::Check(envelope.attack == (since_2008 ? -1 : 0x80) && envelope.decay == 0x10 &&
                                   envelope.sustain == (since_2008 ? -1 : 0xFE) && envelope.release == 0x7F &&
                                   envelope.hold == (since_2008 ? 0x40 : -1),
                               with + ": the track's envelope values that apply");
    }
}

// The Wii's track volume (SeqPlayer::SetTrackVolume, 0x8003e8b0 in Wii Sports' main.dol). Track 0 plays two notes. A
// track volume of 0.5 halves the volume of the track's channels: the product with 0.5 is exact. A track volume of 0
// silences them. A track whose bit isn't set keeps its volume. Track 1 isn't allocated.
void TestWiiTrackVolume()
{
    using Policy = TestPolicy<ntdlib::nwsnd::RevolutionPolicy>;
    using Player = ntdlib::nwsnd::SequenceSoundPlayer<Policy>;

    const auto volume = [](uint32_t track_flags, float track_volume)
    {
        typename Policy::Engine engine;
        const typename Policy::SoundInfo kInfo;
        const std::vector<uint8_t> data = KeyGroupSequence(false, false);
        Player player(0, kInfo, data);
        engine.player = &player;

        player.Start();
        player.SetTrackVolume(track_flags, track_volume);
        player.Update(engine);

        return engine.pool.channels.empty() ? -1.0f : engine.pool.channels[0].track_volume;
    };

    const float full = volume(0, 1.0f);
    xsflib::testing::Check(full > 0.0f, "the Wii: the track's channel has a volume");
    xsflib::testing::Check(volume(1, 0.5f) == full * 0.5f,
                           "the Wii: a track volume of 0.5 halves the channel's volume");
    xsflib::testing::Check(volume(1, 0.0f) == 0.0f, "the Wii: a track volume of 0 silences the track");
    xsflib::testing::Check(volume(2, 0.0f) == full, "the Wii: a track whose bit isn't set keeps its volume");
}

int main()
{
    TestLoop<ntdlib::nwsnd::CafePolicy>("the Wii U");
    TestLoop<ntdlib::nwsnd::NxPolicy>("the Switch");
    TestLoop<ntdlib::nwsnd::RevolutionPolicy>("the Wii");
    TestHooks<ntdlib::nwsnd::CafePolicy>("the Wii U's hooks");
    TestHooks<ntdlib::nwsnd::NxPolicy>("the Switch's hooks");
    TestHooks<ntdlib::nwsnd::RevolutionPolicy>("the Wii's hooks");
    TestWiiClock();
    TestSteps<ntdlib::nwsnd::CafePolicy>("the Wii U's steps");
    TestSteps<ntdlib::nwsnd::NxPolicy>("the Switch's steps");
    TestSteps<ntdlib::nwsnd::RevolutionPolicy>("the Wii's steps");
    TestClockSteps<ntdlib::nwsnd::CafePolicy>("the Wii U's clock in steps");
    TestClockSteps<ntdlib::nwsnd::NxPolicy>("the Switch's clock in steps");
    TestClockSteps<ntdlib::nwsnd::CtrPolicy>("the 3DS's clock in steps");
    TestClockSteps<ntdlib::nwsnd::RevolutionPolicy>("the Wii's clock in steps");
    TestClockSteps<ntdlib::nwsnd::RevolutionPolicy>("the clock in steps of the Wii's build of October 2006",
                                                    {.date = 0, .october_2006 = true});
    TestAnalysisHooks();
    TestStopsTick();
    TestParseLimit<ntdlib::nwsnd::CafePolicy>("the Wii U", false);
    TestParseLimit<ntdlib::nwsnd::NxPolicy>("the Switch", false);
    TestParseLimit<ntdlib::nwsnd::RevolutionPolicy>("the Wii", true);
    TestEndlessCountedLoop<ntdlib::nwsnd::CafePolicy>("the Wii U");
    TestEndlessCountedLoop<ntdlib::nwsnd::NxPolicy>("the Switch");
    TestEndlessCountedLoop<ntdlib::nwsnd::RevolutionPolicy>("the Wii");
    TestTrackLoops<ntdlib::nwsnd::CafePolicy>("the Wii U");
    TestTrackLoops<ntdlib::nwsnd::NxPolicy>("the Switch");
    TestTrackLoops<ntdlib::nwsnd::RevolutionPolicy>("the Wii");
    TestWiiParser();
    TestWiiCallStack();
    TestWiiLengthRules();
    TestWiiPansAndSweep();
    TestWiiTempo();
    TestWiiTempoRatio();
    TestWiiTrackVolume();
    TestNoteOn<ntdlib::nwsnd::CafePolicy>("the Wii U");
    TestNoteOn<ntdlib::nwsnd::NxPolicy>("the Switch");
    TestNoteOn<ntdlib::nwsnd::RevolutionPolicy>("the Wii");
    TestHold<ntdlib::nwsnd::CafePolicy>("the Wii U");
    TestHold<ntdlib::nwsnd::NxPolicy>("the Switch");
    TestHold<ntdlib::nwsnd::RevolutionPolicy>("the Wii");
    TestWiiEnvelopeRange();

    if (xsflib::testing::failures != 0)
    {
        std::fprintf(stderr, "%d checks failed\n", xsflib::testing::failures);
        return 1;
    }

    std::printf("nwsnd_sequence_test: all checks passed\n");

    return 0;
}
