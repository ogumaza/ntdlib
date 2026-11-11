// SPDX-License-Identifier: MIT

// Tests of the enhanced mode of rd2::ArchivePlayer (ntdlib/rd2/output.h) on small synthetic cartridges. No game ROM is
// needed. Each expected value follows a rule of the enhanced mode, and the comments give the rule.

#include "rd2_player_test_util.h"

namespace
{

constexpr size_t kEnhanced = kEnhancedFrameSamples;

// In the enhanced mode, the silence before the first mix's samples: the same time at 804 samples a frame, and the half
// width of the PSG's band-limited steps. The hardware model holds the FIFOs back by that half width, to keep them in
// time with the steps.
constexpr size_t kEnhancedStart = size_t((uint64_t(kFirstByteCycle) * kEnhanced + kFrameCycles / 2) / kFrameCycles) +
                                  size_t(ntdlib::wave::BandLimitedSteps::kHalfWidth);

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

// The enhanced mode's level of a voice on the left and the right: a value of the sample on the scale of 8-bit samples.
double EnhancedLeft(const VoiceMix& mix)
{
    return double(std::max(127 - int(mix.pan), 0)) * double(mix.volume & 0xFFFF) / 32768.0;
}

double EnhancedRight(const VoiceMix& mix)
{
    return double(mix.pan) * double(mix.volume & 0xFFFF) / 32768.0;
}

void TestEnhancedGain()
{
    // 804 samples a frame. A point's value comes through the voice's levels: the pan's part times the volume over
    // 32768, on the scale of 8-bit samples. Full scale lies at 128. The sound starts after the same time as on the
    // console.
    Cart cart;
    FillFirst(cart, 64);
    cart.Sequences({{HeldTrack(0x10)}});
    const Game game(cart);
    ArchivePlayer player(game.rom, game.info);
    XSFLIB_CHECK(Throws<std::logic_error>(
        [&]
        {
            std::vector<float> floats;
            player.RunFrame(floats);
        }));
    player.Start(0, EnhancedOptions());
    XSFLIB_CHECK(player.Enhanced() && player.Rate() == kEnhancedRate);
    std::vector<int16_t> strict;
    XSFLIB_CHECK(Throws<std::logic_error>([&] { player.RunFrame(strict); }));
    const std::vector<float> out = EnhancedFrames(player, 4);
    XSFLIB_CHECK(out.size() == 2 * 4 * kEnhanced);

    const VoiceMix mix = player.Driver().Song()->Mixes().at(0);
    const double left = 64.0 * EnhancedLeft(mix) / 128.0;
    const double right = 64.0 * EnhancedRight(mix) / 128.0;
    XSFLIB_CHECK(left > 0.05 && right > 0.0 && left != right);
    for (size_t i = 0; i < 2 * (kEnhancedStart - 64); i++)
    {
        XSFLIB_CHECK(std::fabs(out[i]) < 0.03f);
    }
    XSFLIB_CHECK(Near(out[2 * (kEnhancedStart + 64)], left));
    XSFLIB_CHECK(Near(out[2 * (kEnhancedStart + 64) + 1], right));
    for (size_t i = 2 * (kEnhancedStart + 64); i < out.size(); i += 2)
    {
        XSFLIB_CHECK(Near(out[i], left) && Near(out[i + 1], right));
    }

    // The console's mode refuses the enhanced mode's frames.
    ArchivePlayer console(game.rom, game.info);
    console.Start(0);
    std::vector<float> floats;
    XSFLIB_CHECK(Throws<std::logic_error>([&] { console.RunFrame(floats); }));
}

void TestEnhancedDoesNotWrap()
{
    // On the console, the sums of the seven voices at these levels wrap or saturate. The mode's sums go on past full
    // scale.
    Cart cart;
    FillFirst(cart, 127);
    std::vector<std::vector<uint8_t>> tracks;
    for (int t = 0; t < ntdlib::rd2::kSampleVoices; t++)
    {
        tracks.push_back(TrackData().Bytes({0xE0, 0xFF, 0xC3, 0, 0xC2, kHeld}).Note(60, 100, 127).Wait(100).End());
    }
    cart.Sequences({tracks});
    const Game game(cart);
    ArchivePlayer player(game.rom, game.info);
    player.Start(0, EnhancedOptions());
    const std::vector<float> out = EnhancedFrames(player, 4);
    const std::vector<VoiceMix>& mixes = player.Driver().Song()->Mixes();
    XSFLIB_CHECK(mixes.size() == size_t(ntdlib::rd2::kSampleVoices));
    double sum = 0.0;
    for (const VoiceMix& mix : mixes)
    {
        sum += 127.0 * EnhancedLeft(mix) / 128.0;
    }

    XSFLIB_CHECK(sum > 1.5);
    XSFLIB_CHECK(Near(out[out.size() - 2], sum, 0.01));
}

void TestEnhancedNoteStart()
{
    // A note that takes a voice back plays from the sample's start. Each of the 9 notes plays 2 frames of a sine, and
    // its release falls silent before the next. The 7 voices have all played by the eighth note. The ninth plays as the
    // first does.
    Cart cart;
    FillSine(cart);
    constexpr uint8_t kNoteFrames = 40;
    TrackData track;
    track.Bytes({0xC2, kHeld});
    for (int n = 0; n < 9; n++)
    {
        track.Note(60, 2, 127).Wait(kNoteFrames);
    }
    cart.Sequences({{track.End()}});
    const Game game(cart);
    ArchivePlayer player(game.rom, game.info);
    player.Start(0, EnhancedOptions());
    const std::vector<float> out = EnhancedFrames(player, kNoteFrames * 9);
    const size_t later = size_t(8 * kNoteFrames) * kEnhanced;
    bool same = true;
    bool sounded = false;
    for (size_t i = 2 * kEnhancedStart; i < 2 * (kEnhancedStart + 2 * kEnhanced); i++)
    {
        same = same && std::fabs(out[i + 2 * later] - out[i]) < 1e-5f;
        sounded = sounded || out[i] != 0.0f;
    }

    XSFLIB_CHECK(sounded);
    XSFLIB_CHECK(same);
}

void TestEnhancedRamp()
{
    // The track sets a new volume three frames after its note. The voice's level moves in a straight line across the
    // frame that takes the new volume. A note's first frame takes its levels at once.
    Cart cart;
    FillFirst(cart, 64);
    cart.Sequences(
        {{TrackData().Bytes({0xC3, 0, 0xC2, kHeld}).Note(60, 100, 127).Wait(3).Bytes({0xE0, 0x20}).Wait(100).End()}});
    const Game game(cart);
    ArchivePlayer player(game.rom, game.info);
    player.Start(0, EnhancedOptions());
    std::vector<float> out;
    std::vector<double> levels;
    for (int f = 0; f < 7; f++)
    {
        player.RunFrame(out);
        levels.push_back(64.0 * EnhancedLeft(player.Driver().Song()->Mixes().at(0)) / 128.0);
    }

    int ramp = 1;
    while (ramp < 7 && levels[size_t(ramp)] == levels[size_t(ramp - 1)])
    {
        ramp++;
    }
    XSFLIB_CHECK(ramp == 3 && levels[3] < levels[2] / 2.0);
    const size_t first = kEnhancedStart + size_t(ramp) * kEnhanced;
    XSFLIB_CHECK(Near(out[2 * (first - 8)], levels[2]));
    XSFLIB_CHECK(Near(out[2 * (first + kEnhanced / 4)], levels[2] + (levels[3] - levels[2]) / 4.0));
    XSFLIB_CHECK(Near(out[2 * (first + kEnhanced / 2)], levels[2] + (levels[3] - levels[2]) / 2.0));
    XSFLIB_CHECK(Near(out[2 * (first + kEnhanced + 8)], levels[3]));
}

void TestEnhancedRate()
{
    // At note 60, a point of the sample goes by for each of the console's 176 points in a frame: 176 / 48 passes of the
    // sine in the sample's loop. Each pass crosses zero twice. An octave down gives half the rate.
    const auto crossings = [](uint8_t note)
    {
        Cart cart;
        FillSine(cart);
        cart.Sequences({{TrackData().Bytes({0xC3, 0, 0xC2, kHeld}).Note(note, 1000, 127).Wait(1000).End()}});
        const Game game(cart);
        ArchivePlayer player(game.rom, game.info);
        player.Start(0, EnhancedOptions());
        const std::vector<float> out = EnhancedFrames(player, 40);
        int count = 0;
        for (size_t i = 2 * (4 * kEnhanced) + 2; i < out.size(); i += 2)
        {
            count += (out[i - 2] < 0.0f) != (out[i] < 0.0f) ? 1 : 0;
        }

        return count;
    };

    constexpr int kExpected = 36 * 176 * 2 / 48;
    XSFLIB_CHECK(std::abs(crossings(60) - kExpected) <= 2);
    XSFLIB_CHECK(std::abs(crossings(48) - kExpected / 2) <= 2);
}

void TestEnhancedEcho()
{
    // The echo's line gives a voice's samples back after 18 frames of 804 samples, at 2^-level of their level. The
    // level reaches 1 well before the note. Without a goal, the line gives nothing.
    Cart cart;
    cart.Sequences({{TrackData().Wait(20).Bytes({0xE3, 0x7F, 0xC3, 0, 0xC2, kPlain}).Note(48, 3, 127).Wait(80).End()}});
    const Game game(cart);
    ArchivePlayer player(game.rom, game.info);
    ArchivePlayerOptions options = EnhancedOptions();
    options.echo_goal = 1;
    player.Start(0, options);
    const std::vector<float> out = EnhancedFrames(player, 60);
    size_t start = 0;
    while (start < out.size() / 2 && std::fabs(out[2 * start]) < 0.05f)
    {
        start++;
    }

    const size_t delay = size_t(kEchoFrames) * kEnhanced;
    XSFLIB_CHECK(start > 20 * kEnhanced && start + delay + 400 < out.size() / 2);
    XSFLIB_CHECK(std::fabs(out[2 * (start + 16)]) > 0.1f);
    for (size_t i = start + 16; i < start + 400; i++)
    {
        XSFLIB_CHECK(Near(out[2 * (i + delay)], 0.5 * double(out[2 * i]), 0.001));
    }

    ArchivePlayer dry(game.rom, game.info);
    dry.Start(0, EnhancedOptions());
    const std::vector<float> dry_out = EnhancedFrames(dry, 60);
    for (size_t i = start + 16; i < start + 400; i++)
    {
        XSFLIB_CHECK(Near(dry_out[2 * i], double(out[2 * i]), 0.001));
        XSFLIB_CHECK(Near(dry_out[2 * (i + delay)], 0.0, 0.001));
    }
}

void TestEnhancedGrownVoices()
{
    // Eight notes at once find seven sample voices on the console. In the mode, the eighth takes a new voice. A mute
    // follows the track behind each voice, the new one included.
    Cart cart;
    FillFirst(cart, 16);
    std::vector<std::vector<uint8_t>> tracks;
    for (int t = 0; t < 8; t++)
    {
        tracks.push_back(HeldTrack(0));
    }
    cart.Sequences({tracks});
    const Game game(cart);
    ArchivePlayer player(game.rom, game.info);
    player.Start(0, EnhancedOptions());
    std::vector<float> out = EnhancedFrames(player, 4);
    const std::vector<VoiceMix>& mixes = player.Driver().Song()->Mixes();
    XSFLIB_CHECK(mixes.size() == 8);
    const double one = 16.0 * EnhancedLeft(mixes.at(0)) / 128.0;
    XSFLIB_CHECK(Near(out[out.size() - 2], 8.0 * one));

    player.MuteTrack(7, true);
    out = EnhancedFrames(player, 3);
    XSFLIB_CHECK(Near(out[out.size() - 2], 7.0 * one));
    player.SoloTrack(7, true);
    player.MuteTrack(7, false);
    out = EnhancedFrames(player, 3);
    XSFLIB_CHECK(Near(out[out.size() - 2], one));

    // The console's mode keeps to its voices.
    ArchivePlayer console(game.rom, game.info);
    console.Start(0);
    Frames(console, 2);
    XSFLIB_CHECK(console.Driver().Song()->Mixes().size() == size_t(ntdlib::rd2::kSampleVoices));
}

void TestEnhancedGrownUnits()
{
    // Two square notes at once find one square voice of each kind on the console. In the mode, the second takes a
    // channel of a new unit, at the same key. With both, the peak is twice that of either, within the band-limited
    // steps' ripple. A mute silences the unit's channel too.
    Cart cart;
    cart.Sequences({{PsgTrack(kSquare, 60, 1000), PsgTrack(kSquare, 60, 1000)}});
    const Game game(cart);

    const auto peak = [&](int muted)
    {
        ArchivePlayer player(game.rom, game.info);
        player.Start(0, EnhancedOptions());
        if (muted == 2)
        {
            player.MuteTrack(0, true);
            player.MuteTrack(1, true);
        }
        else if (muted >= 0)
        {
            player.MuteTrack(muted, true);
        }

        const std::vector<float> out = EnhancedFrames(player, 8);
        XSFLIB_CHECK(player.Driver().PsgUnits() == 2 && player.Hardware().PsgUnits() == 2);
        XSFLIB_CHECK(player.Driver().PsgChannelTrack(0, 1) >= 0 && player.Driver().PsgChannelTrack(1, 1) >= 0);
        XSFLIB_CHECK(player.Driver().PsgChannelTrack(0, 1) != player.Driver().PsgChannelTrack(1, 1));
        float largest = 0.0f;
        for (size_t i = 2 * 2 * kEnhanced; i < out.size(); i++)
        {
            largest = std::max(largest, std::fabs(out[i]));
        }

        return double(largest);
    };

    const double both = peak(-1);
    const double first = peak(0);
    const double second = peak(1);
    XSFLIB_CHECK(both > 0.01);
    XSFLIB_CHECK(first > 0.0 && std::fabs(first - second) < 0.1 * first);
    XSFLIB_CHECK(std::fabs(both - 2.0 * first) < 0.2 * first);
    XSFLIB_CHECK(peak(2) < 0.001);

    // The console's mode keeps to its one unit.
    ArchivePlayer console(game.rom, game.info);
    console.Start(0);
    Frames(console, 4);
    XSFLIB_CHECK(console.Driver().PsgUnits() == 1 && console.Hardware().PsgUnits() == 1);
}

void TestEnhancedFramesWithoutMixing()
{
    // A frame without samples queues silence. The voice's place moves on: the frames after the lead's silence equal
    // those of a run with every frame's samples.
    Cart cart;
    FillSine(cart);
    cart.Sequences({{HeldTrack(0x40)}});
    const Game game(cart);
    ArchivePlayer whole(game.rom, game.info);
    ArchivePlayer seeking(game.rom, game.info);
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
    for (size_t i = 2 * (kEnhancedStart + 64); i < a.size(); i++)
    {
        XSFLIB_CHECK(a[i] == b[i]);
    }
}

void TestEnhancedSnapshotsAndMutes()
{
    Cart cart;
    FillFirst(cart, 64);
    cart.Sequences({{HeldTrack(0), HeldTrack(127)}});
    const Game game(cart);
    ArchivePlayer player(game.rom, game.info);
    player.Start(0, EnhancedOptions());
    EnhancedFrames(player, 4);
    const ArchivePlayer::Snapshot snapshot = player.Save();
    const std::vector<float> run = EnhancedFrames(player, 4);
    player.Restore(snapshot);
    XSFLIB_CHECK(player.Enhanced());
    XSFLIB_CHECK(EnhancedFrames(player, 4) == run);
    const double left = run[run.size() - 2];
    const double right = run[run.size() - 1];
    XSFLIB_CHECK(left > 0.1 && right > 0.1);

    // A mute takes the voice's samples out, and a solo leaves only them. A meter shows the part of a track on the scale
    // of 16-bit samples, and the output's peak on the same scale.
    player.MuteTrack(1, true);
    std::vector<float> muted = EnhancedFrames(player, 3);
    XSFLIB_CHECK(Near(muted[muted.size() - 2], left) && Near(muted[muted.size() - 1], 0.0));
    player.MuteTrack(1, false);
    player.SoloTrack(1, true);
    player.MeterTracks(true);
    muted = EnhancedFrames(player, 3);
    XSFLIB_CHECK(Near(muted[muted.size() - 2], 0.0) && Near(muted[muted.size() - 1], right));
    XSFLIB_CHECK(std::fabs(player.TrackPeaks(1)[1] - right * 32768.0) < 200.0);
    XSFLIB_CHECK(player.TrackPeaks(1)[0] == 0 && player.TrackPeaks(0)[0] == 0);
    XSFLIB_CHECK(std::fabs(player.OutputPeaks()[1] - right * 32768.0) < 200.0);
}

void TestEnhancedEchoSnapshot()
{
    // A snapshot holds the echo's line.
    Cart cart;
    FillSine(cart);
    cart.Sequences({{TrackData().Bytes({0xE3, 0x7F, 0xC2, kHeld}).Note(60, 1000, 127).Wait(1000).End()}});
    const Game game(cart);
    ArchivePlayer player(game.rom, game.info);
    ArchivePlayerOptions options = EnhancedOptions();
    options.echo_goal = 1;
    player.Start(0, options);
    EnhancedFrames(player, 30);
    const ArchivePlayer::Snapshot snapshot = player.Save();
    const std::vector<float> run = EnhancedFrames(player, 30);
    player.Restore(snapshot);
    XSFLIB_CHECK(EnhancedFrames(player, 30) == run);
}

} // namespace

int main()
{
    TestEnhancedGain();
    TestEnhancedDoesNotWrap();
    TestEnhancedNoteStart();
    TestEnhancedRamp();
    TestEnhancedRate();
    TestEnhancedEcho();
    TestEnhancedGrownVoices();
    TestEnhancedGrownUnits();
    TestEnhancedFramesWithoutMixing();
    TestEnhancedSnapshotsAndMutes();
    TestEnhancedEchoSnapshot();
    return xsflib::testing::Summary("rd2_enhanced");
}
