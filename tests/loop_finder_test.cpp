// SPDX-License-Identifier: MIT

// Unit tests for the length analysis's loop finder (jaudio/loop_finder.h), with made-up tracks. Each event takes 1 ms
// at the GameCube's output rate.

#include "ntdlib/jaudio/loop_finder.h"

#include <cstdint>
#include <vector>

#include "xsflib/testing/check.h"

using namespace ntdlib::jaudio;

namespace
{

using Verdict = LoopFinder::Verdict;

// Commands by their first byte. A note changes the sound. The others don't.
constexpr uint8_t kNote = 0x3c;
constexpr uint8_t kReadRegister = 0xda;
constexpr uint8_t kJumpIf = 0xc8;
constexpr uint8_t kWait = 0xf0;

// The frames' rate: the GameCube's output rate. 32 frames are about 1 ms.
constexpr uint32_t kRate = 32028;

// The tracks: the sound's root track, and one that opens after it.
constexpr uint32_t kRoot = 0x1000;
constexpr uint32_t kLater = 0x2000;

// Feeds events to a LoopFinder as a sound that has played all along.
class Feeder
{
public:
    explicit Feeder(LoopFinder& finder) : finder_(finder)
    {
    }

    // `track` runs the command at `at`, `depth` calls deep.
    void Command(uint32_t track, uint32_t at, uint8_t command, uint32_t depth = 0)
    {
        Take({TrackEvent::Kind::kCommand, track, at, 0, depth, command, frame_});
    }

    // `track` jumps back to `to` from the jump command that ends at `at`, `depth` calls deep. Returns the jump's
    // verdict.
    Verdict Jump(uint32_t track, uint32_t at, uint32_t to, uint32_t depth = 0)
    {
        return Take({TrackEvent::Kind::kJump, track, at, to, depth, 0xc7, frame_});
    }

    uint64_t Frame() const
    {
        return frame_;
    }

private:
    Verdict Take(const TrackEvent& event)
    {
        const Verdict verdict = finder_.Take(event, {true, frame_});
        frame_ += 32;

        return verdict;
    }

    LoopFinder& finder_;
    uint64_t frame_ = 0;
};

// One pass of a loop that polls a register: a read, the conditional jump out to 0x22 (not taken), a wait, and the jump
// back. The pass changes nothing.
Verdict PollPass(Feeder& feeder, uint32_t track)
{
    feeder.Command(track, 0x12, kReadRegister);
    feeder.Command(track, 0x17, kJumpIf);
    feeder.Command(track, 0x1c, kWait);

    return feeder.Jump(track, 0x22, 0x12);
}

// The polling loop's last pass: the condition holds, and the conditional jump leaves the loop for 0x22.
void LeavePoll(Feeder& feeder, uint32_t track)
{
    feeder.Command(track, 0x12, kReadRegister);
    feeder.Command(track, 0x17, kJumpIf);
}

void TestLoop()
{
    // A note and a wait, then the jump back to the note.
    LoopFinder finder(kRoot, kRate);
    Feeder feeder(finder);
    std::vector<Verdict> verdicts;

    for (int pass = 0; pass < 2; pass++)
    {
        feeder.Command(kRoot, 0x10, kNote);
        feeder.Command(kRoot, 0x12, kWait);
        verdicts.push_back(feeder.Jump(kRoot, 0x18, 0x10));
    }

    XSFLIB_CHECK(verdicts == (std::vector{Verdict::kNone, Verdict::kLoops}));
}

void TestLoopAroundPoll()
{
    // A note, a loop that polls three times, a wait, then the jump back to the note. The polling loop's jumps back
    // neither hide the note from the outer pass nor start the outer loop's count again.
    LoopFinder finder(kRoot, kRate);
    Feeder feeder(finder);
    std::vector<Verdict> polls;
    std::vector<Verdict> loops;

    for (int pass = 0; pass < 2; pass++)
    {
        feeder.Command(kRoot, 0x10, kNote);
        for (int poll = 0; poll < 3; poll++)
        {
            polls.push_back(PollPass(feeder, kRoot));
        }

        LeavePoll(feeder, kRoot);
        feeder.Command(kRoot, 0x22, kWait);
        loops.push_back(feeder.Jump(kRoot, 0x28, 0x10));
    }

    XSFLIB_CHECK(polls == std::vector(6, Verdict::kNone));
    XSFLIB_CHECK(loops == (std::vector{Verdict::kNone, Verdict::kLoops}));
}

void TestPollBetweenPasses()
{
    // A loop's first pass, then polling outside the loop, then the loop again. The polling starts the loop's count
    // again. The loop then loops at the second jump back after the polling.
    LoopFinder finder(kRoot, kRate);
    Feeder feeder(finder);
    std::vector<Verdict> polls;
    std::vector<Verdict> loops;
    const auto pass = [&feeder, &loops]
    {
        feeder.Command(kRoot, 0x30, kNote);
        feeder.Command(kRoot, 0x32, kWait);
        loops.push_back(feeder.Jump(kRoot, 0x38, 0x30));
    };

    pass();
    polls.push_back(PollPass(feeder, kRoot));
    polls.push_back(PollPass(feeder, kRoot));
    LeavePoll(feeder, kRoot);
    pass();
    pass();

    XSFLIB_CHECK(polls == std::vector(2, Verdict::kNone));
    XSFLIB_CHECK(loops == (std::vector{Verdict::kNone, Verdict::kNone, Verdict::kLoops}));
}

void TestPollHolds()
{
    // A note, then polling for 12 s. The polling loop's passes aren't moves. The sound therefore holds 10 s after the
    // note.
    LoopFinder finder(kRoot, kRate);
    Feeder feeder(finder);
    std::vector<Verdict> polls;
    feeder.Command(kRoot, 0x10, kNote);

    for (int poll = 0; poll < 2'400; poll++)
    {
        polls.push_back(PollPass(feeder, kRoot));
    }

    const long long early_ms = FramesToMs(feeder.Frame(), kRate);
    const bool held_early = finder.Holds(feeder.Frame());
    for (int poll = 0; poll < 600; poll++)
    {
        polls.push_back(PollPass(feeder, kRoot));
    }

    const long long late_ms = FramesToMs(feeder.Frame(), kRate);
    const bool held = finder.Holds(feeder.Frame());

    XSFLIB_CHECK(early_ms < 10'000 && late_ms > 10'000);
    XSFLIB_CHECK(!held_early);
    XSFLIB_CHECK(held);
    XSFLIB_CHECK(polls == std::vector(3'000, Verdict::kNone));
}

void TestLaterTrack()
{
    // A later track loops while the root track plays notes, then while the root track polls. Its loop counts once the
    // root track has settled, from its first pass that starts after the root track's last move.
    LoopFinder finder(kRoot, kRate);
    Feeder feeder(finder);
    std::vector<Verdict> while_moving;
    std::vector<Verdict> polls;
    std::vector<Verdict> while_polling;

    for (int pass = 0; pass < 3; pass++)
    {
        feeder.Command(kRoot, 0x08 + 2 * static_cast<uint32_t>(pass), kNote);
        feeder.Command(kLater, 0x40, kNote);
        feeder.Command(kLater, 0x42, kWait);
        while_moving.push_back(feeder.Jump(kLater, 0x48, 0x40));
    }

    for (int pass = 0; pass < 2; pass++)
    {
        polls.push_back(PollPass(feeder, kRoot));
        feeder.Command(kLater, 0x40, kNote);
        feeder.Command(kLater, 0x42, kWait);
        while_polling.push_back(feeder.Jump(kLater, 0x48, 0x40));
    }

    XSFLIB_CHECK(while_moving == std::vector(3, Verdict::kNone));
    XSFLIB_CHECK(polls == std::vector(2, Verdict::kNone));
    XSFLIB_CHECK(while_polling == (std::vector{Verdict::kNone, Verdict::kLoops}));
}

// The root track plays a note, then calls a routine that polls the game, while a later track loops. Returns the
// verdicts of the later track's jumps back, one for each of `passes` passes. With `return_at`, the routine returns in
// that pass, and the root track plays another note.
std::vector<Verdict> PollInCall(bool idle_in_calls, int passes, int return_at = -1)
{
    constexpr uint8_t kCall = 0xc4;
    LoopFinder finder(kRoot, kRate, ChangesSound, idle_in_calls);
    Feeder feeder(finder);
    std::vector<Verdict> verdicts;
    feeder.Command(kRoot, 0x08, kNote);
    feeder.Command(kRoot, 0x0c, kCall);
    for (int pass = 0; pass < passes; pass++)
    {
        const bool polls = return_at < 0 || pass < return_at;
        if (polls)
        {
            feeder.Command(kRoot, 0x40, kReadRegister, 1);
        }
        else if (pass == return_at)
        {
            feeder.Command(kRoot, 0x11, kNote);
        }

        feeder.Command(kLater, 0x80, kNote);
        feeder.Command(kLater, 0x82, kWait);
        if (polls)
        {
            feeder.Command(kRoot, 0x45, kWait, 1);
            XSFLIB_CHECK(feeder.Jump(kRoot, 0x4a, 0x40, 1) == Verdict::kNone);
        }

        verdicts.push_back(feeder.Jump(kLater, 0x88, 0x80));
    }

    return verdicts;
}

void TestIdleInCall()
{
    // The routine polls for ever, and its passes change nothing. With idle_in_calls, the root track idles there, and
    // the later track loops at its second jump back. The root track's note and its commands up to the routine's first
    // pass came before the later track's first pass. Without the option, each poll is a move of the root track, and
    // the later track never loops.
    XSFLIB_CHECK(PollInCall(true, 2) == (std::vector{Verdict::kNone, Verdict::kLoops}));
    XSFLIB_CHECK(PollInCall(false, 4) == std::vector(4, Verdict::kNone));

    // The routine returns in the second pass, and the root track plays a note. The root track then moves again, and
    // the later track's passes don't count.
    XSFLIB_CHECK(PollInCall(true, 3, 1) == std::vector(3, Verdict::kNone));
}

void TestJAudio1Commands()
{
    // JAudio1's notes, the ends of its notes and the track's parameters change the sound. Its waits, registers, ports,
    // jumps, calls and loops don't.
    for (const int command : {0x3c, 0x81, 0x8f, 0x90, 0xc1, 0xda, 0xe5, 0xf9, 0xfd, 0xff})
    {
        XSFLIB_CHECK(JAudio1ChangesSound(static_cast<uint8_t>(command)));
    }

    for (const int command : {0x80, 0x88, 0xa0, 0xc4, 0xc6, 0xc8, 0xc9, 0xca, 0xcb, 0xcf, 0xea})
    {
        XSFLIB_CHECK(!JAudio1ChangesSound(static_cast<uint8_t>(command)));
    }

    // A loop that polls a port in JAudio1's commands: a read, the conditional jump out, a wait and the jump back. With
    // JAudio1's commands, its passes change nothing. JAudio2 has notes where JAudio1 has the wait.
    const auto poll = [](bool (*changes_sound)(uint8_t command))
    {
        LoopFinder finder(kRoot, kRate, changes_sound);
        Feeder feeder(finder);
        std::vector<Verdict> verdicts;
        for (int pass = 0; pass < 2; pass++)
        {
            feeder.Command(kRoot, 0x12, 0xcb);
            feeder.Command(kRoot, 0x15, 0xc8);
            feeder.Command(kRoot, 0x1a, 0x80);
            verdicts.push_back(feeder.Jump(kRoot, 0x1c, 0x12));
        }

        return verdicts;
    };

    XSFLIB_CHECK(poll(JAudio1ChangesSound) == std::vector(2, Verdict::kNone));
    XSFLIB_CHECK(poll(ChangesSound) == (std::vector{Verdict::kNone, Verdict::kLoops}));
}

} // namespace

int main()
{
    TestLoop();
    TestLoopAroundPoll();
    TestPollBetweenPasses();
    TestPollHolds();
    TestLaterTrack();
    TestIdleInCall();
    TestJAudio1Commands();

    return xsflib::testing::Summary("loop finder");
}
