// SPDX-License-Identifier: MIT

// The steps of the lines that both reverbs of the Wii U run after their early reflections (ReverbLines in
// ntdlib/ax/reverb_lines.h): Init's test of the longest pre-delay, __InitParams's tests and set-up, a sample through
// each comb, allpass and low-pass, and the steps of SettingsUpdate. Each step runs the library's operations in its
// order. src/ax/reverb_lines.h contains the steps that the Wii's reverbs use too.

#pragma once

#include <cstdint>
#include <optional>

#include "ax/reverb_lines.h"
#include "cafe/axfx/reverb_tables.h"
#include "cafe/axfx/util.h"
#include "ntdlib/ax/reverb_lines.h"
#include "ntdlib/cafe/axfx/axfx.h"
#include "ntdlib/fused.h"

namespace ntdlib::cafe::axfx::reverb
{

// The lines of a reverb with `kCombs` combs and `kAllpasses` allpasses on each channel of a bus.
template <int kCombs, int kAllpasses>
using Lines = ntdlib::ax::axfx::ReverbLines<kChannelCount, kCombs, kAllpasses>;

// The steps that the Wii's reverbs use too.
using ntdlib::ax::axfx::reverb::Allocate;
using ntdlib::ax::axfx::reverb::ClearLines;
using ntdlib::ax::axfx::reverb::FreeLines;
using ntdlib::ax::axfx::reverb::PreDelay;
using ntdlib::ax::axfx::reverb::StepLines;

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
bool SetUp(Lines<kCombs, kAllpasses>& lines, const Params& p, const uint32_t* sizes, float rate)
{
    // The library's pre-delay can be longer than its line in two cases: a NaN longest pre-delay, and a longer
    // pre_delay_time_max given to SettingsUpdate. The library would then write past the line. The model fails instead.
    lines.pre_delay_pos = 0;
    lines.pre_delay_length = ntdlib::CafeToUint32(p.pre_delay_time * rate);
    if (lines.pre_delay_length > lines.pre_delay_line[0].size())
    {
        return false;
    }

    ntdlib::ax::axfx::reverb::SetLengths(lines, sizes);
    for (int comb = 0; comb < kCombs; comb++)
    {
        lines.comb_coef[comb] = CombCoef(sizes[comb], p.fused_time, rate);
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
float Comb(Lines<kCombs, kAllpasses>& lines, int channel, int comb, float input)
{
    return CombStep(lines.comb_line[channel][comb][lines.comb_pos[comb]], lines.comb_coef[comb], input);
}

// A sample through a channel's allpass.
template <int kCombs, int kAllpasses>
float Allpass(Lines<kCombs, kAllpasses>& lines, int channel, int allpass, float input)
{
    return AllpassStep(lines.allpass_line[channel][allpass][lines.allpass_pos[allpass]], lines.allpass_coef, input);
}

// A sample through a channel's low-pass: `input` at `input_gain` (1 less the weight, once a frame) plus the last output
// at the weight.
template <int kCombs, int kAllpasses>
float Lowpass(Lines<kCombs, kAllpasses>& lines, int channel, float input, float input_gain)
{
    const float output = Fmadds(input_gain, input, lines.lpf_coef * lines.last_lpf_out[channel]);
    lines.last_lpf_out[channel] = output;

    return output;
}

// SettingsUpdate in both reverbs, once the reverb holds the new parameters: `clear` clears the lines, and `init_params`
// sets the reverb up again on them. After a Shutdown or a failed Init, the library would set the reverb up on lines it
// has freed, and its callback would then run on null lines. The model fails instead, where `has_lines` is false. On a
// failure, `off` turns the reverb off. After a success, the next Process leaves its frame as it is.
template <typename Off, typename Clear, typename InitParams>
bool SettingsUpdate(uint32_t& active, bool has_lines, const Off& off, const Clear& clear, const InitParams& init_params)
{
    active |= kOff;
    if (!has_lines)
    {
        off();
        return false;
    }

    clear();
    if (!init_params())
    {
        off();
        return false;
    }

    active = (active | kSkip) & ~kOff;

    return true;
}

} // namespace ntdlib::cafe::axfx::reverb
