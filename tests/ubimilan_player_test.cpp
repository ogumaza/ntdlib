// SPDX-License-Identifier: MIT

// Tests of ubimilan::ArchivePlayer on small synthetic cartridges. No game ROM is needed. Each expected value follows a
// rule of the driver's code, of the GBA's sound hardware or of the enhanced mode's rules in ntdlib/ubimilan/output.h.
// The comments give the rule.
//
// The driver's mixer runs at 1,025 cycles a point. The hardware's output at 10 bits is the bias of 0x200 plus each
// FIFO's byte times 4 and the PSG's levels, at a resolution of 256 cycles a sample. A step of the 10 bits gives 64 in a
// 16-bit sample. A byte b therefore gives b * 256.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/testing/agb_files.h"
#include "ntdlib/ubimilan/archive_player.h"
#include "ntdlib/ubimilan/archive_source.h"
#include "ntdlib/ubimilan/driver.h"
#include "ntdlib/ubimilan/output.h"
#include "xsflib/binary.h"
#include "xsflib/player/archive_source.h"
#include "xsflib/testing/check.h"

#include "ubimilan_image.h"

namespace
{

namespace agb = ntdlib::agb;
using agb::Rom;
using ntdlib::testing::ubimilan_image::TestImage;
using ntdlib::ubimilan::ArchivePlayer;
using ntdlib::ubimilan::ArchivePlayerOptions;
using ntdlib::ubimilan::DriverInfo;

constexpr int kKitTrack = 9;
constexpr int kByteSample = 256;

// The bytes of a sample of `points`, all `point`, with a loop over its whole length.
int ConstantLoop(TestImage& image, int point, uint32_t points = 256)
{
    return image.AddSample(std::vector<int8_t>(points, int8_t(point)), true, 0, points);
}

// A loop of a sine of `amplitude`, in `period` points of 8 at a time.
int SineLoop(TestImage& image, int amplitude, uint32_t period)
{
    std::vector<int8_t> points;
    for (uint32_t i = 0; i < period; i++)
    {
        points.push_back(int8_t(std::lround(amplitude * std::sin(2 * std::numbers::pi * i / period))));
    }

    return image.AddSample(points, true, 0, period);
}

struct Cart
{
    explicit Cart(const TestImage& image) : rom(image.ToRom())
    {
        std::string error;
        XSFLIB_CHECK(ntdlib::ubimilan::DetectDriver(rom, {}, info, error));
    }

    Rom rom;
    DriverInfo info;
};

// A cart whose one piece plays a kit note of the level `level`, and square 1's key 60.
TestImage KitAndSquare(int level)
{
    TestImage image;
    image.SetKey(40, ConstantLoop(image, level));
    image.SetInstrument(0, 0, {{0, 0x80, 0, 0x80}});
    image.AddPiece(1, {image.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x00, 0x00, 0x90, 60, 0xF0, 0xFF, 0x00, 0xFC})});
    return image;
}

template <typename Exception, typename Function>
bool Throws(Function function)
{
    try
    {
        function();
    }
    catch (const Exception&)
    {
        return true;
    }

    return false;
}

void TestOutput()
{
    // A frame's bytes wait for the bytes that the frame's first store leaves in the FIFOs: the silence up to the
    // stores, 46,816 / 1,025 = 45 points, and the lead of 48. The first of a frame's bytes goes out at overflow 94 of
    // Timer 0.
    TestImage image;
    image.SetKey(40, ConstantLoop(image, 64));
    image.AddPiece(1, {image.AddSequence({0x00, 0x00, 0x99, 40, 64, 0xFF, 0x00, 0xFC})});
    const Cart cart(image);
    ArchivePlayer player(cart.rom, cart.info);
    player.Start(0);
    XSFLIB_CHECK(player.Rate() == 65536);
    std::vector<int16_t> samples;
    player.RunFrame(samples);

    // A frame lasts 280,896 cycles: 1,097.25 of the hardware's samples at 256 cycles each.
    XSFLIB_CHECK(samples.size() == 2 * 1097 || samples.size() == 2 * 1098);
    size_t first = 0;
    while (first < samples.size() && samples[first] == 0)
    {
        first += 2;
    }

    const size_t pair = first / 2;
    XSFLIB_CHECK(pair * 256 >= 93 * 1025 && pair * 256 <= 95 * 1025);

    // The bytes rise over a point's two bytes, 16 and 48, and stay at 64. FIFO A plays on the right, and FIFO B on the
    // left. Both take the same bytes.
    for (size_t i = pair + 16; i < samples.size() / 2; i++)
    {
        XSFLIB_CHECK(samples[2 * i] == 64 * kByteSample);
        XSFLIB_CHECK(samples[2 * i + 1] == 64 * kByteSample);
    }
}

void TestRegisters()
{
    // The game's start: the FIFOs at full volume and the PSG at 100 %, FIFO A on the right and FIFO B on the left, the
    // sound on, the bias at the resolution of 65,536 Hz, the PSG's master volume full and every channel on both sides.
    const Cart cart(KitAndSquare(64));
    ArchivePlayer player(cart.rom, cart.info);
    player.Start(0);
    const agb::Apu& apu = player.Hardware();
    XSFLIB_CHECK(apu.Register(0x04000082) == 0x0E);
    XSFLIB_CHECK((apu.Register(0x04000083) & 0x77) == 0x21);
    XSFLIB_CHECK(apu.Register(0x04000080) == 0x33);
    XSFLIB_CHECK(apu.Register(0x04000081) == 0xFF);
    XSFLIB_CHECK(apu.Register(0x04000084) & 0x80);
    XSFLIB_CHECK(apu.Register(0x04000089) == 0x42);
    XSFLIB_CHECK(apu.Register(0x04000088) == 0x00);
}

void TestBusy()
{
    // The end comes on frame 5, and the sample's 64 points have ended by then. Busy() waits 2 frames after that for the
    // FIFOs' lead.
    TestImage image;
    image.SetKey(40, image.AddSample(std::vector<int8_t>(64, 64)));
    image.AddPiece(0, {image.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x04, 0x00, 0xFC})});
    const Cart cart(image);
    ArchivePlayer player(cart.rom, cart.info);
    XSFLIB_CHECK(!player.Busy());
    player.Start(0);
    XSFLIB_CHECK(player.Busy());
    int frames = 0;
    while (player.Busy() && frames < 100)
    {
        player.RunFrame();
        frames++;
    }

    XSFLIB_CHECK(frames == 7);
    XSFLIB_CHECK(player.Frames() == 7);

    // A voice that loops keeps the song busy after its track has ended, until a note-off stops it.
    TestImage held;
    held.SetKey(40, ConstantLoop(held, 64));
    held.AddPiece(0, {held.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x00, 0x00, 0xFC})});
    const Cart held_cart(held);
    ArchivePlayer held_player(held_cart.rom, held_cart.info);
    held_player.Start(0);
    for (int frame = 0; frame < 200; frame++)
    {
        held_player.RunFrame();
    }

    XSFLIB_CHECK(held_player.Busy());

    // A piece that loops stays busy.
    const Cart loops(KitAndSquare(64));
    ArchivePlayer loop_player(loops.rom, loops.info);
    loop_player.Start(0);
    for (int frame = 0; frame < 2000; frame++)
    {
        loop_player.RunFrame();
    }

    XSFLIB_CHECK(loop_player.Busy());

    // A reset of the game ends the piece.
    TestImage reset;
    reset.SetKey(40, ConstantLoop(reset, 64));
    reset.AddPiece(1, {reset.AddSequence({0x00, 0x00, 0x99, 40, 65, 0x00, 0x00, 0xFC})});
    const Cart reset_cart(reset);
    ArchivePlayer reset_player(reset_cart.rom, reset_cart.info);
    reset_player.Start(0);
    reset_player.RunFrame();
    reset_player.RunFrame();
    reset_player.RunFrame();
    XSFLIB_CHECK(reset_player.Driver().Song().Reset());
    XSFLIB_CHECK(!reset_player.Busy());
}

void TestRefusals()
{
    const Cart cart(KitAndSquare(64));
    ArchivePlayer player(cart.rom, cart.info);
    std::vector<int16_t> samples;
    std::vector<float> enhanced;
    XSFLIB_CHECK(Throws<std::logic_error>([&] { player.RunFrame(samples); }));
    XSFLIB_CHECK(Throws<std::logic_error>([&] { player.RunFrame(enhanced); }));
    XSFLIB_CHECK(Throws<std::logic_error>([&] { player.RunFrame(); }));
    XSFLIB_CHECK(Throws<std::logic_error>([&] { player.Driver(); }));
    XSFLIB_CHECK(Throws<std::logic_error>([&] { player.Save(); }));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { player.Start(5); }));
    ArchivePlayerOptions options;
    options.store_cycle = agb::kFrameCycles;
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { player.Start(0, options); }));
    XSFLIB_CHECK(Throws<std::out_of_range>([&] { player.MuteTrack(16, true); }));
    XSFLIB_CHECK(Throws<std::out_of_range>([&] { player.SoloTrack(-1, true); }));
    player.Start(0);
    XSFLIB_CHECK(Throws<std::logic_error>([&] { player.RunFrame(enhanced); }));
    options = {};
    options.enhanced = true;
    player.Start(0, options);
    XSFLIB_CHECK(Throws<std::logic_error>([&] { player.RunFrame(samples); }));
}

void TestFramesWithoutSamples()
{
    // A frame without samples ends in the state that a frame with samples leaves.
    const Cart cart(KitAndSquare(64));
    ArchivePlayer run(cart.rom, cart.info);
    ArchivePlayer seek(cart.rom, cart.info);
    run.Start(0);
    seek.Start(0);
    std::vector<int16_t> a;
    for (int frame = 0; frame < 40; frame++)
    {
        a.clear();
        run.RunFrame(a);
        seek.RunFrame();
    }

    a.clear();
    std::vector<int16_t> b;
    run.RunFrame(a);
    seek.RunFrame(b);
    XSFLIB_CHECK(a == b);
    XSFLIB_CHECK(!a.empty());
}

void TestSnapshots()
{
    const Cart cart(KitAndSquare(64));
    ArchivePlayer player(cart.rom, cart.info);
    player.Start(0);
    for (int frame = 0; frame < 30; frame++)
    {
        player.RunFrame();
    }

    const ArchivePlayer::Snapshot snapshot = player.Save();
    std::vector<int16_t> first;
    for (int frame = 0; frame < 30; frame++)
    {
        player.RunFrame(first);
    }

    XSFLIB_CHECK(player.Frames() == 60);
    player.Restore(snapshot);
    XSFLIB_CHECK(player.Frames() == 30);
    std::vector<int16_t> again;
    for (int frame = 0; frame < 30; frame++)
    {
        player.RunFrame(again);
    }

    XSFLIB_CHECK(first == again);
    ArchivePlayer other(cart.rom, cart.info);
    other.Start(0);
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { other.Restore(snapshot); }));

    // A mute stays through a restore: it isn't part of a snapshot.
    player.Restore(snapshot);
    player.MuteTrack(kKitTrack, true);
    player.Restore(snapshot);
    XSFLIB_CHECK(player.IsTrackMuted(kKitTrack));
}

void TestMutes()
{
    // Track 9 holds the kit's voices, and track 0 square 1. Muted, each one's part leaves the samples, and the driver's
    // stores stay.
    const Cart cart(KitAndSquare(64));
    ArchivePlayer whole(cart.rom, cart.info);
    ArchivePlayer no_kit(cart.rom, cart.info);
    ArchivePlayer no_square(cart.rom, cart.info);
    ArchivePlayer only_kit(cart.rom, cart.info);
    ArchivePlayer only_square(cart.rom, cart.info);
    ArchivePlayer other_track(cart.rom, cart.info);
    ArchivePlayer soloed_muted(cart.rom, cart.info);
    for (ArchivePlayer* player : {&whole, &no_kit, &no_square, &only_kit, &only_square, &other_track, &soloed_muted})
    {
        player->Start(0);
    }

    no_kit.MuteTrack(kKitTrack, true);
    no_square.MuteTrack(0, true);
    only_kit.SoloTrack(kKitTrack, true);
    only_square.SoloTrack(0, true);
    other_track.MuteTrack(5, true);
    soloed_muted.SoloTrack(kKitTrack, true);
    soloed_muted.MuteTrack(kKitTrack, true);
    XSFLIB_CHECK(no_kit.IsTrackMuted(kKitTrack) && !no_kit.IsTrackMuted(0));
    XSFLIB_CHECK(only_kit.IsTrackSoloed(kKitTrack) && !only_kit.IsTrackSoloed(0));
    long sounding = 0;
    for (int frame = 0; frame < 30; frame++)
    {
        std::array<std::vector<int16_t>, 7> samples;
        whole.RunFrame(samples[0]);
        no_kit.RunFrame(samples[1]);
        no_square.RunFrame(samples[2]);
        only_kit.RunFrame(samples[3]);
        only_square.RunFrame(samples[4]);
        other_track.RunFrame(samples[5]);
        soloed_muted.RunFrame(samples[6]);
        XSFLIB_CHECK(samples[5] == samples[0]);
        XSFLIB_CHECK(samples[1] == samples[4]);
        XSFLIB_CHECK(samples[2] == samples[3]);
        XSFLIB_CHECK(no_kit.Driver().Fifo().size() == whole.Driver().Fifo().size());
        XSFLIB_CHECK(no_kit.Driver().Song().VoicePlays(3) == whole.Driver().Song().VoicePlays(3));
        XSFLIB_CHECK(no_square.Hardware().Register(0x04000064) == whole.Hardware().Register(0x04000064));
        for (size_t i = 0; i < samples[0].size(); i++)
        {
            XSFLIB_CHECK(samples[0][i] == samples[1][i] + samples[2][i]);
            XSFLIB_CHECK(samples[6][i] == 0);
            sounding += samples[1][i] != 0 && samples[2][i] != 0;
        }
    }

    XSFLIB_CHECK(sounding > 1000);

    // A mute in the song's middle takes the voice's points out from the next frame's bytes on, and the voice goes on.
    ArchivePlayer later(cart.rom, cart.info);
    ArchivePlayer never(cart.rom, cart.info);
    later.Start(0);
    never.Start(0);
    for (int frame = 0; frame < 10; frame++)
    {
        later.RunFrame();
        never.RunFrame();
    }

    later.MuteTrack(kKitTrack, true);
    std::vector<int16_t> a;
    std::vector<int16_t> b;
    later.RunFrame(a);
    never.RunFrame(b);
    XSFLIB_CHECK(a.front() == b.front());
    a.clear();
    b.clear();
    later.RunFrame(a);
    never.RunFrame();
    never.RunFrame(b);
    XSFLIB_CHECK(a.back() != b.back());
    later.MuteTrack(kKitTrack, false);
    later.RunFrame();
    later.RunFrame();
    never.RunFrame();
    a.clear();
    b.clear();
    later.RunFrame(a);
    never.RunFrame();
    never.RunFrame(b);
    XSFLIB_CHECK(a.size() > 0);
}

void TestMutesOfEachChannel()
{
    // Tracks 0, 1 and 2 hold square 1, square 2 and the wave. Track 9 holds the noise beside the kit's voices. A track
    // alone and the rest without it add up to the whole, and other tracks hold nothing.
    TestImage image;
    image.SetInstrument(0, 0, {{0, 0x80, 0, 0x80}});
    image.SetInstrument(1, 0, {{0x80, 0, 0x80}});
    image.SetInstrument(2, 1, {{0x20, 0x80}});
    for (uint32_t i = 0; i < 16; i++)
    {
        image.Put8(ntdlib::testing::ubimilan_image::kWaveTable + 32 + i, 0xFF);
    }

    image.AddPiece(1, {image.AddSequence({
                          0x00, 0x00, 0x90, 60, 0xF0, // square 1
                          0x00, 0x00, 0x91, 67, 0xF0, // square 2
                          0x00, 0x00, 0xC2, 1,        // the wave's program loads its wave
                          0x00, 0x00, 0x92, 72, 0x20, // the wave
                          0x00, 0x00, 0x99, 0,  0x03, // the noise
                          0xFF, 0x00, 0xFC,
                      })});
    const Cart cart(image);
    constexpr int kFrames = 12;
    const auto render = [&cart](const auto& setup)
    {
        ArchivePlayer player(cart.rom, cart.info);
        player.Start(0);
        setup(player);
        std::vector<int16_t> samples;
        for (int frame = 0; frame < kFrames; frame++)
        {
            player.RunFrame(samples);
        }

        return samples;
    };

    const std::vector<int16_t> whole = render([](ArchivePlayer&) {});
    XSFLIB_CHECK(render([](ArchivePlayer& player) { player.MuteTrack(3, true); }) == whole);
    std::vector<int32_t> sum(whole.size());
    for (const int track : {0, 1, 2, kKitTrack})
    {
        const std::vector<int16_t> without = render([track](ArchivePlayer& player) { player.MuteTrack(track, true); });
        const std::vector<int16_t> alone = render([track](ArchivePlayer& player) { player.SoloTrack(track, true); });
        XSFLIB_CHECK(without != whole);
        long sounding = 0;
        for (size_t i = 0; i < whole.size(); i++)
        {
            XSFLIB_CHECK(whole[i] == without[i] + alone[i]);
            sounding += alone[i] != 0;
            sum[i] += alone[i];
        }

        XSFLIB_CHECK(sounding > 1000);
    }

    for (size_t i = 0; i < whole.size(); i++)
    {
        XSFLIB_CHECK(sum[i] == whole[i]);
    }
}

void TestMeters()
{
    // A kit voice of level 32 at volume 64 gives bytes of 32: 32 * 256 in a 16-bit sample. A track's peak lies on that
    // scale, and the output's peak on the same one.
    TestImage image;
    image.SetKey(40, ConstantLoop(image, 32));
    image.AddPiece(1, {image.AddSequence({0x00, 0x00, 0x99, 40, 64, 0xFF, 0x00, 0xFC})});
    const Cart cart(image);
    ArchivePlayer player(cart.rom, cart.info);
    player.Start(0);

    // Without meters, every peak is 0.
    player.RunFrame();
    std::vector<int16_t> samples;
    player.RunFrame(samples);
    XSFLIB_CHECK(player.TrackPeaks(kKitTrack)[0] == 0 && player.OutputPeaks()[0] == 0);

    player.MeterTracks(true);
    player.RunFrame(samples);
    XSFLIB_CHECK(player.TrackPeaks(kKitTrack)[0] == 32 * kByteSample);
    XSFLIB_CHECK(player.TrackPeaks(kKitTrack)[1] == 32 * kByteSample);
    XSFLIB_CHECK(player.TrackPeaks(0)[0] == 0 && player.TrackPeaks(1)[1] == 0);
    XSFLIB_CHECK(player.OutputPeaks()[0] == 32 * kByteSample && player.OutputPeaks()[1] == 32 * kByteSample);

    // A frame without samples gives no peaks.
    player.RunFrame();
    XSFLIB_CHECK(player.TrackPeaks(kKitTrack)[0] == 0 && player.OutputPeaks()[0] == 0);

    // A frame's samples hold the points of the frame before. A muted track's meter falls a frame after the mute.
    player.RunFrame(samples);
    XSFLIB_CHECK(player.TrackPeaks(kKitTrack)[0] == 32 * kByteSample);
    player.MuteTrack(kKitTrack, true);
    player.RunFrame(samples);
    XSFLIB_CHECK(player.TrackPeaks(kKitTrack)[0] == 32 * kByteSample);
    player.RunFrame(samples);
    XSFLIB_CHECK(player.TrackPeaks(kKitTrack)[0] == 0);
    player.MeterTracks(false);
    player.RunFrame(samples);
    XSFLIB_CHECK(player.OutputPeaks()[0] == 0);

    // Two voices together: a track's peak is their sum.
    TestImage two;
    two.SetKey(40, ConstantLoop(two, 32));
    two.SetKey(41, ConstantLoop(two, 16));
    two.AddPiece(1, {two.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x00, 0x00, 0x99, 41, 64, 0xFF, 0x00, 0xFC})});
    const Cart two_cart(two);
    ArchivePlayer two_player(two_cart.rom, two_cart.info);
    two_player.Start(0);
    two_player.MeterTracks(true);
    two_player.RunFrame(samples);
    two_player.RunFrame(samples);
    XSFLIB_CHECK(two_player.TrackPeaks(kKitTrack)[0] == 48 * kByteSample);

    // A PSG channel's part reaches its track, and the output.
    const Cart square_cart(KitAndSquare(0));
    ArchivePlayer square(square_cart.rom, square_cart.info);
    square.Start(0);
    square.MeterTracks(true);
    square.RunFrame(samples);
    square.RunFrame(samples);
    XSFLIB_CHECK(square.TrackPeaks(0)[0] > 0 && square.TrackPeaks(0)[1] > 0);
    XSFLIB_CHECK(square.TrackPeaks(kKitTrack)[0] == 0);
    XSFLIB_CHECK(square.OutputPeaks()[0] == square.TrackPeaks(0)[0]);

    // The noise's part reaches track 9, whose voices it plays beside, and no other track.
    TestImage noise;
    noise.AddPiece(1, {noise.AddSequence({0x00, 0x00, 0x99, 0, 0x03, 0xFF, 0x00, 0xFC})});
    const Cart noise_cart(noise);
    ArchivePlayer noise_player(noise_cart.rom, noise_cart.info);
    noise_player.Start(0);
    noise_player.MeterTracks(true);
    noise_player.RunFrame(samples);
    noise_player.RunFrame(samples);
    XSFLIB_CHECK(noise_player.TrackPeaks(kKitTrack)[0] > 0 && noise_player.TrackPeaks(kKitTrack)[1] > 0);
    XSFLIB_CHECK(noise_player.OutputPeaks()[0] == noise_player.TrackPeaks(kKitTrack)[0]);
    for (const int track : {0, 1, 2, 3})
    {
        XSFLIB_CHECK(noise_player.TrackPeaks(track)[0] == 0 && noise_player.TrackPeaks(track)[1] == 0);
    }

    // A frame's peak is that frame's. An envelope of 0xF1 takes a step down each 1/64 of a second, and falls silent in
    // 15 steps: about 14 frames.
    TestImage fading;
    fading.SetInstrument(0, 0, {{0, 0x80, 0, 0x80}});
    fading.AddPiece(1, {fading.AddSequence({0x00, 0x00, 0x90, 60, 0xF1, 0xFF, 0x00, 0xFC})});
    const Cart fading_cart(fading);
    ArchivePlayer fading_player(fading_cart.rom, fading_cart.info);
    fading_player.Start(0);
    fading_player.MeterTracks(true);
    fading_player.RunFrame(samples);
    fading_player.RunFrame(samples);
    XSFLIB_CHECK(fading_player.TrackPeaks(0)[0] > 0);
    for (int frame = 0; frame < 20; frame++)
    {
        fading_player.RunFrame(samples);
    }

    XSFLIB_CHECK(fading_player.TrackPeaks(0)[0] == 0);

    // The frame sequencer's phase sets when the first of those steps comes.
    ArchivePlayer from_start(fading_cart.rom, fading_cart.info);
    ArchivePlayer later_phase(fading_cart.rom, fading_cart.info);
    ArchivePlayerOptions options;
    options.sequencer_step = 6;
    from_start.Start(0);
    later_phase.Start(0, options);
    std::vector<int16_t> from_start_samples;
    std::vector<int16_t> later_phase_samples;
    for (int frame = 0; frame < 3; frame++)
    {
        from_start.RunFrame(from_start_samples);
        later_phase.RunFrame(later_phase_samples);
    }

    XSFLIB_CHECK(from_start_samples != later_phase_samples);
}

// The level of `samples`.
double RmsOf(const std::vector<float>& samples)
{
    double sum = 0;
    for (const float sample : samples)
    {
        sum += double(sample) * double(sample);
    }

    return std::sqrt(sum / double(samples.size()));
}

ArchivePlayerOptions EnhancedOptions()
{
    ArchivePlayerOptions options;
    options.enhanced = true;
    return options;
}

void TestRestoredState()
{
    TestImage image;
    image.SetKey(40, image.AddSample(std::vector<int8_t>(256, 32)));
    image.AddPiece(0, {image.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x00, 0x00, 0xFC})});
    const Cart cart(image);
    ArchivePlayer player(cart.rom, cart.info);

    // A restore makes a song busy again.
    player.Start(0);
    player.RunFrame();
    const ArchivePlayer::Snapshot busy = player.Save();
    for (int frame = 0; frame < 100 && player.Busy(); frame++)
    {
        player.RunFrame();
    }

    XSFLIB_CHECK(!player.Busy());
    player.Restore(busy);
    XSFLIB_CHECK(player.Busy());

    // A restore puts the mode back.
    player.Start(0, EnhancedOptions());
    const ArchivePlayer::Snapshot enhanced = player.Save();
    player.Start(0);
    XSFLIB_CHECK(!player.Enhanced());
    player.Restore(enhanced);
    XSFLIB_CHECK(player.Enhanced());
    std::vector<float> floats;
    player.RunFrame(floats);
    XSFLIB_CHECK(floats.size() == 2 * size_t(agb::kEnhancedFrameSamples));

    // A restore and a new start leave no meters.
    player.Start(0);
    player.MeterTracks(true);
    const ArchivePlayer::Snapshot unmetered = player.Save();
    std::vector<int16_t> samples;
    player.RunFrame(samples);
    player.RunFrame(samples);
    XSFLIB_CHECK(player.TrackPeaks(kKitTrack)[0] > 0 && player.OutputPeaks()[0] > 0);
    player.Restore(unmetered);
    XSFLIB_CHECK(player.TrackPeaks(kKitTrack)[0] == 0 && player.OutputPeaks()[0] == 0);
    player.RunFrame(samples);
    player.RunFrame(samples);
    XSFLIB_CHECK(player.TrackPeaks(kKitTrack)[0] > 0 && player.OutputPeaks()[0] > 0);
    player.Start(0);
    XSFLIB_CHECK(player.TrackPeaks(kKitTrack)[0] == 0 && player.OutputPeaks()[0] == 0);

    // Each mode's frames refuse the other's samples.
    XSFLIB_CHECK(Throws<std::logic_error>([&] { player.RunFrame(floats); }));
    player.Start(0, EnhancedOptions());
    XSFLIB_CHECK(Throws<std::logic_error>([&] { player.RunFrame(samples); }));
}

void TestEnhancedRate()
{
    // Each frame gives agb::kEnhancedFrameSamples samples on each side, at agb::kEnhancedRate.
    const Cart cart(KitAndSquare(64));
    ArchivePlayer player(cart.rom, cart.info);
    player.Start(0, EnhancedOptions());
    XSFLIB_CHECK(player.Enhanced());
    XSFLIB_CHECK(player.Rate() == agb::kEnhancedRate);
    std::vector<float> samples;
    player.RunFrame(samples);
    XSFLIB_CHECK(samples.size() == 2 * agb::kEnhancedFrameSamples);
    player.RunFrame(samples);
    XSFLIB_CHECK(samples.size() == 4 * agb::kEnhancedFrameSamples);
}

void TestEnhancedGain()
{
    // A voice's level is its volume over 64, on the scale of the console's bytes. A byte b gives b / 128 of full scale,
    // as on the console. A slow sine of 64 has a peak of 0.5, and its level comes out as the console's: the mixer's
    // average and the kernel both pass a slow sine. A period of 256 points gives 128 Hz.
    TestImage image;
    image.SetKey(40, SineLoop(image, 64, 256));
    image.AddPiece(1, {image.AddSequence({0x00, 0x00, 0x99, 40, 64, 0xFF, 0x00, 0xFC})});
    const Cart cart(image);
    ArchivePlayer enhanced(cart.rom, cart.info);
    ArchivePlayer console(cart.rom, cart.info);
    enhanced.Start(0, EnhancedOptions());
    console.Start(0);
    std::vector<float> samples;
    std::vector<int16_t> console_samples;
    for (int frame = 0; frame < 5; frame++)
    {
        enhanced.RunFrame();
        console.RunFrame();
    }

    for (int frame = 0; frame < 60; frame++)
    {
        enhanced.RunFrame(samples);
        console.RunFrame(console_samples);
    }

    float peak = 0;
    for (const float sample : samples)
    {
        peak = std::max(peak, std::fabs(sample));
    }

    XSFLIB_CHECK(std::fabs(peak - 0.5f) < 0.005f);
    double sum = 0;
    for (const int16_t sample : console_samples)
    {
        sum += double(sample) * double(sample);
    }

    const double console_rms = std::sqrt(sum / double(console_samples.size())) / 32768.0;
    XSFLIB_CHECK(std::fabs(20.0 * std::log10(RmsOf(samples) / console_rms)) < 0.1);
}

void TestEnhancedVoices()
{
    // The console's 4 voices hold 4 levels of 32, and clip at 127. The enhanced mode's voices hold all 6 notes, and
    // nothing clips them: 6 * 32 / 128 = 1.5 of full scale. A note-off stops the last note's voice: 5 * 32 / 128.
    TestImage image;
    image.SetKey(40, ConstantLoop(image, 32));
    const std::vector<uint8_t> note = {0x99, 40, 64, 0x00, 0x00};
    std::vector<uint8_t> commands = {0x00, 0x00};
    for (int i = 0; i < 6; i++)
    {
        commands.insert(commands.end(), note.begin(), note.end());
    }

    commands[commands.size() - 2] = 0x10;
    const std::vector<uint8_t> tail = {0x89, 40, 0, 0x40, 0x00, 0xFC};
    commands.insert(commands.end(), tail.begin(), tail.end());
    image.AddPiece(0, {image.AddSequence(commands)});
    const Cart cart(image);
    ArchivePlayer enhanced(cart.rom, cart.info);
    ArchivePlayer console(cart.rom, cart.info);
    enhanced.Start(0, EnhancedOptions());
    console.Start(0);
    std::vector<float> samples;
    std::vector<int16_t> console_samples;
    for (int frame = 0; frame < 6; frame++)
    {
        samples.clear();
        console_samples.clear();
        enhanced.RunFrame(samples);
        console.RunFrame(console_samples);
    }

    XSFLIB_CHECK(std::fabs(samples.back() - 1.5f) < 0.01f);
    XSFLIB_CHECK(console_samples.back() == 127 * kByteSample);
    for (int frame = 0; frame < 20; frame++)
    {
        samples.clear();
        enhanced.RunFrame(samples);
    }

    XSFLIB_CHECK(std::fabs(samples.back() - 1.25f) < 0.01f);
}

void TestEnhancedKeptVoice()
{
    // A piece that keeps a voice for the kit plays each note on it: the second note replaces the first.
    TestImage image;
    image.SetKey(40, ConstantLoop(image, 32));
    image.AddPiece(9, {image.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x00, 0x00, 0x99, 40, 64, 0xFF, 0x00, 0xFC})});
    const Cart cart(image);
    ArchivePlayer player(cart.rom, cart.info);
    player.Start(0, EnhancedOptions());
    std::vector<float> samples;
    for (int frame = 0; frame < 5; frame++)
    {
        samples.clear();
        player.RunFrame(samples);
    }

    XSFLIB_CHECK(std::fabs(samples.back() - 0.25f) < 0.01f);
}

void TestEnhancedSampleEnd()
{
    // A sample without a loop plays the console's points and stops: the run that stops a voice leaves out its points.
    // Its voice then goes quiet, and the song stops being busy.
    TestImage image;
    image.SetKey(40, image.AddSample(std::vector<int8_t>(2048, 32)));
    image.AddPiece(0, {image.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x00, 0x00, 0xFC})});
    const Cart cart(image);
    ArchivePlayer player(cart.rom, cart.info);
    player.Start(0, EnhancedOptions());
    std::vector<float> samples;
    player.RunFrame(samples);
    int frames = 0;
    while (player.Busy() && frames < 100)
    {
        samples.clear();
        player.RunFrame(samples);
        frames++;
    }

    XSFLIB_CHECK(frames < 100);
    XSFLIB_CHECK(std::fabs(samples.back()) < 1e-4f);

    // 2,048 points take 256 of the mixer's runs, at 17.13 runs a frame: 14.9 frames. The song stays busy at least that
    // long.
    XSFLIB_CHECK(frames >= 14);
}

void TestEnhancedKernelTail()
{
    // A sample without a loop plays on until the kernel has passed its last point. A sample of 120 points is heard up
    // to point 112. A frame moves a voice by 137 points. The next frame's first points therefore still reach back past
    // the last point heard, and hold the kernel's tail. Those samples come after the FIFOs' lead and the band-limited
    // steps' delay. The frame after that holds nothing of the voice past its first 307 samples.
    TestImage image;
    image.SetKey(40, image.AddSample(std::vector<int8_t>(120, 32)));
    image.AddPiece(1, {image.AddSequence({0x00, 0x00, 0x99, 40, 64, 0xFF, 0x00, 0xFC})});
    const Cart cart(image);
    ArchivePlayer player(cart.rom, cart.info);
    player.Start(0, EnhancedOptions());
    std::vector<float> first;
    std::vector<float> second;
    std::vector<float> third;
    player.RunFrame(first);
    player.RunFrame(second);
    player.RunFrame(third);
    float tail = 0.0f;
    for (size_t i = 2 * 310; i < second.size(); i++)
    {
        tail = std::max(tail, std::fabs(second[i]));
    }

    XSFLIB_CHECK(tail > 0.0f && tail < 0.01f);
    for (size_t i = 2 * 310; i < third.size(); i++)
    {
        XSFLIB_CHECK(third[i] == 0.0f);
    }
}

void TestEnhancedLastPoints()
{
    // The run that stops a voice leaves out its points, in this mode too: the last 8 points of 64 never sound.
    std::vector<int8_t> points(64, 0);
    std::fill(points.begin() + 56, points.end(), int8_t(64));
    TestImage image;
    image.SetKey(40, image.AddSample(points));
    image.AddPiece(0, {image.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x20, 0x00, 0xFC})});
    const Cart cart(image);
    ArchivePlayer player(cart.rom, cart.info);
    player.Start(0, EnhancedOptions());
    std::vector<float> samples;
    for (int frame = 0; frame < 5; frame++)
    {
        player.RunFrame(samples);
    }

    for (const float sample : samples)
    {
        XSFLIB_CHECK(sample == 0.0f);
    }
}

// The indexes of the samples of a side, in order, at which the signal goes from above 0 to 0 or below.
std::vector<size_t> FallsOf(const std::vector<float>& samples)
{
    std::vector<size_t> falls;
    for (size_t i = 2; i < samples.size(); i += 2)
    {
        if (samples[i - 2] > 0.0f && samples[i] <= 0.0f)
        {
            falls.push_back(i / 2);
        }
    }

    return falls;
}

void TestEnhancedPlaces()
{
    // A voice moves through its sample at half the rate of the mixer's output: 280,896 / 2,050 points a frame, and
    // 5.868 samples a point. Its loop lasts whole runs of 8 points, as on the console. The first pass runs up to the
    // run that reaches the loop's end. Each later pass starts at the word that holds the loop's start.
    //
    // Points 0 to 399 stand at 32, and the rest at -32. With a loop from 2 to 806, the first pass lasts 808 points.
    // Each later pass starts at point 0, and lasts 808 points too. The falls lie at points 400, 1,208 and 2,016.
    std::vector<int8_t> points(1024, -32);
    std::fill(points.begin(), points.begin() + 400, int8_t(32));
    TestImage image;
    image.SetKey(40, image.AddSample(points, true, 2, 806));
    image.AddPiece(1, {image.AddSequence({0x00, 0x00, 0x99, 40, 64, 0xFF, 0x00, 0xFC})});
    const Cart cart(image);
    ArchivePlayer player(cart.rom, cart.info);
    player.Start(0, EnhancedOptions());
    std::vector<float> samples;
    for (int frame = 0; frame < 20; frame++)
    {
        player.RunFrame(samples);
    }

    const std::vector<size_t> falls = FallsOf(samples);
    SEQLIB_REQUIRE(falls.size() >= 3);
    const double samples_a_point = double(ntdlib::agb::kEnhancedFrameSamples) * 2050.0 / 280896.0;
    XSFLIB_CHECK(std::fabs(double(falls[1] - falls[0]) / samples_a_point - 808.0) < 0.5);
    XSFLIB_CHECK(std::fabs(double(falls[2] - falls[1]) / samples_a_point - 808.0) < 0.5);

    // The note's first point sounds after the frame's stores and the FIFOs' lead: 96,016 of a frame's 280,896 cycles,
    // at sample 274.8. The hardware model's FIFOs then wait for the band-limited steps' delay of 32 samples.
    TestImage first;
    first.SetKey(40, ConstantLoop(first, 32));
    first.AddPiece(1, {first.AddSequence({0x00, 0x00, 0x99, 40, 64, 0xFF, 0x00, 0xFC})});
    const Cart first_cart(first);
    ArchivePlayer first_player(first_cart.rom, first_cart.info);
    first_player.Start(0, EnhancedOptions());
    std::vector<float> first_samples;
    first_player.RunFrame(first_samples);
    size_t half = 0;
    while (2 * half < first_samples.size() && first_samples[2 * half] < 0.125f)
    {
        half++;
    }

    XSFLIB_CHECK(half + 3 >= 307 && half <= 307 + 3);
}

void TestEnhancedVoiceTakenAgain()
{
    // A piece that keeps a voice for the kit plays each note from the sample's start. Key 41's sample holds 32 for its
    // first 1,000 points and then -32. 10 frames into key 40, 1,370 points have gone by, and the new note still starts
    // at 32.
    std::vector<int8_t> points(4000, -32);
    std::fill(points.begin(), points.begin() + 1000, int8_t(32));
    TestImage image;
    image.SetKey(40, ConstantLoop(image, 16));
    image.SetKey(41, image.AddSample(points));
    image.AddPiece(8, {image.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x09, 0x00, 0x99, 41, 64, 0xFF, 0x00, 0xFC})});
    const Cart cart(image);
    ArchivePlayer player(cart.rom, cart.info);
    player.Start(0, EnhancedOptions());
    std::vector<float> samples;
    for (int frame = 0; frame < 13; frame++)
    {
        samples.clear();
        player.RunFrame(samples);
    }

    XSFLIB_CHECK(std::fabs(samples.back() - 0.25f) < 0.01f);
}

void TestEnhancedFreeVoices()
{
    // A voice whose sample has ended is free again in the next frame. Many short notes one after another therefore take
    // the same voice, and leave the long note on its voice.
    TestImage image;
    image.SetKey(40, ConstantLoop(image, 32));
    image.SetKey(41, image.AddSample(std::vector<int8_t>(64, 0)));
    std::vector<uint8_t> commands = {0x00, 0x00, 0x99, 40, 64, 0x01, 0x00};
    for (int note = 0; note < 40; note++)
    {
        commands.insert(commands.end(), {0x99, 41, 64, 0x01, 0x00});
    }

    commands.insert(commands.end(), {0xFC});
    image.AddPiece(0, {image.AddSequence(commands)});
    const Cart cart(image);
    ArchivePlayer player(cart.rom, cart.info);
    player.Start(0, EnhancedOptions());
    std::vector<float> samples;
    for (int frame = 0; frame < 84; frame++)
    {
        samples.clear();
        player.RunFrame(samples);
    }

    XSFLIB_CHECK(std::fabs(samples.back() - 0.25f) < 0.01f);
}

void TestEnhancedBusyPastTheConsole()
{
    // The console's fifth note replaces the first note's sample with a short one, and its voices soon stop. The
    // enhanced mode's first note plays on, and the song stays busy.
    TestImage image;
    image.SetKey(40, image.AddSample(std::vector<int8_t>(40000, 32)));
    image.SetKey(41, image.AddSample(std::vector<int8_t>(64, 8)));
    std::vector<uint8_t> commands = {0x00, 0x00, 0x99, 40, 64, 0x00, 0x00};
    for (int note = 0; note < 4; note++)
    {
        commands.insert(commands.end(), {0x99, 41, 64, 0x00, 0x00});
    }

    commands.insert(commands.end(), {0xFC});
    image.AddPiece(0, {image.AddSequence(commands)});
    const Cart cart(image);
    ArchivePlayer player(cart.rom, cart.info);
    player.Start(0, EnhancedOptions());
    std::vector<float> samples;
    for (int frame = 0; frame < 20; frame++)
    {
        samples.clear();
        player.RunFrame(samples);
    }

    XSFLIB_CHECK(player.Busy());
    XSFLIB_CHECK(std::fabs(samples.back() - 0.25f) < 0.01f);
}

void TestEnhancedMeters()
{
    // A voice's negative points reach the meter too.
    TestImage image;
    image.SetKey(40, ConstantLoop(image, -32));
    image.AddPiece(1, {image.AddSequence({0x00, 0x00, 0x99, 40, 64, 0xFF, 0x00, 0xFC})});
    const Cart cart(image);
    ArchivePlayer player(cart.rom, cart.info);
    player.Start(0, EnhancedOptions());
    player.MeterTracks(true);
    std::vector<float> samples;
    for (int frame = 0; frame < 4; frame++)
    {
        player.RunFrame(samples);
    }

    XSFLIB_CHECK(std::abs(player.TrackPeaks(kKitTrack)[0] - 32 * kByteSample) < kByteSample);

    // The meter follows the mode's mix, not the console's sum. Points that alternate lie past the kernel's cutoff, and
    // the mode's mix keeps little of them once the note's start has gone. The console's sum holds them whole.
    std::vector<int8_t> alternating(256);
    for (size_t i = 0; i < alternating.size(); i++)
    {
        alternating[i] = i % 2 == 0 ? 100 : -100;
    }

    TestImage bright;
    bright.SetKey(40, bright.AddSample(alternating, true, 0, 256));
    bright.AddPiece(1, {bright.AddSequence({0x00, 0x00, 0x99, 40, 64, 0xFF, 0x00, 0xFC})});
    const Cart bright_cart(bright);
    ArchivePlayer bright_player(bright_cart.rom, bright_cart.info);
    bright_player.Start(0, EnhancedOptions());
    bright_player.MeterTracks(true);
    for (int frame = 0; frame < 4; frame++)
    {
        bright_player.RunFrame(samples);
    }

    XSFLIB_CHECK(bright_player.TrackPeaks(kKitTrack)[0] < 10 * kByteSample);
}

void TestEnhancedReset()
{
    // A reset of the game ends every voice of the enhanced mode, and the output falls silent.
    TestImage image;
    image.SetKey(40, ConstantLoop(image, 32));
    image.AddPiece(0, {image.AddSequence({0x00, 0x00, 0x99, 40, 32, 0x10, 0x00, 0x99, 40, 65, 0x40, 0x00, 0xFC})});
    const Cart cart(image);
    ArchivePlayer player(cart.rom, cart.info);
    player.Start(0, EnhancedOptions());
    std::vector<float> samples;
    for (int frame = 0; frame < 10; frame++)
    {
        samples.clear();
        player.RunFrame(samples);
    }

    XSFLIB_CHECK(std::fabs(samples.back() - 0.125f) < 0.01f);
    int frames = 0;
    while (player.Busy() && frames < 100)
    {
        samples.clear();
        player.RunFrame(samples);
        frames++;
    }

    XSFLIB_CHECK(player.Driver().Song().Reset());
    XSFLIB_CHECK(frames < 100);
    samples.clear();
    player.RunFrame(samples);
    XSFLIB_CHECK(std::fabs(samples.back()) < 1e-4f);
}

void TestEnhancedFramesWithoutMixing()
{
    // A frame without samples moves the voices on. The bytes of the next frame's samples wait in the FIFOs, and the
    // frame after that plays what a run from the start plays.
    TestImage image;
    image.SetKey(40, SineLoop(image, 64, 40));
    image.AddPiece(1, {image.AddSequence({0x00, 0x00, 0x99, 40, 64, 0xFF, 0x00, 0xFC})});
    const Cart cart(image);
    ArchivePlayer run(cart.rom, cart.info);
    ArchivePlayer seek(cart.rom, cart.info);
    run.Start(0, EnhancedOptions());
    seek.Start(0, EnhancedOptions());
    std::vector<float> a;
    std::vector<float> b;
    for (int frame = 0; frame < 20; frame++)
    {
        a.clear();
        run.RunFrame(a);
        seek.RunFrame();
    }

    // The bytes of a frame without samples are silent: the next frame's first sample plays them.
    a.clear();
    run.RunFrame(a);
    seek.RunFrame(b);
    XSFLIB_CHECK(a.front() != 0.0f);
    XSFLIB_CHECK(b.front() == 0.0f);
    a.clear();
    b.clear();
    run.RunFrame(a);
    seek.RunFrame(b);
    XSFLIB_CHECK(a == b);
    XSFLIB_CHECK(RmsOf(a) > 0.1);
}

void TestEnhancedSnapshotsAndMutes()
{
    TestImage image;
    image.SetKey(40, SineLoop(image, 64, 40));
    image.SetInstrument(0, 0, {{0, 0x80, 0, 0x80}});
    image.AddPiece(1, {image.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x00, 0x00, 0x90, 60, 0xF0, 0xFF, 0x00, 0xFC})});
    const Cart cart(image);
    ArchivePlayer whole(cart.rom, cart.info);
    ArchivePlayer muted(cart.rom, cart.info);
    ArchivePlayer alone(cart.rom, cart.info);
    whole.Start(0, EnhancedOptions());
    muted.Start(0, EnhancedOptions());
    alone.Start(0, EnhancedOptions());
    muted.MuteTrack(kKitTrack, true);
    alone.SoloTrack(kKitTrack, true);
    double worst = 0;
    std::vector<float> a;
    std::vector<float> b;
    std::vector<float> c;
    for (int frame = 0; frame < 20; frame++)
    {
        a.clear();
        b.clear();
        c.clear();
        whole.RunFrame(a);
        muted.RunFrame(b);
        alone.RunFrame(c);
        for (size_t i = 0; i < a.size(); i++)
        {
            worst = std::max(worst, double(std::fabs(a[i] - b[i] - c[i])));
        }
    }

    XSFLIB_CHECK(worst < 1e-5);
    XSFLIB_CHECK(RmsOf(b) > 0.01 && RmsOf(c) > 0.1);

    const ArchivePlayer::Snapshot snapshot = whole.Save();
    std::vector<float> first;
    for (int frame = 0; frame < 10; frame++)
    {
        whole.RunFrame(first);
    }

    whole.Restore(snapshot);
    std::vector<float> again;
    for (int frame = 0; frame < 10; frame++)
    {
        whole.RunFrame(again);
    }

    XSFLIB_CHECK(first == again);

    // The meters follow the voices' sum on the scale of 16-bit samples.
    ArchivePlayer metered(cart.rom, cart.info);
    metered.Start(0, EnhancedOptions());
    metered.MeterTracks(true);
    for (int frame = 0; frame < 3; frame++)
    {
        a.clear();
        metered.RunFrame(a);
    }

    XSFLIB_CHECK(std::abs(metered.TrackPeaks(kKitTrack)[0] - 64 * kByteSample) < 400);
    XSFLIB_CHECK(metered.TrackPeaks(0)[0] > 0);
}

struct RomSet
{
    std::vector<uint8_t>* File(const std::string& path)
    {
        const auto file = files.find(path);
        return file != files.end() ? &file->second : nullptr;
    }

    std::map<std::string, std::vector<uint8_t>> files;
    int song = 0;
};

struct RomProgram
{
    using Set = RomSet;

    static std::optional<std::string> Select(RomSet& set, ntdlib::ubimilan::ArchiveSelection& selection)
    {
        selection.rom_path = "game.gba";
        selection.song = set.song;
        return std::nullopt;
    }
};

using RomSource = xsflib::player::ArchiveSource<ntdlib::ubimilan::ArchiveTraits<RomProgram>>;

RomSet SetOf(const Rom& rom, int song)
{
    RomSet set;
    set.files["game.gba"] = std::vector<uint8_t>(rom.Ptr(agb::kRomBase), rom.Ptr(agb::kRomBase) + rom.Size());
    set.song = song;
    return set;
}

void TestArchiveSource()
{
    // Archive mode plays the song as the player does, takes snapshots, and gives the player's refusals as its errors. A
    // ROM without the driver fails to load.
    const Cart cart(KitAndSquare(64));
    RomSource source;
    RomSet set = SetOf(cart.rom, 0);
    std::vector<int16_t> out;
    XSFLIB_CHECK(!source.Start(set, out));
    XSFLIB_CHECK(out.empty() && source.Busy());
    for (int frame = 0; frame < 6; frame++)
    {
        XSFLIB_CHECK(source.RunFrame(out));
    }

    ArchivePlayer player(cart.rom, cart.info);
    player.Start(0);
    std::vector<int16_t> direct;
    for (int frame = 0; frame < 6; frame++)
    {
        player.RunFrame(direct);
    }

    XSFLIB_CHECK(out == direct);

    const std::shared_ptr<ArchivePlayer::Snapshot> snapshot = source.Save(nullptr);
    const size_t saved = out.size();
    for (int frame = 0; frame < 6; frame++)
    {
        source.RunFrame(out);
    }

    source.Restore(*snapshot);
    std::vector<int16_t> again;
    for (int frame = 0; frame < 6; frame++)
    {
        source.RunFrame(again);
    }

    XSFLIB_CHECK(std::equal(again.begin(), again.end(), out.begin() + std::ptrdiff_t(saved), out.end()));

    RomSet missing = SetOf(cart.rom, 5);
    const std::optional<std::string> refused = source.Start(missing, out);
    XSFLIB_CHECK(refused && refused->find("song 5") != std::string::npos);
    RomSet empty;
    const std::optional<std::string> absent = source.Start(empty, out);
    XSFLIB_CHECK(absent && absent->find("game.gba") != std::string::npos);
    RomSet blank;
    blank.files["game.gba"] = std::vector<uint8_t>(0x1000, 0);
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { source.Start(blank, out); }));
}

} // namespace

int main()
{
    TestOutput();
    TestRegisters();
    TestBusy();
    TestRefusals();
    TestFramesWithoutSamples();
    TestSnapshots();
    TestMutes();
    TestMutesOfEachChannel();
    TestMeters();
    TestRestoredState();
    TestEnhancedRate();
    TestEnhancedGain();
    TestEnhancedVoices();
    TestEnhancedKeptVoice();
    TestEnhancedSampleEnd();
    TestEnhancedKernelTail();
    TestEnhancedLastPoints();
    TestEnhancedPlaces();
    TestEnhancedVoiceTakenAgain();
    TestEnhancedFreeVoices();
    TestEnhancedBusyPastTheConsole();
    TestEnhancedMeters();
    TestEnhancedReset();
    TestEnhancedFramesWithoutMixing();
    TestEnhancedSnapshotsAndMutes();
    TestArchiveSource();
    return xsflib::testing::Summary("Ubisoft Milan's player");
}
