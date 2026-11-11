// SPDX-License-Identifier: MIT

// Tests of brownie::ArchivePlayer on small synthetic cartridges. No game ROM is needed. Each expected value follows a
// rule of the driver's code, of the GBA's sound hardware or of the enhanced mode's rules in ntdlib/brownie/sequencer.h.
// The comments give the rule.
//
// The hardware's output at 10 bits is the bias of 0x200 plus each FIFO's byte times 4, rounded down to the resolution's
// steps. A step of the 10 bits gives 64 in a 16-bit sample, so a FIFO's byte b gives b * 256.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "seqlib/testing/check.h"
#include "xsflib/binary.h"
#include "xsflib/player/archive_source.h"
#include "xsflib/testing/check.h"

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/brownie/archive_player.h"
#include "ntdlib/brownie/archive_source.h"
#include "ntdlib/brownie/driver.h"
#include "ntdlib/brownie/sequencer.h"
#include "ntdlib/testing/brownie_cart.h"

namespace
{

using namespace ntdlib::testing::brownie_cart;

using ntdlib::agb::kEnhancedFrameSamples;
using ntdlib::agb::kEnhancedRate;
using ntdlib::agb::kFrameCycles;
using ntdlib::brownie::ArchivePlayer;
using ntdlib::brownie::ArchivePlayerOptions;
using ntdlib::brownie::kChannelCount;

// A note of 192 frames on a sample channel, at full level on both sides. Key 38 plays semitone 2 at the mixer's rate,
// and key 26 semitone 2 at half of it. The cart's even semitones loop.
constexpr uint8_t kHoldKey = 38;

std::vector<uint8_t> SampleChannel(uint8_t key = kHoldKey)
{
    return {0xE1, 0, 0xEA, 0xFF, 0xF0, kSampleHold, 0xF2, 0xFF, 0x8F, key, 0xFF};
}

// A square note of 192 frames at volume 15, on both sides.
std::vector<uint8_t> SquareChannel()
{
    return {0xF0, kPsgHold, 0xF1, 0x80, 0xFD, 0x11, 0x8F, 30, 0xFF};
}

// Fills semitone `semitone`'s sample with `value`.
void Constant(Cart& cart, uint32_t semitone, uint8_t value)
{
    for (uint32_t i = 0; i < kSampleBytes; i++)
    {
        cart.Put8(kSamples + 0x100 * semitone + i, value);
    }
}

// Fills semitone `semitone`'s sample with one period of a sine.
void Sine(Cart& cart, uint32_t semitone)
{
    for (uint32_t i = 0; i < kSampleBytes; i++)
    {
        const double phase = 2 * 3.14159265358979 * double(i) / kSampleBytes;
        cart.Put8(kSamples + 0x100 * semitone + i, uint8_t(int8_t(std::lround(100 * std::sin(phase)))));
    }
}

// Returns a player's samples of `frames` frames.
std::vector<int16_t> Run(ArchivePlayer& player, int frames)
{
    std::vector<int16_t> out;
    for (int f = 0; f < frames; f++)
    {
        player.RunFrame(out);
    }

    return out;
}

// Returns the samples of sound `sound`'s first `frames` frames.
std::vector<int16_t> Played(const Rom& rom, const DriverInfo& info, int sound, int frames,
                            const std::function<void(ArchivePlayer&)>& setup = {})
{
    ArchivePlayer player(rom, info);
    player.Start(sound);
    if (setup)
    {
        setup(player);
    }

    return Run(player, frames);
}

// The sum of two FIFOs' bytes, or their wrap in a byte, follows the mixer's rule. Two negative points take the sum of
// their sizes, rounded, and its negative. Any other pair takes the signed sum, rounded. The sums wrap in the FIFO's
// byte (the mixer at 0x03003150 in Sword of Mana). The output holds each byte times 256.
void TestMixer()
{
    struct Case
    {
        uint8_t first = 0;
        uint8_t second = 0;
        int byte = 0;
    };

    const Case cases[] = {
        {0x40, 0x20, 96},   // (64 * 255 + 32 * 255 + 128) >> 8
        {0xC0, 0xC0, -128}, // -((64 * 255 + 64 * 255 + 128) >> 8)
        {0x60, 0x60, -65},  // (96 * 255 * 2 + 128) >> 8 = 191, which wraps
        {0xC0, 0x40, 0},    // (-64 * 255 + 64 * 255 + 128) >> 8
        {0xFF, 0xFF, -2},   // -((1 * 255 + 1 * 255 + 128) >> 8)
    };
    for (const Case& c : cases)
    {
        Cart cart;
        Constant(cart, 2, c.first);
        Constant(cart, 4, c.second);
        cart.Sound(1, {{8, SampleChannel(), {}}, {9, SampleChannel(40), {}}});
        const Rom rom = cart.ToRom();
        const DriverInfo info = Detect(rom);

        ArchivePlayer player(rom, info);
        player.Start(1);
        SEQLIB_CHECK_EQ(player.Rate(), 32768);
        std::vector<int16_t> out = Run(player, 10);
        SEQLIB_CHECK_EQ(out[out.size() - 2], c.byte * 256);
        SEQLIB_CHECK_EQ(out.back(), c.byte * 256);
    }
}

// In the mode that E2 sets, the mixer sums all 5 voices as signed points. The sum rounds and wraps in the byte.
void TestFiveVoices()
{
    Cart cart;
    Constant(cart, 2, 0x10);
    std::vector<TestChannel> channels;
    for (int c = 8; c < 13; c++)
    {
        std::vector<uint8_t> data = SampleChannel();
        if (c == 8)
        {
            data.insert(data.begin(), {0xE2, 0x01});
        }
        channels.push_back({c, data, {}});
    }
    cart.Sound(1, channels);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const std::vector<int16_t> out = Played(rom, info, 1, 10);

    // (5 * 16 * 255 + 128) >> 8 = 80
    SEQLIB_CHECK_EQ(out.back(), 80 * 256);
}

// Before the first note, the FIFOs hold nothing and the output is silent. Timer 0 starts with the note, at the stores
// of frame 1. Each overflow takes a point out of the FIFOs, 1,024 cycles apart. The first 48 take out the zeros that
// the FIFOs start with, and the 49th the mixer's first byte. A sample of the output lasts 512 cycles, and the first
// sample that starts after that overflow holds the byte.
void TestLead()
{
    Cart cart;
    Constant(cart, 2, 0x40);
    cart.Sound(1, {{8, SampleChannel(), {}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const std::vector<int16_t> out = Played(rom, info, 1, 4);

    // The FIFOs start with 48 points of silence. The note's first point follows them, at the 49th of Timer 0's
    // overflows after the stores of frame 1. The output takes a sample every 512 cycles.
    const size_t store = size_t(kFrameCycles) + kFrameCycles / 6;
    const size_t first = (store + 49 * 1024) / 512 + 1;
    size_t sounding = 0;
    while (sounding < out.size() / 2 && out[2 * sounding] == 0)
    {
        sounding++;
    }
    SEQLIB_CHECK_EQ(sounding, first);
    SEQLIB_CHECK_EQ(out[2 * first], 64 * 256);
    SEQLIB_CHECK_EQ(out[2 * first + 1], 64 * 256);
}

// A mute leaves a voice's points out of the mix, and a solo leaves out every other track. The output is the output of
// the sound without that channel.
void TestMutes()
{
    Cart cart;
    Constant(cart, 2, 0x10);
    Constant(cart, 4, 0x20);
    cart.Sound(1, {{8, SampleChannel(), {}}, {9, SampleChannel(40), {}}});
    cart.Sound(2, {{8, SampleChannel(), {}}});
    cart.Sound(3, {{0, SquareChannel(), {}}, {8, SampleChannel(), {}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const int kFrames = 12;

    const std::vector<int16_t> first = Played(rom, info, 2, kFrames);
    SEQLIB_CHECK(Played(rom, info, 1, kFrames, [](ArchivePlayer& p) { p.MuteTrack(9, true); }) == first);
    SEQLIB_CHECK(Played(rom, info, 1, kFrames, [](ArchivePlayer& p) { p.SoloTrack(8, true); }) == first);
    SEQLIB_CHECK(Played(rom, info, 3, kFrames, [](ArchivePlayer& p) { p.MuteTrack(0, true); }) == first);
    SEQLIB_CHECK(Played(rom, info, 3, kFrames, [](ArchivePlayer& p) { p.SoloTrack(8, true); }) == first);

    // A soloed and muted track stays silent, and a mute's end brings the track back in the next frame.
    const std::vector<int16_t> silent = Played(rom, info, 2, kFrames,
                                               [](ArchivePlayer& p)
                                               {
                                                   p.SoloTrack(8, true);
                                                   p.MuteTrack(8, true);
                                               });
    SEQLIB_CHECK(std::all_of(silent.begin(), silent.end(), [](int16_t s) { return s == 0; }));

    ArchivePlayer player(rom, info);
    player.Start(2);
    player.MuteTrack(8, true);
    Run(player, 6);
    SEQLIB_CHECK(player.IsTrackMuted(8));
    player.MuteTrack(8, false);
    const std::vector<int16_t> later = Run(player, 6);
    SEQLIB_CHECK_EQ(later.back(), 16 * 256);
}

// A sound effect's channel takes its PSG channel from the music's: the channel's mute then follows the sound effect's
// track, and the music's mute changes nothing there.
void TestEffectTakesChannel()
{
    Cart cart;
    const std::vector<uint8_t> effect = {0xE9, 0xF0, kPsgHold, 0xF1, 0x80, 0xFD, 0x11, 0x8F, 34, 0xFF};
    cart.Sound(1, {{0, SquareChannel(), {}}, {4, effect, {}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const int kFrames = 8;

    const std::vector<int16_t> both = Played(rom, info, 1, kFrames);
    const std::vector<int16_t> no_music = Played(rom, info, 1, kFrames, [](ArchivePlayer& p) { p.MuteTrack(0, true); });
    const std::vector<int16_t> no_effect =
        Played(rom, info, 1, kFrames, [](ArchivePlayer& p) { p.MuteTrack(4, true); });
    SEQLIB_CHECK(no_music.size() == both.size());
    SEQLIB_CHECK(std::any_of(both.begin(), both.end(), [&both](int16_t s) { return s != both[0]; }));

    // After frame 1, when both channels' notes ran, only the effect's track reaches the PSG channel.
    const size_t after = 2 * 2 * 600;
    SEQLIB_CHECK(
        std::equal(both.begin() + std::ptrdiff_t(after), both.end(), no_music.begin() + std::ptrdiff_t(after)));
    SEQLIB_CHECK(std::all_of(no_effect.begin() + std::ptrdiff_t(after), no_effect.end(),
                             [&](int16_t s) { return s == no_effect[after]; }));

    // E8 gives the channel back, and a rest of 4 frames later E9 takes it again in frame 5's stores. The music's mute
    // then stops silencing the channel for the rest of that frame.
    Cart later;
    const std::vector<uint8_t> takes_later = {0xE8, 0x80, 0xED, 0xE9, 0xF0, kPsgHold, 0xF1,
                                              0x80, 0xFD, 0x11, 0x8F, 34,   0xFF};
    later.Sound(1, {{0, SquareChannel(), {}}, {4, takes_later, {}}});
    const Rom later_rom = later.ToRom();
    const DriverInfo later_info = Detect(later_rom);
    const std::vector<int16_t> muted_music =
        Played(later_rom, later_info, 1, kFrames, [](ArchivePlayer& p) { p.MuteTrack(0, true); });
    size_t sounding = 0;
    while (sounding < muted_music.size() / 2 && muted_music[2 * sounding] == muted_music[0])
    {
        sounding++;
    }

    const size_t stores = (size_t(5) * kFrameCycles + kFrameCycles / 6) / 512;
    const size_t next_frame = size_t(6) * kFrameCycles / 512;
    SEQLIB_CHECK(sounding >= stores);
    SEQLIB_CHECK(sounding < next_frame);
}

// A voice's meter takes the largest point of its mix, at the voice's level, on the scale of 16-bit samples: a point p
// at level l reaches p * l / 256 in the FIFO's byte, and each of the byte's steps gives 256. A muted track's meter
// stays at 0. A meter changes nothing in the output.
void TestMeters()
{
    Cart cart;
    Constant(cart, 2, 0x40);
    cart.Sound(1, {{8, SampleChannel(), {}}, {0, SquareChannel(), {}}});
    cart.Sound(2, {{8, SampleChannel(), {}}});
    cart.Sound(3, {{8, {0xE1, 0, 0xEA, 0x0F, 0xF0, kSampleHold, 0xF2, 0xFF, 0x8F, kHoldKey, 0xFF}, {}}});
    cart.Sound(4, {{4, {0xE9, 0xF0, kPsgHold, 0xF1, 0x80, 0xFD, 0x11, 0x8F, 34, 0xFF}, {}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ArchivePlayer player(rom, info);
    player.Start(2);
    player.MeterTracks(true);
    const std::vector<int16_t> metered = Run(player, 6);
    SEQLIB_CHECK(metered == Played(rom, info, 2, 6));
    SEQLIB_CHECK_EQ(player.TrackPeaks(8)[0], 64 * 255);
    SEQLIB_CHECK_EQ(player.TrackPeaks(8)[1], 64 * 255);
    SEQLIB_CHECK_EQ(player.OutputPeaks()[0], 64 * 256);
    SEQLIB_CHECK_EQ(player.OutputPeaks()[1], 64 * 256);
    SEQLIB_CHECK_EQ(player.TrackPeaks(0)[0], 0);

    player.MuteTrack(8, true);
    Run(player, 1);
    SEQLIB_CHECK_EQ(player.TrackPeaks(8)[0], 0);

    // A frame without samples holds no peaks.
    player.MuteTrack(8, false);
    player.RunFrame();
    SEQLIB_CHECK_EQ(player.TrackPeaks(8)[0], 0);
    SEQLIB_CHECK_EQ(player.OutputPeaks()[0], 0);

    // A PSG channel's meter follows the channel's track.
    ArchivePlayer both(rom, info);
    both.Start(1);
    both.MeterTracks(true);
    Run(both, 6);
    SEQLIB_CHECK(both.TrackPeaks(0)[0] > 0);
    SEQLIB_CHECK_EQ(both.TrackPeaks(8)[0], 64 * 255);
    SEQLIB_CHECK(both.TrackPeaks(1)[0] == 0);

    // A sample voice's FIFOs give its levels' sides: FIFO A the right and FIFO B the left. A pan of 0x0F leaves only
    // the right side's level.
    ArchivePlayer right(rom, info);
    right.Start(3);
    right.MeterTracks(true);
    Run(right, 8);
    SEQLIB_CHECK_EQ(right.TrackPeaks(8)[0], 0);
    SEQLIB_CHECK_EQ(right.TrackPeaks(8)[1], 64 * 255);

    // A sound effect's channel gives its peak to the effect's track, not to the music's.
    ArchivePlayer effect(rom, info);
    effect.Start(4);
    effect.MeterTracks(true);
    Run(effect, 8);
    SEQLIB_CHECK(effect.TrackPeaks(4)[0] > 0);
    SEQLIB_CHECK_EQ(effect.TrackPeaks(0)[0], 0);
}

// A restore goes back to the place of its save, and the frames after it give what they gave. A frame without samples
// ends in the same state as a frame with them.
void TestSnapshots()
{
    for (const Revision revision : {Revision::kSwordOfMana, Revision::kMagicalVacation})
    {
        Cart cart(true, revision);
        Sine(cart, 2);
        const uint8_t key = revision == Revision::kSwordOfMana ? 38 : 48;
        cart.Sound(1, {{8, {0xE1, 0, 0xF0, kSampleHold, 0xF2, 0xFF, 0x8F, key, 0xFF}, {}}, {0, SquareChannel(), {}}});
        const Rom rom = cart.ToRom();
        const DriverInfo info = Detect(rom);

        ArchivePlayer player(rom, info);
        player.Start(1);
        Run(player, 20);
        const ArchivePlayer::Snapshot snapshot = player.Save();
        const std::vector<int16_t> after = Run(player, 20);
        const uint64_t frames = player.Frames();
        player.Restore(snapshot);
        SEQLIB_CHECK_EQ(player.Frames(), frames - 20);
        SEQLIB_CHECK(Run(player, 20) == after);

        // A snapshot just after a frame that started a timer goes back to the same place. The next frame starts no
        // timer change again.
        ArchivePlayer changed(rom, info);
        changed.Start(1);
        Run(changed, 2);
        SEQLIB_CHECK(!changed.Driver().TimerChanges().empty());
        const ArchivePlayer::Snapshot after_change = changed.Save();
        const std::vector<int16_t> played_on = Run(changed, 6);
        changed.Restore(after_change);
        SEQLIB_CHECK(Run(changed, 6) == played_on);

        ArchivePlayer quick(rom, info);
        quick.Start(1);
        for (int f = 0; f < 20; f++)
        {
            quick.RunFrame();
        }
        SEQLIB_CHECK(Run(quick, 20) == after);

        ArchivePlayer other(rom, info);
        other.Start(1);
        bool refused = false;
        try
        {
            other.Restore(snapshot);
        }
        catch (const std::invalid_argument&)
        {
            refused = true;
        }
        SEQLIB_CHECK(refused);
    }
}

// The sound ends when its channels have ended and the FIFOs have played what they hold.
void TestEnd()
{
    Cart cart;
    Constant(cart, 2, 0x40);
    cart.Sound(1, {{8, {0xE1, 0, 0xEA, 0xFF, 0xF0, kSampleHold, 0xF2, 0xFF, 0x80, kHoldKey, 0xFF}, {}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ArchivePlayer player(rom, info);
    player.Start(1);
    SEQLIB_CHECK(player.Busy());
    int frames = 0;
    std::vector<int16_t> out;
    while (player.Busy() && frames < 100)
    {
        player.RunFrame(out);
        frames++;
    }
    SEQLIB_CHECK(!player.Busy());

    // A length of 4 frames: the note in frame 1, the end in frame 5, and 2 frames of the tail.
    SEQLIB_CHECK_EQ(frames, 7);

    // A restore from before the end plays the tail again.
    ArchivePlayer again(rom, info);
    again.Start(1);
    Run(again, 3);
    const ArchivePlayer::Snapshot playing = again.Save();
    Run(again, 4);
    SEQLIB_CHECK(!again.Busy());
    again.Restore(playing);
    SEQLIB_CHECK(again.Busy());
}

// The player refuses a sound that can't play and calls that don't fit its mode.
void TestRefusals()
{
    Cart cart;
    cart.Sound(1, {{8, SampleChannel(), {}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const auto throws_logic = [](const std::function<void()>& call)
    {
        try
        {
            call();
        }
        catch (const std::logic_error&)
        {
            return true;
        }

        return false;
    };

    ArchivePlayer player(rom, info);
    std::vector<int16_t> samples;
    std::vector<float> floats;
    SEQLIB_CHECK(throws_logic([&] { player.RunFrame(samples); }));
    SEQLIB_CHECK(throws_logic([&] { player.Driver(); }));
    SEQLIB_CHECK(throws_logic([&] { player.Save(); }));
    SEQLIB_CHECK(throws_logic([&] { player.Start(info.song_count); }));

    ArchivePlayerOptions late;
    late.store_cycle = kFrameCycles;
    SEQLIB_CHECK(throws_logic([&] { player.Start(1, late); }));

    SEQLIB_CHECK(throws_logic([&] { player.MuteTrack(kChannelCount, true); }));

    player.Start(1);
    SEQLIB_CHECK(throws_logic([&] { player.RunFrame(floats); }));
    ArchivePlayerOptions enhanced;
    enhanced.enhanced = true;
    player.Start(1, enhanced);
    SEQLIB_CHECK(throws_logic([&] { player.RunFrame(samples); }));
}

// Returns an enhanced player's samples of `frames` frames.
std::vector<float> RunEnhanced(ArchivePlayer& player, int frames)
{
    std::vector<float> out;
    for (int f = 0; f < frames; f++)
    {
        player.RunFrame(out);
    }

    return out;
}

// Returns the frequency of the left side of `out`, from its rising crossings of 0.
double FrequencyOf(const std::vector<float>& out, size_t from)
{
    std::optional<double> first;
    double last = 0;
    int periods = -1;
    for (size_t i = from; i + 2 < out.size() / 2; i++)
    {
        const float a = out[2 * i];
        const float b = out[2 * i + 2];
        if (a < 0 && b >= 0)
        {
            const double place = double(i) + double(-a / (b - a));
            first = first.value_or(place);
            last = place;
            periods++;
        }
    }

    return periods > 0 ? periods * double(kEnhancedRate) / (last - *first) : 0;
}

// The enhanced mode gives a frame's 804 samples in floating point, at a sample's level over its side's: a point p at
// level l gives p / 128 * l / 256 at full scale 1 (the rules in brownie/sequencer.h). A windowed sinc keeps a constant
// level.
void TestEnhancedLevels()
{
    Cart cart;
    Constant(cart, 2, 0x40);
    Constant(cart, 1, 0x40);
    cart.Sound(1, {{8, SampleChannel(), {}}});
    cart.Sound(2, {{8, SampleChannel(37), {}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ArchivePlayerOptions options;
    options.enhanced = true;
    ArchivePlayer player(rom, info);
    player.Start(1, options);
    SEQLIB_CHECK(player.Enhanced());
    SEQLIB_CHECK_EQ(player.Rate(), kEnhancedRate);
    const std::vector<float> looped = RunEnhanced(player, 12);
    SEQLIB_CHECK_EQ(looped.size(), 2 * 12 * kEnhancedFrameSamples);
    const float expected = 0.5f * 255.0f / 256.0f;
    for (size_t i = 2 * 8 * kEnhancedFrameSamples; i < looped.size(); i++)
    {
        SEQLIB_CHECK(std::fabs(looped[i] - expected) < 2e-3f);
    }

    // A sample without a loop ends in silence: semitone 1's 64 points last less than a frame.
    ArchivePlayer one_shot(rom, info);
    one_shot.Start(2, options);
    const std::vector<float> ended = RunEnhanced(one_shot, 12);
    for (size_t i = 2 * 8 * kEnhancedFrameSamples; i < ended.size(); i++)
    {
        SEQLIB_CHECK(std::fabs(ended[i]) < 1e-5f);
    }
    bool sounded = false;
    for (size_t i = 0; i < 2 * 4 * kEnhancedFrameSamples; i++)
    {
        sounded = sounded || std::fabs(ended[i]) > 0.3f;
    }
    SEQLIB_CHECK(sounded);
}

// A voice plays its sample at the rate of the console's points in the frame. A sine of 64 points gives 16,384 / 64 Hz
// at the mixer's rate, and half of that at half the rate. A loop goes on without a click.
void TestEnhancedRates()
{
    Cart cart;
    Sine(cart, 2);
    cart.Sound(1, {{8, SampleChannel(38), {}}});
    cart.Sound(2, {{8, SampleChannel(26), {}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ArchivePlayerOptions options;
    options.enhanced = true;

    ArchivePlayer full(rom, info);
    full.Start(1, options);
    const std::vector<float> at_rate = RunEnhanced(full, 30);
    SEQLIB_CHECK(std::fabs(FrequencyOf(at_rate, 8 * kEnhancedFrameSamples) - 256.0) < 0.5);

    ArchivePlayer half(rom, info);
    half.Start(2, options);
    const std::vector<float> at_half = RunEnhanced(half, 30);
    SEQLIB_CHECK(std::fabs(FrequencyOf(at_half, 8 * kEnhancedFrameSamples) - 128.0) < 0.5);

    // A sine's level over its loop's ends stays within its amplitude of 100 / 128 at the level of 255 / 256.
    float largest = 0;
    for (size_t i = 2 * 8 * kEnhancedFrameSamples; i < at_rate.size(); i++)
    {
        largest = std::max(largest, std::fabs(at_rate[i]));
    }
    SEQLIB_CHECK(std::fabs(largest - 100.0f / 128.0f * 255.0f / 256.0f) < 5e-3f);
}

// A muted track leaves the enhanced mode's mix, and a solo leaves out every other track. Each revision's samples then
// are those of the sound without the other track. A voice moves on while its track is muted, so that a mute's end
// gives the whole sound's samples again.
void TestEnhancedMutes()
{
    for (const Revision revision : {Revision::kSwordOfMana, Revision::kMagicalVacation})
    {
        const bool vacation = revision == Revision::kMagicalVacation;
        Cart cart(true, revision);
        Sine(cart, 2);
        Sine(cart, 4);
        const uint8_t first_key = vacation ? 50 : 38;
        const uint8_t second_key = vacation ? 52 : 40;
        const auto channel = [](uint8_t key)
        {
            return std::vector<uint8_t>{0xE1, 0, 0xF0, kSampleHold, 0xF2, 0xFF, 0x8F, key, 0xFF};
        };
        cart.Sound(1, {{8, channel(first_key), {}}, {9, channel(second_key), {}}});
        cart.Sound(2, {{8, channel(first_key), {}}});
        const Rom rom = cart.ToRom();
        const DriverInfo info = Detect(rom);
        ArchivePlayerOptions options;
        options.enhanced = true;
        constexpr int kFrames = 16;
        const auto played = [&](int sound, const std::function<void(ArchivePlayer&)>& setup = {})
        {
            ArchivePlayer player(rom, info);
            player.Start(sound, options);
            if (setup)
            {
                setup(player);
            }

            return RunEnhanced(player, kFrames);
        };
        const auto near = [](const std::vector<float>& a, const std::vector<float>& b, size_t from)
        {
            bool same = a.size() == b.size();
            for (size_t i = from; same && i < a.size(); i++)
            {
                same = std::fabs(a[i] - b[i]) < 1e-5f;
            }

            return same;
        };

        const std::vector<float> whole = played(1);
        const std::vector<float> first = played(2);
        SEQLIB_CHECK(!near(whole, first, 0));
        SEQLIB_CHECK(near(played(1, [](ArchivePlayer& p) { p.MuteTrack(9, true); }), first, 0));
        SEQLIB_CHECK(near(played(1, [](ArchivePlayer& p) { p.SoloTrack(8, true); }), first, 0));

        ArchivePlayer player(rom, info);
        player.Start(1, options);
        player.MuteTrack(9, true);
        std::vector<float> resumed = RunEnhanced(player, 6);
        player.MuteTrack(9, false);
        const std::vector<float> rest = RunEnhanced(player, kFrames - 6);
        resumed.insert(resumed.end(), rest.begin(), rest.end());
        SEQLIB_CHECK(near(resumed, whole, 2 * 10 * size_t(kEnhancedFrameSamples)));
    }
}

// The mixer's mode 0 leaves voices 2 to 4 out of the enhanced mode's mix too, and E2 brings them in.
void TestEnhancedMixerMode()
{
    Cart cart;
    Constant(cart, 2, 0x40);
    Constant(cart, 4, 0x20);
    std::vector<uint8_t> all_voices = SampleChannel();
    all_voices.insert(all_voices.begin(), {0xE2, 0x01});
    cart.Sound(1, {{8, SampleChannel(), {}}, {10, SampleChannel(40), {}}});
    cart.Sound(2, {{8, all_voices, {}}, {10, SampleChannel(40), {}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ArchivePlayerOptions options;
    options.enhanced = true;
    ArchivePlayer two(rom, info);
    two.Start(1, options);
    SEQLIB_CHECK(std::fabs(RunEnhanced(two, 12).back() - 0.5f * 255.0f / 256.0f) < 2e-3f);
    ArchivePlayer five(rom, info);
    five.Start(2, options);
    SEQLIB_CHECK(std::fabs(RunEnhanced(five, 12).back() - 0.75f * 255.0f / 256.0f) < 2e-3f);
}

// A pan of 0x0F leaves only the right side's level, in the enhanced mode as on the console. The samples hold the left
// side first.
void TestEnhancedSides()
{
    Cart cart;
    Constant(cart, 2, 0x40);
    cart.Sound(1, {{8, {0xE1, 0, 0xEA, 0x0F, 0xF0, kSampleHold, 0xF2, 0xFF, 0x8F, kHoldKey, 0xFF}, {}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ArchivePlayerOptions options;
    options.enhanced = true;
    ArchivePlayer player(rom, info);
    player.Start(1, options);
    const std::vector<float> out = RunEnhanced(player, 12);
    const size_t last = out.size() - 2;
    SEQLIB_CHECK(std::fabs(out[last]) < 1e-3f);
    SEQLIB_CHECK(std::fabs(out[last + 1] - 0.5f * 255.0f / 256.0f) < 2e-3f);
}

// A level moves in a line across each frame in the enhanced mode. The envelope takes 8 from the level at each frame.
// A constant of 0.5 then moves by 8 / 256 * 0.5 a frame, and no two neighbouring samples lie far apart.
void TestEnhancedRamp()
{
    Cart cart;
    Constant(cart, 2, 0x40);
    cart.Sound(1, {{8, {0xE1, 0, 0xEA, 0xFF, 0xF0, kSampleRamp, 0xF2, 0xFF, 0x8F, kHoldKey, 0xFF}, {}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ArchivePlayerOptions options;
    options.enhanced = true;
    ArchivePlayer player(rom, info);
    player.Start(1, options);
    const std::vector<float> out = RunEnhanced(player, 14);
    const size_t first = 2 * 6 * size_t(kEnhancedFrameSamples);
    const size_t last = 2 * 12 * size_t(kEnhancedFrameSamples);
    SEQLIB_CHECK(out[first] - out[last] > 5 * 8.0f / 256.0f * 0.5f);
    for (size_t i = first; i < last; i += 2)
    {
        SEQLIB_CHECK(std::fabs(out[i + 2] - out[i]) < 1e-3f);
    }
}

// In the Sword of Mana revision, a note's first point comes after the stores of frame 1 and a mean wait of 40 of the
// mixer's points. The enhanced mode's samples start there: 804 samples of frame 0, then the wait at 804 samples a
// frame. agb::Apu's output runs 32 samples behind that (ntdlib/agb/apu.h).
void TestEnhancedStart()
{
    Cart cart;
    Constant(cart, 2, 0x40);
    cart.Sound(1, {{8, SampleChannel(), {}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ArchivePlayerOptions options;
    options.enhanced = true;
    ArchivePlayer player(rom, info);
    player.Start(1, options);
    const std::vector<float> out = RunEnhanced(player, 4);
    size_t onset = 0;
    while (onset < out.size() / 2 && std::fabs(out[2 * onset]) < 0.25f)
    {
        onset++;
    }

    const size_t wait = (size_t(kFrameCycles) / 6 + 40 * 1024) * kEnhancedFrameSamples / kFrameCycles + 1;
    SEQLIB_CHECK_EQ(onset, size_t(kEnhancedFrameSamples) + wait + 32);
}

// The Magical Vacation revision's FIFOs play on both sides, each at its timer's rate. A note's byte is the point times
// (volume + 1) / 256, rounded to nearest, half up (the interrupts at 0x0805BFD8 and 0x0805C150 in Magical Vacation).
// Key 48 plays at 16,384 Hz.
void TestVacation()
{
    Cart cart(true, Revision::kMagicalVacation);
    Constant(cart, 0, 0x40);
    Constant(cart, 1, 0x41);
    Constant(cart, 2, 0x10);
    const std::vector<uint8_t> first = {0xE1, 0, 0xF0, kSampleHold, 0xF2, 0x7F, 0x8F, 48, 0xFF};
    const std::vector<uint8_t> second = {0xE1, 2, 0xF0, kSampleHold, 0xF2, 0xFF, 0x8F, 48, 0xFF};
    const std::vector<uint8_t> half = {0xE1, 1, 0xF0, kSampleHold, 0xF2, 0x7F, 0x8F, 48, 0xFF};
    cart.Sound(1, {{8, first, {}}, {9, second, {}}});
    cart.Sound(2, {{8, first, {}}});
    cart.Sound(3, {{8, half, {}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ArchivePlayer player(rom, info);
    player.Start(1);
    SEQLIB_CHECK_EQ(player.Rate(), 32768);
    const std::vector<int16_t> both = Run(player, 8);

    // FIFO A: 64 * 128 >> 8 = 32. FIFO B: 16 * 256 >> 8 = 16.
    SEQLIB_CHECK_EQ(both.back(), (32 + 16) * 256);
    SEQLIB_CHECK_EQ(both[both.size() - 2], (32 + 16) * 256);

    // A muted track's FIFO takes zeros, and the other FIFO plays on.
    const std::vector<int16_t> alone = Played(rom, info, 1, 8, [](ArchivePlayer& p) { p.MuteTrack(9, true); });
    SEQLIB_CHECK(alone == Played(rom, info, 2, 8));
    SEQLIB_CHECK_EQ(alone.back(), 32 * 256);

    // 65 * 128 / 256 is 32.5, and rounds up.
    SEQLIB_CHECK_EQ(Played(rom, info, 3, 8).back(), 33 * 256);

    // The meter of a FIFO's track holds the part of its byte.
    ArchivePlayer metered(rom, info);
    metered.Start(2);
    metered.MeterTracks(true);
    Run(metered, 8);
    SEQLIB_CHECK_EQ(metered.TrackPeaks(8)[0], 32 * 256);
    SEQLIB_CHECK_EQ(metered.TrackPeaks(8)[1], 32 * 256);

    // The enhanced mode: (volume + 1) / 256 of the point over 128, at the timer's rate.
    ArchivePlayerOptions options;
    options.enhanced = true;
    ArchivePlayer enhanced(rom, info);
    enhanced.Start(2, options);
    const std::vector<float> out = RunEnhanced(enhanced, 10);
    SEQLIB_CHECK(std::fabs(out.back() - 0.5f * 0.5f) < 2e-3f);
}

// The Magical Vacation revision's enhanced mode reads a sine at the rate of the FIFO's timer.
void TestVacationRate()
{
    Cart cart(true, Revision::kMagicalVacation);
    Sine(cart, 0);
    cart.Sound(1, {{8, {0xE1, 0, 0xF0, kSampleHold, 0xF2, 0xFF, 0x8F, 48, 0xFF}, {}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ArchivePlayerOptions options;
    options.enhanced = true;
    ArchivePlayer player(rom, info);
    player.Start(1, options);
    const std::vector<float> out = RunEnhanced(player, 30);
    SEQLIB_CHECK(std::fabs(FrequencyOf(out, 8 * kEnhancedFrameSamples) - 256.0) < 0.5);
}

// A set for archive mode: a ROM's bytes under the name of its file.
struct RomSet
{
    std::vector<uint8_t>* File(const std::string& path)
    {
        return files.contains(path) ? &files[path] : nullptr;
    }

    std::map<std::string, std::vector<uint8_t>> files;
    int song = 0;
};

struct RomProgram
{
    using Set = RomSet;

    static std::optional<std::string> Select(const RomSet& set, ntdlib::brownie::ArchiveSelection& selection)
    {
        selection.rom_path = "game.gba";
        selection.song = set.song;
        return std::nullopt;
    }
};

using RomSource = xsflib::player::ArchiveSource<ntdlib::brownie::ArchiveTraits<RomProgram>>;

RomSet SetOf(const Rom& rom, int song)
{
    RomSet set;
    set.files["game.gba"] = std::vector<uint8_t>(rom.Ptr(kRomBase), rom.Ptr(kRomBase) + rom.Size());
    set.song = song;
    return set;
}

// Archive mode plays the sound as the player does, takes snapshots, and gives the player's refusals as its errors. A
// ROM without the driver fails to load.
void TestArchiveSource()
{
    Cart cart;
    Constant(cart, 2, 0x40);
    cart.Sound(1, {{8, SampleChannel(), {}}});
    const Rom rom = cart.ToRom();

    RomSource source;
    RomSet set = SetOf(rom, 1);
    std::vector<int16_t> out;
    const std::optional<std::string> started = source.Start(set, out);
    SEQLIB_CHECK(!started);
    SEQLIB_CHECK(out.empty() && source.Busy());
    for (int f = 0; f < 6; f++)
    {
        SEQLIB_CHECK(source.RunFrame(out));
    }

    const std::shared_ptr<ArchivePlayer::Snapshot> snapshot = source.Save(nullptr);
    const size_t saved = out.size();
    for (int f = 0; f < 6; f++)
    {
        source.RunFrame(out);
    }
    SEQLIB_CHECK_EQ(out.back(), 64 * 256);
    source.Restore(*snapshot);
    std::vector<int16_t> again;
    for (int f = 0; f < 6; f++)
    {
        source.RunFrame(again);
    }
    SEQLIB_CHECK(std::equal(again.begin(), again.end(), out.begin() + std::ptrdiff_t(saved), out.end()));

    RomSet missing = SetOf(rom, 7);
    const std::optional<std::string> refused = source.Start(missing, out);
    SEQLIB_CHECK(refused && refused->find("sound 7") != std::string::npos);
    RomSet empty;
    const std::optional<std::string> absent = source.Start(empty, out);
    SEQLIB_CHECK(absent && absent->find("game.gba") != std::string::npos);

    Cart no_code(false);
    RomSet blank = SetOf(no_code.ToRom(), 1);
    bool threw = false;
    try
    {
        source.Start(blank, out);
    }
    catch (const xsflib::FormatError&)
    {
        threw = true;
    }
    SEQLIB_CHECK(threw);
}

} // namespace

int main()
{
    TestMixer();
    TestFiveVoices();
    TestLead();
    TestMutes();
    TestEffectTakesChannel();
    TestMeters();
    TestSnapshots();
    TestEnd();
    TestRefusals();
    TestEnhancedLevels();
    TestEnhancedRates();
    TestEnhancedMutes();
    TestEnhancedMixerMode();
    TestEnhancedSides();
    TestEnhancedRamp();
    TestEnhancedStart();
    TestVacation();
    TestVacationRate();
    TestArchiveSource();
    return xsflib::testing::Summary("brownie_player");
}
