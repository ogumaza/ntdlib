// SPDX-License-Identifier: MIT

// Tests of the enhanced mode of konami::ArchivePlayer, its echo and its archive source. konami_player_test.cpp says
// how the tests are built.

#include "konami_player_test_util.h"

namespace konami_player_tests
{
namespace
{

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

// Track 4: feedback `feedback` on bus 0, a delay of 2 blocks, voice 0 on bus 0. Twelve frames later, the track starts a
// short note.
Rom EchoSong(uint8_t feedback)
{
    Image m;
    m.Mixer(kRecords24);
    m.OneTrack(4, {0x00, 0xF7, feedback, 0x00, 0xF8, 0x01, 0x00, 0xF9, 0x01, 0x0C, 0xA8, 0x10, 0x00, 0x7F, 0xFD},
               Revision::kUltimateMasters);
    m.Put8(kVolumes + 16 * 64 + 0x3F, 128);
    m.Sample(0, 0x1000, 8, -1, std::vector<int8_t>(8, 64));
    return m.ToRom();
}

void TestEnhancedEcho()
{
    // A feedback of 0x40 moves 8 towards its goal each frame. The bus's line returns the note a delay later at a
    // quarter of its level, and a quarter of that after the next delay. 2 blocks last 2 * 16 * 1254 cycles: 114.86
    // samples, rounded.
    const Rom rom = EchoSong(0x40);

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

void TestEchoLeftOut()
{
    // A bus whose feedback stays 0 outputs only its input: the voice plays dry. Without the echo, the song with
    // feedback plays the same, in both modes. With the echo, it plays otherwise.
    const Rom echo = EchoSong(0x40);
    const Rom dry = EchoSong(0x00);
    ArchivePlayerOptions without;
    without.echo = false;
    ArchivePlayerOptions enhanced_without = EnhancedOptions();
    enhanced_without.echo = false;

    ArchivePlayer with_echo(echo, Blocks());
    ArchivePlayer left_out(echo, Blocks());
    ArchivePlayer reference(dry, Blocks());
    with_echo.Start(0);
    left_out.Start(0, without);
    reference.Start(0);
    const std::vector<int16_t> heard = Frames(with_echo, 16);
    const std::vector<int16_t> out = Frames(left_out, 16);
    const std::vector<int16_t> expected = Frames(reference, 16);

    ArchivePlayer enhanced_echo(echo, Blocks());
    ArchivePlayer enhanced_left_out(echo, Blocks());
    ArchivePlayer enhanced_reference(dry, Blocks());
    enhanced_echo.Start(0, EnhancedOptions());
    enhanced_left_out.Start(0, enhanced_without);
    enhanced_reference.Start(0, EnhancedOptions());
    const std::vector<float> enhanced_heard = EnhancedFrames(enhanced_echo, 16);
    const std::vector<float> enhanced_out = EnhancedFrames(enhanced_left_out, 16);
    const std::vector<float> enhanced_expected = EnhancedFrames(enhanced_reference, 16);

    XSFLIB_CHECK(out == expected);
    XSFLIB_CHECK(heard != expected);
    XSFLIB_CHECK(std::any_of(expected.begin(), expected.end(), [](int16_t sample) { return sample != 0; }));
    XSFLIB_CHECK(enhanced_out == enhanced_expected);
    XSFLIB_CHECK(enhanced_heard != enhanced_expected);
    XSFLIB_CHECK(left_out.Driver().Echo(0).feedback == with_echo.Driver().Echo(0).feedback);
}

void TestEchoLeftOutWithoutEcho()
{
    // The ring mixer's revisions and the revision without a mixer have no echo. They play the same without it.
    Image ring_image;
    ring_image.Mixer(0);
    ring_image.OneTrack(4, {0x00, 0xAF, 0x00, 0x7F, 0xFD}, Revision::kEternalDuelist);
    ring_image.Sample(0, 0x1000, 16, 0, Constant(32));
    const Rom ring = ring_image.ToRom();
    Image fifo_image;
    fifo_image.OneTrack(4, {0x00, 0xBC, 0x01, 0x3C, 0xFD}, Revision::kDungeonDiceMonsters);
    fifo_image.PutBytes(kMap, {0x00, 0x05});
    fifo_image.Put16(kPeriods, 0x400);
    FifoSample(fifo_image, 5, 8, 0x4000, std::vector<int8_t>(8 * 16, 32));
    const Rom fifo = fifo_image.ToRom();
    ArchivePlayerOptions without;
    without.echo = false;

    ArchivePlayer ring_with(ring, InfoOf(Revision::kEternalDuelist));
    ArchivePlayer ring_without(ring, InfoOf(Revision::kEternalDuelist));
    ArchivePlayer fifo_with(fifo, FifoInfo());
    ArchivePlayer fifo_without(fifo, FifoInfo());
    ring_with.Start(0);
    ring_without.Start(0, without);
    fifo_with.Start(0);
    fifo_without.Start(0, without);
    const std::vector<int16_t> ring_heard = Frames(ring_with, 4);
    const std::vector<int16_t> ring_out = Frames(ring_without, 4);
    const std::vector<int16_t> fifo_heard = Frames(fifo_with, 4);
    const std::vector<int16_t> fifo_out = Frames(fifo_without, 4);

    XSFLIB_CHECK(ring_out == ring_heard);
    XSFLIB_CHECK(fifo_out == fifo_heard);
    XSFLIB_CHECK(std::any_of(fifo_heard.begin(), fifo_heard.end(), [](int16_t sample) { return sample != 0; }));
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

int RunTests()
{
    TestEnhancedGain();
    TestEnhancedDoesNotWrap();
    TestEnhancedRingAndFifoGains();
    TestEnhancedRamp();
    TestEnhancedRate();
    TestEnhancedEcho();
    TestEchoLeftOut();
    TestEchoLeftOutWithoutEcho();
    TestEnhancedFramesWithoutMixing();
    TestEnhancedSnapshotsAndMutes();
    TestArchiveSource();

    return xsflib::testing::Summary("Konami's enhanced player");
}

} // namespace konami_player_tests

int main()
{
    return konami_player_tests::RunTests();
}
