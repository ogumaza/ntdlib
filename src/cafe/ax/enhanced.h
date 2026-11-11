// SPDX-License-Identifier: MIT

// The steps that the enhanced mode's voices (render.cpp) and its aux returns (system_output.cpp) share: the fractions
// of the conversions between 32 and 48 kHz, and a gain that moves across a 48 kHz frame.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ntdlib/cafe/ax/pb.h"

namespace ntdlib::cafe::ax
{

// The fraction of a sample, in 65536ths, of output `i` of a conversion that steps two thirds of a sample per output:
// 0, a third or two thirds in turn.
inline uint32_t TwoThirdsFraction(int i)
{
    constexpr std::array<uint32_t, 3> kThirds = {0, 21845, 43691};

    return kThirds[static_cast<std::size_t>(2 * i % 3)];
}

// The gain at sample `i` of a 48 kHz frame, by equal steps from `from` before the frame to `to` at its last sample.
inline float FrameRampAt(float from, float to, int i)
{
    return from + (to - from) * static_cast<float>(i + 1) / static_cast<float>(kSamplesPerFrame48k);
}

} // namespace ntdlib::cafe::ax
