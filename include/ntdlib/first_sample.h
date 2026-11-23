// SPDX-License-Identifier: MIT

// The first sample that isn't 0 in a frame of stereo output. The length analyses' models report it to xsflib's
// analysis (FirstSampleMs in xsflib/rip/length.h), and a rip's length tags count from it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace ntdlib
{

// The milliseconds from the start of `stereo`, a frame of output with the left and the right side in turn, to its first
// sample that isn't 0 on either side. Each pair of samples lasts `pair_ms`. Nothing when every sample is 0.
inline std::optional<double> FirstSampleMs(std::span<const int16_t> stereo, double pair_ms)
{
    for (std::size_t i = 0; i < stereo.size(); ++i)
    {
        if (stereo[i] != 0)
        {
            return static_cast<double>(i / 2) * pair_ms;
        }
    }

    return std::nullopt;
}

} // namespace ntdlib
