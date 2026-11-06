// SPDX-License-Identifier: MIT

// The DS's PSG and noise as samples, for SoundFont's and SFZ's players. The sound unit's timer steps a PSG channel
// through the 8 steps of its cycle, and a noise channel's shift register by one step. Each step becomes
// kPsgStepSamples samples. A sample of kPsgSampleRate plays the steps of a timer of 8006.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ntdlib::sequence_export
{

inline constexpr int kPsgStepSamples = 16;
inline constexpr uint32_t kPsgSampleRate = 33489;

// A cycle of the PSG at `duty`, from 0 to 7: the cycle starts with its low steps, and its last duty + 1 steps are
// high. A duty of 7 stays low. Nobody hears a level that doesn't move, and the samples hold 0.
inline std::vector<int16_t> PsgSamples(int duty)
{
    constexpr int16_t kHigh = 0x7FFF;
    constexpr int16_t kLow = -0x7FFF;
    std::vector<int16_t> samples;
    for (int step = 0; step < 8; step++)
    {
        const int16_t value = duty == 7 ? 0 : step >= 7 - duty ? kHigh : kLow;
        samples.insert(samples.end(), kPsgStepSamples, value);
    }
    return samples;
}

// The noise's 32,767 steps, after which its shift register repeats. The register starts at 0x7FFF. Each step shifts it
// right once. A bit shifted out turns the output low and the register's bits 13 and 14 over.
inline std::vector<int16_t> NoiseSamples()
{
    constexpr int16_t kHigh = 0x7FFF;
    constexpr int16_t kLow = -0x7FFF;
    std::vector<int16_t> samples;
    samples.reserve(std::size_t{0x7FFF} * kPsgStepSamples);
    uint16_t noise = 0x7FFF;
    for (int step = 0; step < 0x7FFF; step++)
    {
        const bool carry = (noise & 1) != 0;
        noise = static_cast<uint16_t>(noise >> 1);
        if (carry)
        {
            noise ^= 0x6000;
        }
        samples.insert(samples.end(), kPsgStepSamples, carry ? kLow : kHigh);
    }
    return samples;
}

} // namespace ntdlib::sequence_export
