// SPDX-License-Identifier: MIT

// Checks the enhanced mixing mode of the model of the audio renderer without game data, against the rules in the
// renderer's header: a sine converted from 32 kHz at each quality, a voice's level without a conversion, the ramps of a
// mute, the depop of a stop, and snapshots.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/nx/audio/dsp.h"
#include "ntdlib/nx/audio/renderer.h"

using namespace xsflib::testing;

namespace
{

namespace audio = ntdlib::nx::audio;

constexpr int kFrame = 240;
constexpr double kPi = 3.14159265358979323846;

// A mono PCM16 voice of `wave` at `rate`, at full volume to the final mix's first buffer, on a renderer in the enhanced
// mode with room for four voices.
struct EnhancedVoice
{
    EnhancedVoice(const std::vector<int16_t>& wave, int32_t rate, audio::SrcQuality quality) : renderer(Parameter())
    {
        for (const int16_t sample : wave)
        {
            const auto bits = static_cast<uint16_t>(sample);
            bytes.push_back(static_cast<uint8_t>(bits & 0xFF));
            bytes.push_back(static_cast<uint8_t>(bits >> 8));
        }
        renderer.AcquireFinalMix(2);
        const int kInputs[] = {0, 1};
        renderer.AddDeviceSink(kInputs);
        voice = renderer.AcquireVoiceSlot(rate, 1, audio::SampleFormat::kPcmInt16, 0, nullptr);
        renderer.SetVoiceDestination(voice, audio::Renderer::kFinalMix);
        renderer.SetVoiceMixVolume(voice, 1.0f, 0, 0);
        renderer.SetVoiceSrcQuality(voice, quality);
        audio::WaveBufferInfo buffer;
        buffer.buffer = bytes;
        buffer.start = 0;
        buffer.end = static_cast<int32_t>(wave.size());
        renderer.AppendWaveBuffer(voice, buffer);
        renderer.SetVoicePlayState(voice, audio::PlayState::kStarted);
    }

    static audio::RendererParameter Parameter()
    {
        audio::RendererParameter parameter;
        parameter.mix_buffer_count = 2;
        parameter.voice_count = 4;
        parameter.enhanced = true;

        return parameter;
    }

    // The left channel of the next `frames` frames, on the scale of 16-bit samples.
    std::vector<double> Left(int frames)
    {
        std::vector<double> left;
        for (int f = 0; f < frames; ++f)
        {
            renderer.RenderFrame();
            const std::span<const float> out = renderer.DeviceOutputFloat();
            for (std::size_t t = 0; t < out.size(); t += 2)
            {
                left.push_back(static_cast<double>(out[t]) * 32768.0);
            }
        }

        return left;
    }

    std::vector<uint8_t> bytes;
    audio::Renderer renderer;
    int voice = -1;
};

// `count` samples of 10000.
std::vector<int16_t> Constant(int count)
{
    return std::vector<int16_t>(static_cast<std::size_t>(count), 10000);
}

void TestSine()
{
    // A sine of 1 kHz at 32 kHz, at an amplitude of 10000, plays at a ratio of 21845 / 32768. Output n then lies at
    // input position n * 21845 / 32768 less 3 samples at medium quality, 5 at high quality and 4 at low quality. Once
    // the kernel has passed the start, each output lies within 3 of the sine there: the interpolator's error, at least
    // 75 dB down, and the rounding of the input to whole samples.
    std::vector<int16_t> wave;
    for (int k = 0; k < 32000; ++k)
    {
        wave.push_back(static_cast<int16_t>(std::lround(10000.0 * std::sin(2.0 * kPi * k / 32.0))));
    }
    const std::pair<audio::SrcQuality, int> kQualities[] = {
        {audio::SrcQuality::kMedium, 3}, {audio::SrcQuality::kHigh, 5}, {audio::SrcQuality::kLow, 4}};
    for (const auto& [quality, behind] : kQualities)
    {
        EnhancedVoice voice(wave, 32000, quality);

        const std::vector<double> left = voice.Left(20);

        double worst = 0.0;
        for (std::size_t n = 100; n < left.size(); ++n)
        {
            const double position = static_cast<double>(n) * 21845.0 / 32768.0 - behind;
            worst = std::max(worst, std::fabs(left[n] - 10000.0 * std::sin(2.0 * kPi * position / 32.0)));
        }
        Check(worst <= 3.0, std::to_string(behind) + " behind: the worst error is " + std::to_string(worst));
    }
}

void TestLevel()
{
    // Without a conversion, the interpolator's rows sum to 1: a wave of 10000 plays at 10000, where the console's
    // filter gives 9959. DeviceOutput gives the samples rounded.
    EnhancedVoice voice(Constant(kFrame * 8), 48000, audio::SrcQuality::kMedium);

    const std::vector<double> left = voice.Left(2);
    const std::span<const int16_t> rounded = voice.renderer.DeviceOutput();

    double worst = 0.0;
    for (std::size_t n = 40; n < left.size(); ++n)
    {
        worst = std::max(worst, std::fabs(left[n] - 10000.0));
    }
    Check(worst <= 0.2, "the level lies " + std::to_string(worst) + " from 10000");
    XSFLIB_CHECK(rounded[0] == 10000 && rounded[2 * (kFrame - 1)] == 10000 && rounded[1] == 0);
}

void TestMute()
{
    // A mute ramps the volume from 1 to 0 across the next frame: sample i of 240 takes 1 - i / 240. Unmuting ramps it
    // up from 0 again.
    EnhancedVoice voice(Constant(kFrame * 8), 48000, audio::SrcQuality::kMedium);
    voice.Left(1);
    voice.renderer.SetVoiceMuted(voice.voice, true);

    const std::vector<double> falling = voice.Left(1);
    const std::vector<double> muted = voice.Left(1);
    voice.renderer.SetVoiceMuted(voice.voice, false);
    const std::vector<double> rising = voice.Left(1);
    const std::vector<double> unmuted = voice.Left(1);

    const auto near = [](double sample, double expected)
    {
        return std::fabs(sample - expected) <= 0.2;
    };
    XSFLIB_CHECK(near(falling[0], 10000.0) && near(falling[120], 5000.0) && near(falling[239], 10000.0 / 240.0));
    XSFLIB_CHECK(std::all_of(muted.begin(), muted.end(), [](double s) { return s == 0.0; }));
    XSFLIB_CHECK(rising[0] == 0.0 && near(rising[1], 10000.0 / 240.0) && near(rising[239], 10000.0 * 239.0 / 240.0));
    XSFLIB_CHECK(near(unmuted[0], 10000.0) && near(unmuted[239], 10000.0));
}

void TestStop()
{
    // A voice that stops leaves its last product, 10000, to the depop. Each sample, the depop multiplies by
    // 31529 / 32768 and adds to the buffer. At the end of the third frame it lies below 2^-16 and becomes 0.
    EnhancedVoice voice(Constant(kFrame * 8), 48000, audio::SrcQuality::kMedium);
    voice.Left(2);
    voice.renderer.SetVoicePlayState(voice.voice, audio::PlayState::kStopped);

    const std::vector<double> fading = voice.Left(3);
    const std::vector<double> after = voice.Left(1);

    const double decay = 31529.0 / 32768.0;
    double worst = 0.0;
    for (std::size_t t = 0; t < fading.size(); ++t)
    {
        const double expected = 10000.0 * std::pow(decay, static_cast<double>(t + 1));
        worst = std::max(worst, std::fabs(fading[t] - expected) / expected);
    }
    Check(worst <= 1e-4, "the depop lies " + std::to_string(worst) + " of its value from the decay");
    XSFLIB_CHECK(std::all_of(after.begin(), after.end(), [](double s) { return s == 0.0; }));
}

void TestSnapshot()
{
    // A restore renders the same samples again. A voice that takes a slot past the snapshot's last one goes with the
    // restore. The same voice taken again gets the same slot and plays the same samples.
    EnhancedVoice voice(Constant(kFrame * 8), 48000, audio::SrcQuality::kMedium);
    voice.Left(1);
    const audio::Renderer::Snapshot snapshot = voice.renderer.Save();
    const auto second_voice = [&]()
    {
        const int slot = voice.renderer.AcquireVoiceSlot(32000, 1, audio::SampleFormat::kPcmInt16, 0, nullptr);
        voice.renderer.SetVoiceDestination(slot, audio::Renderer::kFinalMix);
        voice.renderer.SetVoiceMixVolume(slot, 0.5f, 0, 0);
        audio::WaveBufferInfo buffer;
        buffer.buffer = voice.bytes;
        buffer.start = 0;
        buffer.end = kFrame * 8;
        voice.renderer.AppendWaveBuffer(slot, buffer);
        voice.renderer.SetVoicePlayState(slot, audio::PlayState::kStarted);

        return slot;
    };

    const int first_slot = second_voice();
    const std::vector<double> first = voice.Left(3);
    voice.renderer.Restore(snapshot);
    const std::vector<double> alone = voice.Left(3);
    voice.renderer.Restore(snapshot);
    const int again_slot = second_voice();
    const std::vector<double> again = voice.Left(3);

    XSFLIB_CHECK(first_slot == 1 && again_slot == 1);
    XSFLIB_CHECK(again == first);
    XSFLIB_CHECK(alone != first && std::fabs(alone[kFrame * 2] - 10000.0) <= 0.2);
}

} // namespace

int main()
{
    TestSine();
    TestLevel();
    TestMute();
    TestStop();
    TestSnapshot();

    return xsflib::testing::Summary("nx_audio_enhanced");
}
