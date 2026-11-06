// SPDX-License-Identifier: MIT

// Tests of the length analysis of a DS sound archive's sequence (ntdlib/ntr/length_model.h), on archives that
// testing/sdat_files.h builds. At the default tempo of 120 and 48 ticks a beat, the driver plays a tick each 2 frames,
// from the frame that starts the sequence. A frame's commands sound two frames later. The expected frames are worked
// out by hand from the sequences.

#include <cstdint>
#include <optional>
#include <set>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/rip/length.h"
#include "xsflib/testing/check.h"

#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/ntr/length_model.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/testing/sdat_files.h"

namespace
{

namespace files = ntdlib::testing::sdat_files;
namespace ntr = ntdlib::ntr;
namespace sdat = ntdlib::sdat;
namespace snd = ntdlib::ntr::snd;
using files::Bytes;
using xsflib::rip::LoopAt;
using xsflib::rip::Timing;
using xsflib::testing::ErrorOf;

// A wave of 16 samples that loops.
files::Wave LoopingWave()
{
    return {.loop = 1,
            .loop_length = 4,
            .data = {0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0}};
}

// An archive whose sequence 0 plays `sequence` on the archive's player 1. That player may take channels 4 to 11. The
// bank's one instrument plays `wave` with `region`'s settings.
Bytes ArchiveFile(const Bytes& sequence, const files::Region& region = {}, const files::Wave& wave = LoopingWave())
{
    files::Archive spec;
    spec.items[0] = {files::SequenceItem(0, 0, "SEQ", 1)};
    spec.items[2] = {files::BankItem(1, {0, 0xFFFF, 0xFFFF, 0xFFFF}, "BANK")};
    spec.items[3] = {files::FileItem(2, "WAVE")};
    spec.items[4] = {files::PlayerItem(1, 0, 0, "PLAYER_0"), files::PlayerItem(1, 0x0FF0, 0, "PLAYER_1")};
    spec.files = {files::Sequence(sequence), files::Bank({{.type = 1, .region = region}}), files::WaveArchive({wave})};
    return files::SoundArchive(spec);
}

std::optional<Timing> Measure(const Bytes& sequence)
{
    const Bytes file = ArchiveFile(sequence);
    const sdat::Archive archive = sdat::Archive::Parse(file);
    return ntr::MeasureSequence(archive, {0});
}

bool operator==(const Timing& a, const Timing& b)
{
    return a.length_ms == b.length_ms && a.fade_ms == b.fade_ms && a.silent == b.silent;
}

Timing LoopFrom(uint64_t frame)
{
    return LoopAt(frame, ntr::kFrameMs, 10000.0);
}

// Plays `sequence` for `frames` frames while the driver follows lengths, with `variables`, and gives `check` the driver
// and the tracks of the sequence's loops, in their order. The bank's instrument plays `region`.
template <typename Check>
void AfterFrames(const Bytes& sequence, int frames, const Check& check, const ntr::Variables& variables = {},
                 const files::Region& region = {})
{
    const Bytes file = ArchiveFile(sequence, region);
    const sdat::Archive archive = sdat::Archive::Parse(file);
    ntr::ArchivePlayer player(archive);
    player.Start({0}, {.variables = variables});
    std::vector<int> loops;
    player.FollowLengths({.on_loop = [&loops](int, int track, uint32_t) { loops.push_back(track); }});
    for (int frame = 0; frame < frames; frame++)
    {
        player.RunFrame();
    }
    check(player.Driver(), loops);
}

// The rules: 600 s, and frames of about 5.21 ms.
void TestRules()
{
    const xsflib::rip::LengthRules rules = ntr::LengthRules();
    XSFLIB_CHECK(ntr::kFrameMs > 5.2095 && ntr::kFrameMs < 5.2096);
    XSFLIB_CHECK(rules.max_frames == 115173 && rules.silent_frames == 34552);
    XSFLIB_CHECK(rules.loop_offset == 2 && rules.sound_offset == 2 && rules.steady_offset == 2);
}

// A note of 48 ticks, a wait of 48 and the end. The sequence ends where its note's release falls silent, with the tail.
void TestEnd()
{
    const std::optional<Timing> timing = Measure({0x3C, 0x7F, 0x30, 0x80, 0x30, 0xFF});
    XSFLIB_CHECK(timing && !timing->silent && timing->fade_ms == 0);
    XSFLIB_CHECK(timing && timing->length_ms > 1000 && timing->length_ms < 1100);
}

// A sequence without notes is silent, and takes the tail alone.
void TestSilent()
{
    const std::optional<Timing> timing = Measure({0x80, 0x30, 0xFF});
    XSFLIB_CHECK(timing && timing->silent && timing->length_ms == 500 && timing->fade_ms == 0);
}

// The jump back loops each 96 ticks: at frames 192 and 384. The fade starts two frames after the second loop.
void TestLoop()
{
    const std::optional<Timing> timing = Measure({0x3C, 0x7F, 0x30, 0x80, 0x30, 0x94, 0x00, 0x00, 0x00});
    XSFLIB_CHECK(timing && *timing == LoopFrom(386));
}

// Track 0 loops each 96 ticks, and track 1 each 144. Track 0's second loop comes at frame 384, but the fade waits for
// track 1's second pass, at tick 288 (frame 576). The loop's point moved on with each frame until then: two frames on
// from frame 576.
void TestTracksOfOtherLengths()
{
    const std::optional<Timing> timing = Measure({
        0xFE, 0x03, 0x00,             // 0: tracks 0 and 1
        0x93, 0x01, 0x11, 0x00, 0x00, // 3: track 1 starts at 0x11
        0x3C, 0x7F, 0x30,             // 8: a note of 48 ticks
        0x80, 0x30,                   // 11: a wait of 48 ticks
        0x94, 0x08, 0x00, 0x00,       // 13: back to 8
        0x40, 0x7F, 0x30,             // 17: a note of 48 ticks
        0x80, 0x60,                   // 20: a wait of 96 ticks
        0x94, 0x11, 0x00, 0x00,       // 22: back to 17
    });
    XSFLIB_CHECK(timing && *timing == LoopFrom(578));
}

// Track 0 opens track 1 again each 48 ticks. Track 1 still waits then. Track 1 never loops. Each open after the first
// is a pass of track 1, at its next command: at ticks 48 and 96. Track 0 loops at ticks 48 and 96 too. The fade starts
// two frames after frame 192.
void TestTrackOpenedAgain()
{
    const std::optional<Timing> timing = Measure({
        0xFE, 0x03, 0x00,             // 0: tracks 0 and 1
        0x93, 0x01, 0x0E, 0x00, 0x00, // 3: track 1 starts at 0x0E
        0x80, 0x30,                   // 8: a wait of 48 ticks
        0x94, 0x03, 0x00, 0x00,       // 10: back to 3
        0x3C, 0x7F, 0x30,             // 14: a note of 48 ticks
        0x80, 0x60,                   // 17: a wait of 96 ticks
        0xFF,                         // 19
    });
    XSFLIB_CHECK(timing && *timing == LoopFrom(194));
}

// A note that lasts until the track releases it, on a looping wave: the track waits for ever. The sequence is steady
// once the envelope sustains and the wave has looped twice since.
void TestHeldNote()
{
    const std::optional<Timing> timing = Measure({0x3C, 0x7F, 0x00, 0xFF});
    XSFLIB_CHECK(timing && !timing->silent && timing->fade_ms == 10000);
    XSFLIB_CHECK(timing && timing->length_ms > 0 && timing->length_ms < 100);
}

// An outer loop of 3 passes around an inner loop of 255 notes plays 3 * 255 * 96 ticks, past the limit. The measure
// runs again with the outer loop endless. Its passes end at ticks 24,480 and 48,960, and the fade starts two frames
// after frame 97,920.
void TestCountedLoopPastTheLimit()
{
    const Bytes sequence = {
        0xD4, 0x03,       // 0: 3 passes
        0xD4, 0xFF,       // 2: 255 passes
        0x3C, 0x7F, 0x30, // 4: a note of 48 ticks
        0x80, 0x30,       // 7: a wait of 48 ticks
        0xFC,             // 9
        0xFC,             // 10
        0xFF,             // 11
    };
    const Bytes file = ArchiveFile(sequence);
    const sdat::Archive archive = sdat::Archive::Parse(file);
    const std::optional<Timing> timing = ntr::MeasureSequence(archive, {0});
    XSFLIB_CHECK(timing && *timing == LoopFrom(97922));

    // The outermost counted loop: the place that the outer loop's end goes back to.
    ntr::ArchivePlayer player(archive);
    player.Start({0});
    player.FollowLengths({.on_loop = [](int, int, uint32_t) {}});
    for (int frame = 0; frame < 10; frame++)
    {
        player.RunFrame();
    }
    XSFLIB_CHECK(player.Driver().OutermostCountedLoops(0) == (std::set<snd::LoopPlace>{{0, 0, 2}}));
}

// A pass that draws a random number isn't still, even when the number changes nothing: the sequence doesn't settle. A
// pass that sets a variable to the same value is still.
void TestRandomDraws()
{
    // 0: variable 0 takes a random number from 0 to 5, 4: a wait of a tick, 6: back to 0.
    AfterFrames({0xB6, 0x00, 0x05, 0x00, 0x80, 0x01, 0x94, 0x00, 0x00, 0x00}, 20,
                [](const snd::Driver& driver, const std::vector<int>&)
                { XSFLIB_CHECK(driver.IsTrackIdle(0, 0) && !driver.IsSettled(0)); });

    // The volume takes a random value from 100 to 100. The first pass changes it, and the others leave it.
    AfterFrames({0xA0, 0xC1, 0x64, 0x00, 0x64, 0x00, 0x80, 0x01, 0x94, 0x00, 0x00, 0x00}, 20,
                [](const snd::Driver& driver, const std::vector<int>&)
                {
                    XSFLIB_CHECK(driver.GetTrack(driver.GetPlayer(0).tracks[0]).volume == 100);
                    XSFLIB_CHECK(driver.IsTrackIdle(0, 0) && !driver.IsSettled(0));
                });

    // Variable 0 takes 5.
    AfterFrames({0xB0, 0x00, 0x05, 0x00, 0x80, 0x01, 0x94, 0x00, 0x00, 0x00}, 20,
                [](const snd::Driver& driver, const std::vector<int>&) { XSFLIB_CHECK(driver.IsSettled(0)); });
}

// Track 0 waits in a loop while variable 0 is 0. Track 1 sets the variable to 0, and to 1 at tick 5. Track 0 then
// leaves the loop at tick 6 and runs `after`. Track 1 has ended by then.
Bytes LeavesLoop(const Bytes& after)
{
    Bytes sequence = {
        0xFE, 0x03, 0x00,             // 0: tracks 0 and 1
        0x93, 0x01, 0x1C, 0x00, 0x00, // 3: track 1 starts at 0x1C
        0x80, 0x02,                   // 8: a wait of 2 ticks
        0x80, 0x01,                   // 10: a wait of a tick
        0xB8, 0x00, 0x00, 0x00,       // 12: whether variable 0 is 0
        0xA2, 0x94, 0x0A, 0x00, 0x00, // 16: if so, back to 10
    };
    sequence.insert(sequence.end(), after.begin(), after.end());
    sequence.resize(0x1C, 0xFF);
    sequence.insert(sequence.end(), {
                                        0xB0,
                                        0x00,
                                        0x00,
                                        0x00, // 0x1C: variable 0 takes 0
                                        0x80,
                                        0x05, // 0x20: a wait of 5 ticks
                                        0xB0,
                                        0x00,
                                        0x01,
                                        0x00, // 0x22: variable 0 takes 1
                                        0xFF, // 0x26
                                    });
    return sequence;
}

// After its last pass, a track that changes a setting isn't idle, and one that draws a random number isn't still. A
// track that only waits after its last pass is both: the sequence settles.
void TestAfterTheLastPass()
{
    // 21: the volume takes 100, 23: a wait of 127 ticks.
    AfterFrames(LeavesLoop({0xC1, 0x64, 0x80, 0x7F}), 30, [](const snd::Driver& driver, const std::vector<int>&)
                { XSFLIB_CHECK(!driver.IsTrackOpen(0, 1) && !driver.IsTrackIdle(0, 0)); });

    // 21: variable 1 takes a random number, 25: a wait of 127 ticks.
    AfterFrames(LeavesLoop({0xB6, 0x01, 0x05, 0x00, 0x80, 0x7F}), 30,
                [](const snd::Driver& driver, const std::vector<int>&)
                { XSFLIB_CHECK(driver.IsTrackIdle(0, 0) && !driver.IsSettled(0)); });

    // 21: a wait of 127 ticks.
    AfterFrames(LeavesLoop({0x80, 0x7F}), 30,
                [](const snd::Driver& driver, const std::vector<int>&) { XSFLIB_CHECK(driver.IsSettled(0)); });
}

// A pass whose wait comes from a variable that the pass changes isn't still. Variable 0 starts at 1: each pass waits a
// tick longer than the last. A pass that adds 0 to the variable is still.
void TestVariableReads()
{
    ntr::Variables variables;
    variables.local[0] = 1;

    // 0: a wait of variable 0's ticks, 3: add 1 to variable 0, 7: back to 0.
    AfterFrames(
        {0xA1, 0x80, 0x00, 0xB1, 0x00, 0x01, 0x00, 0x94, 0x00, 0x00, 0x00}, 40,
        [](const snd::Driver& driver, const std::vector<int>&)
        { XSFLIB_CHECK(driver.IsTrackIdle(0, 0) && !driver.IsSettled(0)); }, variables);
    AfterFrames(
        {0xA1, 0x80, 0x00, 0xB1, 0x00, 0x00, 0x00, 0x94, 0x00, 0x00, 0x00}, 40,
        [](const snd::Driver& driver, const std::vector<int>&) { XSFLIB_CHECK(driver.IsSettled(0)); }, variables);
}

// A loop that only waits passes still and quiet: the sequence is stuck. A loop that plays a note each pass isn't.
void TestStuck()
{
    AfterFrames({0x80, 0x01, 0x94, 0x00, 0x00, 0x00}, 10, [](const snd::Driver& driver, const std::vector<int>&)
                { XSFLIB_CHECK(driver.IsSettled(0) && driver.IsStuck(0)); });
    AfterFrames({0x3C, 0x7F, 0x01, 0x80, 0x01, 0x94, 0x00, 0x00, 0x00}, 10,
                [](const snd::Driver& driver, const std::vector<int>&) { XSFLIB_CHECK(!driver.IsStuck(0)); });
}

// A tie struck again in each pass of an endless loop holds the sequence: the track passes still, and the tied note
// doesn't end by itself. It isn't stuck: the note sounds. A loop that strikes a new note each pass doesn't hold. Nor
// does a tie that sweeps in each pass: each pass changes the sound. Nor does a loop of waits while a note of 240 ticks
// sounds, without waiting for it (0xC7 0): that note ends by itself.
void TestHolding()
{
    AfterFrames({0xC8, 0x01, 0xD4, 0x00, 0x3C, 0x7F, 0x18, 0xFC, 0xFF}, 200,
                [](const snd::Driver& driver, const std::vector<int>&)
                { XSFLIB_CHECK(driver.IsHolding(0) && !driver.IsStuck(0)); });
    AfterFrames({0x3C, 0x7F, 0x01, 0x80, 0x01, 0x94, 0x00, 0x00, 0x00}, 10,
                [](const snd::Driver& driver, const std::vector<int>&) { XSFLIB_CHECK(!driver.IsHolding(0)); });
    AfterFrames({0xC8, 0x01, 0xE3, 0x40, 0x00, 0x3C, 0x7F, 0x01, 0x94, 0x05, 0x00, 0x00}, 20,
                [](const snd::Driver& driver, const std::vector<int>&) { XSFLIB_CHECK(!driver.IsHolding(0)); });
    AfterFrames({0xC7, 0x00, 0x3C, 0x7F, 0x81, 0x70, 0xD4, 0x00, 0x80, 0x01, 0xFC, 0xFF}, 40,
                [](const snd::Driver& driver, const std::vector<int>&)
                { XSFLIB_CHECK(driver.IsTrackIdle(0, 0) && !driver.IsHolding(0)); });
}

// A loop whose count is 0 loops for ever. It isn't a counted loop.
void TestEndlessLoop()
{
    AfterFrames({0xD4, 0x00, 0x80, 0x01, 0xFC, 0xFF}, 10, [](const snd::Driver& driver, const std::vector<int>&)
                { XSFLIB_CHECK(driver.OutermostCountedLoops(0).empty()); });
}

// The sequence last changed the sound at its first command, the volume's. The loop after it changes nothing.
void TestChangedAt()
{
    AfterFrames({0xC1, 0x64, 0x80, 0x01, 0x94, 0x02, 0x00, 0x00}, 20,
                [](const snd::Driver& driver, const std::vector<int>&) { XSFLIB_CHECK(driver.ChangedAt(0) == 1); });
}

// A tie to the same key at the same velocity changes the sound when it sweeps the pitch: each pass is a loop. Each pass
// takes 2 frames.
void TestTieThatSweeps()
{
    const Bytes sequence = {
        0xC8, 0x01,             // 0: ties
        0xE3, 0x40, 0x00,       // 2: a sweep of a semitone
        0x3C, 0x7F, 0x01,       // 5: a note of a tick
        0x94, 0x05, 0x00, 0x00, // 8: back to 5
    };
    AfterFrames(sequence, 20, [](const snd::Driver&, const std::vector<int>& loops)
                { XSFLIB_CHECK(loops.size() >= 8 && loops == std::vector<int>(loops.size(), 0)); });
}

// Track 0 and track 1 both play a note each pass. Only track 0's passes are the sequence's loops: track 1 isn't the
// main track while track 0 plays.
void TestMainTrack()
{
    const Bytes sequence = {
        0xFE, 0x03, 0x00,             // 0: tracks 0 and 1
        0x93, 0x01, 0x0F, 0x00, 0x00, // 3: track 1 starts at 0x0F
        0x3C, 0x7F, 0x18,             // 8: a note of 24 ticks
        0x94, 0x08, 0x00, 0x00,       // 11: back to 8
        0x40, 0x7F, 0x18,             // 15: a note of 24 ticks
        0x94, 0x0F, 0x00, 0x00,       // 18: back to 15
    };
    AfterFrames(sequence, 200, [](const snd::Driver&, const std::vector<int>& loops)
                { XSFLIB_CHECK(loops.size() >= 3 && loops == std::vector<int>(loops.size(), 0)); });
}

// Track 1 holds a tied note on a looping wave and changes its volume in each pass. Track 0 runs `track0` in a loop
// meanwhile.
Bytes HoldsNote(const Bytes& track0)
{
    Bytes sequence = {0xFE, 0x03, 0x00, 0x93, 0x01, 0x00, 0x00, 0x00};
    sequence.insert(sequence.end(), track0.begin(), track0.end());
    sequence.insert(sequence.end(), {0x94, 0x08, 0x00, 0x00});
    const auto start = static_cast<uint8_t>(sequence.size());
    sequence[5] = start;
    sequence.insert(sequence.end(), {
                                        0xC8,
                                        0x01, // ties
                                        0x3C,
                                        0x7F,
                                        0x01, // a note of a tick
                                        0xC1,
                                        0x64, // the volume takes 100
                                        0x80,
                                        0x01, // a wait of a tick
                                        0xC1,
                                        0x5A, // the volume takes 90
                                        0x80,
                                        0x01, // a wait of a tick
                                        0x94,
                                        static_cast<uint8_t>(start + 5),
                                        0,
                                        0, // back to the volume of 100
                                    });
    return sequence;
}

// While track 0 rests, track 1's passes are the sequence's loops once its note sustains alone. A decay of 0 keeps the
// note from reaching its sustain. While track 0 plays notes, only track 0's passes are.
void TestNoteHeldAlone()
{
    const Bytes rests = HoldsNote({0x80, 0x01});
    AfterFrames(rests, 100, [](const snd::Driver&, const std::vector<int>& loops)
                { XSFLIB_CHECK(loops.size() >= 3 && loops == std::vector<int>(loops.size(), 1)); });
    AfterFrames(rests, 100, [](const snd::Driver&, const std::vector<int>& loops) { XSFLIB_CHECK(loops.empty()); }, {},
                {.decay = 0, .sustain = 0});
    AfterFrames(HoldsNote({0x3C, 0x7F, 0x01}), 100, [](const snd::Driver&, const std::vector<int>& loops)
                { XSFLIB_CHECK(loops.size() >= 3 && loops == std::vector<int>(loops.size(), 0)); });
}

// A driver whose hooks have no loop hook doesn't follow lengths. It plays a loop without calling the hooks.
void TestWithoutFollowing()
{
    const Bytes file = ArchiveFile({0x3C, 0x7F, 0x30, 0x80, 0x30, 0x94, 0x00, 0x00, 0x00});
    const sdat::Archive archive = sdat::Archive::Parse(file);
    ntr::ArchivePlayer player(archive);
    player.Start({0});
    int commands = 0;
    player.FollowLengths({.on_command_start = [&commands](int, int) { commands++; }});
    for (int frame = 0; frame < 400; frame++)
    {
        player.RunFrame();
    }
    XSFLIB_CHECK(player.Driver().GetPlayer(0).active && commands == 0);
}

// Track 0 plays a note each 48 ticks. Track 1 waits in a loop of idle passes until tick 11, then loops each 192 ticks,
// changing its volume. Its idle passes don't count: at tick 30, track 1 holds the fade back. After its second pass, at
// tick 395, it doesn't.
void TestIdlePassesOfOtherTracks()
{
    const Bytes file = ArchiveFile({
        0xFE, 0x03, 0x00,             // 0: tracks 0 and 1
        0x93, 0x01, 0x0F, 0x00, 0x00, // 3: track 1 starts at 0x0F
        0x3C, 0x7F, 0x30,             // 8: a note of 48 ticks
        0x94, 0x08, 0x00, 0x00,       // 11: back to 8
        0x80, 0x01,                   // 15: a wait of a tick
        0xB1, 0x00, 0x01, 0x00,       // 17: add 1 to variable 0
        0xBC, 0x00, 0x0A, 0x00,       // 21: whether variable 0 is below 10
        0xA2, 0x94, 0x0F, 0x00, 0x00, // 25: if so, back to 15
        0xC1, 0x64,                   // 30: the volume takes 100
        0x80, 0x60,                   // 32: a wait of 96 ticks
        0xC1, 0x5A,                   // 34: the volume takes 90
        0x80, 0x60,                   // 36: a wait of 96 ticks
        0x94, 0x1E, 0x00, 0x00,       // 38: back to 30
    });
    const sdat::Archive archive = sdat::Archive::Parse(file);
    ntr::ArchivePlayer player(archive);
    player.Start({0});
    ntr::LengthModel model(player);
    model.OnLoop([](int, uint32_t) {});
    for (int frame = 0; frame < 60; frame++)
    {
        model.RunFrame();
    }
    XSFLIB_CHECK(!model.OthersLooped(0));
    for (int frame = 60; frame < 800; frame++)
    {
        model.RunFrame();
    }
    XSFLIB_CHECK(model.OthersLooped(0));
}

// The sound unit's place of a channel says whether its wave loops. The wave of 4,096 samples plays for 125 ms.
void TestVoicePlaces()
{
    for (const bool loop : {true, false})
    {
        const files::Wave wave{
            .loop = static_cast<uint8_t>(loop ? 1 : 0), .loop_length = 1024, .data = Bytes(4096, 0x40)};
        const Bytes file = ArchiveFile({0x3C, 0x7F, 0x30, 0xFF}, {}, wave);
        const sdat::Archive archive = sdat::Archive::Parse(file);
        ntr::ArchivePlayer player(archive);
        player.Start({0});
        for (int frame = 0; frame < 4; frame++)
        {
            player.RunFrame();
        }

        int playing = 0;
        for (int channel = 0; channel < snd::kChannelCount; channel++)
        {
            const ntr::SoundUnit::ChannelPlace place = player.Unit().PlaceOf(channel);
            if (place.playing)
            {
                playing++;
                XSFLIB_CHECK(place.loops == loop);
            }
        }
        XSFLIB_CHECK(playing == 1);
    }
}

void TestErrors()
{
    const Bytes file = ArchiveFile({0xFF});
    const sdat::Archive archive = sdat::Archive::Parse(file);
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { ntr::MeasureSequence(archive, {5}); }) ==
                 "the archive has no sequence 5");

    // A track that jumps back to itself without waiting hangs.
    const Bytes hangs = ArchiveFile({0x94, 0x00, 0x00, 0x00});
    const sdat::Archive hanging = sdat::Archive::Parse(hangs);
    XSFLIB_CHECK(ErrorOf<snd::Hang>([&] { ntr::MeasureSequence(hanging, {0}); }) ==
                 "track 0 of player 0 runs 100000 commands in a tick without waiting, up to offset 0x0: the sequence "
                 "hangs");
}

} // namespace

int main()
{
    TestRules();
    TestEnd();
    TestSilent();
    TestLoop();
    TestTracksOfOtherLengths();
    TestTrackOpenedAgain();
    TestHeldNote();
    TestCountedLoopPastTheLimit();
    TestRandomDraws();
    TestAfterTheLastPass();
    TestVariableReads();
    TestStuck();
    TestHolding();
    TestEndlessLoop();
    TestChangedAt();
    TestTieThatSweeps();
    TestMainTrack();
    TestNoteHeldAlone();
    TestWithoutFollowing();
    TestIdlePassesOfOtherTracks();
    TestVoicePlaces();
    TestErrors();

    return xsflib::testing::Summary("DS length model");
}
