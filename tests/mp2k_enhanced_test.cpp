// SPDX-License-Identifier: MIT

// Unit tests for MP2K's enhanced mixing mode: its samples, its reverb, Camelot's mixer in the mode, and the archive
// player that uses it. They build small synthetic cartridges in memory. No game ROM is needed.

#include "mp2k_test_util.h"

namespace ntdlib::sequence_export::mp2k
{
namespace
{

constexpr size_t kEnhanced = ntdlib::agb::kEnhancedFrameSamples;

// The enhanced mode's output of each frame, with the console's, and a channel's state after each frame.
struct EnhancedFrames
{
    std::vector<std::vector<float>> right;
    std::vector<std::vector<float>> left;
    MixedFrames console;
    std::vector<Channel> channels;
};

// Runs a sequencer for `frames` frames in the enhanced mode, and keeps each frame's output and channel `channel`. The
// frames in `without` run without the enhanced mode's mixing.
EnhancedFrames MixEnhancedFrames(Sequencer& seq, int frames, int channel = 0, const std::set<int>& without = {})
{
    EnhancedFrames out;
    seq.StartMixing();
    seq.StartEnhancedMixing();
    for (int f = 0; f < frames; f++)
    {
        seq.SetEnhancedFrames(!without.contains(f));
        seq.Step();
        out.right.emplace_back(seq.EnhancedRight().begin(), seq.EnhancedRight().end());
        out.left.emplace_back(seq.EnhancedLeft().begin(), seq.EnhancedLeft().end());
        out.console.right.emplace_back(seq.Right().begin(), seq.Right().end());
        out.console.left.emplace_back(seq.Left().begin(), seq.Left().end());
        out.channels.push_back(seq.GetChannel(channel));
    }

    return out;
}

// Returns whether every sample of a frame is 0.
bool Silent(const std::vector<float>& frame)
{
    return std::all_of(frame.begin(), frame.end(), [](float sample) { return sample == 0.0f; });
}

// A sine at a quarter of a sample's rate: 0, 100, 0 and -100, over and over.
std::vector<int8_t> QuarterSine(size_t length)
{
    std::vector<int8_t> pcm(length);
    const int8_t kCycle[4] = {0, 100, 0, -100};
    for (size_t i = 0; i < length; i++)
    {
        pcm[i] = kCycle[i % 4];
    }

    return pcm;
}

// A triangle wave of 128 points between -64 and 63, from `start` points into its cycle.
std::vector<int8_t> Triangle(size_t length, size_t start = 0)
{
    std::vector<int8_t> pcm(length);
    for (size_t i = 0; i < length; i++)
    {
        const int at = int((i + start) % 128);
        pcm[i] = int8_t(at < 64 ? at - 32 : 95 - at);
    }

    return pcm;
}

void TestEnhancedSamples()
{
    // A looped sine at a quarter of its rate plays at a fixed pitch, and then a fifth up. Each enhanced sample lies
    // between the console's points, at its share of the frame's move: the sine at its position, within the
    // interpolator's ripple of 0.01 dB. Each side's volume ramps across the frame from the last frame's. A volume
    // change and the release's frames ramp. Once the channel stops, the output is silent. The mono mixer plays both
    // sides at the average of the volumes.
    for (bool mono : {false, true})
    {
        Cart cart;
        if (mono)
        {
            const uint8_t kMono[] = {0xA0, 0x78, 0xE1, 0x78, 0x40, 0x18, 0x68, 0x43, 0x40, 0x0A, 0xA0, 0x72};
            for (uint32_t i = 0; i < 12; i++)
            {
                cart.Put8(kMonoVolume + i, kMono[i]);
            }
        }
        const uint32_t sine = cart.Wave(QuarterSine(64), 13379, 0);
        const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(kFixed, sine, 60, {255, 0, 255, 200})},
                                                 {1, Cart::Voice(0, sine, 60, {255, 0, 255, 200})}});
        for (int key : {60, 67})
        {
            cart.Song({Track()
                           .Cmd(kCmdTempo, 75)
                           .Cmd(kCmdVoice, key == 60 ? 0 : 1)
                           .Cmd(kCmdVol, 127)
                           .Cmd(kCmdPan, 0x30)
                           .Wait(2)
                           .Note(24, key, 127)
                           .Wait(12)
                           .Cmd(kCmdVol, 50)
                           .Wait(60)
                           .Fine()},
                      voices);
        }
        const Rom rom = cart.ToRom();
        const DriverInfo info = Detect(rom);
        SEQLIB_REQUIRE(info.mono == mono);

        for (int song = 0; song < 2; song++)
        {
            Sequencer seq(rom, info, song);
            const EnhancedFrames e = MixEnhancedFrames(seq, 60);
            int first = -1;
            for (int f = 0; f < int(e.channels.size()) && first < 0; f++)
            {
                first = e.channels[size_t(f)].status ? f : -1;
            }
            SEQLIB_REQUIRE(first >= 0);
            SEQLIB_CHECK(Silent(e.right[size_t(first) - 1]) && Silent(e.left[size_t(first) - 1]));

            const auto gains = [&](const Channel& c)
            {
                const float scale = float(c.envelope * (info.master_volume + 1)) / 1048576.0f;
                if (mono)
                {
                    return std::array<float, 2>{float(c.right + c.left) * scale / 2.0f,
                                                float(c.right + c.left) * scale / 2.0f};
                }
                return std::array<float, 2>{float(c.right) * scale, float(c.left) * scale};
            };
            const uint32_t step = song == 0 ? 0x800000u : e.channels[size_t(first)].frequency * info.step_scale;
            double worst = 0.0;
            int compared = 0;
            int ramps = 0;
            int stopped = -1;
            for (int f = first + 1; f < int(e.channels.size()); f++)
            {
                const Channel& c = e.channels[size_t(f)];
                if (!c.status)
                {
                    stopped = f;
                    break;
                }
                const std::array<float, 2> to = gains(c);
                const std::array<float, 2> from = gains(e.channels[size_t(f) - 1]);
                ramps += from != to;
                for (size_t j = 0; j < kEnhanced; j++)
                {
                    const double position =
                        (double(f - first) * 224.0 * step + double(j) * 224.0 * step / double(kEnhanced)) / 8388608.0;
                    const double point = 100.0 * std::sin(2.0 * 3.141592653589793 * position / 4.0);
                    for (size_t side = 0; side < 2; side++)
                    {
                        const double gain = from[side] + double(j) * (to[side] - from[side]) / double(kEnhanced);
                        const float out = side == 0 ? e.right[size_t(f)][j] : e.left[size_t(f)][j];
                        worst =
                            std::max(worst, std::fabs(out - point * gain) / (100.0 * std::max(from[side], to[side])));
                    }
                    compared++;
                }
            }
            SEQLIB_CHECK(worst < 0.002);
            SEQLIB_CHECK(compared > 10 * int(kEnhanced) && ramps > 5);
            SEQLIB_CHECK(mono ? e.right == e.left : e.right[size_t(first) + 1] != e.left[size_t(first) + 1]);
            SEQLIB_REQUIRE(stopped > 0);
            for (int f = stopped; f < int(e.right.size()); f++)
            {
                SEQLIB_CHECK(Silent(e.right[size_t(f)]) && Silent(e.left[size_t(f)]));
            }
        }
    }
}

void TestEnhancedSpecialSamples()
{
    // A compressed triangle wave and a reversed one play a fifth up. Each enhanced sample lies within 2 points of the
    // console's output at its time, interpolated: the console rounds its points down, and interpolates in straight
    // lines.
    Cart cart;
    std::vector<uint8_t> blocks;
    for (int b = 0; b < 16; b++)
    {
        // Each block rises by 1 or falls by 1 for its 64 points.
        const bool rising = b % 2 == 0;
        blocks.push_back(uint8_t(rising ? -32 : 31));
        blocks.push_back(rising ? 0x01 : 0x0F);
        blocks.insert(blocks.end(), 31, uint8_t(rising ? 0x11 : 0xFF));
    }
    const std::vector<int8_t> points = Decompress(blocks);
    SEQLIB_REQUIRE(points.size() == 1024 && points[63] == 31 && points[64] == 31 && points[127] == -32);
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(kVoiceCompressed, cart.CompressedWave(blocks, 1024))},
                                             {1, Cart::Voice(kVoiceReverse, cart.Wave(Triangle(1200, 20)))},
                                             {2, Cart::Voice(0, cart.Wave(Triangle(1200, 20)))}});
    for (int voice = 0; voice < 3; voice++)
    {
        cart.Song({Track().Cmd(kCmdVoice, voice).Cmd(kCmdVol, 127).Note(24, 67, 127).Wait(24).Fine()}, voices);
    }
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    SEQLIB_REQUIRE(info.special_samples);

    std::array<std::vector<float>, 3> outputs;
    for (int song = 0; song < 3; song++)
    {
        Sequencer seq(rom, info, song);
        const EnhancedFrames e = MixEnhancedFrames(seq, 4);
        const int f = FirstSound(e.console);
        SEQLIB_REQUIRE(f >= 0);
        double worst = 0.0;
        for (int frame = f + 1; frame < f + 3; frame++)
        {
            const std::vector<int8_t>& console = e.console.right[size_t(frame)];
            for (size_t j = 0; j < kEnhanced; j++)
            {
                const double at = double(j) * 224.0 / double(kEnhanced);
                const size_t k = size_t(at);
                if (k + 1 >= console.size())
                {
                    continue;
                }
                const double expected = console[k] + (at - double(k)) * (console[k + 1] - console[k]);
                worst = std::max(worst, std::fabs(e.right[size_t(frame)][j] - expected));
            }
        }
        SEQLIB_CHECK(worst < 2.0);
        outputs[size_t(song)] = e.right[size_t(f) + 1];
    }

    // The reversed triangle plays otherwise than the forward one.
    double apart = 0.0;
    for (size_t j = 0; j < kEnhanced; j++)
    {
        apart = std::max(apart, double(std::fabs(outputs[1][j] - outputs[2][j])));
    }
    SEQLIB_CHECK(apart > 5.0);
}

// Returns a value of the enhanced mode's history: 0 below 2^-16.
float Kept(float value)
{
    return std::fabs(value) < 1.0f / 65536.0f ? 0.0f : value;
}

void TestEnhancedReverb()
{
    // A jumpy sample of 224 points plays for a frame at a fixed pitch, on the left, with a reverb of 100. Each later
    // frame holds the reverb alone: both sides of the frames a buffer's length of frames before and one frame later,
    // times the reverb over 512, on both sides. The mono mixer adds its one side, times the reverb over 256.
    for (bool mono : {false, true})
    {
        Cart cart;
        if (mono)
        {
            const uint8_t kMono[] = {0xA0, 0x78, 0xE1, 0x78, 0x40, 0x18, 0x68, 0x43, 0x40, 0x0A, 0xA0, 0x72};
            for (uint32_t i = 0; i < 12; i++)
            {
                cart.Put8(kMonoVolume + i, kMono[i]);
            }
        }
        const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(kFixed, cart.Wave(Jumpy(224)))}});
        cart.Song({Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Cmd(kCmdPan, 0x10).Note(24, 60, 127).Wait(48).Fine()},
                  voices, 0, 0, 0x80 | 100);
        const Rom rom = cart.ToRom();
        const DriverInfo info = Detect(rom);
        SEQLIB_REQUIRE(info.mono == mono);

        Sequencer seq(rom, info, 0);
        const EnhancedFrames e = MixEnhancedFrames(seq, 50);
        const int first = FirstSound(e.console);
        SEQLIB_REQUIRE(first >= 0 && !e.channels[size_t(first)].status);
        const int period = int(ntdlib::mp2k::kPcmBufferSize) / info.samples_per_frame;
        const auto at = [&](const std::vector<std::vector<float>>& side, int f, size_t j)
        {
            return f < 0 ? 0.0f : Kept(side[size_t(f)][j]);
        };

        int differences = 0;
        bool echoes = false;
        for (int f = first + 1; f < int(e.right.size()); f++)
        {
            for (size_t j = 0; j < kEnhanced; j++)
            {
                const float expected =
                    mono ? (at(e.right, f - period, j) + at(e.right, f - period + 1, j)) * (100.0f / 256.0f)
                         : ((at(e.right, f - period, j) + at(e.left, f - period, j)) +
                            (at(e.right, f - period + 1, j) + at(e.left, f - period + 1, j))) *
                               (100.0f / 512.0f);
                differences += e.right[size_t(f)][j] != expected;
                differences += e.left[size_t(f)][j] != expected;
                echoes |= f > first + 2 * period && expected != 0.0f;
            }
        }
        SEQLIB_CHECK_EQ(differences, 0);
        SEQLIB_CHECK(echoes);
        SEQLIB_CHECK(mono ? e.right == e.left : e.right[size_t(first)] != e.left[size_t(first)]);
    }
}

// Returns the root mean square of the enhanced mode's right side, and of the console's, over frames `first` to `last`.
std::pair<double, double> Levels(const EnhancedFrames& e, int first, int last)
{
    double enhanced = 0.0;
    double console = 0.0;
    size_t enhanced_count = 0;
    size_t console_count = 0;
    for (int f = first; f <= last; f++)
    {
        for (float sample : e.right[size_t(f)])
        {
            enhanced += double(sample) * sample;
            enhanced_count++;
        }
        for (int8_t point : e.console.right[size_t(f)])
        {
            console += double(point) * point;
            console_count++;
        }
    }

    return {std::sqrt(enhanced / double(enhanced_count)), std::sqrt(console / double(console_count))};
}

// Returns the first DirectSound channel that sounds in song `song`'s first 4 frames, or -1. Camelot's driver starts a
// note on the last channel that its search finds.
int SoundingChannel(const Rom& rom, const DriverInfo& info, int song)
{
    Sequencer seq(rom, info, song);
    for (int f = 0; f < 4; f++)
    {
        seq.Step();
        for (int n = 0; n < info.max_channels; n++)
        {
            if (seq.GetChannel(n).status)
            {
                return n;
            }
        }
    }

    return -1;
}

// Counts the samples of the frames from `first` on that differ from Camelot's echo of the frames before them, and
// sets `echoes` when a frame 5 or more after `first` holds an echo. The frames in `without` are left out. The echo
// comes from the frames before: 4 frames back on each side, less a share of the other side, a quarter of the right
// side's last frame, and for the left side a quarter of the right side's from a frame and a third back: 268 samples.
int EchoDifferences(const EnhancedFrames& e, int first, const std::set<int>& without, bool& echoes)
{
    int differences = 0;
    for (int f = first; f < int(e.right.size()); f++)
    {
        if (without.contains(f))
        {
            continue;
        }
        const std::vector<float>& before = e.right[size_t(f) - 1];
        for (size_t j = 0; j < kEnhanced; j++)
        {
            const float old_right = Kept(e.right[size_t(f) - 4][j]);
            const float old_left = Kept(e.left[size_t(f) - 4][j]);
            const float delayed = j >= 268 ? before[j - 268] : Kept(e.right[size_t(f) - 2][j + 536]);
            const float expected_right =
                Kept((53.0f * old_right - 8.0f * old_left) / 128.0f + before[j] * (1.0f / 4.0f + 1.0f / 512.0f));
            const float expected_left = Kept((53.0f * old_left - 8.0f * old_right) / 128.0f + delayed * (1.0f / 4.0f));
            differences += e.right[size_t(f)][j] != expected_right;
            differences += e.left[size_t(f)][j] != expected_left;
            echoes |= f >= first + 5 && expected_left != 0.0f;
        }
    }

    return differences;
}

// Songs of Camelot's mixer. Song 0 plays a sample of 40 at a fixed pitch for 6 frames. Songs 1 to 3 hold a pulse, a
// saw and a triangle wave.
struct CamelotSongs
{
    Rom rom;
    DriverInfo info;
};

CamelotSongs MakeCamelotSongs()
{
    Cart cart(kCamelotMode, {10, 3}, true, false);
    cart.WriteCamelot();
    const uint32_t forty = cart.Wave(std::vector<int8_t>(64, 40), 31536, 0);
    const uint32_t pulse = cart.Synth(0, {0x80, 0x10, 0x40, 0x90});
    const uint32_t saw = cart.Synth(1, {});
    const uint32_t triangle = cart.Synth(2, {});
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(kFixed, forty)},
                                             {1, Cart::Voice(0, pulse)},
                                             {2, Cart::Voice(0, saw)},
                                             {3, Cart::Voice(0, triangle)}});
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(6, 60, 127).Wait(48).Fine()},
              voices);
    for (int voice = 1; voice < 4; voice++)
    {
        cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, voice).Cmd(kCmdVol, 50).Note(24, 48, 127).Wait(48).Fine()},
                  voices);
    }
    CamelotSongs songs{cart.ToRom(), {}};
    songs.info = Detect(songs.rom);

    return songs;
}

// The pulse, saw and triangle waves of TestEnhancedCamelot: songs 1 to 3 play at the console's level, echo included,
// and the triangle wave at its points, across the frames' joins.
void CheckCamelotWaves(const Rom& rom, const DriverInfo& info)
{
    for (int song = 1; song < 4; song++)
    {
        const int n = SoundingChannel(rom, info, song);
        SEQLIB_REQUIRE(n >= 0);
        Sequencer synth(rom, info, song);
        const EnhancedFrames e = MixEnhancedFrames(synth, 20, n);
        const int f = FirstSound(e.console);
        SEQLIB_REQUIRE(f >= 0);

        // The console rounds each side's volume down, and halves it again for the saw. The enhanced output, scaled to
        // the console's volume, plays at the console's level.
        const Channel& c = e.channels[size_t(f) + 2];
        const int32_t rounded = ((c.right * (c.envelope + (c.envelope >> 3))) >> 9) >> (song == 2 ? 1 : 0);
        const double exact = c.right * c.envelope * 9.0 / 8.0 / 512.0 / (song == 2 ? 2.0 : 1.0);
        const double scale = rounded / exact;
        const auto [enhanced, console] = Levels(e, f + 2, f + 12);
        SEQLIB_CHECK(console > 5.0 && std::fabs(20.0 * std::log10(enhanced * scale / console)) < 0.15);

        // The triangle wave lies within 2 points of the console's points, at each frame's start and end too. The
        // console's sums and echo round towards 0, and the echo adds up their errors over later frames.
        if (song == 3)
        {
            double worst = 0.0;
            for (int frame = f + 1; frame < f + 5; frame++)
            {
                const std::vector<int8_t>& points = e.console.right[size_t(frame)];
                for (size_t j = 0; j < kEnhanced; j++)
                {
                    const double at = double(j) * double(points.size()) / double(kEnhanced);
                    const size_t k = size_t(at);
                    if (k + 1 >= points.size())
                    {
                        continue;
                    }
                    const double expected = points[k] + (at - double(k)) * (points[k + 1] - points[k]);
                    worst = std::max(worst, std::fabs(e.right[size_t(frame)][j] * scale - expected));
                }
            }
            SEQLIB_CHECK(worst < 2.0);
        }
    }
}

void TestEnhancedCamelot()
{
    // Camelot's mixer plays a sample of 40 at a fixed pitch, at the centre, for 6 frames. In its first frame, the
    // output is the sample at each side's volume: 9/8 of the envelope over 512, and the output stage's 128 to a point.
    // Once the channel stops, each frame holds the echo of the frames before it. The pulse, saw and triangle waves play
    // at the console's level, echo included, and the triangle wave at its points, across the frames' joins.
    const CamelotSongs songs = MakeCamelotSongs();
    const Rom& rom = songs.rom;
    const DriverInfo& info = songs.info;
    SEQLIB_REQUIRE(info.camelot_mixer);

    const int channel = SoundingChannel(rom, info, 0);
    SEQLIB_REQUIRE(channel >= 0);
    Sequencer seq(rom, info, 0);
    const EnhancedFrames e = MixEnhancedFrames(seq, 40, channel);
    int first = -1;
    int last = -1;
    for (int f = 0; f < int(e.channels.size()); f++)
    {
        if (e.channels[size_t(f)].status)
        {
            first = first < 0 ? f : first;
            last = f;
        }
    }
    SEQLIB_REQUIRE(first >= 0 && last > first && last + 10 < int(e.channels.size()));

    const Channel& c = e.channels[size_t(first)];
    const float scale = float(c.envelope) * (9.0f / 8.0f) / (512.0f * 128.0f);
    const float right = 40.0f * float(c.right) * scale;
    const float left = 40.0f * float(c.left) * scale;
    double worst = 0.0;
    for (size_t j = 200; j < kEnhanced; j++)
    {
        worst = std::max(worst, double(std::fabs(e.right[size_t(first)][j] - right)));
        worst = std::max(worst, double(std::fabs(e.left[size_t(first)][j] - left)));
    }
    SEQLIB_CHECK(worst < 1e-4 && right > 1.0f && left > 1.0f);

    bool echoes = false;
    SEQLIB_CHECK_EQ(EchoDifferences(e, last + 1, {}, echoes), 0);
    SEQLIB_CHECK(echoes);

    CheckCamelotWaves(rom, info);
}

void TestEnhancedFramesWithout()
{
    // Frames without the enhanced mode's mixing are silent, and move the channel on. The frames after them give the
    // samples of a run that mixed every frame.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Triangle(4000)), 60, {255, 0, 255, 200})}});
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(24, 67, 127).Wait(48).Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer all(rom, info, 0);
    const EnhancedFrames a = MixEnhancedFrames(all, 30);
    Sequencer some(rom, info, 0);
    const EnhancedFrames b = MixEnhancedFrames(some, 30, 0, {3, 4, 5, 6});
    for (int f = 3; f < 7; f++)
    {
        SEQLIB_CHECK(Silent(b.right[size_t(f)]) && Silent(b.left[size_t(f)]));
        SEQLIB_CHECK(!Silent(a.right[size_t(f)]));
    }
    for (int f = 7; f < 30; f++)
    {
        SEQLIB_CHECK(a.right[size_t(f)] == b.right[size_t(f)] && a.left[size_t(f)] == b.left[size_t(f)]);
    }
    SEQLIB_CHECK(a.console.right == b.console.right && a.console.left == b.console.left);

    // After Camelot's sample, two frames without the mixing. The echo takes them as silence.
    const CamelotSongs camelot = MakeCamelotSongs();
    const int channel = SoundingChannel(camelot.rom, camelot.info, 0);
    SEQLIB_REQUIRE(channel >= 0);
    Sequencer gap(camelot.rom, camelot.info, 0);
    const EnhancedFrames g = MixEnhancedFrames(gap, 30, channel, {12, 13});
    SEQLIB_REQUIRE(!g.channels[10].status);
    bool echoes = false;
    SEQLIB_CHECK_EQ(EchoDifferences(g, 11, {12, 13}, echoes), 0);
    SEQLIB_CHECK(echoes && Silent(g.right[12]) && Silent(g.left[13]) && !Silent(g.right[14]));
}

// Returns the time in seconds of the first sample of `out`'s side `side` (0 for the left) whose magnitude reaches
// `level`, or -1.
template <typename Sample>
double FirstAt(const std::vector<Sample>& out, size_t side, double level, double rate)
{
    for (size_t i = side; i < out.size(); i += 2)
    {
        if (std::fabs(double(out[i])) >= level)
        {
            return double(i / 2) / rate;
        }
    }

    return -1.0;
}

// In the enhanced mode, two square 1 notes of two tracks play together: the second on a grown channel in PSG unit 1.
// Each plays as it plays alone, and the output is the sum. A mute of the second track gives the first alone. The
// second track's meter takes the grown channel's peaks.
void TestEnhancedPlayerGrows()
{
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{1, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}});
    const auto track = [](int key, int pan)
    {
        return Track()
            .Cmd(kCmdTempo, 75)
            .Cmd(kCmdVoice, 1)
            .Cmd(kCmdVol, 100)
            .Cmd(kCmdPan, pan)
            .Wait(3)
            .Note(48, key, 127);
    };
    cart.Song({track(60, 0x40).Wait(60).Fine(), track(67, 0x40).Wait(60).Fine()}, voices);
    cart.Song({track(60, 0x40).Wait(60).Fine()}, voices);
    cart.Song({Track().Cmd(kCmdTempo, 75).Wait(60).Fine(), track(67, 0x40).Wait(60).Fine()}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ArchivePlayerOptions options;
    options.enhanced = true;
    const auto play = [&](int song, bool mute, std::array<int32_t, 2>* meter)
    {
        ArchivePlayer player(rom, info);
        player.Start(song, options);
        player.MuteTrack(1, mute);
        player.MeterTracks(meter != nullptr);
        std::vector<float> out;
        for (int f = 0; f < 30; f++)
        {
            player.RunFrame(out);
            if (meter)
            {
                for (size_t side = 0; side < 2; side++)
                {
                    (*meter)[side] = std::max((*meter)[side], player.TrackPeaks(1)[side]);
                }
            }
        }
        return out;
    };
    std::array<int32_t, 2> meter = {};
    const std::vector<float> both = play(0, false, &meter);
    const std::vector<float> first = play(1, false, nullptr);
    const std::vector<float> second = play(2, false, nullptr);
    const std::vector<float> muted = play(0, true, nullptr);
    SEQLIB_REQUIRE(both.size() == first.size() && both.size() == second.size() && both.size() == muted.size());
    double worst = 0.0;
    double worst_muted = 0.0;
    float loudest = 0.0f;
    for (size_t i = 0; i < both.size(); i++)
    {
        worst = std::max(worst, std::fabs(double(both[i]) - double(first[i]) - double(second[i])));
        worst_muted = std::max(worst_muted, std::fabs(double(muted[i]) - double(first[i])));
        loudest = std::max(loudest, std::fabs(second[i]));
    }
    SEQLIB_CHECK(loudest > 0.05f);
    SEQLIB_CHECK(worst < 1e-6);
    SEQLIB_CHECK(worst_muted < 1e-6);
    SEQLIB_CHECK(meter[0] > 0 && meter[1] > 0);
}

void TestEnhancedPlayer()
{
    // The player in the enhanced mode: about 48,022 Hz, 804 samples a frame on each side, and the console's timing
    // 32 samples later. A sample's onset reaches half its level half a point before the console's output steps. A
    // square's first edge reaches half its level at the console's step.
    Cart cart;
    const uint32_t voices =
        cart.VoiceGroup({{0, Cart::Voice(kFixed, cart.Wave(std::vector<int8_t>(64, 100), 13379, 0))},
                         {1, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}});
    cart.Song(
        {Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Wait(3).Note(24, 60, 127).Wait(48).Fine()},
        voices);
    cart.Song(
        {Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 1).Cmd(kCmdVol, 100).Wait(3).Note(24, 60, 127).Wait(48).Fine()},
        voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const double enhanced_rate = 16777216.0 * double(kEnhanced) / double(kFrameCycles);
    const double delay = double(ntdlib::wave::BandLimitedSteps::kHalfWidth) / enhanced_rate;

    for (int song = 0; song < 2; song++)
    {
        ArchivePlayer console(rom, info);
        console.Start(song);
        const std::vector<int16_t> c = Play(console, 12);
        ArchivePlayer player(rom, info);
        ArchivePlayerOptions options;
        options.enhanced = true;
        player.Start(song, options);
        SEQLIB_CHECK(player.Enhanced() && player.Rate() == ntdlib::agb::kEnhancedRate);
        std::vector<float> e;
        bool sizes = true;
        for (int f = 0; f < 12; f++)
        {
            const size_t before = e.size();
            player.RunFrame(e);
            sizes &= e.size() - before == 2 * kEnhanced;
        }
        SEQLIB_CHECK(sizes);

        // The console's output steps to its level. The enhanced output crosses half of it.
        const int32_t peak = Peak(c, 1);
        SEQLIB_REQUIRE(peak > 0);
        const double console_onset = FirstAt(c, 1, 1.0, double(console.Rate()));
        const double enhanced_onset = FirstAt(e, 1, 0.5 * peak / 32768.0, enhanced_rate);
        SEQLIB_REQUIRE(console_onset > 0.0 && enhanced_onset > 0.0);
        const double offset = enhanced_onset - delay - console_onset - (song == 0 ? -0.5 / 13379.0 : 0.0);
        SEQLIB_CHECK(std::fabs(offset) < 1.5 / enhanced_rate);
        SEQLIB_CHECK(Throws<std::logic_error>(
            [&]
            {
                std::vector<int16_t> out;
                player.RunFrame(out);
            }));
        SEQLIB_CHECK(Throws<std::logic_error>(
            [&]
            {
                std::vector<float> out;
                console.RunFrame(out);
            }));
    }

    // Busy() follows the console's mixer, and a reverb keeps it busy as long.
    const TwoTracks wet = TwoTrackSongs(0x80 | 60);
    ArchivePlayerOptions options;
    options.enhanced = true;
    ArchivePlayer enhanced(wet.rom, wet.info);
    enhanced.Start(0, options);
    ArchivePlayer console(wet.rom, wet.info);
    console.Start(0);
    bool busy = true;
    for (int f = 0; f < 200; f++)
    {
        std::vector<float> frame;
        enhanced.RunFrame(frame);
        console.RunFrame();
        busy &= enhanced.Busy() == console.Busy();
    }
    SEQLIB_CHECK(busy && !enhanced.Busy());

    // A snapshot gives the frames after it again. Two frames after frames without samples, the output is the
    // reference's: the FIFOs' silence of those frames has played, and the squares' steps have settled.
    const TwoTracks dry = TwoTrackSongs();
    ArchivePlayer reference(dry.rom, dry.info);
    reference.Start(0, options);
    std::vector<float> all;
    for (int f = 0; f < 110; f++)
    {
        reference.RunFrame(all);
    }
    ArchivePlayer player(dry.rom, dry.info);
    player.Start(0, options);
    std::vector<float> first;
    for (int f = 0; f < 30; f++)
    {
        player.RunFrame(first);
    }
    for (int f = 0; f < 20; f++)
    {
        player.RunFrame();
    }
    const ArchivePlayer::Snapshot snapshot = player.Save();
    std::vector<float> after;
    for (int f = 0; f < 60; f++)
    {
        player.RunFrame(after);
    }
    player.Restore(snapshot);
    SEQLIB_CHECK(player.Enhanced());
    std::vector<float> again;
    for (int f = 0; f < 60; f++)
    {
        player.RunFrame(again);
    }
    SEQLIB_CHECK(after == again && after.size() == 120 * kEnhanced);
    SEQLIB_CHECK(std::equal(first.begin(), first.end(), all.begin()));
    SEQLIB_CHECK(std::equal(after.begin() + 2 * 2 * kEnhanced, after.end(), all.begin() + 52 * 2 * kEnhanced));
    SEQLIB_CHECK(!std::equal(after.begin(), after.begin() + 2 * kEnhanced, all.begin() + 50 * 2 * kEnhanced));

    // A frame without samples leaves its DirectSound samples silent. At a frame's start, the FIFOs hold 118 samples
    // of the frame before: the band-limited steps' delay of 32, and the 24 points ahead of the timer, 86 samples. The
    // first frame after the frame without samples sounds for those 118, and the second is silent for them.
    ArchivePlayer saw(dry.rom, dry.info);
    saw.Start(2, options);
    std::vector<float> sounding;
    for (int f = 0; f < 10; f++)
    {
        saw.RunFrame(sounding);
    }
    saw.RunFrame();
    std::vector<float> next;
    saw.RunFrame(next);
    saw.RunFrame(next);
    SEQLIB_CHECK(!Silent(std::vector<float>(sounding.end() - 2 * kEnhanced, sounding.end())));
    const auto part = [&](size_t from, size_t to)
    {
        return std::vector<float>(next.begin() + 2 * from, next.begin() + 2 * to);
    };
    SEQLIB_CHECK(!Silent(part(0, 118)) && Silent(part(118, kEnhanced)));
    SEQLIB_CHECK(Silent(part(kEnhanced, kEnhanced + 118)) && !Silent(part(kEnhanced + 118, 2 * kEnhanced)));
}

int RunTests()
{
    TestEnhancedSamples();
    TestEnhancedSpecialSamples();
    TestEnhancedReverb();
    TestEnhancedCamelot();
    TestEnhancedFramesWithout();
    TestEnhancedPlayer();
    TestEnhancedPlayerGrows();

    return seqlib::testing::Summary("mp2k_enhanced");
}

} // namespace
} // namespace ntdlib::sequence_export::mp2k

int main()
{
    return ntdlib::sequence_export::mp2k::RunTests();
}
