// SPDX-License-Identifier: MIT

// Tests the enhanced mixing mode against its rules (ntdlib/n64/enhanced_mix.h). No console has this mode. Each expected
// value comes from a sine's formula and the rule that it tests.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/n64/audio_list.h"
#include "ntdlib/n64/audio_memory.h"
#include "ntdlib/n64/audio_rsp.h"
#include "ntdlib/n64/frame_player.h"

namespace
{

namespace n64 = ntdlib::n64;

constexpr int kGameRate = 32000;
constexpr int kChunkSamples = 160;
constexpr uint32_t kChunkBytes = kChunkSamples * 2;
constexpr uint32_t kBase = 0x5C0;
constexpr uint32_t kLeft = 0;
constexpr uint32_t kRight = 0x140;
constexpr uint32_t kWetLeft = 0x280;
constexpr uint32_t kWetRight = 0x3C0;
constexpr uint32_t kResampled = 0x500;
constexpr uint32_t kInput = 0x650;
constexpr uint32_t kEffect = 0x790;
constexpr int kTrack = 5;
constexpr double kTone = 1000.0;
constexpr double kAmplitude = 16000.0;
constexpr double kPi = 3.14159265358979323846;
constexpr int kWaveSamples = kGameRate * 4;

// What the sine's first output sits behind: the console takes an output between the second and third of its 4 taps, and
// the first input is tap 4.
constexpr double kTapLag = 3.0;

struct Settings
{
    uint16_t pitch = 0x8000;
    uint16_t volume = 0x4000;
    uint16_t target = 0x4000;
    uint16_t rate = 1; // the whole part of the ramp's factor for each 8 samples
    uint16_t dry = 0x7FFF;
    uint16_t wet = 0;
    bool wet_to_output = false; // the wet buses reach the output directly
    bool echo = false;          // the wet buses reach the output a chunk later
    bool constant = false;      // the wave is a constant of kConstant
};

constexpr int16_t kConstant = 8000;

// A voice's commands on the standard microcode, built as libultra's driver builds them.
class ToneModel final : public n64::FrameModel
{
public:
    ToneModel(n64::AudioMemory& memory, const Settings& settings) : settings_(settings)
    {
        format_.microcode = n64::AudioMicrocode::kStandard;
        format_.output_rate = kGameRate;
        format_.frame_step = kChunkSamples;
        format_.areas = {{kBase + kLeft, kChunkBytes, 0, false},
                         {kBase + kRight, kChunkBytes, 1, false},
                         {kBase + kWetLeft, kChunkBytes, 0, true},
                         {kBase + kWetRight, kChunkBytes, 1, true}};
        wave_ = memory.Allocate(kWaveSamples * 2);
        for (int i = 0; i < kWaveSamples; i++)
        {
            const double value =
                settings.constant ? kConstant : kAmplitude * std::sin(2.0 * kPi * kTone * i / kGameRate);
            std::array<uint8_t, 2> bytes = {};
            xsflib::StoreBe16(bytes.data(), static_cast<uint16_t>(static_cast<int16_t>(std::lround(value))));
            memory.Write(wave_ + static_cast<uint32_t>(i) * 2, bytes.data(), bytes.size());
        }

        resampler_ = memory.Allocate(32);
        mixer_ = memory.Allocate(80);
        ring_ = memory.Allocate(kChunkBytes * 2);
    }

    std::unique_ptr<n64::FrameModel> Clone() const override
    {
        return std::make_unique<ToneModel>(*this);
    }

    void TakeState(const n64::FrameModel& other) override
    {
        const auto& held = dynamic_cast<const ToneModel&>(other);
        place_ = held.place_;
        fraction_ = held.fraction_;
        first_ = held.first_;
    }

    void BuildFrame(n64::AudioList& list, n64::AudioMemory& /*memory*/, uint32_t output, int samples) override
    {
        uint32_t out = output;
        for (int left = samples; left > 0; left -= kChunkSamples)
        {
            BuildChunk(list, out);
            out += kChunkBytes * 2;
        }
    }

    bool Busy() const override
    {
        return true;
    }

    const n64::FrameFormat& Format() const override
    {
        return format_;
    }

private:
    void BuildChunk(n64::AudioList& list, uint32_t output)
    {
        const uint8_t init = first_ ? n64::kAudioFlagInit : 0;
        list.ClearBuffer(kLeft, kChunkBytes);
        list.ClearBuffer(kRight, kChunkBytes);
        list.ClearBuffer(kWetLeft, kChunkBytes);
        list.ClearBuffer(kWetRight, kChunkBytes);
        if (settings_.echo)
        {
            list.SetBuffer(0, kEffect, 0, kChunkBytes);
            list.LoadBuffer(ring_);
            list.SetBuffer(0, 0, 0, kChunkBytes);
            list.Mixer(0, 0x7FFF, kEffect, kLeft);
            list.Mixer(0, 0x7FFF, kEffect, kRight);
        }

        list.MarkVoice(kTrack);

        // The inputs that this chunk's outputs move past, and some more.
        const uint32_t end = fraction_ + (kChunkBytes * uint32_t{settings_.pitch});
        const uint32_t inputs = end >> 16;
        const uint32_t address = wave_ + (place_ * 2);
        const uint32_t misaligned = address & 7;
        list.SetBuffer(0, kInput, 0, ((inputs + 8) * 2) + misaligned);
        list.LoadBuffer(address - misaligned);
        list.SetBuffer(0, kInput + misaligned, kResampled, kChunkBytes);
        list.Resample(init, settings_.pitch, resampler_);
        place_ += inputs;
        fraction_ = end & 0xFFFF;

        list.SetBuffer(0, kResampled, kLeft, kChunkBytes);
        list.SetBuffer(n64::kAudioFlagAux, kRight, kWetLeft, kWetRight);
        if (first_)
        {
            list.SetVolume(n64::kAudioFlagVolume | n64::kAudioFlagLeft, settings_.volume, 0, 0);
            list.SetVolume(n64::kAudioFlagVolume, settings_.volume, 0, 0);
            list.SetVolume(n64::kAudioFlagLeft, settings_.target, settings_.rate, 0);
            list.SetVolume(0, settings_.target, settings_.rate, 0);
            list.SetVolume(n64::kAudioFlagAux, settings_.dry, 0, settings_.wet);
        }

        list.EnvMixer(init | n64::kAudioFlagAux, mixer_);
        first_ = false;
        list.MarkVoicesEnd();
        list.MarkChunkEnd();
        if (settings_.wet_to_output)
        {
            list.SetBuffer(0, 0, 0, kChunkBytes);
            list.Mixer(0, 0x7FFF, kWetLeft, kLeft);
            list.Mixer(0, 0x7FFF, kWetRight, kRight);
        }

        if (settings_.echo)
        {
            list.SetBuffer(0, 0, kWetLeft, kChunkBytes);
            list.SaveBuffer(ring_);
        }

        list.SetBuffer(0, 0, kEffect, kChunkBytes);
        list.Interleave(kLeft, kRight);
        list.SetBuffer(0, 0, kEffect, kChunkBytes * 2);
        list.SaveBuffer(output);
    }

    Settings settings_;
    n64::FrameFormat format_;
    uint32_t wave_ = 0;
    uint32_t resampler_ = 0;
    uint32_t mixer_ = 0;
    uint32_t ring_ = 0;
    uint32_t place_ = 0;
    uint32_t fraction_ = 0;
    bool first_ = true;
};

struct Bench
{
    Bench(const Settings& settings, int output_rate)
        : memory({}, 0x100000),
          player(std::make_unique<ToneModel>(memory, settings), memory, kChunkSamples * 2.0,
                 n64::FramePlayerOptions{.enhanced = true, .output_rate = output_rate})
    {
    }

    n64::AudioMemory memory;
    n64::FramePlayer player;
};

// The sine that a voice plays, at a time in the game's samples. An input before the wave's start is 0.
double Tone(double game_samples, double gain)
{
    return game_samples < 0.0 ? 0.0 : kAmplitude * gain * std::sin(2.0 * kPi * kTone * game_samples / kGameRate);
}

// The output against the sine, from a place where the wave's start is behind the interpolator's window.
void CheckTone(const std::vector<float>& out, int output_rate, double pitch, double delay, double gain)
{
    constexpr double kTolerance = 0.002;
    const double most = kAmplitude * gain * kTolerance;
    for (std::size_t i = 400; i < out.size() / 2; i++)
    {
        const double time = (static_cast<double>(i) * kGameRate / output_rate) * pitch;
        const double expected = Tone(time - delay, gain);
        XSFLIB_CHECK(std::abs(out[i * 2] - expected) < most);
        XSFLIB_CHECK(std::abs(out[(i * 2) + 1] - expected) < most);
    }
}

// Gains that hold: 0.5 for the voice's level, and 0x7FFF / 0x8000 for its dry mix.
constexpr double kGain = 0.5 * (32767.0 / 32768.0);

void TestToneAtTheGamesRate()
{
    Bench bench({}, 0);
    XSFLIB_CHECK(bench.player.OutputRate() == kGameRate);
    std::vector<float> out;
    bench.player.Run(4000, out);
    XSFLIB_CHECK(out.size() == 8000);
    CheckTone(out, kGameRate, 1.0, kTapLag, kGain);
}

// A pitch of 0x4000 steps half an input for each output. The state's taps stay where they are, in inputs.
void TestPitch()
{
    Settings settings;
    settings.pitch = 0x4000;
    Bench bench(settings, 0);
    std::vector<float> out;
    bench.player.Run(4000, out);
    CheckTone(out, kGameRate, 0.5, kTapLag, kGain);
}

// A pitch above 1 skips inputs, and the kernel widens.
void TestHighPitch()
{
    Settings settings;
    settings.pitch = 0xC000;
    Bench bench(settings, 0);
    std::vector<float> out;
    bench.player.Run(4000, out);
    CheckTone(out, kGameRate, 1.5, kTapLag, kGain);
}

// At a higher output rate, the same sine at the same time: 48 kHz is 2 times the game's rate, then two thirds of it.
void TestToneAtAHigherRate()
{
    Bench bench({}, 48000);
    XSFLIB_CHECK(bench.player.OutputRate() == 48000);
    std::vector<float> out;
    bench.player.Run(6000, out);
    XSFLIB_CHECK(out.size() == 12000);
    CheckTone(out, 48000, 1.0, kTapLag, kGain);
}

// An exact multiple of the game's rate needs no last interpolator.
void TestToneAtAMultiple()
{
    Bench bench({}, 96000);
    std::vector<float> out;
    bench.player.Run(12000, out);
    CheckTone(out, 96000, 1.0, kTapLag, kGain);
}

// What a voice sends to the wet buses reaches the output at the same time as what it sends to the dry buses, at either
// rate, with the gain of the wet mix and of the mixer: 0x7FFF / 0x8000 for each.
void TestWetPath()
{
    Settings settings;
    settings.dry = 0;
    settings.wet = 0x7FFF;
    settings.wet_to_output = true;
    constexpr double kWetGain = 0.5 * (32767.0 / 32768.0) * (32767.0 / 32768.0);
    {
        Bench bench(settings, 0);
        std::vector<float> out;
        bench.player.Run(4000, out);
        CheckTone(out, kGameRate, 1.0, kTapLag, kWetGain);
    }

    Bench bench(settings, 48000);
    std::vector<float> out;
    bench.player.Run(6000, out);
    CheckTone(out, 48000, 1.0, kTapLag, kWetGain);
}

// A game's delay line in main memory keeps its time at either rate: one chunk.
void TestEffectDelay()
{
    Settings settings;
    settings.dry = 0;
    settings.wet = 0x7FFF;
    settings.echo = true;
    constexpr double kWetGain = 0.5 * (32767.0 / 32768.0) * (32767.0 / 32768.0);
    {
        Bench bench(settings, 0);
        std::vector<float> out;
        bench.player.Run(4000, out);
        CheckTone(out, kGameRate, 1.0, kTapLag + kChunkSamples, kWetGain);
    }

    Bench bench(settings, 48000);
    std::vector<float> out;
    bench.player.Run(6000, out);
    CheckTone(out, 48000, 1.0, kTapLag + kChunkSamples, kWetGain);
}

// The microcode's ramp doubles 0x1000 for each 8 samples and stops at 0x4000. Its first command ends at 0x4000. The
// mode ramps from 0x1000 to there across the 160 samples. A constant comes out of the interpolator as it is, within its
// weights' rounding.
void TestRamp()
{
    Settings settings;
    settings.constant = true;
    settings.volume = 0x1000;
    settings.rate = 2;
    Bench bench(settings, 0);
    std::vector<float> out;
    bench.player.Run(320, out);
    constexpr double kDry = 32767.0 / 32768.0;
    constexpr double kFrom = 0x1000 / 32768.0;
    constexpr double kTo = 0x4000 / 32768.0;
    for (std::size_t i = 80; i < kChunkSamples; i++)
    {
        const double gain = kFrom + ((kTo - kFrom) * (static_cast<double>(i) + 1.0) / kChunkSamples);
        const double expected = kConstant * gain * kDry;
        XSFLIB_CHECK(std::abs(out[i * 2] - expected) < 2.0);
    }

    for (std::size_t i = kChunkSamples + 80; i < 320; i++)
    {
        XSFLIB_CHECK(std::abs(out[i * 2] - (kConstant * kTo * kDry)) < 2.0);
    }
}

// Nothing holds the mode's samples to 16 bits. The sum of the voice's two sides of a bus goes past full scale.
void TestNoLimit()
{
    Settings settings;
    settings.constant = true;
    settings.volume = 0x7FFF;
    settings.target = 0x7FFF;
    settings.wet = 0x7FFF;
    settings.wet_to_output = true;
    Bench bench(settings, 0);
    std::vector<float> out;
    bench.player.Run(320, out);
    constexpr double kGainOne = 32767.0 / 32768.0;
    const double expected = kConstant * kGainOne * kGainOne * (1.0 + kGainOne);
    XSFLIB_CHECK(std::abs(out[400] - expected) < 2.0);
}

// A mute ramps across the frame that it first reaches. The frame after that is silent, on both buses.
void TestMute()
{
    Settings settings;
    settings.constant = true;
    Bench bench(settings, 0);
    std::vector<float> out;
    bench.player.Run(640, out);
    bench.player.MuteTrack(kTrack, true);
    std::vector<float> muted;
    bench.player.Run(640, muted);
    constexpr double kLevel = kConstant * kGain;
    constexpr std::size_t kFrame = kChunkSamples * 2;
    for (std::size_t i = 0; i < kFrame; i++)
    {
        const double share = 1.0 - ((static_cast<double>(i) + 1.0) / kFrame);
        XSFLIB_CHECK(std::abs(muted[i * 2] - (kLevel * share)) < 2.0);
    }

    for (std::size_t i = kFrame; i < muted.size() / 2; i++)
    {
        XSFLIB_CHECK(muted[i * 2] == 0.0F);
    }
}

// A track that's muted from the start never sounds.
void TestMutedFromTheStart()
{
    Bench bench({}, 48000);
    bench.player.MuteTrack(kTrack, true);
    std::vector<float> out;
    bench.player.Run(4000, out);
    for (const float sample : out)
    {
        XSFLIB_CHECK(sample == 0.0F);
    }
}

// The meters add up what the voice adds, and change nothing.
void TestMeters()
{
    Settings settings;
    settings.constant = true;
    Bench plain(settings, 0);
    std::vector<float> plain_out;
    plain.player.Run(3200, plain_out);

    Bench metered(settings, 0);
    metered.player.MeterTracks(true);
    std::vector<float> metered_out;
    metered.player.Run(3200, metered_out);
    XSFLIB_CHECK(plain_out == metered_out);

    // A run on from the start, where the interpolator rings at the wave's first samples.
    metered.player.Run(320, metered_out);
    const std::array<int32_t, 2> peaks = metered.player.TrackPeaks(kTrack);
    const auto expected = static_cast<int32_t>(kConstant * kGain);
    XSFLIB_CHECK(std::abs(peaks[0] - expected) <= 2);
    XSFLIB_CHECK(std::abs(peaks[1] - expected) <= 2);
    XSFLIB_CHECK(metered.player.TrackPeaks(kTrack + 1)[0] == 0);
}

// A snapshot takes back the frames ahead and every state of the mix.
void TestSnapshots()
{
    for (const int rate : {0, 48000})
    {
        Settings settings;
        settings.wet = 0x4000;
        settings.echo = true;
        Bench bench(settings, rate);
        std::vector<float> first;
        bench.player.Run(1234, first);
        const n64::FramePlayer::Snapshot snapshot = bench.player.Save();
        std::vector<float> once;
        bench.player.Run(3000, once);
        bench.player.Restore(snapshot);
        std::vector<float> twice;
        bench.player.Run(3000, twice);
        XSFLIB_CHECK(once == twice);
        XSFLIB_CHECK(bench.player.Position() == 4234);
    }
}

// Frames without mixing move the position on, and the next frames mix. The sine goes on at its place, once the
// interpolator's window has filled with new inputs.
void TestRunWithoutMixing()
{
    Bench bench({}, 0);
    std::vector<float> start;
    bench.player.Run(1600, start);
    bench.player.Run(1600);
    XSFLIB_CHECK(bench.player.Position() == 3200);
    std::vector<float> out;
    bench.player.Run(1600, out);
    XSFLIB_CHECK(bench.player.Position() == 4800);
    XSFLIB_CHECK(out.size() == 3200);
    for (std::size_t i = 800; i < out.size() / 2; i++)
    {
        const double expected = Tone(3200.0 + static_cast<double>(i) - kTapLag, kGain);
        XSFLIB_CHECK(std::abs(out[i * 2] - expected) < kAmplitude * kGain * 0.002);
    }
}

// The later microcode runs its mixer in groups of 16 samples. A list's length of 184 then takes one step past the
// ramp's end, and a release to zero wraps the gain to nearly 1. The mode ramps over the list's length: the step of each
// 8 samples, 23 times.
void TestRampEndsWithTheLength()
{
    constexpr uint32_t kSetRamps = 0x12;
    constexpr uint32_t kSetGains = 0x16;
    constexpr uint32_t kMix = 0x13;
    constexpr uint32_t kLength = 184;
    constexpr uint32_t kLeftRamp = 0xFF80; // -128
    constexpr uint32_t kRightRamp = 0x0040;
    constexpr uint32_t kWetRamp = 0x0010;
    constexpr uint32_t kWetGain = 0x10;
    constexpr uint32_t kLeftGain = 2944; // 128 * 23
    constexpr uint32_t kRightGain = 1000;
    constexpr float kFull = 1.0F / 65536.0F;

    std::vector<uint8_t> image(0x1000, 0);
    n64::AudioMemory memory(image, 0x10000);
    n64::AudioRsp rsp(n64::AudioMicrocode::kEadSecond);
    n64::AudioOps ops;
    rsp.RecordOps(&ops);
    rsp.StartTask();
    const std::array<n64::AudioCommand, 3> commands = {
        n64::AudioCommand{.w0 = (kSetRamps << 24) | (kWetGain << 16) | kWetRamp, .w1 = (kLeftRamp << 16) | kRightRamp},
        n64::AudioCommand{.w0 = kSetGains << 24, .w1 = (kLeftGain << 16) | kRightGain},
        n64::AudioCommand{.w0 = (kMix << 24) | (kLength << 8), .w1 = 0x10203040},
    };
    rsp.Run(commands, memory);

    XSFLIB_CHECK(ops.envs.size() == 1);
    const n64::AudioEnvOp& env = ops.envs[0];
    XSFLIB_CHECK(env.bytes == kLength * 2);
    XSFLIB_CHECK(env.from_left == static_cast<float>(kLeftGain) * kFull);
    XSFLIB_CHECK(env.to_left == 0.0F);
    XSFLIB_CHECK(env.to_right == static_cast<float>(kRightGain + (0x40 * 23)) * kFull);
    XSFLIB_CHECK(env.from_wet == static_cast<float>(0x1000) * kFull);
    XSFLIB_CHECK(env.to_wet == static_cast<float>(0x1000 + (0x10 * 23)) * kFull);
}

// The 16-bit run rounds the same samples.
void TestSixteenBits()
{
    Bench as_floats({}, 0);
    std::vector<float> floats;
    as_floats.player.Run(1000, floats);
    Bench as_shorts({}, 0);
    std::vector<int16_t> shorts;
    as_shorts.player.Run(1000, shorts);
    XSFLIB_CHECK(shorts.size() == floats.size());
    for (std::size_t i = 0; i < shorts.size(); i++)
    {
        XSFLIB_CHECK(shorts[i] == static_cast<int16_t>(std::floor(floats[i] + 0.5F)));
    }
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("n64_enhanced",
                                    []
                                    {
                                        TestToneAtTheGamesRate();
                                        TestPitch();
                                        TestHighPitch();
                                        TestToneAtAHigherRate();
                                        TestToneAtAMultiple();
                                        TestWetPath();
                                        TestEffectDelay();
                                        TestRamp();
                                        TestRampEndsWithTheLength();
                                        TestNoLimit();
                                        TestMute();
                                        TestMutedFromTheStart();
                                        TestMeters();
                                        TestSnapshots();
                                        TestRunWithoutMixing();
                                        TestSixteenBits();
                                    });
}
