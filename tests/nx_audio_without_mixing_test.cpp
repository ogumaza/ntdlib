// SPDX-License-Identifier: MIT

// Checks the frames without mixing of the model of the audio renderer (Renderer::SetMixing) without game data, against
// the rules in the renderer's header. Three voices play while a stretch of frames doesn't mix: DSP-ADPCM converted from
// 32 kHz at high quality in a loop, PCM16 at the renderer's rate without a conversion in a loop, and PCM16 converted
// from 22.05 kHz at low quality, which runs out during the stretch. Their volumes, mixes, pitches, qualities and mutes
// change during the stretch and at its ends. A second, short stretch follows. A fourth voice stops in the frame before
// the stretch, and the depop fades into it. A fifth starts during the stretch, and its mix changes as the stretch ends.
// In both mixing modes, the frames of the stretches are silent, and the voices report the samples played that a
// renderer reports when every frame mixes. The frames after each stretch give that renderer's samples.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/nx/audio/dsp.h"
#include "ntdlib/nx/audio/renderer.h"

using namespace xsflib::testing;

namespace
{

namespace audio = ntdlib::nx::audio;

constexpr int kFrames = 60;

// The stretch without mixing: frames 4 to 39. A second stretch takes frames 50 to 52.
constexpr int kStretchStart = 4;
constexpr int kStretchEnd = 40;
constexpr int kSecondStart = 50;
constexpr int kSecondEnd = 53;

bool Unmixed(int frame)
{
    return (frame >= kStretchStart && frame < kStretchEnd) || (frame >= kSecondStart && frame < kSecondEnd);
}

constexpr int kAdpcmFrames = 100;
constexpr int kLoopSamples = 1000;
constexpr int kShortSamples = 700;

// Predictor 0 takes 0.8 of the last sample, and predictor 1 rings at 1.6 and -0.8.
constexpr audio::AdpcmCoefficients kCoefficients = {1638, 0, 3277, -1638};

// The next number of a fixed sequence.
uint32_t Next(uint32_t& seed)
{
    seed = seed * 1664525u + 1013904223u;

    return seed;
}

// `count` samples of noise from -8192 to 8191, as PCM16.
std::vector<uint8_t> Pcm16Noise(int count, uint32_t seed)
{
    std::vector<uint8_t> bytes;
    for (int i = 0; i < count; ++i)
    {
        const int32_t value = static_cast<int32_t>(Next(seed) >> 16) - 32768;
        const auto bits = static_cast<uint16_t>(static_cast<int16_t>(value / 4));
        bytes.push_back(static_cast<uint8_t>(bits & 0xFF));
        bytes.push_back(static_cast<uint8_t>(bits >> 8));
    }

    return bytes;
}

// `frames` frames of DSP-ADPCM noise. Each frame's header takes predictor 0 or 1 and a scale from 6 to 9.
std::vector<uint8_t> AdpcmNoise(int frames, uint32_t seed)
{
    std::vector<uint8_t> bytes;
    for (int f = 0; f < frames; ++f)
    {
        const uint32_t header = Next(seed);
        bytes.push_back(static_cast<uint8_t>((header >> 31) << 4 | (6 + ((header >> 28) & 3))));
        for (int i = 0; i < 7; ++i)
        {
            bytes.push_back(static_cast<uint8_t>(Next(seed) >> 24));
        }
    }

    return bytes;
}

// The voices on a renderer in either mode, to the final mix's two buffers and a device sink.
class Scene
{
public:
    explicit Scene(bool enhanced) : renderer_(Parameter(enhanced))
    {
        renderer_.AcquireFinalMix(2);
        const int kInputs[] = {0, 1};
        renderer_.AddDeviceSink(kInputs);

        looped_ = Start(32000, audio::SampleFormat::kAdpcm, audio::SrcQuality::kHigh, adpcm_, 14 * kAdpcmFrames, true);
        renderer_.SetVoiceMixVolume(looped_, 0.8f, 0, 0);
        renderer_.SetVoiceMixVolume(looped_, 0.3f, 0, 1);
        plain_ = Start(48000, audio::SampleFormat::kPcmInt16, audio::SrcQuality::kMedium, pcm_, kLoopSamples, true);
        renderer_.SetVoiceMixVolume(plain_, 0.5f, 0, 0);
        renderer_.SetVoiceMixVolume(plain_, 0.5f, 0, 1);
        short_ =
            Start(22050, audio::SampleFormat::kPcmInt16, audio::SrcQuality::kLow, short_pcm_, kShortSamples, false);
        renderer_.SetVoiceMixVolume(short_, 0.9f, 0, 1);
        stopped_ = Start(48000, audio::SampleFormat::kPcmInt16, audio::SrcQuality::kMedium, pcm_, kLoopSamples, true);
        renderer_.SetVoiceMixVolume(stopped_, 1.0f, 0, 0);
    }

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    // The changes due before frame `frame`, then the frame, mixed or not.
    void Frame(int frame, bool mixing)
    {
        switch (frame)
        {
        case kStretchStart - 1:
            renderer_.SetVoicePlayState(stopped_, audio::PlayState::kStopped);
            break;
        case 10:
            renderer_.SetVoiceVolume(looped_, 0.5f);
            renderer_.SetVoicePitch(looped_, 1.25f);
            break;
        case 15:
            renderer_.SetVoiceMixVolume(plain_, 0.6f, 0, 1);
            renderer_.SetVoiceMuted(plain_, true);
            break;
        case 20:
            renderer_.SetVoiceSrcQuality(looped_, audio::SrcQuality::kMedium);
            break;
        case 30:
            late_ =
                Start(32000, audio::SampleFormat::kAdpcm, audio::SrcQuality::kHigh, adpcm_, 14 * kAdpcmFrames, true);
            renderer_.SetVoiceMixVolume(late_, 0.7f, 0, 1);
            break;
        case kStretchEnd - 1:
            renderer_.SetVoiceVolume(looped_, 0.6f);
            break;
        case kStretchEnd:
            renderer_.SetVoiceMuted(plain_, false);
            renderer_.SetVoiceMixVolume(looped_, 0.4f, 0, 0);
            renderer_.SetVoiceMixVolume(late_, 0.2f, 0, 1);
            break;
        case 45:
            renderer_.SetVoicePlayState(looped_, audio::PlayState::kStopped);
            break;
        default:
            break;
        }

        renderer_.SetMixing(mixing);
        renderer_.RenderFrame();
    }

    // The frame's output, interleaved, on the scale of 16-bit samples in the console's mode.
    std::vector<float> Output() const
    {
        const std::span<const float> enhanced = renderer_.DeviceOutputFloat();
        if (!enhanced.empty())
        {
            return {enhanced.begin(), enhanced.end()};
        }

        const std::span<const int16_t> output = renderer_.DeviceOutput();
        return {output.begin(), output.end()};
    }

    std::array<uint64_t, 4> Played() const
    {
        return {renderer_.GetVoicePlayedSampleCount(looped_), renderer_.GetVoicePlayedSampleCount(plain_),
                renderer_.GetVoicePlayedSampleCount(short_),
                late_ >= 0 ? renderer_.GetVoicePlayedSampleCount(late_) : 0};
    }

private:
    static audio::RendererParameter Parameter(bool enhanced)
    {
        audio::RendererParameter parameter;
        parameter.mix_buffer_count = 2;
        parameter.voice_count = 8;
        parameter.enhanced = enhanced;

        return parameter;
    }

    // A mono voice of `count` samples of `bytes`, to the final mix, started.
    int Start(int32_t rate, audio::SampleFormat format, audio::SrcQuality quality, const std::vector<uint8_t>& bytes,
              int32_t count, bool loop)
    {
        const audio::AdpcmCoefficients* coefficients = format == audio::SampleFormat::kAdpcm ? &kCoefficients : nullptr;
        const int voice = renderer_.AcquireVoiceSlot(rate, 1, format, 0, coefficients);
        renderer_.SetVoiceDestination(voice, audio::Renderer::kFinalMix);
        renderer_.SetVoiceSrcQuality(voice, quality);
        audio::WaveBufferInfo buffer;
        buffer.buffer = bytes;
        buffer.end = count;
        buffer.loop = loop;
        renderer_.AppendWaveBuffer(voice, buffer);
        renderer_.SetVoicePlayState(voice, audio::PlayState::kStarted);

        return voice;
    }

    // In the order they're made: the voices' buffers outlive the renderer.
    std::vector<uint8_t> adpcm_ = AdpcmNoise(kAdpcmFrames, 1);
    std::vector<uint8_t> pcm_ = Pcm16Noise(kLoopSamples, 2);
    std::vector<uint8_t> short_pcm_ = Pcm16Noise(kShortSamples, 3);
    audio::Renderer renderer_;
    int looped_ = -1;
    int plain_ = -1;
    int short_ = -1;
    int stopped_ = -1;
    int late_ = -1;
};

// Each frame's output, and the samples that each voice has played after it.
struct Run
{
    std::vector<std::vector<float>> output;
    std::vector<std::array<uint64_t, 4>> played;
};

Run Play(bool enhanced, bool stretch_mixes)
{
    Scene scene(enhanced);
    Run run;
    for (int frame = 0; frame < kFrames; ++frame)
    {
        scene.Frame(frame, stretch_mixes || !Unmixed(frame));
        run.output.push_back(scene.Output());
        run.played.push_back(scene.Played());
    }

    return run;
}

bool Silent(const std::vector<float>& samples)
{
    return std::all_of(samples.begin(), samples.end(), [](float sample) { return sample == 0.0f; });
}

void TestStretch(bool enhanced)
{
    const Run mixed = Play(enhanced, true);
    const Run stretch = Play(enhanced, false);
    const std::string mode = enhanced ? "enhanced mode: " : "console's mode: ";

    // The short voice runs out during the stretch, and the mixed renderer sounds there.
    const auto shorts = [&](int frame)
    {
        return mixed.played[static_cast<std::size_t>(frame)][2];
    };
    Check(shorts(kStretchStart) < shorts(kStretchEnd) && shorts(kStretchEnd) == shorts(kFrames - 1),
          mode + "the short voice runs out during the stretch");
    bool silent = true;
    bool sounds = false;
    bool same = true;
    for (int frame = 0; frame < kFrames; ++frame)
    {
        const auto f = static_cast<std::size_t>(frame);
        if (Unmixed(frame))
        {
            silent = silent && Silent(stretch.output[f]);
            sounds = sounds || !Silent(mixed.output[f]);
        }
        else
        {
            same = same && stretch.output[f] == mixed.output[f];
        }
    }
    Check(silent && sounds, mode + "the stretches are silent");
    Check(stretch.played == mixed.played, mode + "the voices report the same samples played");
    Check(same, mode + "the frames outside the stretches give the same samples");
    Check(!Silent(stretch.output[kStretchEnd]) && !Silent(stretch.output[kFrames - 1]),
          mode + "the frames after the stretch sound");
}

} // namespace

int main()
{
    TestStretch(false);
    TestStretch(true);

    return xsflib::testing::Summary("nx_audio_without_mixing");
}
