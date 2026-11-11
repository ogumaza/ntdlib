// SPDX-License-Identifier: MIT

// Tests of ubimilan::ArchivePlayer on small synthetic cartridges. No game ROM is needed. Each expected value follows a
// rule of the driver's code, of the GBA's sound hardware or of the enhanced mode's rules in ntdlib/ubimilan/output.h.
// The comments give the rule.
//
// The driver's mixer runs at 1,025 cycles a point. The hardware's output at 10 bits is the bias of 0x200 plus each
// FIFO's byte times 4 and the PSG's levels, at a resolution of 256 cycles a sample. A step of the 10 bits gives 64 in a
// 16-bit sample. A byte b therefore gives b * 256.
//
// This file tests the output, the registers, mutes and meters. ubimilan_enhanced_test.cpp tests the enhanced mode and
// the archive source.

#include "ubimilan_player_test_util.h"

namespace ubimilan_player_tests
{
namespace
{

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

} // namespace

int RunTests()
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

    return xsflib::testing::Summary("Ubisoft Milan's player");
}

} // namespace ubimilan_player_tests

int main()
{
    return ubimilan_player_tests::RunTests();
}
