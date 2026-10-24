// SPDX-License-Identifier: MIT

// Checks the enhanced mixing mode of the model of AX without game data, against the rules in render.h and system.h: a
// sine converted from 32 kHz by each kind of conversion, a voice's level, the ramps of the envelope and the mix, a
// voice that starts, an aux bus at 32 kHz, the meters, snapshots and more voices than the console has.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/cafe/ax/memory.h"
#include "ntdlib/cafe/ax/pb.h"
#include "ntdlib/cafe/ax/system.h"
#include "ntdlib/cafe/ax/voice.h"

using namespace xsflib::testing;

namespace
{

namespace ax = ntdlib::cafe::ax;

using ax::System;

constexpr int kFrame = ax::kSamplesPerFrame48k;
constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kSamples = 0x10000000;

// A PCM16 voice of `wave` on AX in the enhanced mode, at full volume to the TV's main bus, front left. AX has room
// for `voices` voices.
class Bench
{
public:
    explicit Bench(const std::vector<int16_t>& wave, int voices = 8)
    {
        for (const int16_t sample : wave)
        {
            const auto bits = static_cast<uint16_t>(sample);
            bytes_.push_back(static_cast<uint8_t>(bits >> 8));
            bytes_.push_back(static_cast<uint8_t>(bits & 0xFF));
        }
        memory_.Map(kSamples, bytes_);
        system_.InitEnhanced(voices);
        voice_ = Add(wave.size(), ax::kUnityVolume);
    }

    Bench(const Bench&) = delete;
    Bench& operator=(const Bench&) = delete;

    // Another voice of the same wave, at `volume` into the same channel. It doesn't start yet.
    ax::Voice* Add(std::size_t length, uint16_t volume)
    {
        ax::Voice* voice = system_.AcquireVoice(10);
        const auto end = static_cast<uint32_t>(length - 1);
        system_.SetVoiceOffsets(*voice, {ax::SampleFormat::kPcm16, 0, 0, end, 0, kSamples});
        system_.SetVoiceVe(*voice, {ax::kUnityVolume, 0});
        SetMix(*voice, 0, volume);

        return voice;
    }

    // The voice's mix into bus `bus` of the TV's front left.
    void SetMix(ax::Voice& voice, int bus, uint16_t volume)
    {
        std::vector<System::ChannelMix> mix(ax::kTvChannels);
        mix[0][static_cast<std::size_t>(bus)].volume = volume;
        system_.SetVoiceDeviceMix(voice, System::Device::kTv, 0, mix);
    }

    // Starts a voice with an SRC type (0 none, 1 linear, 4 a 4-tap filter) and a ratio.
    void Start(ax::Voice& voice, uint32_t type, float ratio)
    {
        system_.SetVoiceSrcType(voice, type);
        system_.SetVoiceSrcRatio(voice, ratio);
        system_.SetVoiceState(voice, ax::VoiceState::kRun);
    }

    // The TV's left output of the next `frames` frames, on the scale of 16-bit samples.
    std::vector<double> Left(int frames)
    {
        std::vector<double> left;
        for (int f = 0; f < frames; ++f)
        {
            system_.RenderFrame();
            const std::span<const float> out = system_.OutputFloat();
            for (std::size_t t = 0; t < out.size(); t += 2)
            {
                left.push_back(static_cast<double>(out[t]) * 32768.0);
            }
        }

        return left;
    }

    // In the order they're made: the memory maps the bytes, and the model reads the memory.
    std::vector<uint8_t> bytes_;
    ax::Memory memory_;
    System system_{memory_};
    ax::Voice* voice_ = nullptr;
};

// A sine of 1 kHz at 32 kHz, at an amplitude of 10000, for a second.
std::vector<int16_t> Sine()
{
    std::vector<int16_t> wave;
    for (int k = 0; k < 32000; ++k)
    {
        wave.push_back(static_cast<int16_t>(std::lround(10000.0 * std::sin(2.0 * kPi * k / 32.0))));
    }

    return wave;
}

double SineAt(double position)
{
    return 10000.0 * std::sin(2.0 * kPi * position / 32.0);
}

// `count` samples of 10000.
std::vector<int16_t> Constant(int count)
{
    return std::vector<int16_t>(static_cast<std::size_t>(count), 10000);
}

bool Near(double sample, double expected)
{
    return std::fabs(sample - expected) <= 0.2;
}

void TestSine()
{
    // The sine plays at a ratio of 2/3, which AX holds as 43690 65536ths. After output n, the voice has stepped
    // (n + 1) * 43690 / 65536 samples from the place before its first. The 4-tap filters' outputs lie 2 samples behind
    // the newest, and linear interpolation's 3. Without a conversion, output n lies at 2n / 3. Once the kernel has
    // passed the start, each output lies within 3 of the sine there: the interpolator's error, at least 75 dB down,
    // and the rounding of the input to whole samples.
    const std::vector<int16_t> wave = Sine();
    const std::pair<uint32_t, int> kTypes[] = {{4, 2}, {1, 3}, {0, -1}};
    for (const auto& [type, behind] : kTypes)
    {
        Bench bench(wave);
        bench.Start(*bench.voice_, type, 2.0f / 3.0f);

        const std::vector<double> left = bench.Left(20);

        const uint32_t ratio = bench.system_.Voices()[bench.voice_->index].renderer.src.ratio;
        double worst = 0.0;
        for (std::size_t n = 100; n < left.size(); ++n)
        {
            const double place = behind < 0 ? 2.0 * static_cast<double>(n) / 3.0
                                            : static_cast<double>((n + 1) * ratio) / 65536.0 - 1.0 - behind;
            worst = std::max(worst, std::fabs(left[n] - SineAt(place)));
        }
        Check(ratio == 43690 && worst <= 3.0,
              "SRC type " + std::to_string(type) + ": the worst error is " + std::to_string(worst));
    }
}

void TestLevel()
{
    // At a ratio of 1, the interpolator's rows sum to 1: a wave of 10000 plays at 10000. Output gives the samples
    // rounded.
    Bench bench(Constant(kFrame * 8));
    bench.Start(*bench.voice_, 4, 1.0f);

    const std::vector<double> left = bench.Left(3);
    const std::span<const int16_t> rounded = bench.system_.Output();

    double worst = 0.0;
    for (std::size_t n = 2 * kFrame; n < left.size(); ++n)
    {
        worst = std::max(worst, std::fabs(left[n] - 10000.0));
    }
    Check(worst <= 0.2, "the level lies " + std::to_string(worst) + " from 10000");
    XSFLIB_CHECK(rounded[0] == 10000 && rounded[2 * (kFrame - 1)] == 10000 && rounded[1] == 0);
}

void TestRamps()
{
    // A new volume ramps across the next frame from the last: sample i of 144 takes 1 - 0.5 * i / 144 on the way to
    // half. The envelope and a mix ramp the same way, and together their gains multiply.
    Bench bench(Constant(kFrame * 8));
    bench.Start(*bench.voice_, 4, 1.0f);
    bench.Left(2);
    bench.system_.SetVoiceVe(*bench.voice_, {0x4000, 0});

    const std::vector<double> envelope = bench.Left(1);
    const std::vector<double> after = bench.Left(1);
    bench.SetMix(*bench.voice_, 0, 0x4000);
    const std::vector<double> mix = bench.Left(1);
    const std::vector<double> quarter = bench.Left(1);

    XSFLIB_CHECK(Near(envelope[0], 10000.0) && Near(envelope[72], 7500.0) &&
                 Near(envelope[143], 10000.0 * (1.0 - 0.5 * 143.0 / 144.0)));
    XSFLIB_CHECK(Near(after[0], 5000.0) && Near(after[143], 5000.0));
    XSFLIB_CHECK(Near(mix[0], 5000.0) && Near(mix[72], 3750.0) && Near(quarter[0], 2500.0));
}

void TestStart()
{
    // A voice that starts takes its gains at once, without a ramp from 0. So does a voice that starts again.
    Bench bench(Constant(kFrame * 4));
    bench.system_.SetVoiceVe(*bench.voice_, {0x4000, 0});
    bench.Start(*bench.voice_, 4, 1.0f);

    const std::vector<double> first = bench.Left(5);
    bench.system_.SetVoiceCurrentOffset(*bench.voice_, 0);
    bench.system_.SetVoiceState(*bench.voice_, ax::VoiceState::kRun);
    const std::vector<double> again = bench.Left(1);

    XSFLIB_CHECK(Near(first[100], 5000.0) && Near(first[400], 5000.0));
    XSFLIB_CHECK(std::all_of(first.begin() + 4 * kFrame, first.end(), [](double s) { return s == 0.0; }));
    XSFLIB_CHECK(Near(again[100], 5000.0));
}

void TestAux()
{
    // The sine goes to aux bus A alone. The callback takes 96 samples at 32 kHz and gives them back as they are. Two
    // frames after the voice, the return reaches the main bus: output n holds the sine at the place of output
    // n - 288, within 3, after two passes through the interpolator. The first frame is silent. Only the interpolators'
    // ringing ahead of the sine comes before the return. The return's peak lies below the sine's, where the outputs
    // miss its crest.
    Bench bench(Sine());
    bench.SetMix(*bench.voice_, 1, ax::kUnityVolume);
    std::vector<uint32_t> counts;
    const auto callback = [&counts](std::span<int32_t* const>, uint32_t samples)
    {
        counts.push_back(samples);
    };
    bench.system_.RegisterAuxCallback(System::Device::kTv, 0, 0, callback);
    bench.Start(*bench.voice_, 4, 2.0f / 3.0f);

    const std::vector<double> left = bench.Left(20);

    double worst = 0.0;
    for (std::size_t n = 600; n < left.size(); ++n)
    {
        const double place = static_cast<double>((n - 288 + 1) * 43690) / 65536.0 - 3.0;
        worst = std::max(worst, std::fabs(left[n] - SineAt(place)));
    }
    Check(worst <= 3.0, "the aux bus's return: the worst error is " + std::to_string(worst));
    XSFLIB_CHECK(std::all_of(left.begin(), left.begin() + kFrame, [](double s) { return s == 0.0; }));
    XSFLIB_CHECK(counts.size() == 20 && std::all_of(counts.begin(), counts.end(), [](uint32_t c) { return c == 96; }));
    const std::array<int32_t, 2> peaks = bench.system_.AuxReturnPeaks(System::Device::kTv, 0, 0);
    XSFLIB_CHECK(peaks[0] >= 9900 && peaks[0] <= 10000 && peaks[1] == 0);
}

void TestMeters()
{
    // A meter's peaks measure the voice's mix into the TV's main bus, as floats.
    Bench bench(Constant(kFrame * 8));
    bench.system_.SetMeters(1);
    bench.system_.SetVoiceMeter(*bench.voice_, 0);
    bench.system_.SetVoiceVe(*bench.voice_, {0x4000, 0});
    bench.Start(*bench.voice_, 4, 1.0f);

    bench.Left(3);

    XSFLIB_CHECK((bench.system_.MeterPeaks(0) == std::array<int32_t, 2>{5000, 0}));
}

void TestSnapshot()
{
    // A restore renders the same samples again. A voice taken after the snapshot goes with the restore. The same voice
    // taken again plays the same samples.
    Bench bench(Constant(kFrame * 8));
    bench.Start(*bench.voice_, 4, 0.75f);
    bench.Left(1);
    const System::Snapshot snapshot = bench.system_.Save();
    const auto second_voice = [&bench]()
    {
        ax::Voice* voice = bench.Add(kFrame * 8, 0x4000);
        bench.Start(*voice, 1, 0.5f);

        return voice->index;
    };

    const uint32_t first_index = second_voice();
    const std::vector<double> first = bench.Left(3);
    bench.system_.Restore(snapshot);
    const std::vector<double> alone = bench.Left(3);
    bench.system_.Restore(snapshot);
    const uint32_t again_index = second_voice();
    const std::vector<double> again = bench.Left(3);

    XSFLIB_CHECK(first_index == 6 && again_index == 6);
    XSFLIB_CHECK(again == first);
    XSFLIB_CHECK(alone != first && Near(alone[2 * kFrame], 10000.0));
    XSFLIB_CHECK(bench.system_.Voices()[5].renderer.state == ax::VoiceState::kStop);
}

void TestVoices()
{
    // The enhanced mode has room for more voices than the console's 96. The free voices go out from the last down.
    Bench bench(Constant(kFrame), 200);

    std::vector<uint32_t> indices;
    for (int i = 0; i < 150; ++i)
    {
        ax::Voice* voice = bench.system_.AcquireVoice(10);
        indices.push_back(voice != nullptr ? voice->index : 0xFFFFFFFF);
    }

    XSFLIB_CHECK(bench.system_.Voices().size() == 200 && bench.voice_->index == 199);
    XSFLIB_CHECK(indices.front() == 198 && indices.back() == 49);
}

} // namespace

int main()
{
    TestSine();
    TestLevel();
    TestRamps();
    TestStart();
    TestAux();
    TestMeters();
    TestSnapshot();
    TestVoices();

    return xsflib::testing::Summary("cafe_ax_enhanced");
}
