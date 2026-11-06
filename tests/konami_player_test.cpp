// SPDX-License-Identifier: MIT

// Tests of konami::ArchivePlayer on small synthetic cartridges. No game ROM is needed. Each expected value follows a
// rule of the driver's code, of the GBA's sound hardware or of the enhanced mode's rules in ntdlib/konami/output.h. The
// comments give the rule.
//
// The carts' mixers run at 1,254 cycles a point: exactly 14 blocks of 16 points a frame. The hardware's output at 10
// bits is the bias of 0x200 plus each FIFO's byte times 4 and the PSG's levels times 16, rounded down to the
// resolution's steps. A step of the 10 bits gives 64 in a 16-bit sample.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/player/archive_source.h"
#include "xsflib/testing/check.h"

#include "konami_output_image.h"
#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/konami/archive_player.h"
#include "ntdlib/konami/archive_source.h"
#include "ntdlib/konami/driver.h"
#include "ntdlib/konami/output.h"
#include "ntdlib/konami/seqformat.h"
#include "ntdlib/wave/blep.h"

namespace
{

using namespace konami_testing;

using ntdlib::agb::kFrameCycles;
using ntdlib::agb::Rom;
using ntdlib::konami::ArchivePlayer;
using ntdlib::konami::ArchivePlayerOptions;
using ntdlib::konami::kTracks;
using ntdlib::konami::Output;

constexpr uint32_t kPoint = 1254;
constexpr uint32_t kStore = kFrameCycles / 6;
constexpr size_t kEnhanced = ntdlib::agb::kEnhancedFrameSamples;

// A block mixer's new byte waits behind the 3 blocks that its mixer and DMA hold back. Before the first stores' bytes,
// the FIFOs hold the zeros of the cycles up to the stores, and of that lead.
constexpr uint32_t kBlockSilence = kStore / kPoint + 48;

// A ring mixer's new byte plays once the DMA comes round the ring of 704 points. The ring's DMA stands 224 points, a
// frame's, ahead of the byte that the first stores mix.
constexpr uint32_t kRingSilence = kStore / kPoint + 704 - 224;

// In the enhanced mode, the silence before the first stores' samples: the same time at 804 samples a frame, from the
// next sample on, and the half width of the PSG's band-limited steps. The hardware model holds the FIFOs back by that
// half width, to keep them in time with the steps.
constexpr size_t kEnhancedStart = size_t((uint64_t(kStore + 48 * kPoint) * kEnhanced + kFrameCycles) / kFrameCycles) +
                                  size_t(ntdlib::wave::BandLimitedSteps::kHalfWidth);

template <typename E, typename F>
bool Throws(F f)
{
    try
    {
        f();
    }
    catch (const E&)
    {
        return true;
    }

    return false;
}

bool AllAre(const std::vector<int8_t>& bytes, int value)
{
    return std::all_of(bytes.begin(), bytes.end(), [value](int8_t b) { return b == value; });
}

// A sample looped from its start, of one value throughout.
std::vector<int8_t> Constant(int value)
{
    return std::vector<int8_t>(16, int8_t(value));
}

// A block mixer's song: track 4 plays a looped sample of `first`, and track 5 one of `second`, each with a level of
// 128. Such a level gives each byte as it stands, and a level in the enhanced mode of 1.
Rom TwoVoices(int first, int second)
{
    Image m;
    m.Mixer(kRecords24);
    m.Song(0, kBase, 0x100, {{4, 0x00}, {5, 0x10}}, Revision::kUltimateMasters);
    m.PutBytes(kBase, {0x00, 0xA8, 0x10, 0x00, 0x7F, 0xFD});
    m.PutBytes(kBase + 0x10, {0x00, 0xA8, 0x10, 0x01, 0x7F, 0xFD});
    m.PutBytes(kBase + 0x100, kWaitingTrack);
    m.Put8(kVolumes + 16 * 64 + 0x3F, 128);
    m.Sample(0, 0x1000, 16, 0, Constant(first));
    m.Sample(1, 0x1000, 16, 0, Constant(second));
    return m.ToRom();
}

DriverInfo Blocks()
{
    return InfoOf(Revision::kUltimateMasters);
}

// A sample of a whole sine in 64 points, looped.
Rom Sine(uint32_t step)
{
    Image m;
    m.Mixer(kRecords24);
    m.OneTrack(4, {0x00, 0xA8, 0x10, 0x00, 0x7F, 0xFD}, Revision::kUltimateMasters);
    m.Put8(kVolumes + 16 * 64 + 0x3F, 128);
    std::vector<int8_t> bytes;
    for (int i = 0; i < 64; i++)
    {
        bytes.push_back(int8_t(std::lround(100.0 * std::sin(2.0 * 3.14159265358979 * i / 64.0))));
    }
    m.Sample(0, step, 64, 0, bytes);
    return m.ToRom();
}

std::vector<int16_t> Frames(ArchivePlayer& player, int frames)
{
    std::vector<int16_t> out;
    for (int f = 0; f < frames; f++)
    {
        player.RunFrame(out);
    }

    return out;
}

std::vector<float> EnhancedFrames(ArchivePlayer& player, int frames)
{
    std::vector<float> out;
    for (int f = 0; f < frames; f++)
    {
        player.RunFrame(out);
    }

    return out;
}

ArchivePlayerOptions EnhancedOptions()
{
    ArchivePlayerOptions options;
    options.enhanced = true;
    return options;
}

bool Near(float value, double expected, double within = 0.002)
{
    const bool near = std::fabs(double(value) - expected) <= within;
    if (!near)
    {
        std::printf("%f, expected %f\n", double(value), expected);
    }

    return near;
}

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

void TestEnhancedGain()
{
    // 804 samples a frame. On a block mixer, a level of 128 gives a sample's value over 128 of full scale. The sound
    // starts after the same time as on the console.
    const Rom rom = TwoVoices(64, 0);
    ArchivePlayer player(rom, Blocks());
    player.Start(0, EnhancedOptions());
    XSFLIB_CHECK(player.Enhanced() && player.Rate() == ntdlib::agb::kEnhancedRate);
    std::vector<int16_t> strict;
    XSFLIB_CHECK(Throws<std::logic_error>([&] { player.RunFrame(strict); }));
    const std::vector<float> out = EnhancedFrames(player, 3);
    XSFLIB_CHECK(out.size() == 2 * 3 * kEnhanced);
    for (size_t i = 0; i < 2 * (kEnhancedStart - 64); i++)
    {
        XSFLIB_CHECK(std::fabs(out[i]) < 0.03f);
    }
    for (size_t i = 2 * kEnhanced; i < out.size(); i++)
    {
        XSFLIB_CHECK(Near(out[i], 0.5));
    }

    // The console's mode refuses the enhanced mode's frames.
    ArchivePlayer console(rom, Blocks());
    console.Start(0);
    std::vector<float> floats;
    XSFLIB_CHECK(Throws<std::logic_error>([&] { console.RunFrame(floats); }));
}

void TestEnhancedDoesNotWrap()
{
    // On the console, 127 + 127 wraps to -2. The mode's sums go on to 254 over 128.
    const Rom rom = TwoVoices(127, 127);
    ArchivePlayer player(rom, Blocks());
    player.Start(0, EnhancedOptions());
    const std::vector<float> out = EnhancedFrames(player, 3);
    XSFLIB_CHECK(AllAre(player.FifoBytes(0), -2));
    XSFLIB_CHECK(Near(out[out.size() - 1], 254.0 / 128.0, 0.01));
    XSFLIB_CHECK(Near(out[out.size() - 2], 254.0 / 128.0, 0.01));
}

void TestEnhancedRingAndFifoGains()
{
    // A ring's volume v gives (v + 1) / 16 on its FIFO.
    Image ring;
    ring.Mixer(0);
    ring.OneTrack(4, {0x00, 0xA7, 0x00, 0x7F, 0xFD}, Revision::kEternalDuelist);
    ring.Sample(0, 0x1000, 16, 0, Constant(64));
    const Rom ring_rom = ring.ToRom();
    ArchivePlayer ring_player(ring_rom, InfoOf(Revision::kEternalDuelist));
    ring_player.Start(0, EnhancedOptions());
    const std::vector<float> ring_out = EnhancedFrames(ring_player, 5);
    XSFLIB_CHECK(Near(ring_out[ring_out.size() - 1], 64.0 / 128.0 * 8.0 / 16.0));
    XSFLIB_CHECK(Near(ring_out[ring_out.size() - 2], 64.0 / 128.0 * 8.0 / 16.0));

    // In the revision without a mixer, a volume v gives (v + 1) / 32, at the FIFO's timer.
    Image fifo;
    fifo.OneTrack(4, {0x00, 0xBC, 0x01, 0x3C, 0xFD}, Revision::kDungeonDiceMonsters);
    fifo.PutBytes(kMap, {0x00, 0x05});
    fifo.Put16(kPeriods, 0x400);
    FifoSample(fifo, 5, 200, 0x4000, std::vector<int8_t>(200 * 16, 64));
    const Rom fifo_rom = fifo.ToRom();
    ArchivePlayer fifo_player(fifo_rom, FifoInfo());
    fifo_player.Start(0, EnhancedOptions());
    const std::vector<float> fifo_out = EnhancedFrames(fifo_player, 2);
    XSFLIB_CHECK(Near(fifo_out[fifo_out.size() - 1], 64.0 / 128.0 * 13.0 / 32.0));
    XSFLIB_CHECK(Near(fifo_out[fifo_out.size() - 2], 64.0 / 128.0 * 13.0 / 32.0));
}

void TestEnhancedRamp()
{
    // Track 4 plays a note, and sets a pan three frames later. The new level ramps across the frame of the pan, a step
    // for each sample: on the right, from 1 down to 1/2.
    Image m;
    m.Mixer(kRecords24);
    m.OneTrack(4, {0x00, 0xA8, 0x10, 0x00, 0x03, 0xF0, 0x10, 0x7F, 0xFD}, Revision::kUltimateMasters);
    m.Put8(kVolumes + 16 * 64 + 0x10, 64);
    m.Put8(kVolumes + 16 * 64 + 0x3F, 128);
    m.Sample(0, 0x1000, 16, 0, Constant(64));
    const Rom rom = m.ToRom();

    ArchivePlayer player(rom, Blocks());
    player.Start(0, EnhancedOptions());
    const std::vector<float> out = EnhancedFrames(player, 6);
    const size_t ramp = kEnhancedStart + 3 * kEnhanced;
    const auto right = [&](size_t sample)
    {
        return out[2 * sample + 1];
    };
    const auto left = [&](size_t sample)
    {
        return out[2 * sample];
    };
    XSFLIB_CHECK(Near(right(ramp - 8), 0.5));
    XSFLIB_CHECK(Near(right(ramp + kEnhanced / 4), 0.5 * (1.0 - 0.5 / 4.0)));
    XSFLIB_CHECK(Near(right(ramp + kEnhanced / 2), 0.5 * (1.0 - 0.5 / 2.0)));
    XSFLIB_CHECK(Near(right(ramp + kEnhanced + 8), 0.25));
    XSFLIB_CHECK(Near(left(ramp + kEnhanced / 2), 0.5));
    XSFLIB_CHECK(Near(left(ramp + kEnhanced + 8), 0.5));
}

void TestEnhancedRate()
{
    // A step of 0x1000 moves a point for each of the console's 224 points in a frame: 3.5 passes of the sine's 64
    // points. Half the step gives half the rate.
    const auto crossings = [](uint32_t step)
    {
        const Rom rom = Sine(step);
        ArchivePlayer player(rom, Blocks());
        player.Start(0, EnhancedOptions());
        const std::vector<float> out = EnhancedFrames(player, 40);
        int count = 0;
        for (size_t i = 2 * (4 * kEnhanced) + 2; i < out.size(); i += 2)
        {
            count += (out[i - 2] < 0.0f) != (out[i] < 0.0f) ? 1 : 0;
        }

        return count;
    };

    constexpr int kFrames = 36;
    const int whole = crossings(0x1000);
    XSFLIB_CHECK(std::abs(whole - kFrames * 7) <= 2);
    const int half = crossings(0x800);
    XSFLIB_CHECK(std::abs(half - kFrames * 7 / 2) <= 2);
}

void TestEnhancedEcho()
{
    // Track 4: feedback 0x40 on bus 0, a delay of 2 blocks, voice 0 on bus 0. The feedback moves 8 towards its goal
    // each frame. Twelve frames later, a short note goes in. The bus's line gives it back a delay later at a quarter of
    // its level, and a quarter of that after the next delay. 2 blocks last 2 * 16 * 1254 cycles: 114.86 samples,
    // rounded.
    Image m;
    m.Mixer(kRecords24);
    m.OneTrack(4, {0x00, 0xF7, 0x40, 0x00, 0xF8, 0x01, 0x00, 0xF9, 0x01, 0x0C, 0xA8, 0x10, 0x00, 0x7F, 0xFD},
               Revision::kUltimateMasters);
    m.Put8(kVolumes + 16 * 64 + 0x3F, 128);
    m.Sample(0, 0x1000, 8, -1, std::vector<int8_t>(8, 64));
    const Rom rom = m.ToRom();

    ArchivePlayer player(rom, Blocks());
    player.Start(0, EnhancedOptions());
    const std::vector<float> out = EnhancedFrames(player, 16);
    size_t start = 0;
    while (start < out.size() / 2 && out[2 * start] < 0.25f)
    {
        start++;
    }
    XSFLIB_CHECK(start + 2 * 115 + 64 < out.size() / 2);
    constexpr size_t kDelay = 115;
    for (size_t i = start - 16; i < start + 48; i++)
    {
        XSFLIB_CHECK(Near(out[2 * (i + kDelay)], 0.25 * double(out[2 * i]), 0.001));
        XSFLIB_CHECK(Near(out[2 * (i + 2 * kDelay)], 0.0625 * double(out[2 * i]), 0.001));
    }
    XSFLIB_CHECK(out[2 * (start + 14)] > 0.4f && out[2 * (start + 14)] < 0.6f);
}

void TestEnhancedFramesWithoutMixing()
{
    // A frame without samples queues silence. The voice's place moves on: the frames after the lead's silence equal
    // those of a run with every frame's samples.
    const Rom rom = Sine(0x1000);
    ArchivePlayer whole(rom, Blocks());
    ArchivePlayer seeking(rom, Blocks());
    whole.Start(0, EnhancedOptions());
    seeking.Start(0, EnhancedOptions());
    EnhancedFrames(whole, 3);
    for (int f = 0; f < 3; f++)
    {
        seeking.RunFrame();
    }

    const std::vector<float> a = EnhancedFrames(whole, 3);
    const std::vector<float> b = EnhancedFrames(seeking, 3);
    for (size_t i = 0; i < 2 * (kEnhancedStart - 64); i++)
    {
        XSFLIB_CHECK(b[i] == 0.0f);
    }
    for (size_t i = 2 * kEnhanced; i < a.size(); i++)
    {
        XSFLIB_CHECK(a[i] == b[i]);
    }
}

void TestEnhancedSnapshotsAndMutes()
{
    const Rom rom = TwoVoices(64, 32);
    ArchivePlayer player(rom, Blocks());
    player.Start(0, EnhancedOptions());
    EnhancedFrames(player, 4);
    const ArchivePlayer::Snapshot snapshot = player.Save();
    const std::vector<float> run = EnhancedFrames(player, 4);
    player.Restore(snapshot);
    XSFLIB_CHECK(EnhancedFrames(player, 4) == run);
    XSFLIB_CHECK(Near(run[run.size() - 1], 0.75));

    // A mute takes the voice's samples out, and a solo leaves only them. A meter shows the part of a track on the scale
    // of 16-bit samples, and the output's peak on the same scale.
    player.MuteTrack(5, true);
    std::vector<float> muted = EnhancedFrames(player, 3);
    XSFLIB_CHECK(Near(muted[muted.size() - 1], 0.5));
    player.MuteTrack(5, false);
    player.SoloTrack(5, true);
    player.MeterTracks(true);
    muted = EnhancedFrames(player, 3);
    XSFLIB_CHECK(Near(muted[muted.size() - 1], 0.25));
    XSFLIB_CHECK(std::abs(player.TrackPeaks(5)[0] - 8192) < 200 && std::abs(player.TrackPeaks(5)[1] - 8192) < 200);
    XSFLIB_CHECK(player.TrackPeaks(4)[0] == 0);
    XSFLIB_CHECK(std::abs(player.OutputPeaks()[0] - 8192) < 200);
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
    bool tables_given = true;
};

struct RomProgram
{
    using Set = RomSet;

    static std::optional<std::string> Select(RomSet& set, ntdlib::konami::ArchiveSelection& selection)
    {
        selection.rom_path = "game.gba";
        selection.song = set.song;
        if (set.tables_given)
        {
            selection.overrides.song_table = kSongs;
            selection.overrides.song_count = 1;
            selection.overrides.sample_table = kSampleTable;
            selection.overrides.mix_rate = kMixRate;
        }

        return std::nullopt;
    }
};

using RomSource = xsflib::player::ArchiveSource<ntdlib::konami::ArchiveTraits<RomProgram>>;

// The start of a command reader of the Ultimate Masters revision: it compares the command with FD third. The output
// stage's load of the volume table: ldrb r2,[r2,#3] / lsls r1,r2,#6 / ldr r0,=table / adds r1,r1,r0, and the literal.
void PutDetectedCode(Image& m)
{
    const uint16_t reader[] = {0x781D, 0x2DFC, 0xDD1E, 0x2DFF, 0xD107, 0x2DFD, 0xD100, 0x2DEF, 0xDC00, 0x2DE0, 0xD110};
    for (uint32_t i = 0; i < 11; i++)
    {
        m.Put16(kRomBase + 0x900 + 2 * i, reader[i]);
    }

    const uint16_t volume[] = {0x78D2, 0x0191, 0x4800, 0x1809};
    for (uint32_t i = 0; i < 4; i++)
    {
        m.Put16(kRomBase + 0xA00 + 2 * i, volume[i]);
    }
    m.Put32(kRomBase + 0xA08, kVolumes);
}

RomSet SetOf(const Rom& rom, int song)
{
    RomSet set;
    set.files["game.gba"].assign(rom.Ptr(kRomBase), rom.Ptr(kRomBase) + rom.Size());
    set.song = song;
    return set;
}

void TestArchiveSource()
{
    // Archive mode plays the song as the player does, takes snapshots, and gives the player's refusals as its errors. A
    // ROM without the driver fails to load. Tables given by hand get past the detection, and the mixer's search
    // refuses.
    Image m;
    PutDetectedCode(m);
    m.Mixer(kRecords24);
    m.OneTrack(4, {0x00, 0xA8, 0x10, 0x00, 0x7F, 0xFD}, Revision::kUltimateMasters);
    m.Put8(kVolumes + 16 * 64 + 0x3F, 128);
    m.Sample(0, 0x1000, 16, 0, Constant(32));
    const Rom rom = m.ToRom();

    RomSource source;
    RomSet set = SetOf(rom, 0);
    std::vector<int16_t> out;
    const std::optional<std::string> started = source.Start(set, out);
    XSFLIB_CHECK(!started);
    XSFLIB_CHECK(out.empty() && source.Busy());
    for (int f = 0; f < 6; f++)
    {
        XSFLIB_CHECK(source.RunFrame(out));
    }

    const std::shared_ptr<ArchivePlayer::Snapshot> snapshot = source.Save(nullptr);
    const size_t saved = out.size();
    for (int f = 0; f < 6; f++)
    {
        source.RunFrame(out);
    }
    XSFLIB_CHECK(out.back() == 32 * 4 * 64);
    source.Restore(*snapshot);
    std::vector<int16_t> again;
    for (int f = 0; f < 6; f++)
    {
        source.RunFrame(again);
    }
    XSFLIB_CHECK(std::equal(again.begin(), again.end(), out.begin() + std::ptrdiff_t(saved), out.end()));

    RomSet missing = SetOf(rom, 5);
    const std::optional<std::string> refused = source.Start(missing, out);
    XSFLIB_CHECK(refused && refused->find("song 5") != std::string::npos);
    RomSet empty;
    const std::optional<std::string> absent = source.Start(empty, out);
    XSFLIB_CHECK(absent && absent->find("game.gba") != std::string::npos);
    const Rom blank_rom = Image().ToRom();
    RomSet blank = SetOf(blank_rom, 0);
    const std::optional<std::string> without_mixer = source.Start(blank, out);
    XSFLIB_CHECK(without_mixer && without_mixer->find("mixer") != std::string::npos);
    blank = SetOf(blank_rom, 0);
    blank.tables_given = false;
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { source.Start(blank, out); }));
}

} // namespace

int main()
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
    TestEnhancedGain();
    TestEnhancedDoesNotWrap();
    TestEnhancedRingAndFifoGains();
    TestEnhancedRamp();
    TestEnhancedRate();
    TestEnhancedEcho();
    TestEnhancedFramesWithoutMixing();
    TestEnhancedSnapshotsAndMutes();
    TestArchiveSource();
    return xsflib::testing::Summary("Konami's player");
}
