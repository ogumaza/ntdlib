// SPDX-License-Identifier: MIT

// Tests of the DSP-ADPCM encoder (ntdlib/wave/encode.h). The decoder of adpcm.h decodes what the encoder writes, and
// each test compares the decoded samples with the input. The bounds follow from the format. A predictor of two samples
// predicts a sine exactly, and a sine then comes back within a few steps of 16 bits. Noise that nothing predicts comes
// back with the error of a 4-bit quantizer: about 24 dB below a uniform signal.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/wave/adpcm.h"
#include "ntdlib/wave/encode.h"
#include "wave_cases.h"

using xsflib::testing::failures;

namespace
{

using ntdlib::wave::AlignDspAdpcmLoop;
using ntdlib::wave::DecodeDspAdpcm;
using ntdlib::wave::DspAdpcmBytes;
using ntdlib::wave::DspAdpcmChannel;
using ntdlib::wave::DspAdpcmSample;
using ntdlib::wave::DspAdpcmSum;
using ntdlib::wave::EncodeDspAdpcm;

constexpr std::array<DspAdpcmSum, 4> kSums = {DspAdpcmSum::kWide, DspAdpcmSum::kWrap, DspAdpcmSum::kNw4rCpu,
                                              DspAdpcmSum::kAccelerator};
constexpr double kPi = 3.14159265358979323846;

// The decoded samples of `channel`, `count` of them, from the start.
std::vector<int16_t> Decode(const DspAdpcmChannel& channel, std::size_t count, DspAdpcmSum sum)
{
    return DecodeDspAdpcm(channel.data, static_cast<uint32_t>(count), channel.coefs, 0, 0, sum);
}

// The ratio of the input's energy to the error's, in decibels. No error gives 200.
double SignalToNoise(std::span<const int16_t> input, std::span<const int16_t> decoded)
{
    double signal = 0.0;
    double noise = 0.0;
    for (std::size_t i = 0; i < input.size(); i++)
    {
        const double x = input[i];
        const double error = x - decoded[i];
        signal += x * x;
        noise += error * error;
    }

    return noise == 0.0 ? 200.0 : 10.0 * std::log10(signal / noise);
}

// A sine of `frequency` cycles a sample, `count` samples long, at `amplitude`.
std::vector<int16_t> Sine(double frequency, double amplitude, std::size_t count)
{
    std::vector<int16_t> samples(count);
    for (std::size_t i = 0; i < count; i++)
    {
        samples[i] = static_cast<int16_t>(std::lround(amplitude * std::sin(2.0 * kPi * frequency * i)));
    }

    return samples;
}

// Noise from the tests' generator, uniform over [-amplitude, amplitude].
std::vector<int16_t> Noise(uint64_t seed, int32_t amplitude, std::size_t count)
{
    ntdlib::wave::synthetic::Random random(seed);
    std::vector<int16_t> samples(count);
    for (int16_t& sample : samples)
    {
        sample = static_cast<int16_t>(static_cast<int32_t>(random.Below(2 * amplitude + 1)) - amplitude);
    }

    return samples;
}

// The samples that a decoder plays from the loop's start on, from the loop's state: the loop's predictor and scale,
// read from the frame's header when the loop starts at a frame's start, and its two samples before the start.
std::vector<int16_t> LaterPass(const DspAdpcmChannel& channel, std::size_t loop_start, std::size_t end, DspAdpcmSum sum)
{
    std::vector<int16_t> pass;
    int32_t header = channel.loop_pred_scale;
    int32_t hist1 = channel.loop_yn1;
    int32_t hist2 = channel.loop_yn2;
    for (std::size_t i = loop_start; i < end; i++)
    {
        const std::size_t frame = i / 14 * 8;
        const std::size_t place = i % 14;
        if (place == 0)
        {
            header = channel.data[frame];
        }
        const uint8_t pair = channel.data[frame + 1 + place / 2];
        const int32_t nibble = place % 2 == 0 ? pair >> 4 : pair & 0xF;
        const int32_t predictor = (header >> 4) & 7;
        const int16_t sample =
            DspAdpcmSample(nibble >= 8 ? nibble - 16 : nibble, header & 0xF, channel.coefs[2 * predictor],
                           channel.coefs[2 * predictor + 1], hist1, hist2, sum);
        pass.push_back(sample);
        hist2 = hist1;
        hist1 = sample;
    }

    return pass;
}

// The samples 0, 1, 2 and on, `count` of them.
std::vector<int16_t> Ramp(std::size_t count)
{
    std::vector<int16_t> samples(count);
    for (std::size_t i = 0; i < count; i++)
    {
        samples[i] = static_cast<int16_t>(i);
    }

    return samples;
}

// Whether the channel's pairs keep the decoder's prediction stable, give or take a step of the file's units.
bool StablePairs(const DspAdpcmChannel& channel)
{
    bool stable = true;
    for (std::size_t p = 0; p < 8; p++)
    {
        const double c1 = channel.coefs[2 * p] / 2048.0;
        const double c2 = channel.coefs[2 * p + 1] / 2048.0;
        stable = stable && c2 >= -1.0 && c2 <= 1.0 && std::abs(c1) <= 1.0 - c2 + 1.0 / 2048.0;
    }

    return stable;
}

// The squared error of the frame of `input` that starts at sample `start`, with the pair (c1, c2) and the scale
// `shift`. The decoder starts from the samples `hist1` and `hist2` before the frame. Each sample takes the nibble whose
// decoded sample comes closest to the input. Of two that come equally close, it takes the higher, as the encoder does.
double FrameError(std::span<const int16_t> input, std::size_t start, int32_t c1, int32_t c2, int32_t shift,
                  int32_t hist1, int32_t hist2, DspAdpcmSum sum)
{
    double error = 0.0;
    for (std::size_t i = start; i < std::min(input.size(), start + 14); i++)
    {
        int16_t closest = DspAdpcmSample(-8, shift, c1, c2, hist1, hist2, sum);
        for (int32_t nibble = -7; nibble <= 7; nibble++)
        {
            const int16_t sample = DspAdpcmSample(nibble, shift, c1, c2, hist1, hist2, sum);
            if (std::abs(sample - input[i]) <= std::abs(closest - input[i]))
            {
                closest = sample;
            }
        }
        const double difference = input[i] - closest;
        error += difference * difference;
        hist2 = hist1;
        hist1 = closest;
    }

    return error;
}

// Whether `function` throws std::invalid_argument.
template <typename Function>
bool Refuses(Function function)
{
    try
    {
        function();
    }
    catch (const std::invalid_argument&)
    {
        return true;
    }

    return false;
}

void TestBytes()
{
    XSFLIB_CHECK(DspAdpcmBytes(0) == 0 && DspAdpcmBytes(1) == 8 && DspAdpcmBytes(14) == 8);
    XSFLIB_CHECK(DspAdpcmBytes(15) == 16 && DspAdpcmBytes(28) == 16 && DspAdpcmBytes(29) == 24);
}

void TestSilence()
{
    const std::vector<int16_t> silence(100, 0);

    const DspAdpcmChannel channel = EncodeDspAdpcm(silence, 50, DspAdpcmSum::kWrap);

    XSFLIB_CHECK(channel.data.size() == 64);
    XSFLIB_CHECK(Decode(channel, silence.size(), DspAdpcmSum::kWrap) == silence);
    XSFLIB_CHECK(channel.loop_yn1 == 0 && channel.loop_yn2 == 0);
}

void TestEmpty()
{
    const DspAdpcmChannel channel = EncodeDspAdpcm({}, 0, DspAdpcmSum::kWrap);

    XSFLIB_CHECK(channel.data.empty() && channel.pred_scale == 0 && channel.loop_pred_scale == 0);
}

// Sines from low to high, quiet to full scale, with every sum.
void TestSines()
{
    for (const DspAdpcmSum sum : kSums)
    {
        for (const double frequency : {0.002, 0.03, 0.12, 0.31})
        {
            for (const double amplitude : {100.0, 6000.0, 32767.0})
            {
                const std::vector<int16_t> sine = Sine(frequency, amplitude, 3000);

                const DspAdpcmChannel channel = EncodeDspAdpcm(sine, 0, sum);
                const std::vector<int16_t> decoded = Decode(channel, sine.size(), sum);

                XSFLIB_CHECK(decoded.size() == sine.size() && SignalToNoise(sine, decoded) > 40.0);
            }
        }
    }
}

// Noise that no pair predicts comes back with a 4-bit quantizer's error.
void TestNoise()
{
    const std::vector<int16_t> loud = Noise(1, 32767, 5000);
    const std::vector<int16_t> quiet = Noise(2, 300, 5000);

    const std::vector<int16_t> loud_back =
        Decode(EncodeDspAdpcm(loud, 0, DspAdpcmSum::kWrap), 5000, DspAdpcmSum::kWrap);
    const std::vector<int16_t> quiet_back =
        Decode(EncodeDspAdpcm(quiet, 0, DspAdpcmSum::kWrap), 5000, DspAdpcmSum::kWrap);

    XSFLIB_CHECK(SignalToNoise(loud, loud_back) > 20.0);
    XSFLIB_CHECK(SignalToNoise(quiet, quiet_back) > 20.0);
}

// A loud passage and a quiet one take their pairs and scales apart: the quiet one comes back as close as alone.
void TestLoudAndQuiet()
{
    std::vector<int16_t> both = Sine(0.01, 30000.0, 2800);
    const std::vector<int16_t> quiet = Sine(0.27, 40.0, 2800);
    both.insert(both.end(), quiet.begin(), quiet.end());

    const std::vector<int16_t> decoded =
        Decode(EncodeDspAdpcm(both, 0, DspAdpcmSum::kWrap), both.size(), DspAdpcmSum::kWrap);

    XSFLIB_CHECK(SignalToNoise(std::span(both).first(2800), std::span(decoded).first(2800)) > 40.0);
    XSFLIB_CHECK(SignalToNoise(quiet, std::span(decoded).subspan(2800)) > 30.0);
}

// Full-scale squares reach the decoder's limits of 16 bits.
void TestExtremes()
{
    std::vector<int16_t> square(1400);
    for (std::size_t i = 0; i < square.size(); i++)
    {
        square[i] = i / 7 % 2 == 0 ? int16_t{32767} : int16_t{-32768};
    }

    for (const DspAdpcmSum sum : kSums)
    {
        const std::vector<int16_t> decoded = Decode(EncodeDspAdpcm(square, 0, sum), square.size(), sum);

        XSFLIB_CHECK(SignalToNoise(square, decoded) > 20.0);
    }
}

// Inputs shorter than a frame, and inputs that end inside one.
void TestShort()
{
    for (const std::size_t count :
         {std::size_t{1}, std::size_t{2}, std::size_t{13}, std::size_t{14}, std::size_t{15}, std::size_t{27}})
    {
        const std::vector<int16_t> sine = Sine(0.05, 20000.0, count);

        const DspAdpcmChannel channel = EncodeDspAdpcm(sine, 0, DspAdpcmSum::kWrap);
        const std::vector<int16_t> decoded = Decode(channel, count, DspAdpcmSum::kWrap);

        XSFLIB_CHECK(channel.data.size() == DspAdpcmBytes(count));
        XSFLIB_CHECK(decoded.size() == count && SignalToNoise(sine, decoded) > 30.0);
    }
}

// The pairs keep the decoder's prediction stable, and the headers name pairs 0 to 7 and scales 0 to 15.
void TestPairs()
{
    std::vector<int16_t> input = Sine(0.004, 25000.0, 4000);
    const std::vector<int16_t> noise = Noise(3, 20000, 4000);
    input.insert(input.end(), noise.begin(), noise.end());

    const DspAdpcmChannel channel = EncodeDspAdpcm(input, 0, DspAdpcmSum::kWrap);

    XSFLIB_CHECK(StablePairs(channel));
    for (std::size_t frame = 0; frame < channel.data.size(); frame += 8)
    {
        XSFLIB_CHECK((channel.data[frame] & 0x80) == 0);
    }
}

// A growing sine's best pair lies outside the range where the decoder's prediction is stable. The pairs stay inside.
void TestGrowth()
{
    std::vector<int16_t> growing(1000);
    for (std::size_t i = 0; i < growing.size(); i++)
    {
        const double x = static_cast<double>(i);
        growing[i] = static_cast<int16_t>(std::lround(200.0 * std::pow(1.004, x) * std::sin(0.1 * x)));
    }

    const DspAdpcmChannel channel = EncodeDspAdpcm(growing, 0, DspAdpcmSum::kWrap);

    XSFLIB_CHECK(StablePairs(channel));
}

// Each nibble gives the decoded sample closest to the input among the 16, with its frame's pair and scale and the
// samples that the decoder gives before it. Each frame takes the pair and the scale with the least error among the
// channel's 8 pairs and the 16 scales. The input ends inside a frame, and the error of that frame counts only the
// input's samples.
void TestNearest()
{
    std::vector<int16_t> input = Noise(5, 6, 700);
    const std::vector<int16_t> loud = Noise(4, 9000, 1400);
    const std::vector<int16_t> sine = Sine(0.021, 20000.0, 1403);
    input.insert(input.end(), loud.begin(), loud.end());
    input.insert(input.end(), sine.begin(), sine.end());
    for (const DspAdpcmSum sum : kSums)
    {
        const DspAdpcmChannel channel = EncodeDspAdpcm(input, 0, sum);

        bool nearest = true;
        bool least = true;
        int32_t hist1 = 0;
        int32_t hist2 = 0;
        for (std::size_t i = 0; i < input.size(); i++)
        {
            const uint8_t header = channel.data[i / 14 * 8];
            const int32_t c1 = channel.coefs[2 * (header >> 4 & 7)];
            const int32_t c2 = channel.coefs[2 * (header >> 4 & 7) + 1];
            if (i % 14 == 0)
            {
                const double chosen = FrameError(input, i, c1, c2, header & 0xF, hist1, hist2, sum);
                for (std::size_t p = 0; p < 8; p++)
                {
                    for (int32_t shift = 0; shift < 16; shift++)
                    {
                        least = least && chosen <= FrameError(input, i, channel.coefs[2 * p], channel.coefs[2 * p + 1],
                                                              shift, hist1, hist2, sum);
                    }
                }
            }
            const uint8_t pair = channel.data[i / 14 * 8 + 1 + i % 14 / 2];
            const int32_t value = i % 2 == 0 ? pair >> 4 : pair & 0xF;
            const int16_t sample =
                DspAdpcmSample(value >= 8 ? value - 16 : value, header & 0xF, c1, c2, hist1, hist2, sum);
            for (int32_t other = -8; other <= 7; other++)
            {
                const int16_t candidate = DspAdpcmSample(other, header & 0xF, c1, c2, hist1, hist2, sum);
                nearest = nearest && std::abs(candidate - input[i]) >= std::abs(sample - input[i]);
            }
            hist2 = hist1;
            hist1 = sample;
        }

        XSFLIB_CHECK(nearest);
        XSFLIB_CHECK(least);
    }
}

// A decoder that goes back to the loop's start with the loop's state plays the first pass's samples again, from a
// loop's start at any place in a frame.
void TestLoopState()
{
    const std::vector<int16_t> input = Sine(0.013, 12000.0, 700);
    for (const DspAdpcmSum sum : kSums)
    {
        for (const std::size_t loop_start : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{13},
                                             std::size_t{14}, std::size_t{15}, std::size_t{350}, std::size_t{699}})
        {
            const DspAdpcmChannel channel = EncodeDspAdpcm(input, static_cast<uint32_t>(loop_start), sum);
            const std::vector<int16_t> first = Decode(channel, input.size(), sum);

            const std::vector<int16_t> later = LaterPass(channel, loop_start, input.size(), sum);

            XSFLIB_CHECK(
                std::equal(later.begin(), later.end(), first.begin() + static_cast<std::ptrdiff_t>(loop_start)));
            XSFLIB_CHECK(channel.loop_pred_scale == channel.data[loop_start / 14 * 8]);
            XSFLIB_CHECK(channel.pred_scale == channel.data[0]);
        }
    }

    // A loop's start past the last sample takes the state after the last sample.
    const DspAdpcmChannel past = EncodeDspAdpcm(input, 5000, DspAdpcmSum::kWrap);
    const std::vector<int16_t> decoded = Decode(past, input.size(), DspAdpcmSum::kWrap);
    XSFLIB_CHECK(past.loop_yn1 == decoded[699] && past.loop_yn2 == decoded[698]);
    XSFLIB_CHECK(past.loop_pred_scale == past.data[past.data.size() - 8]);
}

void TestAlign()
{
    const std::vector<int16_t> ramp = Ramp(40);

    // A start on a frame stays. A start inside one moves to the next frame, and the samples between repeat.
    std::vector<std::vector<int16_t>> on_frame = {ramp, ramp};
    std::vector<std::vector<int16_t>> inside = {ramp, ramp};
    std::vector<std::vector<int16_t>> short_loop = {{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}};

    const uint32_t kept = AlignDspAdpcmLoop(on_frame, 28);
    const uint32_t moved = AlignDspAdpcmLoop(inside, 17);
    const uint32_t wrapped = AlignDspAdpcmLoop(short_loop, 10);

    XSFLIB_CHECK(kept == 28 && on_frame[0] == ramp && on_frame[1] == ramp);
    XSFLIB_CHECK(moved == 28 && inside[0].size() == 51 && inside[1] == inside[0]);
    XSFLIB_CHECK(std::equal(inside[0].begin() + 40, inside[0].end(), ramp.begin() + 17));
    XSFLIB_CHECK(wrapped == 14);
    XSFLIB_CHECK(short_loop[0] == std::vector<int16_t>({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 11, 12, 11, 12}));
    XSFLIB_CHECK(Refuses([&] { AlignDspAdpcmLoop(inside, 51); }));
    XSFLIB_CHECK(Refuses(
        [&]
        {
            std::vector<std::vector<int16_t>> uneven = {ramp, {1, 2}};
            AlignDspAdpcmLoop(uneven, 5);
        }));
}

} // namespace

int main()
{
    TestBytes();
    TestSilence();
    TestEmpty();
    TestSines();
    TestNoise();
    TestLoudAndQuiet();
    TestExtremes();
    TestShort();
    TestPairs();
    TestGrowth();
    TestNearest();
    TestLoopState();
    TestAlign();

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::printf("Wave encoder tests passed\n");

    return 0;
}
