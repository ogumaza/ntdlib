// SPDX-License-Identifier: MIT

// The audio renderer's light limiter, after Nintendo's renderer for the CPU. The functions follow
// InitializeLightLimiterEffect (0x227500) and ApplyLightLimiterEffect (0x227600) in WarioWare: Move It!'s sdk, and the
// client's coefficients for the attack and the release (SetLightLimiterAttackTime, 0x1d1d80, with the sdk's expf,
// 0x5a5ea0). Each channel follows its level with an envelope, and its gain follows the gain that would hold the
// envelope to the threshold. The look-ahead delays the samples. The gain from a sample's level then reaches the sample
// sooner. The arithmetic is in single precision, on samples scaled to 1.0 for 32,768.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ntdlib::nx::audio
{

// The most channels a light limiter has.
inline constexpr int kLightLimiterChannels = 6;

// nn::audio::LightLimiterParameter as the client sets it, without the buffers and the statistics: the look-ahead in
// samples, and the coefficients for the attack and the release.
struct LightLimiterParameter
{
    int channel_count = 0;
    int max_channel_count = 0;
    int32_t sample_rate = 0;
    int32_t look_ahead = 1;
    int32_t max_look_ahead = 1;
    float attack = 1.0f;
    float release = 1.0f;
    float threshold = 0.0f;
    float input_gain = 0.0f;
    float output_gain = 0.0f;

    // nn::audio::LightLimiterType::ProcessingMode. 1 takes the reciprocal of the envelope from the processor's
    // estimate without refining it.
    uint8_t processing_mode = 0;
};

// nn::audio::LightLimiterState: each channel's envelope and gain, the samples of the look-ahead, channel after channel
// within each place, and the place to read and write next.
struct LightLimiterState
{
    std::array<float, kLightLimiterChannels> envelope{};
    std::array<float, kLightLimiterChannels> gain{};
    std::vector<float> look_ahead;
    int32_t position = 0;
};

// Starts each channel with no envelope at a gain of 1, and clears the look-ahead (InitializeLightLimiterEffect).
void InitializeLightLimiter(const LightLimiterParameter& parameter, LightLimiterState& state);

// Runs `count` samples of each channel from inputs[k] to outputs[k] (ApplyLightLimiterEffect). A channel's input and
// output may be the same buffer. A limiter of 3, 5 or more than 6 channels passes its inputs through. A single channel
// divides by its envelope and holds its output to 24 bits. The others take the processor's reciprocal estimate and
// hold their output to 32 bits. With 6 channels, the last four refine the estimate in every processing mode.
void ApplyLightLimiter(const LightLimiterParameter& parameter, LightLimiterState& state,
                       std::span<const int32_t* const> inputs, std::span<int32_t* const> outputs, int count);

// Passes `count` samples of each channel from inputs[k] to outputs[k] where they differ. A disabled limiter does that.
void PassLightLimiter(const LightLimiterParameter& parameter, std::span<const int32_t* const> inputs,
                      std::span<int32_t* const> outputs, int count);

// The coefficient that the client works out for an attack or a release of `microseconds`: 1 for less than 1 µs,
// otherwise 1 - e^(-1 / (rate * t)), in single precision.
float LightLimiterCoefficient(int32_t sample_rate, int32_t microseconds);

// The look-ahead in samples that the client works out from a time in microseconds: rate * t towards zero, and at
// least 1.
int32_t LightLimiterSamples(int32_t sample_rate, int64_t microseconds);

// The sdk's expf, to the bit: e^x from a table of 2^(i/32) and a polynomial, in double precision.
float Expf(float x);

} // namespace ntdlib::nx::audio
