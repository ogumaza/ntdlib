// SPDX-License-Identifier: MIT

// The enhanced mixing mode of the 3DS's archive mode (ntdlib/ctr/snd/engine.h) on the model of the DSP's audio firmware
// (ntdlib/ctr/dsp/firmware_model.h). Sound 0 plays a sine of 32 samples a period on a looped PCM16 wave at the DSP's
// rate. Sound 1 starts 40 notes at once on a constant PCM16 wave. Sound 2 plays notes on a looped DSP-ADPCM wave, a
// PCM8 wave and the constant wave at several keys: three at once, then one at a time. Sound 3 plays the sine with sends
// to both aux buses.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <numbers>
#include <span>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/ctr/dsp/dsp.h"
#include "ntdlib/ctr/snd/engine.h"
#include "ntdlib/ctr/snd/sar_options.h"
#include "ntdlib/ctr/snd/util.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/testing/sar_files.h"

namespace
{

namespace dsp = ntdlib::ctr::dsp;
namespace files = ntdlib::testing::sar_files;
namespace sar = ntdlib::sar;
using ntdlib::ctr::snd::ArchiveModel;
using ntdlib::ctr::snd::DspKind;

// The frames of a second of output: 160 samples each at about 32,728 Hz, or 235 in the enhanced mode.
constexpr int kSecond = 205;
constexpr std::size_t kEnhancedFrame = 2 * dsp::kEnhancedFrameSamples;

// The sine's period in samples of its wave, and its amplitude. The wave plays at the DSP's rate.
constexpr int kPeriod = 32;
constexpr double kAmplitude = 4000.0;

// A looped PCM16 wave of 100 periods of the sine, at the DSP's rate.
files::WaveSpec SineWave()
{
    constexpr uint32_t kLength = 100 * kPeriod;
    std::vector<uint8_t> bytes(2 * kLength);
    for (uint32_t i = 0; i < kLength; i++)
    {
        const auto value =
            static_cast<int16_t>(std::lround(kAmplitude * std::sin(2.0 * std::numbers::pi * i / kPeriod)));
        bytes[2 * i] = static_cast<uint8_t>(value);
        bytes[2 * i + 1] = static_cast<uint8_t>(static_cast<uint16_t>(value) >> 8);
    }

    files::WaveSpec wave;
    wave.encoding = 1;
    wave.loop = true;
    wave.sample_rate = 32728;
    wave.loop_end = kLength;
    wave.channels = {bytes};

    return wave;
}

// A looped DSP-ADPCM wave of 400 frames of 14 samples, from a fixed run of numbers. The loop runs from frame 100 to the
// end.
files::WaveSpec AdpcmWave()
{
    std::vector<uint8_t> bytes(400 * 8);
    uint32_t state = 1;
    for (std::size_t i = 0; i < bytes.size(); i++)
    {
        state = state * 1103515245u + 12345u;
        const auto value = static_cast<uint8_t>(state >> 16);
        bytes[i] = i % 8 == 0 ? static_cast<uint8_t>((value & 0x70) | (value % 12)) : value;
    }

    files::WaveSpec wave;
    wave.encoding = 2;
    wave.loop = true;
    wave.loop_start = 100 * 14;
    wave.loop_end = 400 * 14;
    wave.channels = {bytes};

    return wave;
}

// A PCM8 wave of a sawtooth, 0.5 s long. It plays once.
files::WaveSpec Pcm8Wave()
{
    std::vector<uint8_t> bytes(16000);
    for (std::size_t i = 0; i < bytes.size(); i++)
    {
        bytes[i] = static_cast<uint8_t>(3 * i);
    }

    files::WaveSpec wave;
    wave.encoding = 0;
    wave.loop_end = static_cast<uint32_t>(bytes.size());
    wave.channels = {bytes};

    return wave;
}

// The archive. Program 0 plays the constant wave, program 1 the DSP-ADPCM wave, program 2 the PCM8 wave and program 3
// the sine, each from original key 60.
std::vector<uint8_t> Archive()
{
    constexpr sar::Flavor kCtr = sar::Flavor::kCtr;
    files::ArchiveSpec spec;
    spec.sounds = {{.name = "SEQ_SINE", .file = 0, .banks = {0}},
                   {.name = "SEQ_CHORD", .file = 3, .banks = {0}},
                   {.name = "SEQ_NOTES", .file = 4, .banks = {0}},
                   {.name = "SEQ_SENDS", .file = 5, .banks = {0}}};
    spec.banks = {{"BANK", 1}};
    spec.wave_archives = {{"WARC", 2}};
    spec.players = {{"PLAYER", 2}};

    // Program 3, key 60 at velocity 127 for 3 s: 288 ticks at the default tempo. The sends take bus A at 64 and bus B
    // at 100.
    const std::vector<uint8_t> sine = {0x81, 0x03, 0x3C, 0x7F, 0x82, 0x20, 0xFF};
    std::vector<uint8_t> sends = {0xD9, 0x40, 0xDA, 0x64};
    sends.insert(sends.end(), sine.begin(), sine.end());

    // With the note wait off, keys 30 to 69 at velocity 30 for 96 ticks, then a wait of 96 ticks.
    std::vector<uint8_t> chord = {0xC7, 0x00};
    for (uint8_t key = 30; key < 70; key++)
    {
        chord.insert(chord.end(), {key, 0x1E, 0x60});
    }
    chord.insert(chord.end(), {0x80, 0x60, 0xFF});

    const std::vector<uint8_t> notes = {0xC0, 0x30, 0xC7, 0x00, 0x81, 0x01, 0x40, 0x7F, 0x60, 0x81, 0x02, 0x37, 0x50,
                                        0x60, 0x81, 0x00, 0x48, 0x7F, 0x60, 0x80, 0x60, 0xC7, 0x01, 0x81, 0x01, 0x30,
                                        0x7F, 0x30, 0x81, 0x02, 0x3C, 0x7F, 0x30, 0x81, 0x00, 0x43, 0x7F, 0x30, 0xFF};
    spec.files = {
        {files::Sequence(kCtr, sine, {{"SEQ_SINE", 0}})},
        {files::Bank(kCtr, files::OneRegionBank(4))},
        {files::WaveArchive(kCtr, {files::Wave(kCtr, files::ConstantWave(false, 64000)), files::Wave(kCtr, AdpcmWave()),
                                   files::Wave(kCtr, Pcm8Wave()), files::Wave(kCtr, SineWave())})},
        {files::Sequence(kCtr, chord, {{"SEQ_CHORD", 0}})},
        {files::Sequence(kCtr, notes, {{"SEQ_NOTES", 0}})},
        {files::Sequence(kCtr, sends, {{"SEQ_SENDS", 0}})}};

    return files::Archive(kCtr, spec);
}

// A model and its DSP's output: the firmware's samples, and each frame of the enhanced mode's output in floats.
struct Model
{
    Model(DspKind kind, const sar::SoundArchive& archive) : model(std::make_unique<ArchiveModel>(kind))
    {
        const auto keep_sample = [this](std::array<int16_t, 2> sample)
        {
            strict.insert(strict.end(), sample.begin(), sample.end());
        };
        model->dsp->SetSampleSink(keep_sample);

        const auto keep_frame = [this](std::span<const float> frame)
        {
            XSFLIB_CHECK(frame.size() == kEnhancedFrame);
            enhanced.insert(enhanced.end(), frame.begin(), frame.end());
            frames++;
        };
        model->dsp->SetFrameSink(keep_frame);

        XSFLIB_CHECK(model->Start(archive));
    }

    bool Run(int frames_to_run)
    {
        for (int i = 0; i < frames_to_run; i++)
        {
            if (!model->engine->RunFrame())
            {
                return false;
            }
        }

        return true;
    }

    std::unique_ptr<ArchiveModel> model;
    std::vector<int16_t> strict;
    std::vector<float> enhanced;
    int frames = 0;
};

// The magnitude of the frequency `cycles` cycles a sample in every `stride`th value of `samples`: the sine's amplitude.
// The values are interleaved stereo by default, and the magnitude is the left channel's.
template <typename T>
double Magnitude(std::span<const T> samples, double cycles, std::size_t stride = 2)
{
    double re = 0.0;
    double im = 0.0;
    const std::size_t count = samples.size() / stride;
    for (std::size_t n = 0; n < count; n++)
    {
        const double angle = 2.0 * std::numbers::pi * cycles * static_cast<double>(n);
        re += static_cast<double>(samples[stride * n]) * std::cos(angle);
        im += static_cast<double>(samples[stride * n]) * std::sin(angle);
    }

    return 2.0 * std::hypot(re, im) / static_cast<double>(count);
}

// The voices that nn::snd plays.
int Playing(const ArchiveModel& model)
{
    int playing = 0;
    for (int id = 0; id < model.snd.VoiceCount(); id++)
    {
        playing += model.snd.PlaceOf(id).playing ? 1 : 0;
    }

    return playing;
}

// With fewer voices than the console has, the enhanced mode steps every voice through its buffers as the console
// does: every frame's reports match. Its output sounds while the firmware's output stays silent, and each frame that
// the DSP mixes gives one frame of output.
void TestSteps(const sar::SoundArchive& archive)
{
    Model strict(DspKind::kModel, archive);
    Model enhanced(DspKind::kEnhancedModel, archive);
    XSFLIB_CHECK(strict.model->engine->StartSequence(2) != nullptr);
    XSFLIB_CHECK(enhanced.model->engine->StartSequence(2) != nullptr);
    int sounding = 0;
    for (int frame = 0; frame < 3 * kSecond; frame++)
    {
        XSFLIB_CHECK(strict.Run(1) && enhanced.Run(1));
        XSFLIB_CHECK(enhanced.model->snd.VoiceCount() == strict.model->snd.VoiceCount());
        for (int id = 0; id < strict.model->snd.VoiceCount(); id++)
        {
            const auto a = strict.model->snd.PlaceOf(id);
            const auto b = enhanced.model->snd.PlaceOf(id);
            XSFLIB_CHECK(a.playing == b.playing && a.loops == b.loops && a.position == b.position);
        }

        sounding += Playing(*enhanced.model) > 0 ? 1 : 0;
    }

    XSFLIB_CHECK(sounding > kSecond);
    XSFLIB_CHECK(
        std::all_of(enhanced.strict.begin(), enhanced.strict.end(), [](int16_t sample) { return sample == 0; }));
    XSFLIB_CHECK(
        std::any_of(enhanced.enhanced.begin(), enhanced.enhanced.end(), [](float sample) { return sample != 0.0f; }));
    XSFLIB_CHECK(std::abs(enhanced.frames - 3 * kSecond) <= 2);
    XSFLIB_CHECK(enhanced.model->engine->Channels().ActiveCount() == strict.model->engine->Channels().ActiveCount());
}

// The sine plays at the console's pitch and level. Its wave plays at the DSP's rate, at a period of 32 samples. The
// sine's frequency is therefore a 32nd of the DSP's rate. The enhanced mode's output takes 235 samples in the time of
// the DSP's 160. The firmware's resampler and limiter change the sine's level by less than 0.1 dB. Nothing else
// sounds: the harmonics lie 60 dB below the sine.
void TestSine(const sar::SoundArchive& archive)
{
    Model strict(DspKind::kModel, archive);
    Model enhanced(DspKind::kEnhancedModel, archive);
    XSFLIB_CHECK(strict.model->engine->StartSequence(0) != nullptr);
    XSFLIB_CHECK(enhanced.model->engine->StartSequence(0) != nullptr);
    XSFLIB_CHECK(strict.Run(2 * kSecond) && enhanced.Run(2 * kSecond));

    // A second from 0.5 s on.
    const std::span<const int16_t> strict_second =
        std::span<const int16_t>(strict.strict).subspan(kSecond * 160, 2 * kSecond * 160);
    const std::span<const float> enhanced_second =
        std::span<const float>(enhanced.enhanced).subspan(kSecond / 2 * kEnhancedFrame, kSecond * kEnhancedFrame);
    const double strict_level = Magnitude(strict_second, 1.0 / kPeriod);
    const double enhanced_level =
        32768.0 * Magnitude(enhanced_second, 1.0 / kPeriod * 160.0 / dsp::kEnhancedFrameSamples);
    XSFLIB_CHECK(strict_level > 1000.0);
    XSFLIB_CHECK(std::abs(20.0 * std::log10(enhanced_level / strict_level)) < 0.1);
    for (int harmonic = 2; harmonic <= 5; harmonic++)
    {
        const double level = 32768.0 * Magnitude(enhanced_second, harmonic / static_cast<double>(kPeriod) * 160.0 /
                                                                      dsp::kEnhancedFrameSamples);
        XSFLIB_CHECK(level < enhanced_level / 1000.0);
    }

    // The two sides play the centre alike.
    double difference = 0.0;
    for (std::size_t n = 0; n + 1 < enhanced_second.size(); n += 2)
    {
        difference = std::max<double>(difference, std::abs(enhanced_second[n] - enhanced_second[n + 1]));
    }
    XSFLIB_CHECK(difference < 1e-6);
}

// The console plays 23 of the 40 notes. The enhanced mode plays all 40: nw::snd's voices and nn::snd's grow past the
// console's. The voices stay once the notes end.
void TestManyVoices(const sar::SoundArchive& archive)
{
    Model strict(DspKind::kModel, archive);
    Model enhanced(DspKind::kEnhancedModel, archive);
    XSFLIB_CHECK(strict.model->engine->StartSequence(1) != nullptr);
    XSFLIB_CHECK(enhanced.model->engine->StartSequence(1) != nullptr);
    XSFLIB_CHECK(strict.Run(kSecond / 2) && enhanced.Run(kSecond / 2));
    XSFLIB_CHECK(Playing(*strict.model) == 23);
    XSFLIB_CHECK(Playing(*enhanced.model) == 40);
    XSFLIB_CHECK(strict.model->snd.VoiceCount() == ntdlib::ctr::nnsnd::kNumSources);
    XSFLIB_CHECK(enhanced.model->snd.VoiceCount() == 40);
    XSFLIB_CHECK(enhanced.model->dsp->SourceCount() == 40);

    XSFLIB_CHECK(enhanced.Run(2 * kSecond));
    XSFLIB_CHECK(Playing(*enhanced.model) == 0 && enhanced.model->snd.VoiceCount() == 40);
}

// A restore of a snapshot plays the frames after it again, sample for sample. A snapshot from before the voices grew
// gives back the console's voices, and the voices grow again as before.
void TestSnapshot(const sar::SoundArchive& archive)
{
    Model model(DspKind::kEnhancedModel, archive);
    const ArchiveModel::Snapshot start = model.model->Save(nullptr);
    XSFLIB_CHECK(model.model->engine->StartSequence(2) != nullptr);
    XSFLIB_CHECK(model.Run(20));
    const ArchiveModel::Snapshot snapshot = model.model->Save(&start);
    model.enhanced.clear();
    XSFLIB_CHECK(model.Run(40));
    const std::vector<float> after = model.enhanced;

    model.model->Restore(snapshot);
    model.enhanced.clear();
    XSFLIB_CHECK(model.Run(40));
    XSFLIB_CHECK(std::any_of(after.begin(), after.end(), [](float sample) { return sample != 0.0f; }));
    XSFLIB_CHECK(model.enhanced == after);

    model.model->Restore(start);
    model.enhanced.clear();
    XSFLIB_CHECK(model.model->engine->StartSequence(1) != nullptr);
    XSFLIB_CHECK(model.Run(20));
    const std::vector<float> grown = model.enhanced;
    XSFLIB_CHECK(model.model->snd.VoiceCount() == 40);

    model.model->Restore(start);
    XSFLIB_CHECK(model.model->snd.VoiceCount() == ntdlib::ctr::nnsnd::kNumSources);
    XSFLIB_CHECK(model.model->dsp->SourceCount() == ntdlib::ctr::nnsnd::kNumSources);
    model.enhanced.clear();
    XSFLIB_CHECK(model.model->engine->StartSequence(1) != nullptr);
    XSFLIB_CHECK(model.Run(20));
    XSFLIB_CHECK(model.enhanced == grown && model.model->snd.VoiceCount() == 40);
}

// A track's meter follows its notes, at the output's level: the enhanced mode has no limiter. The meters change
// nothing in the output.
void TestMeters(const sar::SoundArchive& archive)
{
    Model metered(DspKind::kEnhancedModel, archive);
    Model plain(DspKind::kEnhancedModel, archive);
    const int player = metered.model->engine->StartSequence(2)->Id();
    XSFLIB_CHECK(plain.model->engine->StartSequence(2) != nullptr);
    metered.model->engine->MeterTracks(player);

    int loud_frames = 0;
    for (int i = 0; i < 3 * kSecond; i++)
    {
        XSFLIB_CHECK(metered.Run(1) && plain.Run(1));
        XSFLIB_CHECK(metered.model->engine->TrackPeaks(1) == (std::array<int32_t, 2>{0, 0}));
        const std::span<const int16_t> frame = metered.model->dsp->MixedFrame();
        XSFLIB_CHECK(frame.size() == kEnhancedFrame);
        int32_t loudest = 0;
        for (const int16_t sample : frame)
        {
            loudest = std::max(loudest, std::abs(int32_t{sample}));
        }

        const std::array<int32_t, 2> peaks = metered.model->engine->TrackPeaks(0);
        const int32_t meter = std::max(peaks[0], peaks[1]);
        if (loudest >= 1000)
        {
            loud_frames++;
            XSFLIB_CHECK(std::abs(meter - loudest) <= 2);
        }
    }

    XSFLIB_CHECK(loud_frames > kSecond / 2);
    XSFLIB_CHECK(metered.enhanced == plain.enhanced);
}

// What the callbacks of the aux buses got: each frame's front left channel, and the frames' lengths.
struct AuxTaps
{
    std::array<std::vector<int32_t>, 2> samples;
    std::array<std::vector<std::size_t>, 2> lengths;
};

// Gives `model` a callback on each aux bus that keeps the bus's front left channel in `taps`. It leaves the samples as
// they are, or with `silence`, it silences the bus.
void TapAux(ArchiveModel& model, AuxTaps& taps, bool silence = false)
{
    for (int bus = 0; bus < 2; bus++)
    {
        std::vector<int32_t>& samples = taps.samples[static_cast<std::size_t>(bus)];
        std::vector<std::size_t>& lengths = taps.lengths[static_cast<std::size_t>(bus)];
        const auto tap = [&samples, &lengths, silence](const std::array<int32_t*, 4>& channels, std::size_t count)
        {
            samples.insert(samples.end(), channels[0], channels[0] + count);
            lengths.push_back(count);
            if (silence)
            {
                for (int32_t* const channel : channels)
                {
                    std::fill(channel, channel + count, 0);
                }
            }
        };
        model.snd.SetAuxCallback(bus, tap);
    }
}

// The difference of two outputs, value by value, up to the shorter one's end.
template <typename T>
std::vector<double> Difference(const std::vector<T>& a, const std::vector<T>& b)
{
    std::vector<double> difference(std::min(a.size(), b.size()));
    for (std::size_t i = 0; i < difference.size(); i++)
    {
        difference[i] = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    }

    return difference;
}

// The first frame of `frame` values in which `samples` don't all lie within `floor` of 0, or -1.
template <typename T>
int FirstFrame(std::span<const T> samples, std::size_t frame, double floor)
{
    for (std::size_t i = 0; i < samples.size(); i++)
    {
        if (std::abs(static_cast<double>(samples[i])) > floor)
        {
            return static_cast<int>(i / frame);
        }
    }

    return -1;
}

// The aux buses of the enhanced mode take frames of 235 samples. A bus's samples reach its callback on the firmware's
// scale: the sine on each bus lies at the console's level, within 0.1 dB. The buses come back at the return volume two
// frames after their mix, at the console's level against the main bus. A callback's changes come back, and without a
// callback the bus adds nothing. A restore of a snapshot plays the buses' frames again, sample for sample.
void TestAux(const sar::SoundArchive& archive)
{
    Model strict_dry(DspKind::kModel, archive);
    Model strict_wet(DspKind::kModel, archive);
    Model dry(DspKind::kEnhancedModel, archive);
    Model wet(DspKind::kEnhancedModel, archive);
    Model silenced(DspKind::kEnhancedModel, archive);
    AuxTaps strict_taps;
    AuxTaps taps;
    AuxTaps silenced_taps;
    TapAux(*strict_wet.model, strict_taps);
    TapAux(*wet.model, taps);
    TapAux(*silenced.model, silenced_taps, true);
    for (Model* model : {&strict_dry, &strict_wet, &dry, &wet, &silenced})
    {
        XSFLIB_CHECK(model->model->engine->StartSequence(3) != nullptr);
        XSFLIB_CHECK(model->Run(2 * kSecond));
    }

    for (std::size_t bus = 0; bus < 2; bus++)
    {
        XSFLIB_CHECK(std::all_of(strict_taps.lengths[bus].begin(), strict_taps.lengths[bus].end(),
                                 [](std::size_t length) { return length == dsp::kSamplesPerFrame; }));
        XSFLIB_CHECK(std::all_of(taps.lengths[bus].begin(), taps.lengths[bus].end(),
                                 [](std::size_t length) { return length == dsp::kEnhancedFrameSamples; }));

        // A second from 0.5 s on.
        const std::span<const int32_t> strict_second =
            std::span<const int32_t>(strict_taps.samples[bus])
                .subspan(kSecond / 2 * dsp::kSamplesPerFrame, kSecond * dsp::kSamplesPerFrame);
        const std::span<const int32_t> enhanced_second =
            std::span<const int32_t>(taps.samples[bus])
                .subspan(kSecond / 2 * dsp::kEnhancedFrameSamples, kSecond * dsp::kEnhancedFrameSamples);
        const double strict_level = Magnitude(strict_second, 1.0 / kPeriod, 1);
        const double enhanced_level = Magnitude(enhanced_second, 1.0 / kPeriod * 160.0 / dsp::kEnhancedFrameSamples, 1);
        XSFLIB_CHECK(strict_level > 100.0);
        XSFLIB_CHECK(std::abs(20.0 * std::log10(enhanced_level / strict_level)) < 0.1);
    }

    // The buses' share of the output, against the main bus's, in a second from 0.5 s on.
    const std::vector<double> strict_returns = Difference(strict_wet.strict, strict_dry.strict);
    const std::vector<double> returns = Difference(wet.enhanced, dry.enhanced);
    const std::size_t strict_start = kSecond * dsp::kSamplesPerFrame;
    const std::size_t strict_length = 2 * kSecond * dsp::kSamplesPerFrame;
    const std::size_t start = kSecond / 2 * kEnhancedFrame;
    const std::size_t length = kSecond * kEnhancedFrame;
    const double cycles = 1.0 / kPeriod * 160.0 / dsp::kEnhancedFrameSamples;
    const double strict_share =
        Magnitude(std::span<const double>(strict_returns).subspan(strict_start, strict_length), 1.0 / kPeriod) /
        Magnitude(std::span<const int16_t>(strict_dry.strict).subspan(strict_start, strict_length), 1.0 / kPeriod);
    const double share = Magnitude(std::span<const double>(returns).subspan(start, length), cycles) /
                         Magnitude(std::span<const float>(dry.enhanced).subspan(start, length), cycles);
    XSFLIB_CHECK(strict_share > 0.1);
    XSFLIB_CHECK(std::abs(20.0 * std::log10(share / strict_share)) < 0.1);

    // The first frame of the buses' share comes two frames after the first frame of the sound, each above the
    // output's least step.
    const double step = 1.0 / 32768.0;
    const int first_sound = FirstFrame(std::span<const float>(dry.enhanced), kEnhancedFrame, step);
    XSFLIB_CHECK(first_sound >= 0);
    XSFLIB_CHECK(FirstFrame(std::span<const double>(returns), kEnhancedFrame, step) == first_sound + 2);

    XSFLIB_CHECK(silenced.enhanced == dry.enhanced);
    XSFLIB_CHECK(silenced_taps.samples[0] == taps.samples[0] && silenced_taps.samples[1] == taps.samples[1]);

    const ArchiveModel::Snapshot snapshot = wet.model->Save(nullptr);
    wet.enhanced.clear();
    XSFLIB_CHECK(wet.Run(40));
    const std::vector<float> after = wet.enhanced;
    wet.model->Restore(snapshot);
    wet.enhanced.clear();
    XSFLIB_CHECK(wet.Run(40));
    XSFLIB_CHECK(wet.enhanced == after);
    XSFLIB_CHECK(std::any_of(after.begin(), after.end(), [](float sample) { return sample != 0.0f; }));
}

// The one-pole's coefficients put its cutoff at the frequency that nw::snd asks for: 3 dB down at 32 kHz on the
// console, and at 48 kHz in the enhanced mode. The biquad's presets take the library's tables for 48 kHz there.
void TestFilters()
{
    for (const bool enhanced : {false, true})
    {
        const double rate = enhanced ? 48000.0 : 32000.0;
        for (const uint16_t frequency : std::array<uint16_t, 4>{500, 2000, 6000, 12000})
        {
            int16_t b0 = 0;
            int16_t a1 = 0;
            ntdlib::ctr::snd::util::CalcMonoFilterCoefficients(frequency, b0, a1, enhanced);
            const double w = 2.0 * std::numbers::pi * frequency / rate;
            const double b = b0 / 32768.0;
            const double a = a1 / 32768.0;
            const double gain = b / std::hypot(1.0 - a * std::cos(w), a * std::sin(w));
            XSFLIB_CHECK(std::abs(20.0 * std::log10(gain) + 3.01) < 0.1);
        }
    }

    for (int type = 1; type <= 5; type++)
    {
        const int16_t* console = ntdlib::ctr::snd::util::BiquadPresetCoefficients(type, 0.5f);
        const int16_t* enhanced = ntdlib::ctr::snd::util::BiquadPresetCoefficients(type, 0.5f, true);
        XSFLIB_CHECK(console != nullptr && enhanced != nullptr && console != enhanced);
    }

    XSFLIB_CHECK(ntdlib::ctr::snd::util::BiquadPresetCoefficients(6, 0.5f, true) == nullptr);
}

} // namespace

int main()
{
    const std::vector<uint8_t> bytes = Archive();
    const sar::SoundArchive archive = sar::SoundArchive::Load(bytes, ntdlib::ctr::kSarOptions);
    TestSteps(archive);
    TestSine(archive);
    TestManyVoices(archive);
    TestSnapshot(archive);
    TestMeters(archive);
    TestAux(archive);
    TestFilters();

    return xsflib::testing::Summary("3DS archive model's enhanced mode");
}
