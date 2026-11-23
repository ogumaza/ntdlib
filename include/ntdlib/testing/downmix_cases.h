// SPDX-License-Identifier: MIT

// The cases of the check of the Switch's downmix of 6 channels to 2 (ntdlib/nx/audio/downmix.h): seven examples, and
// FNV-1a over the outputs of 65,536 cases from a linear congruential generator. nx_audio_test checks the model's
// outputs against the values that the audio DSP's code gives for them. A tool that runs the DSP's code prints those
// values from the same cases.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ntdlib/testing/fnv1a.h"

namespace ntdlib::testing
{

// The six channels of a frame, in the device sink's order: the front pair, the centre, the low frequency and the rear
// pair.
using DownmixChannels = std::array<int16_t, 6>;

// The examples: the front pair alone, the low frequency alone, the rear pair at -6 dB, the rear pair at -1 and 1
// (rounded down, and halved to -1 in mono), the centre alone, sums beyond 16 bits, and every channel but the low
// frequency.
inline constexpr std::array<DownmixChannels, 7> kDownmixExamples = {{
    {1000, -1000, 0, 0, 0, 0},
    {0, 0, 0, 32767, 0, 0},
    {0, 0, 0, 0, 16384, -16384},
    {0, 0, 0, 0, -1, 1},
    {0, 0, 1000, 0, 0, 0},
    {32767, -32768, 32767, 0, 32767, -32768},
    {1000, 3001, -3, 0, 0, -1},
}};

// FNV-1a over the outputs that `downmix(channels)` gives for 65,536 cases. Each case's six channels are the high 16
// bits of the next six states of a linear congruential generator from state 1. Each output adds its left and right
// samples to the hash, each sample's low byte first.
template <class Downmix>
uint64_t DownmixCaseHash(Downmix downmix)
{
    uint32_t state = 1;
    Fnv1a hash;
    for (int i = 0; i < 65536; ++i)
    {
        DownmixChannels channels{};
        for (int16_t& channel : channels)
        {
            state = state * 1664525u + 1013904223u;
            channel = static_cast<int16_t>(state >> 16);
        }

        const std::array<int16_t, 2> output = downmix(channels);
        hash.Add(static_cast<uint16_t>(output[0]), 2);
        hash.Add(static_cast<uint16_t>(output[1]), 2);
    }

    return hash.Value();
}

} // namespace ntdlib::testing
