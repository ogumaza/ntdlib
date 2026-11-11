// SPDX-License-Identifier: MIT

#include "ntdlib/nx/audio/downmix.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace ntdlib::nx::audio
{

namespace
{

constexpr std::size_t kChannels = 6;
constexpr int64_t kSideGain = 0xb53c;   // the centre's and the rear channels' gain, in Q16
constexpr int64_t kScaledGain = 0x6007; // the scaled downmix's gain, in Q16

// A product in Q16, rounded down: the DSP's smull, then a shift of the 64-bit product by 16.
int64_t Scale(int64_t sample, int64_t gain)
{
    return (sample * gain) >> 16;
}

int16_t Saturate(int64_t value)
{
    return static_cast<int16_t>(std::clamp<int64_t>(value, INT16_MIN, INT16_MAX));
}

} // namespace

std::array<int16_t, 2> DownmixSample(std::span<const int16_t, 6> channels, Downmix downmix)
{
    // The sums of the stereo downmix, before they're saturated to 16 bits. The centre's product is shared.
    const int64_t centre = Scale(channels[2], kSideGain);
    const int64_t left = channels[0] + centre + Scale(channels[4], kSideGain);
    const int64_t right = channels[1] + centre + Scale(channels[5], kSideGain);

    switch (downmix)
    {
    case Downmix::kStereo:
        return {Saturate(left), Saturate(right)};

    case Downmix::kMono:
        {
            const int16_t mono = static_cast<int16_t>((Saturate(left) + Saturate(right)) >> 1);
            return {mono, mono};
        }

    case Downmix::kScaled:
        return {Saturate(Scale(left, kScaledGain)), Saturate(Scale(right, kScaledGain))};

    case Downmix::kFrontPair:
        return {channels[0], channels[1]};
    }

    throw std::invalid_argument("unknown downmix");
}

void DownmixFrame(std::span<const int16_t> input, Downmix downmix, std::vector<int16_t>& output)
{
    if (input.size() % kChannels != 0)
    {
        throw std::invalid_argument("a downmix takes whole samples of 6 channels");
    }

    for (std::size_t i = 0; i < input.size(); i += kChannels)
    {
        const std::array<int16_t, 2> sample = DownmixSample(input.subspan(i).first<kChannels>(), downmix);
        output.push_back(sample[0]);
        output.push_back(sample[1]);
    }
}

} // namespace ntdlib::nx::audio
