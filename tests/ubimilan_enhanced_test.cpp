// SPDX-License-Identifier: MIT

// Tests of the enhanced mode of ubimilan::ArchivePlayer and of its archive source. ubimilan_player_test.cpp says how
// the tests are built.

#include "ubimilan_player_test_util.h"

namespace ubimilan_player_tests
{
namespace
{

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

int RunTests()
{
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

    return xsflib::testing::Summary("Ubisoft Milan's enhanced player");
}

} // namespace ubimilan_player_tests

int main()
{
    return ubimilan_player_tests::RunTests();
}
