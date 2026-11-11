// SPDX-License-Identifier: MIT

// Tests of konami::ArchivePlayer on small synthetic cartridges. No game ROM is needed. Each expected value follows a
// rule of the driver's code, of the GBA's sound hardware or of the enhanced mode's rules in ntdlib/konami/output.h. The
// comments give the rule.
//
// The carts' mixers run at 1,254 cycles a point: exactly 14 blocks of 16 points a frame. The hardware's output at 10
// bits is the bias of 0x200 plus each FIFO's byte times 4 and the PSG's levels times 16, rounded down to the
// resolution's steps. A step of the 10 bits gives 64 in a 16-bit sample.
//
// This file tests the output, the registers, the FIFOs, mutes and meters. konami_enhanced_test.cpp tests the enhanced
// mode, the echo and the archive source.

#include "konami_player_test_util.h"

namespace konami_player_tests
{
namespace
{

void TestBlockOutput()
{
    // The two voices' bytes add up: 32 + 16. FIFO A goes to the right and FIFO B to the left.
    const Rom rom = TwoVoices(32, 16);
    ArchivePlayer player(rom, Blocks());
    player.Start(0);
    XSFLIB_CHECK(player.Busy() && player.Frames() == 0);
    XSFLIB_CHECK(player.Rate() == 65536);
    std::vector<int16_t> out;
    for (int f = 0; f < 4; f++)
    {
        player.RunFrame(out);
        XSFLIB_CHECK(player.FifoBytes(0).size() == 224 && player.FifoBytes(1).size() == 224);
        XSFLIB_CHECK(AllAre(player.FifoBytes(0), 48) && AllAre(player.FifoBytes(1), 48));
    }

    XSFLIB_CHECK(player.Frames() == 4);

    // The output takes a sample each 256 cycles. The first new byte reaches the output at its overflow.
    XSFLIB_CHECK(out.size() == 2 * size_t(4 * kFrameCycles / 256));
    const size_t first = (kBlockSilence + 1) * kPoint / 256;
    for (size_t i = 0; i < 2 * (first - 1); i++)
    {
        XSFLIB_CHECK(out[i] == 0);
    }
    for (size_t i = 2 * (first + 1); i < out.size(); i++)
    {
        XSFLIB_CHECK(out[i] == 48 * 4 * 64);
    }
}

void TestRegisters()
{
    // The bytes that the game's init leaves in the sound registers, over the BIOS's bias of 0x200. Only the mixer's
    // resolution differs in SOUNDBIAS. Each register's index here is its address less 0x04000060.
    const auto expect = [](std::initializer_list<std::pair<uint32_t, int>> differences)
    {
        std::array<uint8_t, ntdlib::agb::kSoundRegisterBytes> bytes = {};
        for (const auto& [address, value] : {std::pair<uint32_t, int>{0x65, 0x80},
                                             {0x6D, 0x80},
                                             {0x75, 0x80},
                                             {0x80, 0x77},
                                             {0x82, 0x0E},
                                             {0x84, 0x80}})
        {
            bytes[address - 0x60] = uint8_t(value);
        }
        for (const auto& [address, value] : differences)
        {
            bytes[address - 0x60] = uint8_t(value);
        }

        return bytes;
    };

    const Rom block = TwoVoices(1, 1);
    Output block_output(block, Blocks());
    block_output.Play(0);
    XSFLIB_CHECK(block_output.SoundRegisters() == expect({{0x70, 0xC0}, {0x81, 0xBB}, {0x83, 0x21}, {0x89, 0x42}}));

    Image ring_image;
    ring_image.Mixer(0);
    ring_image.OneTrack(4, {0x00, 0xAF, 0x00, 0x7F, 0xFD}, Revision::kEternalDuelist);
    const Rom ring = ring_image.ToRom();
    Output ring_output(ring, InfoOf(Revision::kEternalDuelist));
    ring_output.Play(0);
    XSFLIB_CHECK(ring_output.SoundRegisters() ==
                 expect({{0x70, 0xC0}, {0x73, 0x20}, {0x81, 0xFF}, {0x83, 0x33}, {0x89, 0x42}}));

    // The revision without a mixer also loads wave 0 of its table.
    Image fifo_image;
    fifo_image.OneTrack(4, {0x00, 0xBC, 0x01, 0x3C, 0xFD}, Revision::kDungeonDiceMonsters);
    for (uint32_t i = 0; i < 16; i++)
    {
        fifo_image.Put8(kWaves + i, uint8_t(0x10 + i));
    }
    const Rom fifo = fifo_image.ToRom();
    Output fifo_output(fifo, FifoInfo());
    fifo_output.Play(0);
    std::array<uint8_t, ntdlib::agb::kSoundRegisterBytes> expected =
        expect({{0x70, 0x80}, {0x7D, 0x80}, {0x81, 0xFF}, {0x83, 0x33}, {0x89, 0x02}});
    for (uint32_t i = 0; i < 16; i++)
    {
        expected[0x30 + i] = uint8_t(0x10 + i);
    }
    XSFLIB_CHECK(fifo_output.SoundRegisters() == expected);
}

void TestRingOutput()
{
    // Track 4: note (vol 15, sample 0). A volume of 15 gives a level of 16, and the product over 16 gives each byte as
    // it stands. A ring's FIFO A goes to both sides, and voice 0 plays on it.
    Image m;
    m.Mixer(0);
    m.OneTrack(4, {0x00, 0xAF, 0x00, 0x7F, 0xFD}, Revision::kEternalDuelist);
    m.Sample(0, 0x1000, 16, 0, Constant(32));
    const Rom rom = m.ToRom();

    ArchivePlayer player(rom, InfoOf(Revision::kEternalDuelist));
    player.Start(0);
    player.MeterTracks(true);
    XSFLIB_CHECK(player.Rate() == 65536);

    // The ring's lead of 480 points takes 2 frames and a bit more. A voice's bytes reach its meter in the frame that
    // plays them.
    std::vector<int16_t> out;
    for (int f = 0; f < 3; f++)
    {
        player.RunFrame(out);
        XSFLIB_CHECK(AllAre(player.FifoBytes(0), 32));
        XSFLIB_CHECK(AllAre(player.FifoBytes(1), 0));
        const int expected = f < 2 ? 0 : 32 * 4 * 64;
        XSFLIB_CHECK(player.TrackPeaks(4)[0] == expected && player.TrackPeaks(4)[1] == expected);
    }

    const size_t first = (kRingSilence + 1) * kPoint / 256;
    XSFLIB_CHECK(out[2 * (first - 1)] == 0 && out[2 * (first - 1) + 1] == 0);
    XSFLIB_CHECK(out[2 * (first + 1)] == 32 * 4 * 64 && out[2 * (first + 1) + 1] == 32 * 4 * 64);
}

void TestFifoRestart()
{
    // A note's start resets FIFO A and stores 4 bytes of 0. The first DMA then takes the 16 zeros of the buffer as the
    // start left it. The sample's first byte is the 21st after the stores. Timer 0 overflows each 0x400 cycles from
    // the stores on. The voice's bytes are the product of each sample and its volume plus 1, over 32: 32 * 13 / 32.
    // FIFO A goes to both sides here. The output's resolution gives steps of 2, and a sample at each multiple of 512
    // cycles.
    Image m;
    m.OneTrack(4, {0x00, 0xBC, 0x01, 0x3C, 0xFD}, Revision::kDungeonDiceMonsters);
    m.PutBytes(kMap, {0x00, 0x05});
    m.Put16(kPeriods, 0x400);
    FifoSample(m, 5, 8, 0x4000, std::vector<int8_t>(8 * 16, 32));
    const Rom rom = m.ToRom();

    ArchivePlayer player(rom, FifoInfo());
    player.Start(0);
    XSFLIB_CHECK(player.Rate() == 32768);
    const std::vector<int16_t> out = Frames(player, 1);
    XSFLIB_CHECK(player.Driver().FifoRestarted(0) && !player.Driver().FifoRestarted(1));
    XSFLIB_CHECK(out.size() == 2 * size_t((kFrameCycles + 511) / 512));

    const size_t first = (kStore + 21 * 0x400) / 512;
    XSFLIB_CHECK(first + 2 < out.size() / 2);
    for (size_t i = 0; i < 2 * (first - 1); i++)
    {
        XSFLIB_CHECK(out[i] == 0);
    }
    XSFLIB_CHECK(out[2 * (first + 1)] == 13 * 4 * 64 && out[2 * (first + 1) + 1] == 13 * 4 * 64);
}

void TestFifoTimers()
{
    // The timer takes its period at the stores, and overflows a period later. The FIFO asks for 16 bytes once it holds
    // no more than 16. It holds the 4 bytes of 0 at the stores: its first overflow takes the buffer's 16 zeros, and the
    // voice's first block fills the buffer. The fourth overflow takes that block, and each 16th after it the next. The
    // overflow that takes the sample's last block finds the sample's end, and stops the timer. That's overflow 52 for 4
    // blocks. 4 + 16 + 16 + 16 bytes have then played. The last two blocks stay in the FIFO, and the last byte that
    // played stays at the output.
    Image m;
    m.OneTrack(4, {0x00, 0xBC, 0x01, 0x3C, 0xFD}, Revision::kDungeonDiceMonsters);
    m.PutBytes(kMap, {0x00, 0x05});
    m.Put16(kPeriods, 0x100);
    std::vector<int8_t> bytes = Block({32});
    for (int value : {-32, 64, 64})
    {
        const std::vector<int8_t> block = Block({value});
        bytes.insert(bytes.end(), block.begin(), block.end());
    }
    FifoSample(m, 5, 4, 0x4000, bytes);
    const Rom rom = m.ToRom();

    ArchivePlayer player(rom, FifoInfo());
    player.Start(0);
    const std::vector<int16_t> out = Frames(player, 1);
    const std::vector<ntdlib::konami::FifoTimerChange>& changes = player.Driver().TimerChanges();
    XSFLIB_CHECK(changes.size() == 2);
    XSFLIB_CHECK(changes[0].cycle == 0 && changes[0].timer == 0 && changes[0].period == 0x100);
    XSFLIB_CHECK(changes[0].first == 0x100);
    XSFLIB_CHECK(changes[1].cycle == 52 * 0x100 && changes[1].timer == 0 && changes[1].period == 0);

    // 32 * 13 / 32 from the 21st overflow on, and -32 * 13 / 32 from the 37th. A sample comes each 512 cycles.
    const auto pair_after = [](uint32_t overflow)
    {
        return (kStore + overflow * 0x100) / 512 + 1;
    };
    XSFLIB_CHECK(out[2 * (pair_after(20) - 2)] == 0);
    XSFLIB_CHECK(out[2 * pair_after(21)] == 13 * 4 * 64);
    XSFLIB_CHECK(out[2 * (pair_after(36) - 2)] == 13 * 4 * 64);
    for (size_t i = 2 * pair_after(37); i < out.size(); i++)
    {
        XSFLIB_CHECK(out[i] == -13 * 4 * 64);
    }

    // The stopped timer gives no change in the next frames.
    std::vector<int16_t> more;
    player.RunFrame(more);
    XSFLIB_CHECK(player.Driver().TimerChanges().empty());
    XSFLIB_CHECK(std::all_of(more.begin(), more.end(), [](int16_t s) { return s == -13 * 4 * 64; }));
}

void TestFifoTimerChange()
{
    // A note on a running timer gives the timer a new period, and the FIFO doesn't start again. The timer's reload
    // takes effect at the next overflow, and that overflow stays where the old period put it. Overflows of 0x400
    // cycles come from the first frame's stores on. A frame holds 274 of them and 0x140 cycles, so the second frame's
    // stores come 0x2C0 cycles before the next one. A sample comes each 512 cycles, and the bytes alternate, so each
    // overflow moves the output.
    Image m;
    m.OneTrack(4, {0x00, 0xBC, 0x01, 0x01, 0x9C, 0x01, 0x3C, 0xFD}, Revision::kDungeonDiceMonsters);
    m.PutBytes(kMap, {0x00, 0x05});
    m.Put16(kPeriods, 0x400);
    m.Put16(kPeriods + 2 * 17, 0x800);
    std::vector<int8_t> bytes;
    for (int i = 0; i < 64 * 16; i++)
    {
        bytes.push_back(int8_t(i % 2 == 0 ? 64 : -64));
    }
    FifoSample(m, 5, 64, 0x4000, bytes);
    const Rom rom = m.ToRom();

    ArchivePlayer player(rom, FifoInfo());
    player.Start(0);
    Frames(player, 1);
    XSFLIB_CHECK(player.Driver().TimerChanges().size() == 1);
    XSFLIB_CHECK(player.Driver().TimerChanges()[0].period == 0x400 && player.Driver().TimerChanges()[0].first == 0x400);

    const std::vector<int16_t> out = Frames(player, 1);
    XSFLIB_CHECK(!player.Driver().FifoRestarted(0));
    XSFLIB_CHECK(player.Driver().TimerChanges().size() == 1);
    XSFLIB_CHECK(player.Driver().TimerChanges()[0].cycle == 0 && player.Driver().TimerChanges()[0].period == 0x800);
    XSFLIB_CHECK(player.Driver().TimerChanges()[0].first == 0x2C0);

    // The next overflow moves the output at the first sample at or after its cycle. Each later one comes 0x800 cycles
    // on, 4 samples apart.
    const size_t first = (kStore + 0x2C0 + 511) / 512;
    XSFLIB_CHECK(first > 1 && 2 * (first + 4) < out.size());
    XSFLIB_CHECK(out[2 * (first - 1)] != out[2 * first]);
    for (size_t i = first + 1; i < first + 4; i++)
    {
        XSFLIB_CHECK(out[2 * (i - 1)] == out[2 * i]);
    }
    XSFLIB_CHECK(out[2 * (first + 3)] != out[2 * (first + 4)]);
}

void TestBusy()
{
    // Track 4 stops the song after 2 frames. The frame after the stop stops every voice. A block mixer's FIFOs then
    // play their lead within the next frames, and the output stays silent.
    Image m;
    m.Mixer(kRecords24);
    m.Song(0, kBase, 0x100, {{4, 0x00}, {5, 0x10}}, Revision::kUltimateMasters);
    m.PutBytes(kBase, {0x02, 0xFF, 0x00});
    m.PutBytes(kBase + 0x10, {0x00, 0xA8, 0x10, 0x00, 0x7F, 0xFD});
    m.PutBytes(kBase + 0x100, kWaitingTrack);
    m.Put8(kVolumes + 16 * 64 + 0x3F, 128);
    m.Sample(0, 0x1000, 16, 0, Constant(32));
    const Rom rom = m.ToRom();

    ArchivePlayer player(rom, Blocks());
    player.Start(0);
    Frames(player, 4);
    XSFLIB_CHECK(player.Busy());
    int frames = 0;
    while (player.Busy() && frames < 16)
    {
        player.RunFrame();
        frames++;
    }
    XSFLIB_CHECK(!player.Busy());
    const std::vector<int16_t> out = Frames(player, 2);
    XSFLIB_CHECK(std::all_of(out.begin(), out.end(), [](int16_t s) { return s == 0; }));
}

void TestBusyOnRing()
{
    // Track 4 stops the song after 2 frames, and the frame after the stop stops every voice. A ring's lead of 480
    // points takes 2 frames and a bit more. The song stays busy until those points have played. Nothing moves after.
    Image m;
    m.Mixer(0);
    m.Song(0, kBase, 0x100, {{4, 0x00}, {5, 0x10}}, Revision::kEternalDuelist);
    m.PutBytes(kBase, {0x02, 0xFF, 0x00});
    m.PutBytes(kBase + 0x10, {0x00, 0xAF, 0x00, 0x7F, 0xFD});
    m.PutBytes(kBase + 0x100, kWaitingTrack);
    m.Sample(0, 0x1000, 16, 0, Constant(32));
    const Rom rom = m.ToRom();

    ArchivePlayer player(rom, InfoOf(Revision::kEternalDuelist));
    player.Start(0);
    std::vector<int16_t> out;
    int frames = 0;
    while (player.Busy() && frames < 32)
    {
        player.RunFrame(out);
        frames++;
    }
    XSFLIB_CHECK(!player.Busy());
    XSFLIB_CHECK(std::any_of(out.begin(), out.end(), [](int16_t s) { return s == 32 * 4 * 64; }));
    XSFLIB_CHECK(out.back() == 0);
    const std::vector<int16_t> after = Frames(player, 4);
    XSFLIB_CHECK(std::all_of(after.begin(), after.end(), [](int16_t s) { return s == 0; }));
}

void TestFramesWithoutSamples()
{
    // A frame without samples ends in the state that a frame with samples leaves.
    const Rom rom = Sine(0x1000);
    ArchivePlayer whole(rom, Blocks());
    ArchivePlayer seeking(rom, Blocks());
    whole.Start(0);
    seeking.Start(0);
    Frames(whole, 5);
    for (int f = 0; f < 5; f++)
    {
        seeking.RunFrame();
    }

    XSFLIB_CHECK(seeking.Frames() == 5);
    XSFLIB_CHECK(Frames(seeking, 3) == Frames(whole, 3));
}

void TestSnapshots()
{
    const Rom rom = TwoVoices(32, 16);
    ArchivePlayer player(rom, Blocks());
    XSFLIB_CHECK(Throws<std::logic_error>([&] { player.Save(); }));
    player.Start(0);
    Frames(player, 6);
    const ArchivePlayer::Snapshot snapshot = player.Save();
    const std::vector<int16_t> run = Frames(player, 6);
    player.Restore(snapshot);
    XSFLIB_CHECK(player.Frames() == 6);
    XSFLIB_CHECK(Frames(player, 6) == run);

    ArchivePlayer other(rom, Blocks());
    other.Start(0);
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { other.Restore(snapshot); }));

    // A snapshot leaves the mutes out: a mute stays through a restore.
    player.MuteTrack(5, true);
    player.Restore(snapshot);
    XSFLIB_CHECK(player.IsTrackMuted(5));
    player.RunFrame();
    XSFLIB_CHECK(AllAre(player.FifoBytes(0), 32));
}

void TestSeekFifoTimers()
{
    // A restore and frames without samples also keep the revision without a mixer's timers. Their changes come in a
    // frame's time, after the stores.
    Image m;
    m.OneTrack(4, {0x00, 0xBC, 0x01, 0x02, 0xBC, 0x01, 0x3C, 0xFD}, Revision::kDungeonDiceMonsters);
    m.PutBytes(kMap, {0x00, 0x05});
    m.Put16(kPeriods, 0x100);
    std::vector<int8_t> bytes = Block({32});
    FifoSample(m, 5, 1, 0x4000, bytes);
    const Rom rom = m.ToRom();

    ArchivePlayer player(rom, FifoInfo());
    player.Start(0);
    Frames(player, 1);
    const ArchivePlayer::Snapshot snapshot = player.Save();
    const std::vector<int16_t> run = Frames(player, 4);
    player.Restore(snapshot);
    XSFLIB_CHECK(Frames(player, 4) == run);

    ArchivePlayer seeking(rom, FifoInfo());
    seeking.Start(0);
    seeking.RunFrame();
    XSFLIB_CHECK(Frames(seeking, 4) == run);
}

void TestMutesOnBlocks()
{
    const Rom rom = TwoVoices(32, 16);
    ArchivePlayer whole(rom, Blocks());
    ArchivePlayer player(rom, Blocks());
    whole.Start(0);
    player.Start(0);
    XSFLIB_CHECK(Throws<std::out_of_range>([&] { player.MuteTrack(kTracks, true); }));
    XSFLIB_CHECK(Throws<std::out_of_range>([&] { player.SoloTrack(-1, true); }));

    // A mute takes the voice's points out of the sums. The voice runs on as before.
    player.MuteTrack(5, true);
    XSFLIB_CHECK(player.IsTrackMuted(5) && !player.IsTrackMuted(4));
    Frames(whole, 2);
    Frames(player, 2);
    XSFLIB_CHECK(AllAre(whole.FifoBytes(0), 48));
    XSFLIB_CHECK(AllAre(player.FifoBytes(0), 32) && AllAre(player.FifoBytes(1), 32));
    const auto same_voices = [&]
    {
        const auto& a = whole.Driver().Voices();
        const auto& b = player.Driver().Voices();
        for (size_t v = 0; v < a.size(); v++)
        {
            if (a[v].position != b[v].position || a[v].count != b[v].count || a[v].flags != b[v].flags)
            {
                return false;
            }
        }

        return true;
    };
    XSFLIB_CHECK(same_voices());

    // A solo leaves every other track out.
    player.MuteTrack(5, false);
    player.SoloTrack(5, true);
    XSFLIB_CHECK(player.IsTrackSoloed(5));
    player.RunFrame();
    whole.RunFrame();
    XSFLIB_CHECK(AllAre(player.FifoBytes(0), 16) && AllAre(player.FifoBytes(1), 16));
    XSFLIB_CHECK(same_voices());

    // A mute still holds on a soloed track.
    player.MuteTrack(5, true);
    player.RunFrame();
    XSFLIB_CHECK(AllAre(player.FifoBytes(0), 0));

    // Back to the whole sound.
    player.MuteTrack(5, false);
    player.SoloTrack(5, false);
    player.RunFrame();
    XSFLIB_CHECK(AllAre(player.FifoBytes(0), 48));

    // A mute of a sum that wraps takes the voice's points out, and the wrap with them. 100 + 100 wraps to -56.
    const Rom wrapping = TwoVoices(100, 100);
    ArchivePlayer wrapped(wrapping, Blocks());
    wrapped.Start(0);
    wrapped.RunFrame();
    XSFLIB_CHECK(AllAre(wrapped.FifoBytes(0), -56));
    wrapped.MuteTrack(5, true);
    wrapped.RunFrame();
    XSFLIB_CHECK(AllAre(wrapped.FifoBytes(0), 100));
}

void TestMutesOnRing()
{
    // Voices 0 to 2 play on FIFO A, and 3 to 5 on FIFO B.
    Image m;
    m.Mixer(0);
    m.Song(0, kBase, 0x100, {{4, 0x00}, {5, 0x10}, {7, 0x20}}, Revision::kEternalDuelist);
    m.PutBytes(kBase, {0x00, 0xAF, 0x00, 0x7F, 0xFD});
    m.PutBytes(kBase + 0x10, {0x00, 0xAF, 0x01, 0x7F, 0xFD});
    m.PutBytes(kBase + 0x20, {0x00, 0xAF, 0x02, 0x7F, 0xFD});
    m.PutBytes(kBase + 0x100, kWaitingTrack);
    m.Sample(0, 0x1000, 16, 0, Constant(32));
    m.Sample(1, 0x1000, 16, 0, Constant(16));
    m.Sample(2, 0x1000, 16, 0, Constant(8));
    const Rom rom = m.ToRom();

    ArchivePlayer player(rom, InfoOf(Revision::kEternalDuelist));
    player.Start(0);
    player.RunFrame();
    XSFLIB_CHECK(AllAre(player.FifoBytes(0), 48) && AllAre(player.FifoBytes(1), 8));
    player.MuteTrack(5, true);
    player.RunFrame();
    XSFLIB_CHECK(AllAre(player.FifoBytes(0), 32) && AllAre(player.FifoBytes(1), 8));
    player.MuteTrack(5, false);
    player.SoloTrack(7, true);
    player.RunFrame();
    XSFLIB_CHECK(AllAre(player.FifoBytes(0), 0) && AllAre(player.FifoBytes(1), 8));
}

void TestMutesOnFifo()
{
    // Track 4 plays a note on each of FIFO A's voices. Two voices at their samples' rates both sound, at the second's
    // rate. Their bytes are 64 * 13 / 32 and -64 * 7 / 32. The fourth overflow takes the samples' one block, and stops
    // the timer. A mute of either leaves the FIFO's timer as it was.
    Image m;
    m.OneTrack(4, {0x00, 0xBC, 0x01, 0x00, 0xA6, 0x02, 0x3C, 0xFD}, Revision::kDungeonDiceMonsters);
    m.PutBytes(kMap, {0x00, 0x05, 0x07});
    FifoSample(m, 5, 1, 0x300, Block({64}));
    FifoSample(m, 7, 1, 0x200, Block({-64}));
    const Rom rom = m.ToRom();

    for (const auto& [muted, expected] : {std::pair{-1, 12}, std::pair{4, -14}, std::pair{5, 26}})
    {
        ArchivePlayer player(rom, FifoInfo());
        player.Start(0);
        if (muted >= 0)
        {
            player.MuteTrack(muted, true);
        }

        player.RunFrame();
        const std::vector<int8_t>& a = player.FifoBytes(0);
        XSFLIB_CHECK(a.size() == 32);
        XSFLIB_CHECK(std::all_of(a.begin(), a.begin() + 16, [](int8_t b) { return b == 0; }));
        XSFLIB_CHECK(std::all_of(a.begin() + 16, a.end(), [v = expected](int8_t b) { return b == v; }));
        const std::vector<ntdlib::konami::FifoTimerChange>& changes = player.Driver().TimerChanges();
        XSFLIB_CHECK(changes.size() == 2 && changes[0].period == 0x200 && changes[1].cycle == 4 * 0x200);
    }

    // A muted track's meter stays at 0, and the other track's goes on.
    ArchivePlayer metered(rom, FifoInfo());
    metered.Start(0);
    metered.MeterTracks(true);
    metered.MuteTrack(4, true);
    std::vector<int16_t> samples;
    metered.RunFrame(samples);
    XSFLIB_CHECK(metered.Driver().VoicePeaks(4)[0] == 0.0f);
    XSFLIB_CHECK(metered.Driver().VoicePeaks(5)[0] > 0.0f);
}

void TestMutesOnPsg()
{
    // A pan off centre on a square's track takes two channels: the channel plays the right side's level, and the next
    // channel the left side's. NR51 sends the first to the right and the second to the left. Each level gives the side
    // 2 * 8 of the 10 bits: the master volume's 7 plus 1, at the PSG's full share.
    Image m;
    m.Mixer(kRecords24);
    m.OneTrack(0, {0x00, 0xF0, 0x8F, 0x00, 0xD8, 0x30, 0x10, 0xFD}, Revision::kWct2004);
    for (uint32_t volume = 0; volume < 256; volume++)
    {
        m.Put8(kVolumes + volume * 16 + 15, 5);
        m.Put8(kVolumes + volume * 16 + 8, 3);
    }
    const Rom rom = m.ToRom();

    const auto peaks = [&](int muted, int soloed)
    {
        ArchivePlayer player(rom, InfoOf(Revision::kWct2004));
        player.Start(0);
        player.MeterTracks(true);
        if (muted >= 0)
        {
            player.MuteTrack(muted, true);
        }
        if (soloed >= 0)
        {
            player.SoloTrack(soloed, true);
        }

        std::array<int32_t, 2> track = {};
        std::array<int32_t, 2> output = {};
        for (int f = 0; f < 3; f++)
        {
            std::vector<int16_t> out;
            player.RunFrame(out);
            XSFLIB_CHECK(player.Driver().PsgChannelTrack(0) == 0 && player.Driver().PsgChannelTrack(1) == 0);
            XSFLIB_CHECK(player.Driver().PsgChannelTrack(2) == 2 && player.Driver().PsgChannelTrack(3) == 3);
            for (size_t side = 0; side < 2; side++)
            {
                track[side] = std::max(track[side], player.TrackPeaks(0)[side]);
                output[side] = std::max(output[side], player.OutputPeaks()[side]);
            }
        }

        return std::pair{track, output};
    };

    const auto whole = peaks(-1, -1);
    XSFLIB_CHECK(whole.first[0] == 3 * 16 * 64 && whole.first[1] == 5 * 16 * 64);
    XSFLIB_CHECK(whole.second[0] == 3 * 16 * 64 && whole.second[1] == 5 * 16 * 64);

    // The next track's channel counts as the track's. A mute of the next track changes nothing.
    const auto muted = peaks(0, -1);
    XSFLIB_CHECK(muted.first[0] == 0 && muted.first[1] == 0 && muted.second[0] == 0 && muted.second[1] == 0);
    const auto next = peaks(1, -1);
    XSFLIB_CHECK(next.first == whole.first && next.second == whole.second);
    const auto alone = peaks(-1, 0);
    XSFLIB_CHECK(alone.first == whole.first && alone.second == whole.second);
    const auto other = peaks(-1, 2);
    XSFLIB_CHECK(other.second[0] == 0 && other.second[1] == 0);

    // A mute leaves the stores as they are.
    ArchivePlayer played(rom, InfoOf(Revision::kWct2004));
    ArchivePlayer silent(rom, InfoOf(Revision::kWct2004));
    played.Start(0);
    silent.Start(0);
    silent.MuteTrack(0, true);
    played.RunFrame();

    // The note's frame stays silent: the stores give the next channel to the muted track.
    const std::vector<int16_t> silence = Frames(silent, 1);
    XSFLIB_CHECK(std::all_of(silence.begin(), silence.end(), [](int16_t s) { return s == 0; }));
    XSFLIB_CHECK(played.Hardware().Register(0x04000063) == 5 << 4 || played.Hardware().Register(0x04000063) >> 4 == 5);
    XSFLIB_CHECK(silent.Hardware().Register(0x04000063) == played.Hardware().Register(0x04000063));
}

void TestWaveChannel()
{
    // The init restarts the wave channel, and a note of most revisions restarts nothing. A note on track 2: volume 3,
    // note 0x18. The table's wave at volume 3 holds a first digit of 15 and other digits of 0. The channel plays at
    // full volume. As for a square, each level gives the side 2 * 8 of the 10 bits.
    for (const Revision revision : {Revision::kUltimateMasters, Revision::kWct2004, Revision::kEternalDuelist})
    {
        Image m;
        m.Mixer(revision == Revision::kEternalDuelist ? 0 : kRecords24);
        m.OneTrack(2, {0x00, 0xD3, 0x18, 0x7F, 0xFD}, revision);
        m.Put8(kWaves + 3 * 16, 0xF0);
        const Rom rom = m.ToRom();

        ArchivePlayer player(rom, InfoOf(revision));
        player.Start(0);
        player.MeterTracks(true);
        std::array<int32_t, 2> peaks = {};
        for (int f = 0; f < 60; f++)
        {
            Frames(player, 1);
            peaks[0] = std::max(peaks[0], player.TrackPeaks(2)[0]);
            peaks[1] = std::max(peaks[1], player.TrackPeaks(2)[1]);
        }
        XSFLIB_CHECK(peaks[0] == 15 * 16 * 64 && peaks[1] == 15 * 16 * 64);
    }
}

void TestMeters()
{
    // Track 4: pan 0x10, then a note. The levels of 48 and 96 give FIFO A, the right, 64 * 48 / 128, and FIFO B, the
    // left, 64 * 96 / 128.
    Image m;
    m.Mixer(kRecords24);
    m.OneTrack(4, {0x00, 0xF0, 0x10, 0x00, 0xA8, 0x10, 0x00, 0x7F, 0xFD}, Revision::kUltimateMasters);
    m.Put8(kVolumes + 16 * 64 + 0x10, 48);
    m.Put8(kVolumes + 16 * 64 + 0x3F, 96);
    m.Sample(0, 0x1000, 16, 0, Constant(64));
    const Rom rom = m.ToRom();

    ArchivePlayer player(rom, Blocks());
    player.Start(0);

    // Without meters, every peak is 0.
    Frames(player, 1);
    XSFLIB_CHECK(player.TrackPeaks(4)[0] == 0 && player.OutputPeaks()[0] == 0);

    player.MeterTracks(true);
    Frames(player, 2);
    XSFLIB_CHECK(player.TrackPeaks(4)[0] == 48 * 4 * 64 && player.TrackPeaks(4)[1] == 24 * 4 * 64);
    XSFLIB_CHECK(player.TrackPeaks(5)[0] == 0 && player.TrackPeaks(5)[1] == 0);
    XSFLIB_CHECK(player.OutputPeaks()[0] == 48 * 4 * 64 && player.OutputPeaks()[1] == 24 * 4 * 64);

    // A frame without samples gives no peaks.
    player.RunFrame();
    XSFLIB_CHECK(player.TrackPeaks(4)[0] == 0 && player.OutputPeaks()[0] == 0);

    // A muted track's meter stays at 0.
    player.MuteTrack(4, true);
    Frames(player, 2);
    XSFLIB_CHECK(player.TrackPeaks(4)[0] == 0 && player.TrackPeaks(4)[1] == 0);
    player.MeterTracks(false);
    player.MuteTrack(4, false);
    Frames(player, 1);
    XSFLIB_CHECK(player.TrackPeaks(4)[0] == 0 && player.OutputPeaks()[0] == 0);
}

} // namespace

int RunTests()
{
    TestBlockOutput();
    TestRegisters();
    TestRingOutput();
    TestFifoRestart();
    TestFifoTimers();
    TestFifoTimerChange();
    TestBusy();
    TestBusyOnRing();
    TestFramesWithoutSamples();
    TestSnapshots();
    TestSeekFifoTimers();
    TestMutesOnBlocks();
    TestMutesOnRing();
    TestMutesOnFifo();
    TestMutesOnPsg();
    TestWaveChannel();
    TestMeters();

    return xsflib::testing::Summary("Konami's player");
}

} // namespace konami_player_tests

int main()
{
    return konami_player_tests::RunTests();
}
