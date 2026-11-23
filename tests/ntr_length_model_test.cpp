// SPDX-License-Identifier: MIT

// Tests of the length analysis of a DS sound archive's sequence (ntdlib/ntr/length_model.h), on archives that
// testing/sdat_files.h builds. At the default tempo of 120 and 48 ticks a beat, the driver plays a tick each 2 frames,
// from the frame that starts the sequence. A frame's commands sound two frames later. The expected frames are worked
// out by hand from the sequences.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <utility>
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
    return LoopAt(frame, ntr::kFrameMs, 7000.0);
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

// A note of 48 ticks, a wait of 48 and the end. The sequence ends where its note's release falls silent.
void TestEnd()
{
    const std::optional<Timing> timing = Measure({0x3C, 0x7F, 0x30, 0x80, 0x30, 0xFF});
    XSFLIB_CHECK(timing && !timing->silent && timing->fade_ms == 0);
    XSFLIB_CHECK(timing && timing->length_ms > 500 && timing->length_ms < 600);
}

// A sequence without notes is silent, with a length of 0 and no first sound.
void TestSilent()
{
    const std::optional<Timing> timing = Measure({0x80, 0x30, 0xFF});
    XSFLIB_CHECK(timing && timing->silent && timing->length_ms == 0 && timing->fade_ms == 0);
    XSFLIB_CHECK(timing && !timing->first_sample_ms && !timing->first_sound_ms);
}

// The first sample that isn't 0 of the same player's output, played frame by frame, is the timing's first sample,
// rounded to the millisecond. The output runs at kOutputRate. The first sound over -72 dB starts with a frame, at or
// before the first sample's frame ends.
void TestFirstSample()
{
    const Bytes sequence = {0x3C, 0x7F, 0x30, 0x80, 0x30, 0xFF};
    const Bytes file = ArchiveFile(sequence);
    const sdat::Archive archive = sdat::Archive::Parse(file);
    ntr::ArchivePlayer player(archive);
    player.Start({0});
    std::optional<double> first_ms;
    std::vector<ntr::SoundOutput> samples;
    std::vector<int16_t> output;
    for (uint64_t frame = 0; frame < 100 && !first_ms; frame++)
    {
        samples.clear();
        output.clear();
        player.RunFrame(samples);
        ntr::AppendPcm16(samples, output);
        for (std::size_t i = 0; i < output.size() && !first_ms; i++)
        {
            if (output[i] != 0)
            {
                first_ms =
                    static_cast<double>(frame) * ntr::kFrameMs + static_cast<double>(i / 2) * 1000.0 / ntr::kOutputRate;
            }
        }
    }

    const std::optional<Timing> timing = ntr::MeasureSequence(archive, {0});

    XSFLIB_CHECK(first_ms && *first_ms > 0.0);
    XSFLIB_CHECK(first_ms && timing && timing->first_sample_ms == static_cast<long long>(*first_ms + 0.5));
    XSFLIB_CHECK(first_ms && timing && timing->first_sound_ms &&
                 static_cast<double>(*timing->first_sound_ms) < *first_ms + ntr::kFrameMs);
}

// The jump back loops each 96 ticks: at frames 192 and 384. The fade starts two frames after the second loop. A pass
// is 192 frames, 1,000 ms, and the two passes start 11 ms after the start, at the end of 386 frames less 2,000 ms.
void TestLoop()
{
    const std::optional<Timing> timing = Measure({0x3C, 0x7F, 0x30, 0x80, 0x30, 0x94, 0x00, 0x00, 0x00});
    XSFLIB_CHECK(timing && *timing == LoopFrom(386));
    XSFLIB_CHECK(timing && timing->loop == (xsflib::rip::LoopPass{.start_ms = 11, .length_ms = 1000}));
}

// The commands after the start set the tempo's ratio to 512, twice the sequence's tempo: the jump back loops each 96
// ticks at frames 96 and 192, and a pass is 96 frames, 500 ms. The same commands addressed to player 1 change nothing.
void TestCommandsAfterStart()
{
    const Bytes file = ArchiveFile({0x3C, 0x7F, 0x30, 0x80, 0x30, 0x94, 0x00, 0x00, 0x00});
    const sdat::Archive archive = sdat::Archive::Parse(file);
    const auto ratio = [](uint32_t player)
    {
        return snd::CommandList{{.id = snd::CommandId::kPlayerParameter, .args = {player, 0x1A, 512, 2}}};
    };

    const std::optional<Timing> faster =
        ntr::MeasureSequence(archive, {0}, {}, {}, {}, xsflib::rip::Limit::kDefault, ratio(0));
    const std::optional<Timing> other =
        ntr::MeasureSequence(archive, {0}, {}, {}, {}, xsflib::rip::Limit::kDefault, ratio(1));

    XSFLIB_CHECK(faster && *faster == LoopFrom(194));
    XSFLIB_CHECK(faster && faster->loop && faster->loop->length_ms == 500);
    XSFLIB_CHECK(other && *other == LoopFrom(386));
}

// A list of the game's code at tick 0 sets the tempo's ratio to 512 in the game's first frame after the start. The
// game's frames last one frame of the screen, 3.2 of the driver's frames: the frame that starts the sequence reads the
// count, and the next sends the list before the driver's frame 4. The driver has run ticks 0 and 1 by then, in frames 0
// and 2, and runs a tick in each frame from frame 4: tick 192, the second loop, comes in frame 194, two frames after
// the commands at the start make it come. A list for a tick that never comes holds the fade back to the limit.
void TestTickCommands()
{
    const Bytes file = ArchiveFile({0x3C, 0x7F, 0x30, 0x80, 0x30, 0x94, 0x00, 0x00, 0x00});
    const sdat::Archive archive = sdat::Archive::Parse(file);
    const snd::CommandList faster = {{.id = snd::CommandId::kPlayerParameter, .args = {0, 0x1A, 512, 2}}};
    const auto measure = [&archive](const std::vector<ntr::TickCommands>& tick_commands)
    {
        return ntr::MeasureSequence(archive, {0}, {}, {}, {}, xsflib::rip::Limit::kDefault, {}, tick_commands);
    };

    const std::optional<Timing> later = measure({{.tick = 0, .frames = {faster}}});
    const std::optional<Timing> never = measure({{.tick = 100000, .frames = {faster}}});

    XSFLIB_CHECK(later && *later == LoopFrom(196));
    XSFLIB_CHECK(later && later->loop && later->loop->length_ms == 500);
    XSFLIB_CHECK(never && never->at_limit && !never->loop);
}

// The game's frames last two frames of the screen: 1,120,380 system clocks, 6.42 of the driver's frames of 174,592.
// Game frame g sends its list before the first driver frame d with g * 1,120,380 <= d * 174,592. With a tick of 0, game
// frame 0 reads the count, and game frames 1, 2 and 3 send their lists before driver frames 7, 13 and 20. With a tick
// of 3 and a tick in each second driver frame, game frame 0 reads 0 ticks, and game frame 1, before driver frame 7,
// reads 4: game frames 2, 3 and 4 send before driver frames 13, 20 and 26. The SendDue that sends the last list returns
// true, and Done holds from then on.
void TestTickListProgress()
{
    const auto ratio = [](uint32_t value)
    {
        return snd::CommandList{{.id = snd::CommandId::kPlayerParameter, .args = {0, 0x1A, value, 2}}};
    };
    const auto check = [&ratio](uint32_t tick, const std::vector<uint64_t>& expected)
    {
        const std::vector<ntr::TickCommands> lists = {
            {.tick = tick, .video_frames = 2, .frames = {ratio(300), ratio(400), ratio(500)}}};
        ntr::TickListProgress progress(lists.size());
        std::vector<uint64_t> sent;
        std::vector<uint64_t> finished;
        bool done_in_time = true;
        for (uint64_t frame = 0; frame < 40; frame++)
        {
            const auto ticks = static_cast<uint32_t>((frame + 1) / 2);
            const auto send = [&sent, frame](const snd::CommandList&)
            {
                sent.push_back(frame);
            };
            if (progress.SendDue(lists, frame * snd::kFrameClocks, ticks, send))
            {
                finished.push_back(frame);
            }
            done_in_time = done_in_time && progress.Done(lists) == (frame >= expected.back());
        }

        XSFLIB_CHECK(sent == expected);
        XSFLIB_CHECK(finished == std::vector<uint64_t>{expected.back()});
        XSFLIB_CHECK(done_in_time);
    };

    check(0, {7, 13, 20});
    check(3, {13, 20, 26});
}

// The search for the main loop starts again after the last list. The jump back loops at ticks 96 and 192, in driver
// frames 192 and 384, and the list still to come holds the fade back. A list for tick 200 comes in game frame 126 of
// one frame of the screen each, before driver frame 405: game frame 125, before driver frame 402, reads 201 ticks. The
// driver runs tick 202 in frame 404 and takes the list after the tick of frame 405. Frame 405 has no tick. From frame
// 406 a tick runs in each frame: tick 203 in frame 406. The loops at ticks 288 and 384 therefore come in frames 491 and
// 587. The fade starts two frames after the second, and a pass is 96 frames, 500 ms. The loops before the list don't
// count.
void TestLoopSearchAfterLastList()
{
    const Bytes file = ArchiveFile({0x3C, 0x7F, 0x30, 0x80, 0x30, 0x94, 0x00, 0x00, 0x00});
    const sdat::Archive archive = sdat::Archive::Parse(file);
    const snd::CommandList faster = {{.id = snd::CommandId::kPlayerParameter, .args = {0, 0x1A, 512, 2}}};

    const std::optional<Timing> timing = ntr::MeasureSequence(archive, {0}, {}, {}, {}, xsflib::rip::Limit::kDefault,
                                                              {}, {{.tick = 200, .frames = {faster}}});

    XSFLIB_CHECK(timing && *timing == LoopFrom(589));
    XSFLIB_CHECK(timing && timing->loop && timing->loop->length_ms == 500);
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
    XSFLIB_CHECK(timing && !timing->silent && timing->fade_ms == 7000 && !timing->loop);
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

// A loop of 704 s without counted loops: 44 calls of a part of 16 notes of 48 ticks, each followed by a wait of 48
// ticks, then a jump back. The pass takes 67,584 ticks, and the jumps back come at frames 135,168 and 270,336. The
// first measure reaches the limit of 600 s with no loop found: the limit's length, the fade and `at_limit`. With the
// long limit, the second measure finds two passes: the fade starts two frames after frame 270,336.
void TestLongLimit()
{
    Bytes sequence;
    constexpr uint8_t kPart = 44 * 4 + 4;
    for (int i = 0; i < 44; i++)
    {
        sequence.insert(sequence.end(), {0x95, kPart, 0x00, 0x00}); // call the part
    }

    sequence.insert(sequence.end(), {0x94, 0x00, 0x00, 0x00}); // back to 0
    for (int i = 0; i < 16; i++)
    {
        sequence.insert(sequence.end(), {0x3C, 0x7F, 0x30, 0x80, 0x30}); // a note and a wait of 48 ticks
    }

    sequence.push_back(0xFD); // return
    const Bytes file = ArchiveFile(sequence);
    const sdat::Archive archive = sdat::Archive::Parse(file);

    const std::optional<Timing> first = ntr::MeasureSequence(archive, {0});
    const std::optional<Timing> long_limit = ntr::MeasureSequence(archive, {0}, {}, {}, {}, xsflib::rip::Limit::kLong);

    XSFLIB_CHECK(first && *first == (Timing{.length_ms = 600000, .fade_ms = 7000}) && first->at_limit);
    XSFLIB_CHECK(long_limit && *long_limit == LoopFrom(270338) && !long_limit->at_limit);
}

// The counted loops past the limit get the same verdict with the long limit: the second measure makes the outer loop
// endless whatever the limit.
void TestCountedLoopWithLongLimit()
{
    const Bytes file = ArchiveFile({0xD4, 0x03, 0xD4, 0xFF, 0x3C, 0x7F, 0x30, 0x80, 0x30, 0xFC, 0xFC, 0xFF});
    const sdat::Archive archive = sdat::Archive::Parse(file);

    const std::optional<Timing> timing = ntr::MeasureSequence(archive, {0}, {}, {}, {}, xsflib::rip::Limit::kLong);

    XSFLIB_CHECK(timing && *timing == LoopFrom(97922));
}

// The reverb of the capture units rings after a sequence's last note: a sequence that ends with the reverb running ends
// at its output's last frame with sound, a frame of the reverb's echo. The reverb is the one that Blue
// Rescue Team runs: 0x900 bytes of PCM16 at 16,000 Hz, at a volume of 45.
void TestReverbRings()
{
    const Bytes sequence = {0x3C, 0x7F, 0x30, 0x80, 0x30, 0xFF};
    const Bytes file = ArchiveFile(sequence);
    const sdat::Archive archive = sdat::Archive::Parse(file);
    const ntr::GameSettings settings = {.reverb = ntr::Reverb{.size = 0x900, .sample_rate = 16000, .volume = 45}};

    // The last frame with sound in the output of the same player, played for 10 s.
    ntr::ArchivePlayer player(archive);
    player.Start({0}, settings);
    uint64_t last_sound = 0;
    std::vector<ntr::SoundOutput> samples;
    std::vector<int16_t> output;
    for (uint64_t frame = 0; frame < 1920; frame++)
    {
        samples.clear();
        output.clear();
        player.RunFrame(samples);
        ntr::AppendPcm16(samples, output);
        if (xsflib::rip::Audible(output))
        {
            last_sound = frame;
        }
    }

    const std::optional<Timing> dry = ntr::MeasureSequence(archive, {0});
    const std::optional<Timing> wet = ntr::MeasureSequence(archive, {0}, settings);

    XSFLIB_CHECK(dry && wet && wet->length_ms > dry->length_ms);
    XSFLIB_CHECK(wet && *wet == xsflib::rip::EndAt(last_sound + 1, ntr::kFrameMs));
}

// The length of `sequence` with `settings`.
std::optional<Timing> MeasureWith(const Bytes& sequence, const ntr::GameSettings& settings)
{
    const Bytes file = ArchiveFile(sequence);
    const sdat::Archive archive = sdat::Archive::Parse(file);
    return ntr::MeasureSequence(archive, {0}, settings);
}

// While the capture units run, channels 1 and 3 play the capture buffer in a loop. A sequence that is stuck after its
// last note still ends where its echo dies away, as one that ends with fin does. A note held from the first frame still
// loops.
void TestStuckWithCaptures()
{
    const ntr::GameSettings reverb = {.reverb = ntr::Reverb{.size = 0x900, .sample_rate = 16000, .volume = 45}};
    const ntr::GameSettings effect = {.effect = ntr::OutputEffect{}};
    const Bytes fin = {0x3C, 0x7F, 0x30, 0x80, 0x30, 0xFF};
    const Bytes poll = {0x3C, 0x7F, 0x30, 0x80, 0x30, 0x80,
                        0x01, 0x94, 0x05, 0x00, 0x00}; // a wait of a tick, back to 5
    const Bytes poll_at_once = {0x3C, 0x7F, 0x30, 0x80, 0x01, 0x94, 0x03, 0x00, 0x00};
    const Bytes wait_for_ever = {0x3C, 0x7F, 0x30, 0x80, 0x30, 0xA1, 0x80, 0x00, 0xFF}; // a wait of variable 0, at -1
    for (const ntr::GameSettings& settings : {reverb, effect})
    {
        const std::optional<Timing> ended = MeasureWith(fin, settings);
        XSFLIB_CHECK(ended && ended->fade_ms == 0);
        const auto ends_the_same = [&ended, &settings](const Bytes& sequence)
        {
            const std::optional<Timing> timing = MeasureWith(sequence, settings);
            return ended && timing && *timing == *ended;
        };
        XSFLIB_CHECK(ends_the_same(poll));
        XSFLIB_CHECK(ends_the_same(poll_at_once));
        XSFLIB_CHECK(ends_the_same(wait_for_ever));
    }

    const std::optional<Timing> held = MeasureWith({0x3C, 0x7F, 0x00, 0xFF}, effect);
    XSFLIB_CHECK(held && held->fade_ms == 7000);
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

// A note that a track strikes at a level of -72.3 dB or less gets a volume of 0 in SOUNDxCNT, and changes nothing in
// the sound. The table of levels gives -32768 for a volume of 0, -72.1 dB for a velocity of 2, -72.2 dB for a velocity
// of 1 and -4.1 dB for a volume of 100.
void TestSilentNotes()
{
    // Track 0 sets its volume to 0 and strikes a note of 2 ticks, in an endless loop. Track 1 plays a note of 24 ticks
    // in a loop. Track 0's first pass changes its volume, and is a loop of the sequence. Its later passes are idle.
    // Track 1 is then the main track: its passes at ticks 24, 48, 72 and 96 are the sequence's loops.
    const Bytes busy = {
        0xFE, 0x03, 0x00,             // 0: tracks 0 and 1
        0x93, 0x01, 0x11, 0x00, 0x00, // 3: track 1 starts at 0x11
        0xD4, 0x00,                   // 8: an endless loop
        0xC1, 0x00,                   // 10: the volume takes 0
        0x3C, 0x02, 0x02,             // 12: a note of 2 ticks at velocity 2
        0xFC,                         // 15: the loop's end
        0xFF,                         // 16
        0x40, 0x7F, 0x18,             // 17: a note of 24 ticks
        0x94, 0x11, 0x00, 0x00,       // 20: back to 17
    };
    AfterFrames(busy, 200,
                [](const snd::Driver& driver, const std::vector<int>& loops)
                {
                    XSFLIB_CHECK(loops == (std::vector<int>{0, 1, 1, 1, 1}));
                    XSFLIB_CHECK(driver.IsTrackIdle(0, 0) && !driver.IsTrackIdle(0, 1));
                });

    // A note at velocity 1 and volume 100 is at -76.3 dB. Its passes are idle, and the sequence has no loop.
    const Bytes faint = {
        0xC1, 0x64,             // 0: the volume takes 100
        0x3C, 0x01, 0x18,       // 2: a note of 24 ticks at velocity 1
        0x94, 0x02, 0x00, 0x00, // 5: back to 2
    };
    AfterFrames(faint, 200, [](const snd::Driver& driver, const std::vector<int>& loops)
                { XSFLIB_CHECK(loops.empty() && driver.IsTrackIdle(0, 0)); });

    // A silent note counts as no note. The passes are still and quiet, and the player is stuck. Each pass strikes the
    // note again, and the note's channel stays silent.
    AfterFrames(faint, 200,
                [](const snd::Driver& driver, const std::vector<int>&)
                {
                    bool silent = true;
                    for (int c = 0; c < snd::kChannelCount; c++)
                    {
                        silent = silent && (!driver.GetChannel(c).active || driver.IsChannelSilent(c));
                    }
                    XSFLIB_CHECK(driver.IsStuck(0) && driver.ActiveChannels() > 0 && silent);
                });

    // The same note at velocity 2 and volume 127 is at -72.1 dB, and sounds. Each pass is a loop.
    const Bytes quiet = {
        0x3C, 0x02, 0x18,       // 0: a note of 24 ticks at velocity 2
        0x94, 0x00, 0x00, 0x00, // 3: back to 0
    };
    AfterFrames(quiet, 200, [](const snd::Driver&, const std::vector<int>& loops)
                { XSFLIB_CHECK(loops == (std::vector<int>{0, 0, 0, 0})); });

    // A note struck at volume 0 sounds once its track raises the volume. Notes don't wait here (0xC7 0). Each pass
    // changes the volume, and is a loop.
    const Bytes swell = {
        0xC7, 0x00,             // 0: notes don't wait
        0xC1, 0x00,             // 2: the volume takes 0
        0x3C, 0x7F, 0x18,       // 4: a note of 24 ticks
        0x80, 0x0C,             // 7: a wait of 12 ticks
        0xC1, 0x7F,             // 9: the volume takes 127
        0x80, 0x0C,             // 11: a wait of 12 ticks
        0x94, 0x02, 0x00, 0x00, // 13: back to 2
    };
    AfterFrames(swell, 200, [](const snd::Driver&, const std::vector<int>& loops)
                { XSFLIB_CHECK(loops == (std::vector<int>{0, 0, 0, 0})); });

    // The note of 24 ticks in a loop straight back, after `settings`. A note that sounds makes each pass a loop, at
    // ticks 24, 48, 72 and 96. A silent note makes none.
    const auto loops_after = [](const Bytes& settings, uint8_t key, uint8_t velocity)
    {
        Bytes sequence = settings;
        const auto start = static_cast<uint8_t>(sequence.size());
        sequence.insert(sequence.end(), {key, velocity, 0x18, 0x94, start, 0x00, 0x00});
        std::vector<int> found;
        AfterFrames(sequence, 200, [&found](const snd::Driver&, const std::vector<int>& loops) { found = loops; });
        return found;
    };
    const std::vector<int> each_pass = {0, 0, 0, 0};

    // Velocity 1 at volume 126 is at -72.2 dB - 0.1 dB = -72.3 dB, the level of a volume of 0.
    XSFLIB_CHECK(loops_after({0xC1, 0x7E}, 0x3C, 0x01).empty());
    XSFLIB_CHECK(loops_after({0xC1, 0x7F}, 0x3C, 0x01) == each_pass);

    // An expression of 0 or a player volume of 0 silences a note at velocity 127 and volume 127.
    XSFLIB_CHECK(loops_after({0xD5, 0x00}, 0x3C, 0x7F).empty());
    XSFLIB_CHECK(loops_after({0xC2, 0x00}, 0x3C, 0x7F).empty());

    // An LFO on the volume (0xCC 1) with a depth (0xCA) and a range (0xCD) can raise the level of the note at -76.3
    // dB. Its notes count as sounding. Without the depth or the range, or on the pitch, an LFO leaves the note silent.
    XSFLIB_CHECK(loops_after({0xC1, 0x64, 0xCC, 0x01, 0xCA, 0x7F}, 0x3C, 0x01) == each_pass);
    XSFLIB_CHECK(loops_after({0xC1, 0x64, 0xCC, 0x01}, 0x3C, 0x01).empty());
    XSFLIB_CHECK(loops_after({0xC1, 0x64, 0xCC, 0x01, 0xCA, 0x7F, 0xCD, 0x00}, 0x3C, 0x01).empty());
    XSFLIB_CHECK(loops_after({0xC1, 0x64, 0xCA, 0x7F}, 0x3C, 0x01).empty());

    // Two silent notes of 12 ticks at keys 60 and 62 in each pass. The key that a portamento starts from changes with
    // each note. A silent note's key doesn't count, and the passes stay idle.
    const Bytes keys = {
        0xC1, 0x64,             // 0: the volume takes 100
        0x3C, 0x01, 0x0C,       // 2: note 60 for 12 ticks at velocity 1
        0x3E, 0x01, 0x0C,       // 5: note 62 for 12 ticks at velocity 1
        0x94, 0x02, 0x00, 0x00, // 8: back to 2
    };
    AfterFrames(keys, 200, [](const snd::Driver& driver, const std::vector<int>& loops)
                { XSFLIB_CHECK(loops.empty() && driver.IsTrackIdle(0, 0)); });
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
    TestFirstSample();
    TestLoop();
    TestCommandsAfterStart();
    TestTickCommands();
    TestTickListProgress();
    TestLoopSearchAfterLastList();
    TestTracksOfOtherLengths();
    TestTrackOpenedAgain();
    TestHeldNote();
    TestCountedLoopPastTheLimit();
    TestLongLimit();
    TestCountedLoopWithLongLimit();
    TestReverbRings();
    TestStuckWithCaptures();
    TestRandomDraws();
    TestAfterTheLastPass();
    TestVariableReads();
    TestStuck();
    TestHolding();
    TestEndlessLoop();
    TestChangedAt();
    TestTieThatSweeps();
    TestMainTrack();
    TestSilentNotes();
    TestNoteHeldAlone();
    TestWithoutFollowing();
    TestIdlePassesOfOtherTracks();
    TestVoicePlaces();
    TestErrors();

    return xsflib::testing::Summary("DS length model");
}
