// SPDX-License-Identifier: MIT

// What the meters of the GBA drivers' archive players share: the peaks of a frame's output.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace ntdlib::agb
{

// Raises each side's peak in `peaks`, the left side's first, to the largest magnitude of that side's samples from
// `first` on: the 16-bit samples of `out`, or, where `out` is null, the samples of `enhanced_out` at 16 bits. The
// samples hold the left and the right side in turn.
inline void KeepOutputPeaks(std::array<int32_t, 2>& peaks, const std::vector<int16_t>* out,
                            const std::vector<float>* enhanced_out, size_t first)
{
    const auto keep = [&peaks](size_t i, int32_t value)
    {
        int32_t& peak = peaks[i % 2];
        peak = std::max(peak, std::abs(value));
    };

    if (out)
    {
        for (size_t i = first; i < out->size(); i++)
        {
            keep(i - first, (*out)[i]);
        }
    }
    else
    {
        for (size_t i = first; i < enhanced_out->size(); i++)
        {
            keep(i - first, int32_t(std::lround(double((*enhanced_out)[i]) * 32768.0)));
        }
    }
}

} // namespace ntdlib::agb
