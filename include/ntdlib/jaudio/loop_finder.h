// SPDX-License-Identifier: MIT

// The length analysis's view of a JAudio1 or JAudio2 sound's tracks in game mode (see game_lengths.h). The analysis
// watches the game's sequence parser and passes on what each track does: the start of each command, and each jump.
// LoopFinder works out from them when a track's loop has played twice, which tracks idle, and when the sound holds.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace ntdlib::jaudio
{

// The milliseconds that `frames` frames last at `sample_rate` frames a second.
long long FramesToMs(uint64_t frames, uint32_t sample_rate);

// A track that has run no command for 10 s no longer holds back the loops of the tracks after it. A sound in which a
// track idles and nothing else has moved for 10 s holds.
inline constexpr long long kStillMs = 10'000;

// The sequence command that ends a track, in JAudio1 and JAudio2. A track that runs it outside any call (at depth 0)
// has finished.
inline constexpr uint8_t kFinishCommand = 0xff;

// The events of a track that the length analysis takes as the track plays: the start of each command, and each jump.
// `at` is where the track reads: at a command's start, or after a jump.
struct TrackEvent
{
    enum class Kind
    {
        kCommand,
        kJump,
    };

    Kind kind;
    uint32_t track;
    uint32_t at;
    uint32_t to;    // where a jump goes
    uint32_t depth; // how deep the track is in calls and counted loops
    uint8_t command;
    uint64_t frame;
};

// What a track waits for: the ticks left to wait, and whether it waits for its notes to finish.
struct TrackWait
{
    int32_t ticks;
    bool notes;
};

// The sound so far: whether it has made a sound, and the frame of its last sound.
struct Sounded
{
    bool any;
    uint64_t last;
};

// Whether a sequence command changes the sound: a note or a note's end, a change of the track's parameters, a track
// opened or closed, or the track's end. Jumps, calls, waits and the commands on ports and registers don't. ChangesSound
// takes JAudio2's commands. JAudio1ChangesSound takes the commands of JAudio1, the sound library's first generation. In
// JAudio1, a command from 0xb0 to 0xbf takes its arguments from registers. The command that it runs follows it.
bool ChangesSound(uint8_t command);
bool JAudio1ChangesSound(uint8_t command);

// Follows the tracks of a sound for the length analysis: the loops they idle in, when they last moved, and their jumps
// back. A pass of a loop runs from the track's last visit to the jump's target up to the jump back. A loop inside the
// pass belongs to the pass. A track idles in a loop whose passes poll or change nothing. It moves when it runs a
// command outside that loop.
//
// A loop inside a call never counts. With `idle_in_calls`, a track can still idle in one. Pikmin 2's root tracks call
// a routine that polls the game once a beat and never returns. Without the option, every poll is a move, and the root
// track holds back the loops of the tracks after it.
class LoopFinder
{
public:
    // What a jump back means for the sound's length.
    enum class Verdict
    {
        kNone,
        kLoops, // the second pass back to the same command: the sound loops
        kEnded, // a pass without a sound: the sound ended at its last sound
    };

    // `root` is the sound's root track. The other tracks follow it in the order in which they first run a command. The
    // events' frames count at `sample_rate` frames a second: the rate of the output. `changes_sound` is the test of
    // whether a command changes the sound.
    LoopFinder(uint32_t root, uint32_t sample_rate, bool (*changes_sound)(uint8_t command) = ChangesSound,
               bool idle_in_calls = false);

    // Takes one event, in the order in which the events happened.
    Verdict Take(const TrackEvent& event, const Sounded& sounded);

    // Takes what each track waits for. `read` gives it for a track.
    void TakeWaits(const std::function<TrackWait(uint32_t track)>& read);

    // Whether the sound holds at frame `now`: a track idles, every track has settled, and no track has moved for 10 s.
    bool Holds(uint64_t now) const;

private:
    // A loop, from the command that its jump back goes to up to the jump back. Its commands run at `depth`: 0 outside
    // any call.
    struct Loop
    {
        uint32_t start;
        uint32_t end;
        uint32_t depth = 0;
    };

    // When a track ran a command: the frame, and the number of events before it.
    struct Time
    {
        uint64_t frame;
        uint64_t order;
    };

    struct Track
    {
        std::optional<Loop> idle;           // the loop that the track idles in
        uint64_t changed = 0;               // the order of its last command that changed the sound, or 0
        uint32_t call_site = 0;             // where it last ran a command outside any call
        std::map<uint32_t, uint64_t> moves; // where it moved that no jump back has judged, and the frame it last did
        uint64_t judged_move = 0;           // the frame of its last move that a jump back has judged
        std::map<uint32_t, Time> reached;   // where it ran commands outside any call, and when it last did
        std::map<uint32_t, Time> reached_in_calls; // with idle_in_calls: the same for its commands in calls
        uint64_t first_command = 0;
        uint64_t last_command = 0;
        uint64_t pass_start = 0;     // the frame at which the track last reached the command that it jumps back to
        std::optional<Loop> counted; // the loop whose passes the track counts
        int loops = 0;
        bool finished = false;
        TrackWait wait = {};
    };

    Track& Find(uint32_t track, uint64_t frame);
    void Command(Track& track, const TrackEvent& event);

    // The frame of the track's last move.
    static uint64_t LastMove(const Track& track);

    Verdict Jump(Track& track, const TrackEvent& event, const Sounded& sounded);
    void JumpInCall(Track& track, const TrackEvent& event);

    // Whether a track can't move by itself: it idles, it has finished (kFinishCommand), or it has run no command for
    // 10 s and has no ticks left to wait.
    bool Settled(const Track& track, uint64_t now) const;

    // Whether a track that opened before `id` holds back its loop: it moved during the pass, or it can still move.
    bool HeldBack(uint32_t id, const Track& track, uint64_t now) const;

    std::vector<uint32_t> order_;
    std::map<uint32_t, Track> tracks_;
    uint64_t taken_ = 0; // the events taken so far
    uint32_t sample_rate_;
    bool (*changes_sound_)(uint8_t command);
    bool idle_in_calls_;
};

} // namespace ntdlib::jaudio
