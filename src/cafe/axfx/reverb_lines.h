// SPDX-License-Identifier: MIT

// The steps of the lines that both reverbs share (detail::ReverbLines): Init's test of the longest pre-delay and its
// allocation, __InitParams's tests and set-up, the clear and the release of the lines, a sample through each line and
// filter, and the positions' step. Each step runs the library's operations in its order.

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "cafe/axfx/reverb_tables.h"
#include "cafe/axfx/util.h"
#include "ntdlib/cafe/axfx/axfx.h"
#include "ntdlib/cafe/axfx/reverb_lines.h"
#include "ntdlib/fused.h"

namespace ntdlib::cafe::axfx::reverb
{

// Init's tests of the longest pre-delay: the line's length in samples at `rate`, or nothing for a negative time or a
// line longer than kMaxPreDelayLength.
inline std::optional<uint32_t> PreDelayMaxLength(float pre_delay_time_max, uint32_t rate)
{
    if (pre_delay_time_max < 0.0f)
    {
        return std::nullopt;
    }

    const uint32_t length = ntdlib::CafeToUint32(pre_delay_time_max * ToFloat(rate));
    if (length > kMaxPreDelayLength)
    {
        return std::nullopt;
    }

    return length;
}

// Gives each channel's lines their longest lengths, cleared: `pre_delay_max_length` samples for the pre-delay, and
// `longest`'s lengths for the combs and then the allpasses.
template <int kCombs, int kAllpasses>
void Allocate(detail::ReverbLines<kCombs, kAllpasses>& lines, uint32_t pre_delay_max_length, const uint32_t* longest)
{
    for (int channel = 0; channel < kChannelCount; channel++)
    {
        lines.pre_delay_line[channel].assign(pre_delay_max_length, 0.0f);
        for (int comb = 0; comb < kCombs; comb++)
        {
            lines.comb_line[channel][comb].assign(longest[comb], 0.0f);
        }

        for (int allpass = 0; allpass < kAllpasses; allpass++)
        {
            lines.allpass_line[channel][allpass].assign(longest[kCombs + allpass], 0.0f);
        }
    }
}

// __InitParams's tests of the parameters that both reverbs take: whether any of them is out of its range. A NaN gain
// passes them, as it passes the library's.
template <typename Params>
bool AnyOutOfRange(const Params& p)
{
    return p.early_mode >= kEarlyModes || p.pre_delay_time < 0.0f || p.pre_delay_time > p.pre_delay_time_max ||
           p.fused_mode >= kFusedModes || p.fused_time < 0.0f || OutOfRange(p.coloration) || OutOfRange(p.damping) ||
           OutOfRange(p.early_gain) || OutOfRange(p.fused_gain) || OutOfRange(p.out_gain) || OutOfRange(p.send_gain);
}

// __InitParams's set-up of the shared lines for `p`: the pre-delay, the lengths of the combs and the allpasses from
// the fused mode's `sizes`, the combs' gains, and the coloration and the damping. `rate` is the renderer's rate as a
// float. Returns false for a pre-delay longer than its line.
template <int kCombs, int kAllpasses, typename Params>
bool SetUp(detail::ReverbLines<kCombs, kAllpasses>& lines, const Params& p, const uint32_t* sizes, float rate)
{
    // The library's pre-delay can be longer than its line in two cases: a NaN longest pre-delay, and a longer
    // pre_delay_time_max given to SettingsUpdate. The library would then write past the line. The model fails instead.
    lines.pre_delay_pos = 0;
    lines.pre_delay_length = ntdlib::CafeToUint32(p.pre_delay_time * rate);
    if (lines.pre_delay_length > lines.pre_delay_line[0].size())
    {
        return false;
    }

    for (int comb = 0; comb < kCombs; comb++)
    {
        lines.comb_pos[comb] = 0;
        lines.comb_length[comb] = sizes[comb];
        lines.comb_coef[comb] = CombCoef(sizes[comb], p.fused_time, rate);
    }

    for (int allpass = 0; allpass < kAllpasses; allpass++)
    {
        lines.allpass_pos[allpass] = 0;
        lines.allpass_length[allpass] = sizes[kCombs + allpass];
    }

    // The coloration is the allpasses' gain. The low-pass weighs its last output by 1 - damping, at most 0.95.
    lines.allpass_coef = p.coloration;
    lines.lpf_coef = 1.0f - p.damping;
    if (lines.lpf_coef > 0.95f)
    {
        lines.lpf_coef = 0.95f;
    }

    lines.last_lpf_out.fill(0.0f);

    return true;
}

// Sets every sample of the lines to 0.
template <int kCombs, int kAllpasses>
void ClearLines(detail::ReverbLines<kCombs, kAllpasses>& lines)
{
    for (int channel = 0; channel < kChannelCount; channel++)
    {
        Clear(lines.pre_delay_line[channel]);
        for (std::vector<float>& line : lines.comb_line[channel])
        {
            Clear(line);
        }

        for (std::vector<float>& line : lines.allpass_line[channel])
        {
            Clear(line);
        }
    }
}

// Frees the lines.
template <int kCombs, int kAllpasses>
void FreeLines(detail::ReverbLines<kCombs, kAllpasses>& lines)
{
    for (int channel = 0; channel < kChannelCount; channel++)
    {
        lines.pre_delay_line[channel] = {};
        lines.comb_line[channel] = {};
        lines.allpass_line[channel] = {};
    }
}

// A sample through a channel's pre-delay: the line's oldest sample comes out, and `input` goes in. A pre-delay of 0
// samples passes `input` as it is.
template <int kCombs, int kAllpasses>
float PreDelay(detail::ReverbLines<kCombs, kAllpasses>& lines, int channel, float input)
{
    if (lines.pre_delay_length == 0)
    {
        return input;
    }

    float& sample = lines.pre_delay_line[channel][lines.pre_delay_pos];
    const float delayed = sample;
    sample = input;

    return delayed;
}

// A comb's step at its position's `sample`: the oldest sample comes out, and goes back in at `gain` with `input`.
inline float CombStep(float& sample, float gain, float input)
{
    const float output = sample;
    sample = Fmadds(output, gain, input);

    return output;
}

// An allpass's step at its position's `sample`: the oldest sample at `gain` with `input` goes in, and the oldest
// sample less the new one at `gain` comes out.
inline float AllpassStep(float& sample, float gain, float input)
{
    const float delayed = sample;
    sample = Fmadds(delayed, gain, input);

    return NegatedFms(sample, gain, delayed);
}

// A sample through a channel's comb.
template <int kCombs, int kAllpasses>
float Comb(detail::ReverbLines<kCombs, kAllpasses>& lines, int channel, int comb, float input)
{
    return CombStep(lines.comb_line[channel][comb][lines.comb_pos[comb]], lines.comb_coef[comb], input);
}

// A sample through a channel's allpass.
template <int kCombs, int kAllpasses>
float Allpass(detail::ReverbLines<kCombs, kAllpasses>& lines, int channel, int allpass, float input)
{
    return AllpassStep(lines.allpass_line[channel][allpass][lines.allpass_pos[allpass]], lines.allpass_coef, input);
}

// A sample through a channel's low-pass: `input` at `input_gain` (1 less the weight, once a frame) plus the last output
// at the weight.
template <int kCombs, int kAllpasses>
float Lowpass(detail::ReverbLines<kCombs, kAllpasses>& lines, int channel, float input, float input_gain)
{
    const float output = Fmadds(input_gain, input, lines.lpf_coef * lines.last_lpf_out[channel]);
    lines.last_lpf_out[channel] = output;

    return output;
}

// Moves the positions of the pre-delay, the combs and the allpasses on by a sample.
template <int kCombs, int kAllpasses>
void StepLines(detail::ReverbLines<kCombs, kAllpasses>& lines)
{
    if (lines.pre_delay_length != 0)
    {
        Step(lines.pre_delay_pos, lines.pre_delay_length);
    }

    for (int comb = 0; comb < kCombs; comb++)
    {
        Step(lines.comb_pos[comb], lines.comb_length[comb]);
    }

    for (int allpass = 0; allpass < kAllpasses; allpass++)
    {
        Step(lines.allpass_pos[allpass], lines.allpass_length[allpass]);
    }
}

} // namespace ntdlib::cafe::axfx::reverb
