// SPDX-License-Identifier: MIT

// Tests of the models of the 3DS's aux effects (ntdlib/ctr_fx/effects.h). Each test feeds impulses or noise through a
// model and checks what follows from the effect's structure and its documented gains: the times of the echoes in whole
// frames, their levels, the damping filters' responses, the comb filters' gains, the shortest buffers, the parameters
// that the libraries refuse, the layout after new parameters, and copies that run on as the original.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/ctr_fx/effects.h"

namespace
{

using ntdlib::ctr_fx::Channels;
using ntdlib::ctr_fx::Delay;
using ntdlib::ctr_fx::DelayFrames;
using ntdlib::ctr_fx::DelayParam;
using ntdlib::ctr_fx::FrameFormat;
using ntdlib::ctr_fx::Library;
using ntdlib::ctr_fx::Reverb;
using ntdlib::ctr_fx::ReverbFrames;
using ntdlib::ctr_fx::ReverbParam;
using xsflib::testing::ErrorOf;

constexpr int32_t kImpulse = 1 << 20;
constexpr FrameFormat kStrict{};
constexpr FrameFormat kEnhanced{235, 48070.0f};

// The four channels of a run, sample after sample.
using Output = std::array<std::vector<int32_t>, 4>;

// Runs `frames` frames through `effect`. `input(channel, sample)` gives the input at a sample of the whole run.
template <typename Effect, typename Input>
Output Run(Effect& effect, std::size_t frame_samples, std::size_t frames, Input input)
{
    Output output;
    std::array<std::vector<int32_t>, 4> frame;
    for (auto& channel : frame)
    {
        channel.resize(frame_samples);
    }

    for (std::size_t f = 0; f < frames; f++)
    {
        for (std::size_t c = 0; c < 4; c++)
        {
            for (std::size_t n = 0; n < frame_samples; n++)
            {
                frame[c][n] = input(c, f * frame_samples + n);
            }
        }

        const Channels channels{frame[0].data(), frame[1].data(), frame[2].data(), frame[3].data()};
        effect.Process(channels, frame_samples);
        for (std::size_t c = 0; c < 4; c++)
        {
            output[c].insert(output[c].end(), frame[c].begin(), frame[c].end());
        }
    }

    return output;
}

// The samples of `samples` that aren't 0, with their places.
std::vector<std::pair<std::size_t, int32_t>> Nonzero(const std::vector<int32_t>& samples)
{
    std::vector<std::pair<std::size_t, int32_t>> found;
    for (std::size_t i = 0; i < samples.size(); i++)
    {
        if (samples[i] != 0)
        {
            found.emplace_back(i, samples[i]);
        }
    }

    return found;
}

void TestFrames()
{
    // 0.20458265 frames a millisecond from 4.888 ms. The product of two floats is exact in double precision. It rounds
    // to nearest in single precision, and its whole part gives the frames. 168 of these times give a frame more than
    // the exact product's whole part, among them every time below 5 ms.
    int wrong = 0;
    for (uint32_t ms = 0; ms <= 100000; ms++)
    {
        const double from = std::max(static_cast<double>(ms), static_cast<double>(4.888f));
        const auto product = static_cast<float>(from * static_cast<double>(0.20458265f));
        wrong += ReverbFrames(ms) == static_cast<uint32_t>(product) ? 0 : 1;
    }
    XSFLIB_CHECK(wrong == 0);
    XSFLIB_CHECK(ReverbFrames(0) == 1u);
    XSFLIB_CHECK(ReverbFrames(4) == 1u);
    XSFLIB_CHECK(ReverbFrames(5) == 1u);
    XSFLIB_CHECK(ReverbFrames(60) == 12u);

    // The delay: the microseconds over 4,888, and 1 at least. The microseconds wrap at 2^32.
    wrong = 0;
    for (uint32_t ms = 0; ms <= 100000; ms++)
    {
        const uint32_t frames = ms * 1000 / 4888;
        wrong += DelayFrames(ms) == std::max<uint32_t>(frames, 1) ? 0 : 1;
    }
    XSFLIB_CHECK(wrong == 0);
    XSFLIB_CHECK(DelayFrames(0) == 1u);
    XSFLIB_CHECK(DelayFrames(250) == 51u);
    XSFLIB_CHECK(DelayFrames(4888) == 1000u);
    XSFLIB_CHECK(DelayFrames(4294968) == 1u);
}

void CheckEchoes(FrameFormat format)
{
    // Without damping, each echo is the last one times -0.5: the feedback's 64/128, applied to the magnitude.
    Delay delay(DelayParam{100, 0.5f, 0.0f, false}, format);
    const std::size_t length = std::size_t{DelayFrames(100)} * format.samples;
    const auto input = [](std::size_t c, std::size_t i) -> int32_t
    {
        if (c >= 2)
        {
            return 7;
        }
        return i == 3 ? (c == 0 ? kImpulse : -kImpulse) : 0;
    };
    const Output output = Run(delay, format.samples, 4 * length / format.samples + 2, input);

    std::vector<std::pair<std::size_t, int32_t>> expected;
    int32_t echo = kImpulse;
    for (std::size_t k = 1; k <= 4; k++)
    {
        expected.emplace_back(k * length + 3, echo);
        echo = -echo / 2;
    }
    XSFLIB_CHECK(Nonzero(output[0]) == expected);
    for (auto& [place, value] : expected)
    {
        value = -value;
    }
    XSFLIB_CHECK(Nonzero(output[1]) == expected);

    // Without surround, the rear channels keep their samples.
    XSFLIB_CHECK(std::all_of(output[2].begin(), output[2].end(), [](int32_t s) { return s == 7; }));
    XSFLIB_CHECK(std::all_of(output[3].begin(), output[3].end(), [](int32_t s) { return s == 7; }));
}

void TestDelayEchoes()
{
    CheckEchoes(kStrict);
    CheckEchoes(kEnhanced);
}

// The damping filter's response to an impulse through a line without feedback, from its gains in 128ths.
std::vector<int32_t> DampedImpulse(int32_t input_gain, int32_t feedback_gain, std::size_t count)
{
    std::vector<int32_t> response;
    int32_t state = 0;
    for (std::size_t n = 0; n < count; n++)
    {
        state = (input_gain * (n == 0 ? kImpulse : 0) + feedback_gain * state) >> 7;
        response.push_back(state);
    }

    return response;
}

void TestDelayDamping()
{
    // A damping of 0.5: half the input and half the last output. A damping of 1 counts as 0.95: (1 - 0.95)·128 and
    // 0.95·128, rounded toward zero, are 6 and 121.
    const std::array<std::array<int32_t, 2>, 2> gains{{{64, 64}, {6, 121}}};
    const std::array<float, 2> dampings{0.5f, 1.0f};
    for (std::size_t i = 0; i < 2; i++)
    {
        Delay delay(DelayParam{50, 0.0f, dampings[i], false});
        const std::size_t length = std::size_t{DelayFrames(50)} * 160;
        const Output output = Run(delay, 160, length / 160 + 3,
                                  [](std::size_t c, std::size_t n) { return c == 0 && n == 0 ? kImpulse : 0; });

        const std::vector<int32_t> response = DampedImpulse(gains[i][0], gains[i][1], 3 * 160);
        XSFLIB_CHECK(std::all_of(output[0].begin(), output[0].begin() + static_cast<std::ptrdiff_t>(length),
                                 [](int32_t s) { return s == 0; }));
        XSFLIB_CHECK(
            std::equal(response.begin(), response.end(), output[0].begin() + static_cast<std::ptrdiff_t>(length)));
    }
}

void TestDelayParams()
{
    // The libraries refuse gains outside 0 to 1.
    XSFLIB_CHECK(!ErrorOf<std::invalid_argument>([] { Delay(DelayParam{100, 1.5f, 0.5f, false}); }).empty());
    XSFLIB_CHECK(!ErrorOf<std::invalid_argument>([] { Delay(DelayParam{100, 0.5f, -0.1f, false}); }).empty());
    XSFLIB_CHECK(!ErrorOf<std::invalid_argument>([] { Delay(DelayParam{}, FrameFormat{0, 32728.0f}); }).empty());

    // After the setup, the delay can't grow and surround can't start. A refusal changes nothing.
    Delay delay(DelayParam{100, 0.5f, 0.0f, false});
    XSFLIB_CHECK(!delay.SetParam(DelayParam{101, 0.5f, 0.0f, false}));
    XSFLIB_CHECK(!delay.SetParam(DelayParam{100, 0.5f, 0.0f, true}));
    XSFLIB_CHECK(delay.Param().delay_time == 100u);
    XSFLIB_CHECK(delay.ChannelCount() == std::size_t{2});

    // A shorter delay wraps the line's position earlier.
    XSFLIB_CHECK(delay.SetParam(DelayParam{50, 0.0f, 0.0f, false}));
    const std::size_t length = std::size_t{DelayFrames(50)} * 160;
    const Output output =
        Run(delay, 160, length / 160 + 1, [](std::size_t c, std::size_t n) { return c == 0 && n == 9 ? kImpulse : 0; });
    XSFLIB_CHECK((Nonzero(output[0]) == std::vector<std::pair<std::size_t, int32_t>>{{length + 9, kImpulse}}));

    // A frame of another length.
    std::array<int32_t, 100> samples{};
    const Channels channels{samples.data(), samples.data(), samples.data(), samples.data()};
    XSFLIB_CHECK(!ErrorOf<std::invalid_argument>([&] { delay.Process(channels, samples.size()); }).empty());
}

void CheckEarly(Library library, FrameFormat format)
{
    // With no fused gain and a full early gain, the output is the early reflection alone: the input 12 frames later.
    ReverbParam param;
    param.early_gain = 1.0f;
    param.fused_gain = 0.0f;
    param.enable_surround = library == Library::kNwSnd;
    Reverb reverb(library, param, format);
    const auto input = [](std::size_t c, std::size_t i) -> int32_t
    {
        return i == 5 + c ? kImpulse : 0;
    };
    const Output output = Run(reverb, format.samples, 14, input);

    const std::size_t length = std::size_t{ReverbFrames(param.early_reflection_time)} * format.samples;
    for (std::size_t c = 0; c < reverb.ChannelCount(); c++)
    {
        XSFLIB_CHECK((Nonzero(output[c]) == std::vector<std::pair<std::size_t, int32_t>>{{length + 5 + c, kImpulse}}));
    }
}

void TestReverbEarly()
{
    CheckEarly(Library::kNwSnd, kStrict);
    CheckEarly(Library::kNnSnd, kStrict);
    CheckEarly(Library::kNnSnd2011, kStrict);
    CheckEarly(Library::kNwSnd, kEnhanced);
}

// A comb filter's gain in 128ths: 10^(-3·length/(time·rate)), rounded down. Returns -1 for a gain too near a whole
// 128th for double precision to settle.
int32_t CombGain(double length, double time, double rate)
{
    const double gain = std::pow(10.0, -3.0 * length / (time * 0.001 * rate)) * 128.0;
    const double fraction = gain - std::floor(gain);

    return fraction < 0.01 || fraction > 0.99 ? -1 : static_cast<int32_t>(gain);
}

void CheckCombGains(Library library, FrameFormat format)
{
    // The early reflection, the all-pass filter and the damping change nothing: no early gain, no coloration, no
    // damping and the full fused gain. The output is then the first comb filter's output less the second comb filter's
    // output, a frame later. The comb filters of 3 and 40 frames echo at different times after the pre-delay of a
    // frame. An echo of 2^20 after one with a gain of g in 128ths is g·2^13.
    ReverbParam param{5, 500, 5, 0.0f, 0.0f, {3 * 160, 40 * 160, 160}, 0.0f, 1.0f, false, false};
    Reverb reverb(library, param, format);
    const std::size_t s = format.samples;
    const Output output =
        Run(reverb, s, 90, [](std::size_t c, std::size_t i) { return c == 0 && i == 0 ? kImpulse : 0; });

    const int32_t first = CombGain(3.0 * static_cast<double>(s), 500.0, format.rate);
    const int32_t second = CombGain(40.0 * static_cast<double>(s), 500.0, format.rate);
    XSFLIB_CHECK(first > 0 && second > 0);
    XSFLIB_CHECK(output[0][2 * s + 3 * s] == kImpulse);
    XSFLIB_CHECK(output[0][2 * s + 6 * s] == first << 13);
    XSFLIB_CHECK(output[0][2 * s + 40 * s] == -kImpulse);
    XSFLIB_CHECK(output[0][2 * s + 80 * s] == -(second << 13));
}

void TestReverbCombGains()
{
    CheckCombGains(Library::kNwSnd, kStrict);
    CheckCombGains(Library::kNnSnd, kStrict);
    CheckCombGains(Library::kNnSnd2011, kStrict);
    CheckCombGains(Library::kNwSnd, kEnhanced);
}

void TestReverbDamping()
{
    // The parameters of CheckCombGains with a damping of 0.5: gains of 64/128. The impulse leaves the all-pass filter
    // at sample 800, and the comb filters' next echo comes 3 frames later. In between, the output is the damping
    // filter's response. nw::snd and the later nn::snd add half the input and half the last output. The high-pass
    // damping subtracts both. nn::snd of 2011 takes half the sum of the input and the last output from the input.
    const auto response = [](Library library, bool high_pass)
    {
        ReverbParam param{5, 500, 5, 0.0f, 0.5f, {3 * 160, 40 * 160, 160}, 0.0f, 1.0f, false, high_pass};
        Reverb reverb(library, param);
        const Output output =
            Run(reverb, 160, 8, [](std::size_t c, std::size_t i) { return c == 0 && i == 0 ? kImpulse : 0; });
        XSFLIB_CHECK(Nonzero(std::vector<int32_t>(output[0].begin(), output[0].begin() + 800)).empty());
        return std::vector<int32_t>(output[0].begin() + 800, output[0].begin() + 1280);
    };

    std::vector<int32_t> low;
    std::vector<int32_t> high;
    std::vector<int32_t> older;
    int32_t low_state = 0;
    int32_t high_state = 0;
    int32_t older_state = 0;
    for (std::size_t n = 0; n < 480; n++)
    {
        const int32_t x = n == 0 ? kImpulse : 0;
        low_state = (64 * x + 64 * low_state) >> 7;
        high_state = (-64 * x - 64 * high_state) >> 7;
        older_state = x - ((64 * (older_state + x)) >> 7);
        low.push_back(low_state);
        high.push_back(high_state);
        older.push_back(older_state);
    }

    XSFLIB_CHECK(response(Library::kNwSnd, false) == low);
    XSFLIB_CHECK(response(Library::kNnSnd, false) == low);
    XSFLIB_CHECK(response(Library::kNwSnd, true) == high);
    XSFLIB_CHECK(response(Library::kNnSnd, true) == high);
    XSFLIB_CHECK(response(Library::kNnSnd2011, false) == older);
    XSFLIB_CHECK(older[1] < 0 && older[2] > 0);
}

void TestReverbShortTimes()
{
    // An early time of 0 ms counts as 4.888 ms, a frame. With the full early gain and no fused gain, each library's
    // output is the input a frame late.
    ReverbParam param{0, 1000, 100, 0.5f, 0.0f, {160, 160, 160}, 1.0f, 0.0f, false, false};
    const auto input = [](std::size_t c, std::size_t i)
    {
        return c == 0 && i == 7 ? kImpulse : 0;
    };
    const std::vector<std::pair<std::size_t, int32_t>> late{{167, kImpulse}};
    for (const Library library : {Library::kNwSnd, Library::kNnSnd, Library::kNnSnd2011})
    {
        Reverb reverb(library, param);
        XSFLIB_CHECK(Nonzero(Run(reverb, 160, 30, input)[0]) == late);
    }

    // nn::snd lays the buffers out again from new parameters, and nw::snd keeps those of the setup. After a change from
    // 60 ms to 0 ms, each early reflection runs a frame, and the output is the input a frame late again. nn::snd's new
    // setup then refuses the old time.
    ReverbParam setup = param;
    setup.early_reflection_time = 60;
    Reverb nw_changed(Library::kNwSnd, setup);
    Reverb nn_changed(Library::kNnSnd, setup);
    XSFLIB_CHECK(nw_changed.SetParam(param));
    XSFLIB_CHECK(nn_changed.SetParam(param));
    XSFLIB_CHECK(Nonzero(Run(nw_changed, 160, 30, input)[0]) == late);
    XSFLIB_CHECK(Nonzero(Run(nn_changed, 160, 30, input)[0]) == late);
    XSFLIB_CHECK(nw_changed.SetParam(setup));
    XSFLIB_CHECK(!nn_changed.SetParam(setup));
}

void TestReverbParams()
{
    // The libraries refuse gains outside 0 to 1, and nw::snd refuses filters of no samples. The model refuses those for
    // nn::snd too, and filters that aren't whole frames, and surround for nn::snd's two channels.
    const auto refused = [](Library library, ReverbParam param)
    {
        return !ErrorOf<std::invalid_argument>([&] { Reverb(library, param); }).empty();
    };
    ReverbParam param;
    param.coloration = 1.5f;
    XSFLIB_CHECK(refused(Library::kNwSnd, param));
    param = ReverbParam{};
    param.filter_sizes[1] = 0;
    XSFLIB_CHECK(refused(Library::kNwSnd, param));
    param.filter_sizes[1] = 3000;
    XSFLIB_CHECK(refused(Library::kNwSnd, param));
    param = ReverbParam{};
    param.enable_surround = true;
    XSFLIB_CHECK(refused(Library::kNnSnd, param));
    XSFLIB_CHECK(refused(Library::kNnSnd2011, param));
    XSFLIB_CHECK(!refused(Library::kNwSnd, param));

    // nn::snd of 2011 has no high-pass damping.
    param = ReverbParam{};
    param.high_pass_damping = true;
    XSFLIB_CHECK(refused(Library::kNnSnd2011, param));
    XSFLIB_CHECK(!refused(Library::kNnSnd, param));

    // After the setup, the times and the filters can't grow, and surround can't start. A refusal changes nothing.
    Reverb reverb(Library::kNwSnd, ReverbParam{});
    ReverbParam longer;
    longer.pre_delay_time = 101;
    XSFLIB_CHECK(!reverb.SetParam(longer));
    longer = ReverbParam{};
    longer.filter_sizes[2] = 2240;
    XSFLIB_CHECK(!reverb.SetParam(longer));
    longer = ReverbParam{};
    longer.enable_surround = true;
    XSFLIB_CHECK(!reverb.SetParam(longer));
    XSFLIB_CHECK(reverb.Param().pre_delay_time == 100u);
    XSFLIB_CHECK(reverb.ChannelCount() == std::size_t{2});

    // A new setting silences the buffers. Without damping, nothing of the tail remains.
    ReverbParam undamped;
    undamped.damping = 0.0f;
    Reverb silenced(Library::kNwSnd, undamped);
    Run(silenced, 160, 30, [](std::size_t, std::size_t i) { return i < 160 ? kImpulse : 0; });
    XSFLIB_CHECK(silenced.SetParam(undamped));
    const Output after = Run(silenced, 160, 30, [](std::size_t, std::size_t) { return 0; });
    XSFLIB_CHECK(Nonzero(after[0]).empty() && Nonzero(after[1]).empty());
}

void TestCopies()
{
    // A copy runs on as the original: noise through the effects, a copy halfway, and the same noise into both.
    std::mt19937 random(5);
    std::uniform_int_distribution<int32_t> sample(-32768, 32767);
    std::vector<int32_t> noise(4 * 160 * 100);
    for (int32_t& s : noise)
    {
        s = sample(random);
    }

    const auto first = [&](std::size_t c, std::size_t i)
    {
        return noise[(i * 4 + c) % noise.size()];
    };
    const auto second = [&](std::size_t c, std::size_t i)
    {
        return noise[(i * 4 + c + 12345) % noise.size()];
    };
    ReverbParam param;
    param.enable_surround = true;
    Reverb reverb(Library::kNwSnd, param);
    Delay delay(DelayParam{300, 0.7f, 0.3f, true});
    Run(reverb, 160, 50, first);
    Run(delay, 160, 50, first);

    Reverb reverb_copy = reverb;
    Delay delay_copy = delay;
    XSFLIB_CHECK(Run(reverb, 160, 50, second) == Run(reverb_copy, 160, 50, second));
    XSFLIB_CHECK(Run(delay, 160, 50, second) == Run(delay_copy, 160, 50, second));
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("ctr_fx",
                                    []
                                    {
                                        TestFrames();
                                        TestDelayEchoes();
                                        TestDelayDamping();
                                        TestDelayParams();
                                        TestReverbEarly();
                                        TestReverbCombGains();
                                        TestReverbDamping();
                                        TestReverbShortTimes();
                                        TestReverbParams();
                                        TestCopies();
                                    });
}
