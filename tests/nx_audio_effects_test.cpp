// SPDX-License-Identifier: MIT

// Checks the models of the audio renderer's delay, reverb and light limiter without game data. The values come from the
// formulas in delay.h, reverb.h and light_limiter.h, worked out by hand on impulses and steady signals: a delay's
// length, its feedback and spread with 1, 2, 4 and 6 channels, its low-pass filter and its dry gain, a reverb's dry
// path and its pass-through, and a limiter's look-ahead, envelope, gain, reciprocals and output holds, with the sdk's
// expf against the exponential function. The effects then run on a voice through the renderer: a delay of a frame, the
// calls that start it again or give it new gains, a disabled delay, frames without mixing, a new reverb and a disabled
// one, the modes that the renderer refuses, a snapshot, a new limiter, and the enhanced mixing mode. Nintendo's code is
// the anchor for the reverb's sums. A test outside this repository runs it against the model.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/nx/audio/delay.h"
#include "ntdlib/nx/audio/light_limiter.h"
#include "ntdlib/nx/audio/renderer.h"
#include "ntdlib/nx/audio/reverb.h"

using namespace xsflib::testing;

namespace
{

namespace audio = ntdlib::nx::audio;

constexpr int32_t kOne = 0x4000; // 1.0 in Q14
constexpr int32_t kRate = 48000;
constexpr int kFrame = 240;
constexpr int64_t kFrameTime = 5000000; // 5 ms in nanoseconds: a frame at 48 kHz
constexpr int64_t kSecond = 1000000000;

using Channels = std::vector<std::vector<int32_t>>;

// `count` samples of silence with `value` at `at`.
std::vector<int32_t> Impulse(int count, int at, int32_t value)
{
    std::vector<int32_t> samples(static_cast<std::size_t>(count), 0);
    samples[static_cast<std::size_t>(at)] = value;

    return samples;
}

// Silence with the given values at the given places.
std::vector<int32_t> Echoes(int count, std::initializer_list<std::pair<int, int32_t>> echoes)
{
    std::vector<int32_t> samples(static_cast<std::size_t>(count), 0);
    for (const auto& [at, value] : echoes)
    {
        samples[static_cast<std::size_t>(at)] = value;
    }

    return samples;
}

// A delay of `delay` ms on `channels` channels at 48 kHz, with full input and wet gains, no dry gain, no feedback and
// no low-pass filter.
audio::DelayParameter Line(int channels, int32_t delay)
{
    audio::DelayParameter parameter;
    parameter.channel_count = channels;
    parameter.max_channel_count = channels;
    parameter.max_delay = 1000;
    parameter.delay = delay;
    parameter.sample_rate = kRate << 14;
    parameter.in_gain = kOne;
    parameter.dry_gain = 0;

    return parameter;
}

// Starts a delay, and runs the channels through it in place.
Channels Delayed(const audio::DelayParameter& parameter, Channels channels)
{
    audio::DelayState state;
    audio::InitializeDelay(parameter, state);
    std::vector<const int32_t*> in;
    std::vector<int32_t*> out;
    for (std::vector<int32_t>& channel : channels)
    {
        in.push_back(channel.data());
        out.push_back(channel.data());
    }
    audio::ApplyDelay(parameter, state, in, out, static_cast<int>(channels[0].size()));

    return channels;
}

// A line of 5 ms at 48 kHz holds 240 samples. At 44.1 kHz, the console's rate in Q14 a millisecond is 722,534, and
// 7 ms gives 309 samples from 308.5 and more. A longest delay of 5 ms holds that line to 220 samples, 220.5 towards
// zero.
void TestDelayLength()
{
    XSFLIB_CHECK(Delayed(Line(1, 5), {Impulse(1000, 0, 1000)})[0] == Impulse(1000, 240, 1000));

    audio::DelayParameter parameter = Line(1, 7);
    parameter.sample_rate = 44100 << 14;
    XSFLIB_CHECK(Delayed(parameter, {Impulse(400, 0, 1000)})[0] == Impulse(400, 309, 1000));
    parameter.max_delay = 5;
    XSFLIB_CHECK(Delayed(parameter, {Impulse(400, 0, 1000)})[0] == Impulse(400, 220, 1000));
}

// A delay of 0 ms keeps a line of one cell: the output is the input a sample late.
void TestDelayOfNoTime()
{
    XSFLIB_CHECK(Delayed(Line(1, 0), {Impulse(8, 0, 1000)})[0] == Impulse(8, 1, 1000));
}

// A single channel takes the feedback gain as it is. At 0.5, each echo has half the level of the one before.
void TestDelayFeedback()
{
    audio::DelayParameter parameter = Line(1, 5);
    parameter.feedback_gain = kOne / 2;

    const std::vector<int32_t> out = Delayed(parameter, {Impulse(1000, 0, 1000)})[0];

    XSFLIB_CHECK(out == Echoes(1000, {{240, 1000}, {480, 500}, {720, 250}, {960, 125}}));
}

// With 2 channels, a feedback of 1 scales to 16,056 / 16,384. At a spread of 1, all of it goes to the other channel.
// The impulse on the left comes back on the left after 5 ms, then on the right as 64,000 × 16,056 / 16,384 = 62,718
// on the line's scale (979 out), on the left as 61,462 (960), and on the right as 60,231 (941).
void TestDelayTwoChannels()
{
    audio::DelayParameter parameter = Line(2, 5);
    parameter.feedback_gain = kOne;
    parameter.channel_spread = kOne;

    const Channels out = Delayed(parameter, {Impulse(1000, 0, 1000), std::vector<int32_t>(1000, 0)});

    XSFLIB_CHECK(out[0] == Echoes(1000, {{240, 1000}, {720, 960}}));
    XSFLIB_CHECK(out[1] == Echoes(1000, {{480, 979}, {960, 941}}));
}

// With 4 channels, each neighbour takes half the spread: 8,028 of the scaled feedback. The front left's impulse reaches
// its neighbours, the front right and the rear left, as 31,359 (489 out). They reach the front left and the rear
// right as 30,731 (480). Those echoes come back to the first two as 30,115 (470).
void TestDelayFourChannels()
{
    audio::DelayParameter parameter = Line(4, 5);
    parameter.feedback_gain = kOne;
    parameter.channel_spread = kOne;
    const std::vector<int32_t> silence(1000, 0);

    const Channels out = Delayed(parameter, {Impulse(1000, 0, 1000), silence, silence, silence});

    XSFLIB_CHECK(out[0] == Echoes(1000, {{240, 1000}, {720, 480}}));
    XSFLIB_CHECK(out[1] == Echoes(1000, {{480, 489}, {960, 470}}));
    XSFLIB_CHECK(out[2] == out[1]);
    XSFLIB_CHECK(out[3] == Echoes(1000, {{720, 480}}));
}

// With 6 channels, the centre's neighbours are the front left and right. They pass its echo on to the rear right and
// left as 15,365 (240 out). The LFE's line takes its feedback as it is, and none of the others' feedback: at 1, its
// impulse comes back unchanged every 5 ms. No echo of the LFE reaches the others. The front left's neighbours are the
// centre and the rear left, and theirs pass its echo on to the front right and the rear right.
void TestDelaySixChannels()
{
    audio::DelayParameter parameter = Line(6, 5);
    parameter.feedback_gain = kOne;
    parameter.channel_spread = kOne;
    const std::vector<int32_t> silence(721, 0);

    const Channels out =
        Delayed(parameter, {silence, silence, Impulse(721, 0, 1000), Impulse(721, 0, 1000), silence, silence});
    const Channels left = Delayed(parameter, {Impulse(721, 0, 1000), silence, silence, silence, silence, silence});

    XSFLIB_CHECK(out[0] == Echoes(721, {{480, 489}}));
    XSFLIB_CHECK(out[1] == out[0]);
    XSFLIB_CHECK(out[2] == Echoes(721, {{240, 1000}, {720, 480}}));
    XSFLIB_CHECK(out[3] == Echoes(721, {{240, 1000}, {480, 1000}, {720, 1000}}));
    XSFLIB_CHECK(out[4] == Echoes(721, {{720, 240}}));
    XSFLIB_CHECK(out[5] == out[4]);
    XSFLIB_CHECK(left[0] == Echoes(721, {{240, 1000}, {720, 480}}));
    XSFLIB_CHECK(left[2] == Echoes(721, {{480, 489}}) && left[4] == left[2]);
    XSFLIB_CHECK(left[1] == Echoes(721, {{720, 240}}) && left[5] == left[1] && left[3] == silence);
}

// A low-pass amount of 1 scales to 15,564 / 16,384, and the input takes the other 820. An impulse of 16,384 (2^20 on
// the line's scale) enters the line as 52,480, then the filter's 49,853 and 47,357: 820, 778 and 739 out.
void TestDelayLowPass()
{
    audio::DelayParameter parameter = Line(1, 5);
    parameter.low_pass_amount = kOne;

    const std::vector<int32_t> out = Delayed(parameter, {Impulse(243, 0, 16384)})[0];

    XSFLIB_CHECK(out == Echoes(243, {{240, 820}, {241, 778}, {242, 739}}));
}

// At a dry gain of 0.5 and no wet gain, the output is half the input, towards minus infinity. An input of 2^25 wraps
// on the line's scale to -2^31.
void TestDelayDryGain()
{
    audio::DelayParameter parameter = Line(1, 5);
    parameter.in_gain = 0;
    parameter.dry_gain = kOne / 2;
    parameter.wet_gain = 0;

    const std::vector<int32_t> out = Delayed(parameter, {{3, -3, 1000, -1000, 1 << 25}})[0];

    XSFLIB_CHECK(out == std::vector<int32_t>({1, -2, 500, -500, -(1 << 24)}));
}

// A delay of 3 channels passes its inputs through. So does a disabled one.
void TestDelayPassesThrough()
{
    const audio::DelayParameter parameter = Line(3, 0);
    audio::DelayState state;
    audio::InitializeDelay(parameter, state);
    const std::array<std::vector<int32_t>, 3> in = {{{1, 2}, {3, 4}, {5, 6}}};
    std::array<std::vector<int32_t>, 3> out = {{{0, 0}, {0, 0}, {0, 0}}};
    const std::array<const int32_t*, 3> from = {in[0].data(), in[1].data(), in[2].data()};
    const std::array<int32_t*, 3> to = {out[0].data(), out[1].data(), out[2].data()};

    audio::ApplyDelay(parameter, state, from, to, 2);
    const bool applied = out == in;
    for (std::vector<int32_t>& channel : out)
    {
        std::fill(channel.begin(), channel.end(), 0);
    }

    audio::PassDelay(Line(2, 5), from, to, 2);

    XSFLIB_CHECK(applied);
    XSFLIB_CHECK(out[0] == in[0] && out[1] == in[1] && out[2] == std::vector<int32_t>({0, 0}));
}

// A reverb of `channels` channels at 48 kHz with the client's defaults: a full dry gain and no reverb. Its output is
// the dry gain times the input, in Q14, plus the out gain times the reverb.
audio::ReverbParameter DryReverb(int channels)
{
    audio::ReverbParameter parameter;
    parameter.channel_count = channels;
    parameter.max_channel_count = channels;
    parameter.sample_rate = 48 << 14;

    return parameter;
}

// Starts a reverb and runs the channels through it. Each output is its input: the dry gain of 1 keeps it, and an out
// gain of 0 adds nothing.
void TestReverbDryOnly()
{
    bool kept = true;
    for (const int channels : {1, 2, 4, 6})
    {
        const audio::ReverbParameter parameter = DryReverb(channels);
        audio::ReverbState state;
        audio::InitializeReverb(parameter, state, true);
        Channels in;
        for (int k = 0; k < channels; ++k)
        {
            in.push_back({1, -2, 30000, -32768, 8388607, -8388608, 0, k});
        }
        Channels out = in;
        std::vector<const int32_t*> from;
        std::vector<int32_t*> to;
        for (int k = 0; k < channels; ++k)
        {
            from.push_back(in[static_cast<std::size_t>(k)].data());
            to.push_back(out[static_cast<std::size_t>(k)].data());
            std::fill(out[static_cast<std::size_t>(k)].begin(), out[static_cast<std::size_t>(k)].end(), 0);
        }

        audio::ApplyReverb(parameter, state, from, to, static_cast<int>(in[0].size()));
        kept = kept && out == in;
    }

    XSFLIB_CHECK(kept);
}

// A reverb of 3 channels passes its inputs through, and PassReverb copies the channels that the parameter has.
void TestReverbPassesThrough()
{
    const audio::ReverbParameter parameter = DryReverb(3);
    audio::ReverbState state;
    audio::InitializeReverb(parameter, state, true);
    const std::array<std::vector<int32_t>, 3> in = {{{1, 2}, {3, 4}, {5, 6}}};
    std::array<std::vector<int32_t>, 3> out = {{{0, 0}, {0, 0}, {0, 0}}};
    const std::array<const int32_t*, 3> from = {in[0].data(), in[1].data(), in[2].data()};
    const std::array<int32_t*, 3> to = {out[0].data(), out[1].data(), out[2].data()};

    audio::ApplyReverb(parameter, state, from, to, 2);
    const bool applied = out == in;
    for (std::vector<int32_t>& channel : out)
    {
        std::fill(channel.begin(), channel.end(), 0);
    }

    audio::PassReverb(DryReverb(2), from, to, 2);

    XSFLIB_CHECK(applied);
    XSFLIB_CHECK(out[0] == in[0] && out[1] == in[1] && out[2] == std::vector<int32_t>({0, 0}));
}

// A limiter of `channels` channels at 48 kHz with a look-ahead of a sample, an instant attack and release, a threshold
// of 0.5 and gains of 1.
audio::LightLimiterParameter Limiter(int channels)
{
    audio::LightLimiterParameter parameter;
    parameter.channel_count = channels;
    parameter.max_channel_count = channels;
    parameter.sample_rate = kRate;
    parameter.look_ahead = 1;
    parameter.max_look_ahead = 48;
    parameter.attack = 1.0f;
    parameter.release = 1.0f;
    parameter.threshold = 0.5f;
    parameter.input_gain = 1.0f;
    parameter.output_gain = 1.0f;

    return parameter;
}

// Starts a limiter, and runs `channels` copies of `samples` through it in place.
Channels Limited(const audio::LightLimiterParameter& parameter, const std::vector<int32_t>& samples)
{
    audio::LightLimiterState state;
    audio::InitializeLightLimiter(parameter, state);
    Channels channels(static_cast<std::size_t>(parameter.channel_count), samples);
    std::vector<const int32_t*> in;
    std::vector<int32_t*> out;
    for (std::vector<int32_t>& channel : channels)
    {
        in.push_back(channel.data());
        out.push_back(channel.data());
    }
    audio::ApplyLightLimiter(parameter, state, in, out, static_cast<int>(samples.size()));

    return channels;
}

// Whether every channel is `expected`.
bool AllAre(const Channels& channels, const std::vector<int32_t>& expected)
{
    for (const std::vector<int32_t>& channel : channels)
    {
        if (channel != expected)
        {
            return false;
        }
    }

    return true;
}

// Below the threshold, the gain stays at 1, and the output is the input as many samples late as the look-ahead holds.
void TestLimiterBelowThreshold()
{
    audio::LightLimiterParameter parameter = Limiter(1);
    parameter.threshold = 1.0f;
    const std::vector<int32_t> in = {1000, -2000, 3000, 0, 4, 5, 6, 7};

    XSFLIB_CHECK(AllAre(Limited(parameter, in), {0, 1000, -2000, 3000, 0, 4, 5, 6}));
    parameter.channel_count = 2;
    parameter.look_ahead = 3;
    XSFLIB_CHECK(AllAre(Limited(parameter, in), {0, 0, 0, 1000, -2000, 3000, 0, 4}));
}

// A level of 0.75 over a threshold of 0.5 takes a gain of 2/3 at once, and the output holds at 0.5: 16,384. A single
// channel divides. The others take the processor's estimate of 1 / 0.75, 341 / 256. Two steps refine it to the
// nearest single of 4/3. In processing mode 1, the estimate stays as it is, and the output is 0.75 × 0.5 × 341 / 256 ×
// 32,768 = 16,368. With 6 channels, the last four refine the estimate in every mode.
void TestLimiterLimits()
{
    const std::vector<int32_t> in(8, 24576);
    const std::vector<int32_t> held = {0, 16384, 16384, 16384, 16384, 16384, 16384, 16384};
    const std::vector<int32_t> estimated = {0, 16368, 16368, 16368, 16368, 16368, 16368, 16368};

    XSFLIB_CHECK(AllAre(Limited(Limiter(1), in), held));
    XSFLIB_CHECK(AllAre(Limited(Limiter(2), in), held));
    XSFLIB_CHECK(AllAre(Limited(Limiter(6), in), held));

    audio::LightLimiterParameter parameter = Limiter(2);
    parameter.processing_mode = 1;
    XSFLIB_CHECK(AllAre(Limited(parameter, in), estimated));
    parameter.channel_count = 6;
    const Channels six = Limited(parameter, in);
    XSFLIB_CHECK(six[0] == estimated && six[1] == estimated);
    XSFLIB_CHECK(six[2] == held && six[3] == held && six[4] == held && six[5] == held);
}

// An impulse of 1.0 takes the gain to 0.5 at once. At the next sample, the envelope falls by the release of 0.5 to the
// threshold, the gain rises halfway back to 1, and the impulse comes out at that gain of 0.75: 24,576. The estimate of
// 1 / 1.0 is 511 / 512. Unrefined, the gain is 0.499 and then 0.7495, and the impulse comes out as 24,560. At a
// threshold of 0.25, the envelope of 0.5 stays over the threshold, and the gain rises halfway from 0.25 to 0.5: 12,288
// out. The estimate of 1 / 0.5 is 511 / 256, and unrefined, the impulse comes out as 12,264.
void TestLimiterRelease()
{
    audio::LightLimiterParameter parameter = Limiter(1);
    parameter.release = 0.5f;
    const std::vector<int32_t> in = {32768, 0, 0, 0};

    XSFLIB_CHECK(AllAre(Limited(parameter, in), {0, 24576, 0, 0}));
    parameter.channel_count = 2;
    XSFLIB_CHECK(AllAre(Limited(parameter, in), {0, 24576, 0, 0}));
    parameter.processing_mode = 1;
    XSFLIB_CHECK(AllAre(Limited(parameter, in), {0, 24560, 0, 0}));

    parameter.threshold = 0.25f;
    XSFLIB_CHECK(AllAre(Limited(parameter, in), {0, 12264, 0, 0}));
    parameter.processing_mode = 0;
    XSFLIB_CHECK(AllAre(Limited(parameter, in), {0, 12288, 0, 0}));
    parameter.channel_count = 1;
    XSFLIB_CHECK(AllAre(Limited(parameter, in), {0, 12288, 0, 0}));
}

// A single channel holds its output to 24 bits. The others hold theirs to 32 bits.
void TestLimiterHolds()
{
    audio::LightLimiterParameter parameter = Limiter(1);
    parameter.threshold = 1e6f;
    parameter.output_gain = 4.0f;
    const std::vector<int32_t> in = {1 << 22, -(1 << 22), 1 << 30, 0};

    XSFLIB_CHECK(AllAre(Limited(parameter, in), {0, 8388607, -8388608, 8388607}));
    parameter.channel_count = 2;
    XSFLIB_CHECK(AllAre(Limited(parameter, in), {0, 1 << 24, -(1 << 24), std::numeric_limits<int32_t>::max()}));
}

// A limiter of 3 channels passes its inputs through, and one of no channels leaves its outputs as they are.
void TestLimiterPassesThrough()
{
    audio::LightLimiterParameter parameter = Limiter(3);
    parameter.input_gain = 0.0f;
    audio::LightLimiterState state;
    audio::InitializeLightLimiter(parameter, state);
    const std::array<std::vector<int32_t>, 3> in = {{{24576, 32768}, {-5, 6}, {7, -8}}};
    std::array<std::vector<int32_t>, 3> out = {{{0, 0}, {0, 0}, {0, 0}}};
    const std::array<const int32_t*, 3> from = {in[0].data(), in[1].data(), in[2].data()};
    const std::array<int32_t*, 3> to = {out[0].data(), out[1].data(), out[2].data()};

    audio::ApplyLightLimiter(parameter, state, from, to, 2);
    const bool passed = out == in;
    for (std::vector<int32_t>& channel : out)
    {
        std::fill(channel.begin(), channel.end(), 1);
    }

    parameter.channel_count = 0;
    audio::ApplyLightLimiter(parameter, state, from, to, 2);

    XSFLIB_CHECK(passed);
    XSFLIB_CHECK(out[0] == std::vector<int32_t>({1, 1}) && out[2] == std::vector<int32_t>({1, 1}));
}

// The distance in units in the last place between two finite singles of the same sign.
int64_t Ulps(float a, float b)
{
    return std::abs(static_cast<int64_t>(std::bit_cast<uint32_t>(a)) -
                    static_cast<int64_t>(std::bit_cast<uint32_t>(b)));
}

// The sdk's expf against e^x in double precision, rounded to a single, over its range: within one unit in the last
// place. Its special values follow the C library's: 1 at 0, 0 at minus infinity, infinity at infinity, a NaN for a
// NaN, and infinity and 0 beyond the range.
void TestExpf()
{
    int64_t worst = 0;
    for (int i = -1040000; i <= 887000; ++i)
    {
        const float x = static_cast<float>(i) / 10000.0f;
        const auto expected = static_cast<float>(std::exp(static_cast<double>(x)));
        worst = std::max(worst, Ulps(audio::Expf(x), expected));
    }

    const float infinity = std::numeric_limits<float>::infinity();

    XSFLIB_CHECK(worst <= 1);
    XSFLIB_CHECK(audio::Expf(0.0f) == 1.0f && audio::Expf(-0.0f) == 1.0f);
    XSFLIB_CHECK(audio::Expf(-infinity) == 0.0f && audio::Expf(infinity) == infinity);
    XSFLIB_CHECK(std::isnan(audio::Expf(std::numeric_limits<float>::quiet_NaN())));
    XSFLIB_CHECK(audio::Expf(88.8f) == infinity && audio::Expf(-104.0f) == 0.0f);
}

// An attack or release under 1 µs moves at once. Otherwise the coefficient is 1 - e^(-1 / (rate × t)), here within
// 2.5 × 10^-7 of its value in double precision. A look-ahead is rate × t samples towards zero, and at least 1.
void TestLimiterTimes()
{
    const double coefficient = 1.0 - std::exp(-1.0 / (48000.0 * 0.05));

    XSFLIB_CHECK(audio::LightLimiterCoefficient(kRate, 0) == 1.0f);
    XSFLIB_CHECK(audio::LightLimiterCoefficient(kRate, -5) == 1.0f);
    XSFLIB_CHECK(std::abs(static_cast<double>(audio::LightLimiterCoefficient(kRate, 50000)) - coefficient) < 2.5e-7);
    XSFLIB_CHECK(audio::LightLimiterSamples(kRate, 1000) == 48 && audio::LightLimiterSamples(kRate, 500) == 24);
    XSFLIB_CHECK(audio::LightLimiterSamples(44100, 1000) == 44 && audio::LightLimiterSamples(kRate, 50000) == 2400);
    XSFLIB_CHECK(audio::LightLimiterSamples(kRate, 0) == 1 && audio::LightLimiterSamples(kRate, 1) == 1);
}

// A mono PCM16 voice of noise at 48 kHz, at full volume to the final mix's first buffer. The device sink takes that
// buffer as its left channel.
struct NoiseVoice
{
    explicit NoiseVoice(bool enhanced_mode = false) : renderer(Parameter(enhanced_mode)), enhanced(enhanced_mode)
    {
        uint32_t seed = 1;
        for (int i = 0; i < kFrame * 12; ++i)
        {
            seed = seed * 1664525u + 1013904223u;
            const auto value = static_cast<uint16_t>(static_cast<int32_t>(seed >> 18) - 8192);
            bytes.push_back(static_cast<uint8_t>(value & 0xFF));
            bytes.push_back(static_cast<uint8_t>(value >> 8));
        }

        renderer.AcquireFinalMix(2);
        constexpr std::array<int, 2> kInputs = {0, 1};
        renderer.AddDeviceSink(kInputs);
        voice = renderer.AcquireVoiceSlot(kRate, 1, audio::SampleFormat::kPcmInt16, 0, nullptr);
        renderer.SetVoiceDestination(voice, audio::Renderer::kFinalMix);
        renderer.SetVoiceMixVolume(voice, 1.0f, 0, 0);
        audio::WaveBufferInfo buffer;
        buffer.buffer = bytes;
        buffer.start = 0;
        buffer.end = kFrame * 12;
        renderer.AppendWaveBuffer(voice, buffer);
        renderer.SetVoicePlayState(voice, audio::PlayState::kStarted);
    }

    static audio::RendererParameter Parameter(bool enhanced_mode)
    {
        audio::RendererParameter parameter;
        parameter.mix_buffer_count = 2;
        parameter.voice_count = 4;
        parameter.enhanced = enhanced_mode;

        return parameter;
    }

    // The left channel of the next frame, on the scale of 16-bit samples.
    std::vector<double> Left()
    {
        renderer.RenderFrame();
        std::vector<double> left;
        if (enhanced)
        {
            const std::span<const float> out = renderer.DeviceOutputFloat();
            for (std::size_t t = 0; t < out.size(); t += 2)
            {
                left.push_back(static_cast<double>(out[t]) * 32768.0);
            }
        }
        else
        {
            const std::span<const int16_t> out = renderer.DeviceOutput();
            for (std::size_t t = 0; t < out.size(); t += 2)
            {
                left.push_back(out[t]);
            }
        }

        return left;
    }

    std::vector<uint8_t> bytes;
    audio::Renderer renderer;
    bool enhanced = false;
    int voice = -1;
};

constexpr std::array<int8_t, 1> kFirstBuffer = {0};

// A delay of a frame on the final mix's first buffer, with the input at full gain and no dry gain. Returns the delay.
int AddLine(audio::Renderer& renderer)
{
    const int delay = renderer.AddDelay(audio::Renderer::kFinalMix, kSecond, 1);
    renderer.SetDelayInputOutput(delay, kFirstBuffer, kFirstBuffer);
    renderer.SetDelayTime(delay, kFrameTime);
    renderer.SetDelayInGain(delay, 1.0f);
    renderer.SetDelayDryGain(delay, 0.0f);

    return delay;
}

bool Silent(const std::vector<double>& samples)
{
    return std::all_of(samples.begin(), samples.end(), [](double s) { return s == 0.0; });
}

// The delay gives its buffer a frame late. Its first frame is silent, and each later frame is the frame before it from
// a renderer without the delay.
void TestRendererDelay()
{
    NoiseVoice plain;
    NoiseVoice delayed;
    AddLine(delayed.renderer);
    std::vector<double> before = plain.Left();
    const bool first_silent = Silent(delayed.Left());
    bool late = true;

    for (int frame = 1; frame < 5; ++frame)
    {
        const std::vector<double> now = plain.Left();
        late = late && delayed.Left() == before;
        before = now;
    }

    XSFLIB_CHECK(first_silent && late && !Silent(before));
}

// A new time starts the delay again at the next frame. Its line is cleared, and that frame is silent. A new gain keeps
// the line.
void TestRendererDelayStartsAgain()
{
    NoiseVoice plain;
    NoiseVoice timed;
    NoiseVoice gained;
    const int timed_delay = AddLine(timed.renderer);
    const int gained_delay = AddLine(gained.renderer);
    std::vector<double> before;
    for (int frame = 0; frame < 3; ++frame)
    {
        before = plain.Left();
        timed.Left();
        gained.Left();
    }

    timed.renderer.SetDelayTime(timed_delay, kFrameTime);
    gained.renderer.SetDelayInGain(gained_delay, 1.0f);
    const std::vector<double> now = plain.Left();
    XSFLIB_CHECK(Silent(timed.Left()) && gained.Left() == before);
    XSFLIB_CHECK(timed.Left() == now && gained.Left() == now);
}

// A disabled delay passes its buffer through, and the frame is that of a renderer without it. Enabled again, the delay
// starts again with a silent frame.
void TestRendererDelayDisabled()
{
    NoiseVoice plain;
    NoiseVoice delayed;
    const int delay = AddLine(delayed.renderer);
    for (int frame = 0; frame < 2; ++frame)
    {
        plain.Left();
        delayed.Left();
    }

    delayed.renderer.SetDelayEnabled(delay, false);
    XSFLIB_CHECK(delayed.Left() == plain.Left());
    delayed.renderer.SetDelayEnabled(delay, true);
    const std::vector<double> now = plain.Left();
    XSFLIB_CHECK(Silent(delayed.Left()));
    XSFLIB_CHECK(delayed.Left() == now);
}

// Frames without mixing leave the delay's line as it was. The first frame mixed after two of them is the last frame
// mixed before them. A new time during those frames starts the delay again, and the first frame mixed is silent.
void TestRendererDelayWithoutMixing()
{
    NoiseVoice plain;
    NoiseVoice kept;
    NoiseVoice timed;
    AddLine(kept.renderer);
    const int delay = AddLine(timed.renderer);
    std::vector<double> last;
    for (int frame = 0; frame < 3; ++frame)
    {
        last = plain.Left();
        kept.Left();
        timed.Left();
    }

    kept.renderer.SetMixing(false);
    timed.renderer.SetMixing(false);
    timed.renderer.SetDelayTime(delay, kFrameTime);
    for (int frame = 0; frame < 2; ++frame)
    {
        kept.Left();
        timed.Left();
    }
    kept.renderer.SetMixing(true);
    timed.renderer.SetMixing(true);

    XSFLIB_CHECK(kept.Left() == last && Silent(timed.Left()));
}

// A reverb on the final mix's first buffer. Returns the reverb.
int AddReverb(audio::Renderer& renderer)
{
    const int reverb = renderer.AddReverb(audio::Renderer::kFinalMix, 1);
    renderer.SetReverbInputOutput(reverb, kFirstBuffer, kFirstBuffer);

    return reverb;
}

// A new reverb keeps its buffer as it is: a dry gain of 1 and an out gain of 0. So does a disabled one with gains that
// would change it, and the modes outside 0 to 4 are refused.
void TestRendererReverb()
{
    NoiseVoice plain;
    NoiseVoice dry;
    NoiseVoice disabled;
    AddReverb(dry.renderer);
    const int reverb = AddReverb(disabled.renderer);
    disabled.renderer.SetReverbEarlyMode(reverb, 0);
    disabled.renderer.SetReverbEarlyGain(reverb, 1.0f);
    disabled.renderer.SetReverbReverbGain(reverb, 1.0f);
    disabled.renderer.SetReverbOutGain(reverb, 1.0f);
    disabled.renderer.SetReverbDryGain(reverb, 0.0f);
    disabled.renderer.SetReverbEnabled(reverb, false);
    bool same = true;
    bool sounds = false;
    for (int frame = 0; frame < 4; ++frame)
    {
        const std::vector<double> now = plain.Left();
        same = same && dry.Left() == now && disabled.Left() == now;
        sounds = sounds || !Silent(now);
    }

    bool refused = false;
    try
    {
        dry.renderer.SetReverbLateMode(0, 5);
    }
    catch (const std::invalid_argument&)
    {
        refused = true;
    }

    XSFLIB_CHECK(same && sounds);
    XSFLIB_CHECK(refused);
}

// A new limiter has a threshold and gains of 0, and its output is silent. With a threshold and gains of 1, it gives
// its buffer a sample late, at the look-ahead of a sample that it starts with. Its first sample then comes from the
// silent input before.
void TestRendererLimiter()
{
    NoiseVoice plain;
    NoiseVoice limited;
    const int limiter = limited.renderer.AddLightLimiter(audio::Renderer::kFinalMix, 1000000, 1);
    limited.renderer.SetLightLimiterInputOutput(limiter, kFirstBuffer, kFirstBuffer);
    const bool sounds = !Silent(plain.Left());
    XSFLIB_CHECK(sounds && Silent(limited.Left()));

    limited.renderer.SetLightLimiterThreshold(limiter, 1.0f);
    limited.renderer.SetLightLimiterInputGain(limiter, 1.0f);
    limited.renderer.SetLightLimiterOutputGain(limiter, 1.0f);
    std::vector<double> expected = {0.0};
    std::vector<double> out;
    for (int frame = 0; frame < 3; ++frame)
    {
        const std::vector<double> now = plain.Left();
        const std::vector<double> got = limited.Left();
        expected.insert(expected.end(), now.begin(), now.end());
        out.insert(out.end(), got.begin(), got.end());
    }

    expected.pop_back();

    XSFLIB_CHECK(out == expected);
}

// A snapshot keeps the delay's line, the reverb's lines and filters, and the limiter's envelope, gain and look-ahead.
// The frames after a restore repeat those after the save.
void TestRendererEffectsSnapshot()
{
    NoiseVoice voice;
    AddLine(voice.renderer);
    const int reverb = AddReverb(voice.renderer);
    voice.renderer.SetReverbEarlyMode(reverb, 1);
    voice.renderer.SetReverbEarlyGain(reverb, 0.7f);
    voice.renderer.SetReverbLateGain(reverb, 0.7f);
    voice.renderer.SetReverbPredelayTime(reverb, kFrameTime);
    voice.renderer.SetReverbDecayTime(reverb, kSecond);
    voice.renderer.SetReverbHighFrequencyDecayRatio(reverb, 0.8f);
    voice.renderer.SetReverbReverbGain(reverb, 0.5f);
    voice.renderer.SetReverbOutGain(reverb, 1.0f);
    voice.renderer.SetReverbDryGain(reverb, 0.5f);
    const int limiter = voice.renderer.AddLightLimiter(audio::Renderer::kFinalMix, 1000000, 1);
    voice.renderer.SetLightLimiterInputOutput(limiter, kFirstBuffer, kFirstBuffer);
    voice.renderer.SetLightLimiterReleaseTime(limiter, 2000000);
    voice.renderer.SetLightLimiterLookAheadTime(limiter, 500000);
    voice.renderer.SetLightLimiterThreshold(limiter, 0.1f);
    voice.renderer.SetLightLimiterInputGain(limiter, 1.0f);
    voice.renderer.SetLightLimiterOutputGain(limiter, 1.0f);
    for (int frame = 0; frame < 2; ++frame)
    {
        voice.Left();
    }

    const audio::Renderer::Snapshot snapshot = voice.renderer.Save();
    std::vector<double> first;
    for (int frame = 0; frame < 3; ++frame)
    {
        const std::vector<double> got = voice.Left();
        first.insert(first.end(), got.begin(), got.end());
    }

    voice.renderer.Restore(snapshot);
    std::vector<double> second;
    for (int frame = 0; frame < 3; ++frame)
    {
        const std::vector<double> got = voice.Left();
        second.insert(second.end(), got.begin(), got.end());
    }

    XSFLIB_CHECK(first == second && !Silent(first));
}

// In the enhanced mode, the delay takes its samples rounded to integers, halves away from zero, and gives those
// integers back.
void TestRendererEnhancedDelay()
{
    NoiseVoice plain(true);
    NoiseVoice delayed(true);
    AddLine(delayed.renderer);
    std::vector<double> before = plain.Left();
    delayed.Left();
    bool rounded = true;
    bool fractions = false;

    for (int frame = 1; frame < 4; ++frame)
    {
        const std::vector<double> now = plain.Left();
        const std::vector<double> got = delayed.Left();
        for (std::size_t t = 0; t < before.size(); ++t)
        {
            rounded = rounded && got[t] == std::round(before[t]);
            fractions = fractions || before[t] != std::round(before[t]);
        }
        before = now;
    }

    XSFLIB_CHECK(rounded && fractions);
}

} // namespace

int main()
{
    TestDelayLength();
    TestDelayOfNoTime();
    TestDelayFeedback();
    TestDelayTwoChannels();
    TestDelayFourChannels();
    TestDelaySixChannels();
    TestDelayLowPass();
    TestDelayDryGain();
    TestDelayPassesThrough();
    TestReverbDryOnly();
    TestReverbPassesThrough();
    TestLimiterBelowThreshold();
    TestLimiterLimits();
    TestLimiterRelease();
    TestLimiterHolds();
    TestLimiterPassesThrough();
    TestExpf();
    TestLimiterTimes();
    TestRendererDelay();
    TestRendererDelayStartsAgain();
    TestRendererDelayDisabled();
    TestRendererDelayWithoutMixing();
    TestRendererReverb();
    TestRendererLimiter();
    TestRendererEffectsSnapshot();
    TestRendererEnhancedDelay();

    return xsflib::testing::Summary("nx_audio_effects");
}
