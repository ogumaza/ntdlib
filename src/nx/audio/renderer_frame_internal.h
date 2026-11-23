// SPDX-License-Identifier: MIT

// Definitions that the parts of the renderer's frame share (renderer_frame.cpp and renderer_commands.cpp): the
// precision of the gains, the depop's decay, the ramps of gains across a frame, and the peaks of a meter. The two files
// build as one translation unit. The definitions therefore keep internal linkage. Only those files include this header.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "nx/audio/arithmetic.h"

namespace ntdlib::nx::audio
{
namespace
{

// The precision of volumes and mix volumes from REV7 on. This client requests REV8.
constexpr int kGainBits = 23;

// The depop's decay a sample, in Q15, for a mix at 48 kHz and at any other rate.
constexpr int32_t kDepopDecay48k = 31529;
constexpr int32_t kDepopDecay = 30923;

// The enhanced mode's depop: its decay a sample at 48 kHz and at any other rate, and the value below which it ends.
constexpr float kFloatDepopDecay48k = kDepopDecay48k / 32768.0f;
constexpr float kFloatDepopDecay = kDepopDecay / 32768.0f;
constexpr float kFloatDepopFloor = 1.0f / 65536.0f;

// The step of a ramp across a frame: the difference of the gains over the sample count. The difference wraps.
int32_t RampStep(int32_t from, int32_t to, int count)
{
    return WrapSub(to, from) / count;
}

// The enhanced mode's ramp across a frame: out[i] = in[i] * (from + i * step), with the step that reaches `to` after
// `count` samples. Returns the last product.
float Ramp(float* out, const float* in, float from, float to, int count)
{
    const float step = (to - from) / static_cast<float>(count);
    float last = 0.0f;
    for (int i = 0; i < count; ++i)
    {
        last = in[i] * (from + static_cast<float>(i) * step);
        out[i] = last;
    }

    return last;
}

// The same ramp added to `out`. Returns the last product.
float MixRamp(float* out, const float* in, float from, float to, int count)
{
    const float step = (to - from) / static_cast<float>(count);
    float last = 0.0f;
    for (int i = 0; i < count; ++i)
    {
        last = in[i] * (from + static_cast<float>(i) * step);
        out[i] += last;
    }

    return last;
}

// The largest magnitude of `count` samples, held to 32 bits.
int32_t PeakOf(const float* samples, std::size_t count)
{
    float peak = 0.0f;
    for (std::size_t t = 0; t < count; ++t)
    {
        peak = std::max(peak, std::fabs(samples[t]));
    }

    return static_cast<int32_t>(std::min(peak, 2147483520.0f));
}

int32_t PeakOf(const int32_t* samples, std::size_t count)
{
    int64_t peak = 0;
    for (std::size_t t = 0; t < count; ++t)
    {
        const int64_t x = samples[t];
        peak = std::max(peak, x < 0 ? -x : x);
    }

    return static_cast<int32_t>(std::min<int64_t>(peak, std::numeric_limits<int32_t>::max()));
}

} // namespace
} // namespace ntdlib::nx::audio
