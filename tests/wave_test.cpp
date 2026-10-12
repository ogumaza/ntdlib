// SPDX-License-Identifier: MIT

// Tests of the wave decoders, on bytes built here and on the synthetic cases in wave_cases.h.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/wave/adpcm.h"
#include "ntdlib/wave/decode.h"
#include "ntdlib/wave/pcm.h"
#include "wave_cases.h"
#include "xsflib/testing/check.h"

using namespace ntdlib::wave;
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;

namespace
{

// The synthetic cases.
constexpr uint64_t kBlockSeed = 1;
constexpr std::size_t kBlockCount = 3000;
constexpr uint64_t kStepSeed = 2;
constexpr std::size_t kStepCount = 200000;

// The hashes of the decoders' output for the block cases. They're fixed: a change to them is a change of ntdlib's
// output. The two DSP-ADPCM hashes come from DecodeDspAdpcm with the sums kWide and kWrap. The PCM16 hashes come from
// DecodePcm16 on little-endian and big-endian data.
constexpr uint64_t kDspAdpcmWideHash = 0x7088ad0d5388c253;
constexpr uint64_t kDspAdpcmWrapHash = 0x8030c333832e5134;
constexpr uint64_t kImaAdpcmHash = 0xf5ca7b19fddf687c; // DecodeImaAdpcm
constexpr uint64_t kAfcHash = 0x897560d57bc6d3d5;      // DecodeAfc
constexpr uint64_t kPcm8Hash = 0xb0280fda19652269;
constexpr uint64_t kPcm16LittleHash = 0x966a0a3292dc9926;
constexpr uint64_t kPcm16BigHash = 0x7e96edebea279272;

// The hashes of the samples for the step cases, one for each DspAdpcmSum form. They're fixed: a change to them is a
// change of ntdlib's output.
constexpr uint64_t kStepWideHash = 0x229cb69a35aca481;
constexpr uint64_t kStepWrapHash = 0x6aa397a4ea33c05b;
constexpr uint64_t kStepNw4rCpuHash = 0xb277700f7a82e14c;
constexpr uint64_t kStepAcceleratorHash = 0x5d11bc52b8d1fbc7;

using BlockDecoder = std::function<std::vector<int16_t>(const synthetic::BlockCase&)>;

// The hash of a decoder's output for every block case, with each case's index.
uint64_t HashBlocks(const std::vector<synthetic::BlockCase>& cases, const BlockDecoder& decode)
{
    synthetic::Hash hash;
    for (std::size_t i = 0; i < cases.size(); i++)
    {
        hash.Add(i, 4);
        hash.Add(decode(cases[i]));
    }

    return hash.Value();
}

// The hash of DspAdpcmSample's sample for every step case.
uint64_t HashSteps(const std::vector<synthetic::StepCase>& cases, DspAdpcmSum sum)
{
    synthetic::Hash hash;
    for (const synthetic::StepCase& c : cases)
    {
        const int16_t s = DspAdpcmSample(c.nibble, c.shift, c.c1, c.c2, c.hist1, c.hist2, sum);
        hash.Add(static_cast<uint16_t>(s), 2);
    }

    return hash.Value();
}

void TestDspAdpcmScalesNibbles()
{
    // Header 0x12: scale 1 << 2, coefficient pair 1. With zero coefficients a sample is nibble * scale.
    const int16_t kCoefs[16] = {};
    const std::vector<uint8_t> kFrame = {0x12, 0x12, 0x34, 0xf0, 0x00, 0x00, 0x00, 0x00};
    const std::vector<int16_t> kExpect = {4, 8, 12, 16, -4, 0};

    for (const DspAdpcmSum sum :
         {DspAdpcmSum::kWide, DspAdpcmSum::kWrap, DspAdpcmSum::kNw4rCpu, DspAdpcmSum::kAccelerator})
    {
        const std::vector<int16_t> out = DecodeDspAdpcm(kFrame, 6, kCoefs, 0, 0, sum);

        XSFLIB_CHECK(out == kExpect);
    }
}

void TestDspAdpcmPredictsFromHistory()
{
    // Coefficient pair 0 = (2048, 0) repeats the previous sample.
    const int16_t kHold[16] = {2048, 0};

    const auto held =
        DecodeDspAdpcm(std::vector<uint8_t>{0x00, 0x00, 0, 0, 0, 0, 0, 0}, 3, kHold, 100, 0, DspAdpcmSum::kWide);

    XSFLIB_CHECK(held.size() == 3 && held[0] == 100 && held[2] == 100);
}

void TestDspAdpcmLargePredictions()
{
    // Two coefficients and two history samples at -32768 sum to 2^31 + 1024. In 64 bits that clamps to 32767. AX's and
    // the Switch's audio renderer's 32-bit sum wraps that round to a negative number before the shift. So do NW4R SND's
    // decoder on the CPU and the DSP's accelerator.
    const int16_t kCoefs[16] = {-32768, -32768};
    const std::vector<uint8_t> kFrame = {0x00, 0x00, 0, 0, 0, 0, 0, 0};

    const auto wide = DecodeDspAdpcm(kFrame, 1, kCoefs, -32768, -32768, DspAdpcmSum::kWide);
    const auto wrap = DecodeDspAdpcm(kFrame, 1, kCoefs, -32768, -32768, DspAdpcmSum::kWrap);
    const auto cpu = DecodeDspAdpcm(kFrame, 1, kCoefs, -32768, -32768, DspAdpcmSum::kNw4rCpu);
    const auto accelerator = DecodeDspAdpcm(kFrame, 1, kCoefs, -32768, -32768, DspAdpcmSum::kAccelerator);

    XSFLIB_CHECK(wide.size() == 1 && wide[0] == 32767);
    XSFLIB_CHECK(wrap.size() == 1 && wrap[0] == -32768);
    XSFLIB_CHECK(cpu.size() == 1 && cpu[0] == -32768);
    XSFLIB_CHECK(accelerator.size() == 1 && accelerator[0] == -32768);
}

void TestDspAdpcmSumsDiffer()
{
    // NW4R SND's decoder on the CPU takes a scale of 2^15 as -2^15.
    XSFLIB_CHECK(DspAdpcmSample(1, 15, 0, 0, 0, 0, DspAdpcmSum::kWide) == 32767);
    XSFLIB_CHECK(DspAdpcmSample(1, 15, 0, 0, 0, 0, DspAdpcmSum::kWrap) == 32767);
    XSFLIB_CHECK(DspAdpcmSample(1, 15, 0, 0, 0, 0, DspAdpcmSum::kNw4rCpu) == -32768);
    XSFLIB_CHECK(DspAdpcmSample(1, 15, 0, 0, 0, 0, DspAdpcmSum::kAccelerator) == 32767);

    // These terms add up to 56 below 2^31. kWrap's rounding passes 2^31 and wraps. kNw4rCpu's rounding doesn't.
    XSFLIB_CHECK(DspAdpcmSample(4, 14, -32768, 32760, -32768, 28679, DspAdpcmSum::kWide) == 32767);
    XSFLIB_CHECK(DspAdpcmSample(4, 14, -32768, 32760, -32768, 28679, DspAdpcmSum::kWrap) == -32768);
    XSFLIB_CHECK(DspAdpcmSample(4, 14, -32768, 32760, -32768, 28679, DspAdpcmSum::kNw4rCpu) == 32767);

    // The accelerator adds the nibble's term after the shift. Its prediction alone stays below 2^31.
    XSFLIB_CHECK(DspAdpcmSample(4, 14, -32768, 32760, -32768, 28679, DspAdpcmSum::kAccelerator) == 32767);
}

void TestDspAdpcmStopsWhereTheDataEnds()
{
    // A whole frame of 14 samples, then a header and three bytes of the next frame: 6 more samples.
    const int16_t kCoefs[16] = {};
    const std::vector<uint8_t> kData(12, 0x11);

    const auto out = DecodeDspAdpcm(kData, 100, kCoefs, 0, 0, DspAdpcmSum::kWide);

    XSFLIB_CHECK(out.size() == 20);
}

void TestImaAdpcm()
{
    // Nibble 7 at step index 0 adds 7/8 + 7/4 + 7/2 + 7, each rounded down: 11. It moves the index to 8. There, nibble
    // 0 adds 16/8. A step index past 88 counts as 88.
    const auto out = DecodeImaAdpcm(std::vector<uint8_t>{0x07}, 2, 0, 0);
    const auto clamped = DecodeImaAdpcm(std::vector<uint8_t>{0x00}, 1, 0, 200);

    XSFLIB_CHECK(out == (std::vector<int16_t>{11, 13}));
    XSFLIB_CHECK(clamped == (std::vector<int16_t>{4095}));
}

void TestAfc()
{
    // A frame of predictor 1 and nothing else, and a frame whose first nibble is 1 at a shift of 2 (predictor 0).
    const std::vector<uint8_t> kFrame = {0x01, 0, 0, 0, 0, 0, 0, 0, 0};
    const std::vector<uint8_t> kStep = {0x20, 0x10, 0, 0, 0, 0, 0, 0, 0};

    const std::vector<int16_t> samples = DecodeAfc(kFrame, 16, 100, 0);
    const std::vector<int16_t> stepped = DecodeAfc(kStep, 2, 0, 0);
    const std::vector<int16_t> cut = DecodeAfc(std::vector<uint8_t>(17, 0), 32, 0, 0);

    Check(samples.size() == 16, "a frame holds 16 samples");
    Check(samples.size() == 16 && samples[0] == 100 && samples[15] == 100, "predictor 1 repeats the last sample");
    Check(stepped.size() == 2 && stepped[0] == 4 && stepped[1] == 0, "a nibble scales by its frame's shift");
    Check(cut.size() == 16, "a frame that the data cuts short isn't decoded");
}

void TestPcm()
{
    const std::vector<uint8_t> kBytes = {0x80, 0x7f, 0x01};

    const auto pcm8 = DecodePcm8(kBytes, 10);
    const auto little = DecodePcm16(kBytes, 10, false);
    const auto big = DecodePcm16(kBytes, 10, true);
    const auto limited = DecodePcm8(kBytes, 2);

    XSFLIB_CHECK(pcm8 == (std::vector<int16_t>{-32768, 32512, 256}));
    XSFLIB_CHECK(little == (std::vector<int16_t>{0x7f80}));
    XSFLIB_CHECK(big == (std::vector<int16_t>{-32641})); // 0x807f
    XSFLIB_CHECK(limited.size() == 2);
}

// A wave of `encoding`, `samples` samples long, with a channel for each span of `data`.
EncodedWave WaveOf(Encoding encoding, uint32_t samples, std::initializer_list<std::span<const uint8_t>> data)
{
    EncodedWave encoded;
    encoded.encoding = encoding;
    encoded.loop_end = samples;
    for (const std::span<const uint8_t> d : data)
    {
        encoded.channels.push_back({.data = d});
    }

    return encoded;
}

void TestDecodeWaveEncodings()
{
    // Each encoding decodes as its decoder does, loop_end samples a channel. PCM16 takes the wave's byte order, and
    // IMA-ADPCM the channel's first sample and step index.
    const std::vector<uint8_t> kBytes = {0x80, 0x7f, 0x01, 0x02};
    const std::vector<uint8_t> kImaBytes = {0x07, 0x31};
    EncodedWave little = WaveOf(Encoding::kPcm16, 2, {kBytes});
    little.big_endian = false;
    EncodedWave ima = WaveOf(Encoding::kImaAdpcm, 3, {kImaBytes});
    ima.channels[0].ima_sample = 100;
    ima.channels[0].ima_step_index = 8;

    const Pcm pcm8 = DecodeWave(WaveOf(Encoding::kPcm8, 3, {kBytes}));
    const Pcm big_pcm = DecodeWave(WaveOf(Encoding::kPcm16, 2, {kBytes}));
    const Pcm little_pcm = DecodeWave(little);
    const Pcm ima_pcm = DecodeWave(ima);

    XSFLIB_CHECK(pcm8.channels.size() == 1 && pcm8.channels[0] == DecodePcm8(kBytes, 3));
    XSFLIB_CHECK(big_pcm.channels.size() == 1 && big_pcm.channels[0] == DecodePcm16(kBytes, 2, true));
    XSFLIB_CHECK(little_pcm.channels.size() == 1 && little_pcm.channels[0] == DecodePcm16(kBytes, 2, false));
    XSFLIB_CHECK(ima_pcm.channels.size() == 1 && ima_pcm.channels[0] == DecodeImaAdpcm(kImaBytes, 3, 100, 8));
}

void TestDecodeWaveDspAdpcm()
{
    // The channel's coefficients and history, and the wave's sum, reach the decoder. Two coefficients and two history
    // samples at -32768 clamp to 32767 in 64 bits, and wrap to -32768 in 32 bits.
    const std::array<int16_t, 16> kCoefs = {-32768, -32768};
    const std::vector<uint8_t> kFrame = {0x00, 0x00, 0, 0, 0, 0, 0, 0};
    EncodedWave wide = WaveOf(Encoding::kDspAdpcm, 1, {kFrame});
    wide.channels[0].coefs = &kCoefs;
    wide.channels[0].yn1 = -32768;
    wide.channels[0].yn2 = -32768;
    EncodedWave wrap = wide;
    wrap.sum = DspAdpcmSum::kWrap;

    const Pcm wide_pcm = DecodeWave(wide);
    const Pcm wrap_pcm = DecodeWave(wrap);

    XSFLIB_CHECK(wide_pcm.channels.size() == 1 && wide_pcm.channels[0] == (std::vector<int16_t>{32767}));
    XSFLIB_CHECK(wrap_pcm.channels.size() == 1 && wrap_pcm.channels[0] == (std::vector<int16_t>{-32768}));
}

void TestDecodeWaveChannelsAndLoop()
{
    // Only the first two channels decode. A third channel too short for the wave isn't read. The rate and the loop come
    // through. A loop that doesn't end after its start plays as no loop.
    const std::vector<uint8_t> kLeft = {1, 2, 3};
    const std::vector<uint8_t> kRight = {4, 5, 6};
    const std::vector<uint8_t> kShort = {7};
    EncodedWave three = WaveOf(Encoding::kPcm8, 3, {kLeft, kRight, kShort});
    three.sample_rate = 22050;
    three.loop = true;
    three.loop_start = 1;
    EncodedWave empty_loop = WaveOf(Encoding::kPcm8, 3, {kLeft});
    empty_loop.loop = true;
    empty_loop.loop_start = 3;

    const Pcm pcm = DecodeWave(three);
    const Pcm unlooped = DecodeWave(empty_loop);

    XSFLIB_CHECK(pcm.channels.size() == 2 && pcm.channels[0] == DecodePcm8(kLeft, 3) &&
                 pcm.channels[1] == DecodePcm8(kRight, 3));
    XSFLIB_CHECK(pcm.sample_rate == 22050 && pcm.loop && pcm.loop_start == 1 && pcm.loop_end == 3 && pcm.Length() == 3);
    XSFLIB_CHECK(!unlooped.loop && unlooped.loop_start == 3 && unlooped.Length() == 3);
}

void TestDecodeWaveErrors()
{
    // An encoding that can't be decoded, a DSP-ADPCM channel without coefficients, a channel shorter than the wave and
    // a wave without channels are errors. A wave without channels is reported as such, whatever its encoding.
    const std::vector<uint8_t> kBytes = {1, 2, 3, 4};
    const std::vector<uint8_t> kShort = {1};

    const std::string unknown = ErrorOf([&] { DecodeWave(WaveOf(Encoding::kUnknown, 2, {kBytes})); });
    const std::string no_coefs = ErrorOf([&] { DecodeWave(WaveOf(Encoding::kDspAdpcm, 2, {kBytes})); });
    const std::string short_right = ErrorOf([&] { DecodeWave(WaveOf(Encoding::kPcm8, 2, {kBytes, kShort})); });
    const std::string empty = ErrorOf([&] { DecodeWave(WaveOf(Encoding::kPcm8, 2, {})); });
    const std::string empty_unknown = ErrorOf([&] { DecodeWave(WaveOf(Encoding::kUnknown, 2, {})); });

    XSFLIB_CHECK(unknown == "unknown wave encoding");
    XSFLIB_CHECK(no_coefs == "DSP-ADPCM wave without coefficients");
    XSFLIB_CHECK(short_right == "wave data shorter than the declared sample count");
    XSFLIB_CHECK(empty == "wave has no channels");
    XSFLIB_CHECK(empty_unknown == "wave has no channels");
}

void TestGoldenBlocks()
{
    const std::vector<synthetic::BlockCase> cases = synthetic::BlockCases(kBlockSeed, kBlockCount);
    auto dsp = [](DspAdpcmSum sum)
    {
        return [sum](const synthetic::BlockCase& c)
        {
            return DecodeDspAdpcm(c.data, c.samples, c.coefs, c.hist1, c.hist2, sum);
        };
    };
    auto ima = [](const synthetic::BlockCase& c)
    {
        return DecodeImaAdpcm(c.data, c.samples, c.hist1, c.index);
    };
    auto afc = [](const synthetic::BlockCase& c)
    {
        return DecodeAfc(c.data, c.samples, c.hist1, c.hist2);
    };
    auto pcm8 = [](const synthetic::BlockCase& c)
    {
        return DecodePcm8(c.data, c.samples);
    };
    auto pcm16 = [](bool big_endian)
    {
        return [big_endian](const synthetic::BlockCase& c)
        {
            return DecodePcm16(c.data, c.samples, big_endian);
        };
    };

    Check(HashBlocks(cases, dsp(DspAdpcmSum::kWide)) == kDspAdpcmWideHash, "DSP-ADPCM, kWide, on the block cases");
    Check(HashBlocks(cases, dsp(DspAdpcmSum::kWrap)) == kDspAdpcmWrapHash, "DSP-ADPCM, kWrap, on the block cases");
    Check(HashBlocks(cases, ima) == kImaAdpcmHash, "IMA-ADPCM on the block cases");
    Check(HashBlocks(cases, afc) == kAfcHash, "AFC on the block cases");
    Check(HashBlocks(cases, pcm8) == kPcm8Hash, "PCM8 on the block cases");
    Check(HashBlocks(cases, pcm16(false)) == kPcm16LittleHash, "little-endian PCM16 on the block cases");
    Check(HashBlocks(cases, pcm16(true)) == kPcm16BigHash, "big-endian PCM16 on the block cases");
}

void TestGoldenSteps()
{
    const std::vector<synthetic::StepCase> cases = synthetic::StepCases(kStepSeed, kStepCount);

    const uint64_t wide = HashSteps(cases, DspAdpcmSum::kWide);
    const uint64_t wrap = HashSteps(cases, DspAdpcmSum::kWrap);
    const uint64_t cpu = HashSteps(cases, DspAdpcmSum::kNw4rCpu);
    const uint64_t accelerator = HashSteps(cases, DspAdpcmSum::kAccelerator);

    Check(wide == kStepWideHash, "DspAdpcmSample, kWide, on the step cases");
    Check(wrap == kStepWrapHash, "DspAdpcmSample, kWrap, on the step cases");
    Check(cpu == kStepNw4rCpuHash, "DspAdpcmSample, kNw4rCpu, on the step cases");
    Check(accelerator == kStepAcceleratorHash, "DspAdpcmSample, kAccelerator, on the step cases");
}

} // namespace

int main()
{
    TestDspAdpcmScalesNibbles();
    TestDspAdpcmPredictsFromHistory();
    TestDspAdpcmLargePredictions();
    TestDspAdpcmSumsDiffer();
    TestDspAdpcmStopsWhereTheDataEnds();
    TestImaAdpcm();
    TestAfc();
    TestPcm();
    TestDecodeWaveEncodings();
    TestDecodeWaveDspAdpcm();
    TestDecodeWaveChannelsAndLoop();
    TestDecodeWaveErrors();
    TestGoldenBlocks();
    TestGoldenSteps();

    if (xsflib::testing::failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", xsflib::testing::failures);
        return 1;
    }

    std::printf("wave tests passed\n");

    return 0;
}
