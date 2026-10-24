// SPDX-License-Identifier: MIT

// Checks the frames without mixing of the model of AX (System::SetMixing) without game data, against the rules in
// system.h. Three voices play while a stretch of frames doesn't mix: ADPCM through a 4-tap filter in a loop, PCM16
// without a conversion in a loop, and PCM16 by linear interpolation, which runs out during the stretch. Their
// envelopes, mixes, ratios and mutes change during the stretch and at its ends. An envelope and a mix move all through
// the stretch. A fourth voice starts during the stretch, and its mix changes as the stretch ends. Outside the enhanced
// mode, a fifth voice plays through its low-pass at 60 Hz and a biquad low-pass at 150 Hz. In fixed point, their states
// would keep a difference for good if the stretch left them as they were. A second, short stretch follows. With the 32
// kHz renderer, the 48 kHz renderer and the enhanced mode, the frames of the stretches are silent, the frame callbacks
// run, and the voices stand where they stand when every frame mixes. The frames after each stretch give the samples of
// AX when every frame mixes. With the 32 kHz renderer, that holds from the second frame after each stretch: the first
// takes the upsampler's history from before the stretch.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/ax/lpf.h"
#include "ntdlib/cafe/ax/memory.h"
#include "ntdlib/cafe/ax/pb.h"
#include "ntdlib/cafe/ax/system.h"
#include "ntdlib/cafe/ax/voice.h"

using namespace xsflib::testing;

namespace
{

namespace ax = ntdlib::cafe::ax;

using ax::System;

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

constexpr uint32_t kAdpcmAddress = 0x10000000;
constexpr uint32_t kLoopAddress = 0x11000000;
constexpr uint32_t kShortAddress = 0x12000000;

enum class Setup
{
    k32k,
    k48k,
    kEnhanced,
};

// The next number of a fixed sequence.
uint32_t Next(uint32_t& seed)
{
    seed = seed * 1664525u + 1013904223u;

    return seed;
}

// `count` samples of noise from -8192 to 8191, as big-endian PCM16.
std::vector<uint8_t> Pcm16Noise(int count, uint32_t seed)
{
    std::vector<uint8_t> bytes;
    for (int i = 0; i < count; ++i)
    {
        const int32_t value = static_cast<int32_t>(Next(seed) >> 16) - 32768;
        const auto bits = static_cast<uint16_t>(static_cast<int16_t>(value / 4));
        bytes.push_back(static_cast<uint8_t>(bits >> 8));
        bytes.push_back(static_cast<uint8_t>(bits & 0xFF));
    }

    return bytes;
}

// `frames` frames of ADPCM noise. Each frame's header takes predictor 0 or 1 and a scale from 6 to 9.
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

// A mix into the TV's main bus, front left and right.
std::vector<System::ChannelMix> FrontMix(ax::VolumeRamp left, ax::VolumeRamp right)
{
    std::vector<System::ChannelMix> mix(ax::kTvChannels);
    mix[0][0] = left;
    mix[1][0] = right;

    return mix;
}

// The voices on AX in one of the setups.
class Scene
{
public:
    explicit Scene(Setup setup)
    {
        memory_.Map(kAdpcmAddress, adpcm_);
        memory_.Map(kLoopAddress, pcm_);
        memory_.Map(kShortAddress, short_pcm_);
        if (setup == Setup::kEnhanced)
        {
            system_.InitEnhanced(8);
        }
        else
        {
            system_.Init(System::Library::kSndCore2, setup == Setup::k48k);
        }
        system_.RegisterFrameCallback([this]() { ++callbacks_; });

        // ADPCM nibbles: each frame of 8 bytes holds its header, then 14 samples. The loop starts at frame 1.
        looped_ = system_.AcquireVoice(10);
        system_.SetVoiceOffsets(*looped_, {ax::SampleFormat::kAdpcm, 1, 18, 16 * kAdpcmFrames - 1, 2, kAdpcmAddress});
        ax::Adpcm adpcm;
        adpcm.coefs = {1638, 0, 3277, -1638};
        adpcm.predictor_scale = adpcm_[0];
        system_.SetVoiceAdpcm(*looped_, adpcm);
        system_.SetVoiceAdpcmLoop(*looped_, {adpcm_[8], 0, 0});
        Start(*looped_, 4, 1.1f, FrontMix({0x6000, 0}, {0x2000, 0}));

        plain_ = system_.AcquireVoice(10);
        system_.SetVoiceOffsets(*plain_, {ax::SampleFormat::kPcm16, 1, 0, kLoopSamples - 1, 0, kLoopAddress});
        Start(*plain_, 0, 1.0f, FrontMix({0x4000, 0}, {0x4000, 0}));

        short_ = system_.AcquireVoice(10);
        system_.SetVoiceOffsets(*short_, {ax::SampleFormat::kPcm16, 0, 0, kShortSamples - 1, 0, kShortAddress});
        Start(*short_, 1, 0.7f, FrontMix({0, 0}, {0x7000, 0}));

        if (setup != Setup::kEnhanced)
        {
            filtered_ = system_.AcquireVoice(10);
            system_.SetVoiceOffsets(*filtered_, {ax::SampleFormat::kPcm16, 1, 0, kLoopSamples - 1, 0, kLoopAddress});
            const ntdlib::ax::LpfCoefs lpf = system_.ComputeLpfCoefs(60);
            system_.SetVoiceLpf(*filtered_, {1, 0, static_cast<int16_t>(lpf.a0), static_cast<int16_t>(lpf.b0)});
            system_.SetVoiceBiquad(*filtered_, {1, 0, 0, 0, 0, 3, 7, 3, 32086, -15715});
            Start(*filtered_, 0, 1.0f, FrontMix({0x7FFF, 0}, {0x7FFF, 0}));
        }
    }

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    // The changes due before frame `frame`, then the frame, mixed or not.
    void Frame(int frame, bool mixing)
    {
        switch (frame)
        {
        case 10:
            system_.SetVoiceVe(*looped_, {0x6000, -4});
            break;
        case 15:
            system_.SetVoiceDeviceMix(*plain_, System::Device::kTv, 0, FrontMix({0x4000, 4}, {0x4000, 0}));
            break;
        case 20:
            system_.SetVoiceSrcRatio(*looped_, 1.3f);
            system_.SetVoiceMuted(*plain_, true);
            break;
        case 30:
            late_ = system_.AcquireVoice(10);
            system_.SetVoiceOffsets(*late_, {ax::SampleFormat::kPcm16, 1, 0, kLoopSamples - 1, 0, kLoopAddress});
            Start(*late_, 4, 0.9f, FrontMix({0x3000, 0}, {0x3000, 0}));
            break;
        case kStretchEnd - 1:
            system_.SetVoiceVe(*plain_, {0x5000, 0});
            break;
        case kStretchEnd:
            system_.SetVoiceMuted(*plain_, false);
            system_.SetVoiceDeviceMix(*late_, System::Device::kTv, 0, FrontMix({0x5000, 0}, {0x1000, 0}));
            system_.SetVoiceDeviceMix(*looped_, System::Device::kTv, 0, FrontMix({0x6000, -24}, {0x2000, 0}));
            break;
        case kStretchEnd + 1:
            system_.SetVoiceDeviceMix(*looped_, System::Device::kTv, 0, FrontMix({0x4000, 0}, {0x2000, 0}));
            break;
        case 45:
            system_.SetVoiceState(*looped_, ax::VoiceState::kStop);
            break;
        default:
            break;
        }

        system_.SetMixing(mixing);
        system_.RenderFrame();
    }

    // The frame's stereo output, on the scale of 16-bit samples outside the enhanced mode.
    std::vector<float> Output() const
    {
        const std::span<const float> enhanced = system_.OutputFloat();
        if (!enhanced.empty())
        {
            return {enhanced.begin(), enhanced.end()};
        }

        const std::span<const int16_t> output = system_.Output();
        return {output.begin(), output.end()};
    }

    // Where each voice stands as the frame starts.
    std::array<uint32_t, 4> Positions() const
    {
        return {system_.GetVoiceOffsets(*looped_).current, system_.GetVoiceOffsets(*plain_).current,
                system_.GetVoiceOffsets(*short_).current,
                late_ != nullptr ? system_.GetVoiceOffsets(*late_).current : 0};
    }

    int Callbacks() const
    {
        return callbacks_;
    }

private:
    void Start(ax::Voice& voice, uint32_t type, float ratio, const std::vector<System::ChannelMix>& mix)
    {
        system_.SetVoiceSrcType(voice, type);
        system_.SetVoiceSrcRatio(voice, ratio);
        system_.SetVoiceVe(voice, {ax::kUnityVolume, 0});
        system_.SetVoiceDeviceMix(voice, System::Device::kTv, 0, mix);
        system_.SetVoiceState(voice, ax::VoiceState::kRun);
    }

    // In the order they're made: the memory maps the bytes, and the model reads the memory.
    std::vector<uint8_t> adpcm_ = AdpcmNoise(kAdpcmFrames, 1);
    std::vector<uint8_t> pcm_ = Pcm16Noise(kLoopSamples, 2);
    std::vector<uint8_t> short_pcm_ = Pcm16Noise(kShortSamples, 3);
    ax::Memory memory_;
    System system_{memory_};
    ax::Voice* looped_ = nullptr;
    ax::Voice* plain_ = nullptr;
    ax::Voice* short_ = nullptr;
    ax::Voice* late_ = nullptr;
    ax::Voice* filtered_ = nullptr;
    int callbacks_ = 0;
};

// Each frame's output, where the voices stand after it, and the frame callbacks that have run.
struct Run
{
    std::vector<std::vector<float>> output;
    std::vector<std::array<uint32_t, 4>> positions;
    int callbacks = 0;
};

Run Play(Setup setup, bool stretch_mixes)
{
    Scene scene(setup);
    Run run;
    for (int frame = 0; frame < kFrames; ++frame)
    {
        scene.Frame(frame, stretch_mixes || !Unmixed(frame));
        run.output.push_back(scene.Output());
        run.positions.push_back(scene.Positions());
    }
    run.callbacks = scene.Callbacks();

    return run;
}

bool Silent(const std::vector<float>& samples)
{
    return std::all_of(samples.begin(), samples.end(), [](float sample) { return sample == 0.0f; });
}

void TestStretch(Setup setup)
{
    const Run mixed = Play(setup, true);
    const Run stretch = Play(setup, false);
    const std::string name = setup == Setup::k32k ? "32 kHz: " : setup == Setup::k48k ? "48 kHz: " : "enhanced mode: ";

    // The short voice runs out during the stretch: a voice that has stopped stands at 0. The mixed run sounds there.
    const auto shorts = [&](int frame)
    {
        return mixed.positions[static_cast<std::size_t>(frame)][2];
    };
    Check(shorts(kStretchStart) > 0 && shorts(kStretchEnd) == 0, name + "the short voice runs out during the stretch");
    const auto compared = [setup](int frame)
    {
        return setup != Setup::k32k || (frame != kStretchEnd && frame != kSecondEnd);
    };
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
        else if (compared(frame))
        {
            same = same && stretch.output[f] == mixed.output[f];
        }
    }
    Check(silent && sounds, name + "the stretches are silent");
    Check(stretch.positions == mixed.positions, name + "the voices stand at the same places");
    Check(stretch.callbacks == kFrames && mixed.callbacks == kFrames, name + "the frame callbacks run");
    Check(same, name + "the frames outside the stretches give the same samples");
    Check(!Silent(stretch.output[kStretchEnd]) && !Silent(stretch.output[kFrames - 1]),
          name + "the frames after the stretch sound");
}

} // namespace

int main()
{
    TestStretch(Setup::k32k);
    TestStretch(Setup::k48k);
    TestStretch(Setup::kEnhanced);

    return xsflib::testing::Summary("cafe_ax_without_mixing");
}
