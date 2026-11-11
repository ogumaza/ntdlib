// SPDX-License-Identifier: MIT

#include "ntdlib/nx/audio/light_limiter.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "ntdlib/aarch64.h"
#include "nx/audio/arithmetic.h"
#include "nx/audio/effect_channels.h"

namespace ntdlib::nx::audio
{
namespace
{

// The most samples the model's look-ahead holds, all channels together. The console's buffer holds what the client
// asked for. No game asks for more than this: 87 s of one channel at 48 kHz.
constexpr std::size_t kMostCells = std::size_t{1} << 22;

// expf's table (0xb6a9e0 in the sdk): the bits of 2^(i/32) less i << 47. Adding k << 47 to entry k mod 32 gives
// 2^(k/32) for any k in range.
constexpr std::array<uint64_t, 32> kExp2Table = {
    0x3FF0000000000000, 0x3FEFD9B0D3158574, 0x3FEFB5586CF9890F, 0x3FEF9301D0125B51, 0x3FEF72B83C7D517B,
    0x3FEF54873168B9AA, 0x3FEF387A6E756238, 0x3FEF1E9DF51FDEE1, 0x3FEF06FE0A31B715, 0x3FEEF1A7373AA9CB,
    0x3FEEDEA64C123422, 0x3FEECE086061892D, 0x3FEEBFDAD5362A27, 0x3FEEB42B569D4F82, 0x3FEEAB07DD485429,
    0x3FEEA47EB03A5585, 0x3FEEA09E667F3BCD, 0x3FEE9F75E8EC5F74, 0x3FEEA11473EB0187, 0x3FEEA589994CCE13,
    0x3FEEACE5422AA0DB, 0x3FEEB737B0CDC5E5, 0x3FEEC49182A3F090, 0x3FEED503B23E255D, 0x3FEEE89F995AD3AD,
    0x3FEEFF76F2FB5E47, 0x3FEF199BDD85529C, 0x3FEF3720DCEF9069, 0x3FEF5818DCFBA487, 0x3FEF7C97337B9B5F,
    0x3FEFA4AFA2A490DA, 0x3FEFD0765B6E4540,
};

// 32 / ln 2, and the polynomial's coefficients for 2^(r/32) - 1 - c2 r.
constexpr double kInverseLn2N = std::bit_cast<double>(uint64_t{0x40471547652B82FE});
constexpr double kC0 = std::bit_cast<double>(uint64_t{0x3EBC6AF84B912394});
constexpr double kC1 = std::bit_cast<double>(uint64_t{0x3F2EBFCE50FAC4F3});
constexpr double kC2 = std::bit_cast<double>(uint64_t{0x3F962E42FF0C52D6});

// The range beyond which expf overflows (88.72...) and underflows (-103.97...).
constexpr uint32_t kOverflowBits = 0x42B17217;
constexpr uint32_t kUnderflowBits = 0xC2CFF1B4;

// The reciprocal of the envelope: the processor's estimate, refined twice by Newton's method unless the processing
// mode leaves it as it is.
float Reciprocal(float envelope, bool refine)
{
    float r = aarch64::Frecpe(envelope);
    if (refine)
    {
        r = r * aarch64::Frecps(envelope, r);
        r = r * aarch64::Frecps(envelope, r);
    }

    return r;
}

// The look-ahead's next place, round the samples it holds.
void Advance(LightLimiterState& state, int32_t look_ahead)
{
    ++state.position;
    while (state.position >= look_ahead)
    {
        state.position -= look_ahead;
    }
}

} // namespace

float Expf(float x)
{
    // |x| of 88 or more, from bits 20 to 30: -infinity gives 0, infinity and NaN give x + x, and a result out of range
    // gives infinity or 0.
    const uint32_t bits = std::bit_cast<uint32_t>(x);
    const uint32_t top = bits >> 20 & 0x7FF;
    if (top >= 0x42B)
    {
        if (bits == 0xFF800000)
        {
            return 0.0f;
        }
        if (top >= 0x7F8)
        {
            return x + x;
        }
        if (x > std::bit_cast<float>(kOverflowBits))
        {
            return std::numeric_limits<float>::infinity();
        }
        if (x < std::bit_cast<float>(kUnderflowBits))
        {
            return 0.0f;
        }
    }

    // e^x = 2^(k/32) 2^(r/32), with k the nearest integer to x 32 / ln 2, halves away from zero.
    const double z = kInverseLn2N * static_cast<double>(x);
    const double kd = std::round(z);
    const auto ki = static_cast<int64_t>(kd);
    const double r = z - kd;
    const double scale =
        std::bit_cast<double>(kExp2Table[static_cast<std::size_t>(ki & 31)] + (static_cast<uint64_t>(ki) << 47));
    const double y = r * r * (kC1 + kC0 * r) + (r * kC2 + 1.0);

    return static_cast<float>(y * scale);
}

float LightLimiterCoefficient(int32_t sample_rate, int32_t microseconds)
{
    if (microseconds < 1)
    {
        return 1.0f;
    }

    const float step = -1.0f / static_cast<float>(sample_rate);
    const float seconds = static_cast<float>(microseconds) / 1000.0f / 1000.0f;

    return 1.0f - Expf(step / seconds);
}

int32_t LightLimiterSamples(int32_t sample_rate, int64_t microseconds)
{
    const float seconds = static_cast<float>(microseconds) / 1000.0f / 1000.0f;

    return std::max(ToInt(seconds * static_cast<float>(sample_rate)), 1);
}

void InitializeLightLimiter(const LightLimiterParameter& parameter, LightLimiterState& state)
{
    state.envelope.fill(0.0f);
    state.gain.fill(1.0f);
    const auto channels = static_cast<std::size_t>(std::clamp(parameter.channel_count, 0, kLightLimiterChannels));
    const auto most = static_cast<std::size_t>(std::max(parameter.max_look_ahead, 0));
    state.look_ahead.assign(std::min(channels * most, kMostCells), 0.0f);
    state.position = 0;
}

void PassLightLimiter(const LightLimiterParameter& parameter, std::span<const int32_t* const> inputs,
                      std::span<int32_t* const> outputs, int count)
{
    PassChannels(parameter.channel_count, inputs, outputs, count);
}

void ApplyLightLimiter(const LightLimiterParameter& parameter, LightLimiterState& state,
                       std::span<const int32_t* const> inputs, std::span<int32_t* const> outputs, int count)
{
    const int channels = parameter.channel_count;
    if (channels == 0)
    {
        return;
    }
    if (!ProcessesChannels(channels))
    {
        PassLightLimiter(parameter, inputs, outputs, count);
        return;
    }
    const auto lanes = static_cast<std::size_t>(channels);
    if (inputs.size() < lanes || outputs.size() < lanes)
    {
        return;
    }

    // The look-ahead holds at least a sample. The console reads past its buffer for a look-ahead longer than the
    // buffer. The client never asks for one. The model's buffer grows to it.
    const int32_t look_ahead = std::max(parameter.look_ahead, 1);
    const std::size_t cells = std::min(lanes * static_cast<std::size_t>(look_ahead), kMostCells);
    if (state.look_ahead.size() < cells)
    {
        state.look_ahead.resize(cells, 0.0f);
    }

    const int32_t places = static_cast<int32_t>(cells / lanes);
    if (state.position >= places)
    {
        state.position = 0;
    }

    const float threshold = parameter.threshold;
    std::array<float, kLightLimiterChannels> samples{};
    for (int t = 0; t < count; ++t)
    {
        for (std::size_t c = 0; c < lanes; ++c)
        {
            // One channel takes the magnitude by its sign, and divides. The others take the absolute value and the
            // processor's reciprocal.
            const float x = static_cast<float>(inputs[c][t]) * 0x1p-15f;
            const float s = parameter.input_gain * x;
            samples[c] = s;
            float& envelope = state.envelope[c];
            float& gain = state.gain[c];
            if (channels == 1)
            {
                const float level = s < 0.0f ? -s : s;
                envelope = envelope + (level > envelope ? parameter.attack : parameter.release) * (level - envelope);
                const float target = envelope > threshold ? threshold / envelope : 1.0f;
                gain = gain + (target < gain ? parameter.attack : parameter.release) * (target - gain);
                continue;
            }

            const float level = std::fabs(s);
            envelope = envelope + (level - envelope) * (level > envelope ? parameter.attack : parameter.release);
            const bool refine = parameter.processing_mode != 1 || (channels == 6 && c >= 2);
            const float target = envelope > threshold ? threshold * Reciprocal(envelope, refine) : 1.0f;
            gain = gain + (target - gain) * (gain > target ? parameter.attack : parameter.release);
        }

        // Each channel's sample from the look-ahead at the new gain, and the new sample in its place.
        const std::size_t base = static_cast<std::size_t>(state.position) * lanes;
        for (std::size_t c = 0; c < lanes; ++c)
        {
            float& cell = state.look_ahead[base + c];
            const float limited = cell * state.gain[c] * parameter.output_gain;
            cell = samples[c];
            if (channels == 1)
            {
                outputs[c][t] = ToInt(aarch64::Fmaxnm(aarch64::Fmin(limited * 32768.0f, 8388607.0f), -8388608.0f));
            }
            else
            {
                outputs[c][t] = ToFixed(limited, 15);
            }
        }

        Advance(state, places);
    }
}

} // namespace ntdlib::nx::audio
