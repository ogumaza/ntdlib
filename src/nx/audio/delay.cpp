// SPDX-License-Identifier: MIT

#include "ntdlib/nx/audio/delay.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "nx/audio/arithmetic.h"

namespace ntdlib::nx::audio
{
namespace
{

// The scalings of the feedback gain (16,056 / 16,384) and of the low-pass amount (15,564 / 16,384).
constexpr int64_t kFeedbackScale = 0x3EB8;
constexpr int64_t kLowPassScale = 0x3CCC;

// The longest line the model keeps. A longer delay is cut to this length. No game asks for one: it lasts 87 s at
// 48 kHz.
constexpr uint32_t kMostCells = uint32_t{1} << 22;

// The low 32 bits of a * gain shifted right by 14, the product in 64 bits.
int32_t MulQ14(int64_t a, int64_t gain)
{
    return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(a * gain) >> 14));
}

// A sample on the delay's scale: shifted left by 6, wrapping.
int32_t Scaled(int32_t sample)
{
    return static_cast<int32_t>(static_cast<uint32_t>(sample) << 6);
}

// One channel's step after its feedback `g`: the filter, the write into the line, the line's next cell, and the output
// from the dry input `x` and the line's oldest sample `d`.
int32_t Step(const DelayParameter& parameter, DelayState& state, int k, int32_t g, int32_t x, int32_t d)
{
    const auto c = static_cast<std::size_t>(k);
    const int32_t filtered = WrapAdd(MulQ14(g, state.low_pass_in), MulQ14(state.low_pass[c], state.low_pass_feedback));
    state.low_pass[c] = filtered;
    std::vector<int32_t>& line = state.lines[c];
    line[state.positions[c]] = filtered;
    state.positions[c] = state.positions[c] + 1 == line.size() ? 0 : state.positions[c] + 1;

    return WrapAdd(MulQ14(x, parameter.dry_gain), MulQ14(d, parameter.wet_gain)) >> 6;
}

} // namespace

void SetDelayGains(const DelayParameter& parameter, DelayState& state)
{
    // The spread to each of two neighbours is half the spread with 4 or 6 channels.
    state.feedback = MulQ14(parameter.feedback_gain, kFeedbackScale);
    state.direct = MulQ14(state.feedback, int64_t{0x4000} - parameter.channel_spread);
    const bool ring = (parameter.channel_count | 2) == 6;
    state.cross = MulQ14(state.feedback, ring ? parameter.channel_spread >> 1 : parameter.channel_spread);
    state.low_pass_feedback = MulQ14(parameter.low_pass_amount, kLowPassScale);
    state.low_pass_in = WrapSub(0x4000, state.low_pass_feedback);
}

void InitializeDelay(const DelayParameter& parameter, DelayState& state)
{
    // The lengths in 32 bits, the console's way: the longest delay from the rate and the longest time, and the delay
    // from the rate a millisecond in Q14, rounded half up, then the shorter of the two, unsigned.
    const int32_t rate = parameter.sample_rate >> 14;
    const int32_t most = WrapMul(rate, parameter.max_delay) / 1000;
    const int32_t product = WrapMul(parameter.delay, parameter.sample_rate / 1000);
    const auto wanted = static_cast<uint32_t>(WrapAdd(product >> 14, (product >> 13) & 1));
    const uint32_t length = std::min(std::min(wanted, static_cast<uint32_t>(most)), kMostCells);

    const int channels = std::clamp(parameter.channel_count, 0, kDelayChannels);
    for (std::size_t k = 0; k < static_cast<std::size_t>(channels); ++k)
    {
        std::vector<int32_t>& line = state.lines[k];
        if (length == 0)
        {
            line.resize(1, 0);
        }
        else
        {
            line.assign(length, 0);
        }

        state.positions[k] = 0;
        state.low_pass[k] = 0;
    }

    SetDelayGains(parameter, state);
}

void PassDelay(const DelayParameter& parameter, std::span<const int32_t* const> inputs,
               std::span<int32_t* const> outputs, int count)
{
    const auto channels =
        std::min({static_cast<std::size_t>(std::max(parameter.channel_count, 0)), inputs.size(), outputs.size()});
    for (std::size_t k = 0; k < channels; ++k)
    {
        if (inputs[k] != outputs[k])
        {
            std::copy(inputs[k], inputs[k] + count, outputs[k]);
        }
    }
}

void ApplyDelay(const DelayParameter& parameter, DelayState& state, std::span<const int32_t* const> inputs,
                std::span<int32_t* const> outputs, int count)
{
    const int channels = parameter.channel_count;
    if (channels != 1 && channels != 2 && channels != 4 && channels != 6)
    {
        PassDelay(parameter, inputs, outputs, count);
        return;
    }
    if (inputs.size() < static_cast<std::size_t>(channels) || outputs.size() < static_cast<std::size_t>(channels))
    {
        return;
    }
    for (std::size_t k = 0; k < static_cast<std::size_t>(channels); ++k)
    {
        // A delay runs only once InitializeDelay has given it its lines.
        if (state.lines[k].empty())
        {
            return;
        }
    }

    const auto at = [&](int k) -> int32_t
    {
        const auto c = static_cast<std::size_t>(k);
        return state.lines[c][state.positions[c]];
    };

    const auto mix = [&](int32_t x, int32_t d, int32_t neighbours)
    {
        // The input, the line's feedback, and the neighbours' feedback through the spread.
        return WrapAdd(WrapAdd(MulQ14(d, state.direct), MulQ14(x, parameter.in_gain)), MulQ14(neighbours, state.cross));
    };

    std::array<int32_t, kDelayChannels> x{};
    std::array<int32_t, kDelayChannels> d{};
    std::array<int32_t, kDelayChannels> g{};
    for (int t = 0; t < count; ++t)
    {
        for (int k = 0; k < channels; ++k)
        {
            const auto c = static_cast<std::size_t>(k);
            x[c] = Scaled(inputs[c][t]);
            d[c] = at(k);
        }

        switch (channels)
        {
        case 1:
            g[0] = WrapAdd(MulQ14(d[0], parameter.feedback_gain), MulQ14(x[0], parameter.in_gain));
            break;

        case 2:
            g[0] = mix(x[0], d[0], d[1]);
            g[1] = mix(x[1], d[1], d[0]);
            break;

        case 4:
            g[0] = mix(x[0], d[0], WrapAdd(d[1], d[2]));
            g[1] = mix(x[1], d[1], WrapAdd(d[0], d[3]));
            g[2] = mix(x[2], d[2], WrapAdd(d[0], d[3]));
            g[3] = mix(x[3], d[3], WrapAdd(d[1], d[2]));
            break;

        default:
            g[0] = mix(x[0], d[0], WrapAdd(d[2], d[4]));
            g[1] = mix(x[1], d[1], WrapAdd(d[2], d[5]));
            g[2] = mix(x[2], d[2], WrapAdd(d[0], d[1]));
            g[3] = WrapAdd(MulQ14(d[3], parameter.feedback_gain), MulQ14(x[3], parameter.in_gain));
            g[4] = mix(x[4], d[4], WrapAdd(d[0], d[5]));
            g[5] = mix(x[5], d[5], WrapAdd(d[1], d[4]));
            break;
        }

        // Every channel's line gives its sample before any line takes one. The outputs come last. A channel's output
        // may therefore be its input.
        for (int k = 0; k < channels; ++k)
        {
            const auto c = static_cast<std::size_t>(k);
            d[c] = Step(parameter, state, k, g[c], x[c], d[c]);
        }
        for (int k = 0; k < channels; ++k)
        {
            const auto c = static_cast<std::size_t>(k);
            outputs[c][t] = d[c];
        }
    }
}

} // namespace ntdlib::nx::audio
