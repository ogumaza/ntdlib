// SPDX-License-Identifier: MIT

// The steps of the lines that both reverbs of the Wii run after their early reflections (ReverbLines in
// ntdlib/ax/reverb_lines.h): Init's test of the longest pre-delay, __InitParams's tests and set-up, a sample through
// each comb, allpass and low-pass, and the steps of Settings and SettingsUpdate. Each step runs the game's operations
// in its order. src/ax/reverb_lines.h contains the steps that the Wii U's reverbs use too.

#pragma once

#include <cstdint>
#include <optional>

#include "ax/reverb_lines.h"
#include "ntdlib/ax/reverb_lines.h"
#include "ntdlib/rvl/axfx/axfx.h"
#include "ntdlib/rvl/axfx/util.h"

namespace ntdlib::rvl::axfx::reverb
{

// The lines of a reverb with `kCombs` combs and `kAllpasses` allpasses on each channel of a bus.
template <int kCombs, int kAllpasses>
using Lines = ntdlib::ax::axfx::ReverbLines<kChannelCount, kCombs, kAllpasses>;

// The steps that the Wii U's reverbs use too.
using ntdlib::ax::axfx::reverb::Allocate;
using ntdlib::ax::axfx::reverb::ClearLines;
using ntdlib::ax::axfx::reverb::FreeLines;
using ntdlib::ax::axfx::reverb::PreDelay;
using ntdlib::ax::axfx::reverb::StepLines;

// The longest pre-delay line that the model allocates. Three lines of this length would take 96 MB. The Wii has only
// 88 MB of memory. The game's allocation fails long before this length.
constexpr uint32_t kMaxPreDelayLength = 1u << 23;

// Init's tests of the longest pre-delay: the line's length in samples, or nothing for a negative time or a line of
// kMaxPreDelayLength samples or more.
inline std::optional<uint32_t> PreDelayMaxLength(float pre_delay_time_max)
{
    const uint32_t length = ToUnsigned(kSampleRate * pre_delay_time_max);
    if (pre_delay_time_max < 0.0f || length >= kMaxPreDelayLength)
    {
        return std::nullopt;
    }

    return length;
}

// __InitParams's tests of the parameters that both reverbs take, for `early_modes` early modes and `fused_modes` fused
// modes: whether any of them is out of its range. A NaN gain passes them, as it passes the games' comparisons.
template <typename Params>
bool AnyOutOfRange(const Params& p, uint32_t early_modes, uint32_t fused_modes)
{
    return p.early_mode >= early_modes || p.pre_delay_time < 0.0f || p.pre_delay_time > p.pre_delay_time_max ||
           p.fused_mode >= fused_modes || p.fused_time < 0.0f || OutOfRange(p.coloration) || OutOfRange(p.damping) ||
           OutOfRange(p.early_gain) || OutOfRange(p.fused_gain) || OutOfRange(p.out_gain) || OutOfRange(p.send_gain);
}

// __InitParams's set-up of the shared lines for `p`: the pre-delay, the lengths of the combs and the allpasses from
// the fused mode's `sizes`, the combs' gains, and the coloration and the damping. Returns false for a pre-delay longer
// than its line.
template <int kCombs, int kAllpasses, typename Params>
bool SetUp(Lines<kCombs, kAllpasses>& lines, const Params& p, const uint32_t* sizes)
{
    // The game's pre-delay can be longer than its line in two cases: a NaN time, and a longer pre_delay_time_max
    // passed to SettingsUpdate. The game would then write past the line. The model fails instead.
    lines.pre_delay_pos = 0;
    lines.pre_delay_length = ToUnsigned(kSampleRate * p.pre_delay_time);
    if (lines.pre_delay_length > lines.pre_delay_line[0].size())
    {
        return false;
    }

    ntdlib::ax::axfx::reverb::SetLengths(lines, sizes);
    for (int comb = 0; comb < kCombs; comb++)
    {
        lines.comb_coef[comb] = CombCoef(sizes[comb], p.fused_time);
    }

    // The coloration is the allpasses' gain. The damping sets the low-pass's weight.
    lines.allpass_coef = p.coloration;
    lines.lpf_coef = LowpassCoef(p.damping);
    lines.last_lpf_out.fill(0.0f);

    return true;
}

// A sample through a channel's comb: the oldest sample comes out, and goes back in at the comb's gain with `input`.
template <int kCombs, int kAllpasses>
float Comb(Lines<kCombs, kAllpasses>& lines, int channel, int comb, float input)
{
    float& sample = lines.comb_line[channel][comb][lines.comb_pos[comb]];
    const float output = sample;
    sample = input + output * lines.comb_coef[comb];

    return output;
}

// A sample through a channel's allpass.
template <int kCombs, int kAllpasses>
float Allpass(Lines<kCombs, kAllpasses>& lines, int channel, int allpass, float input)
{
    return axfx::Allpass(lines.allpass_line[channel][allpass][lines.allpass_pos[allpass]], input, lines.allpass_coef);
}

// A sample through a channel's low-pass: `input` at `input_gain` (1 less the weight, once a frame) plus the last output
// at the weight.
template <int kCombs, int kAllpasses>
float Lowpass(Lines<kCombs, kAllpasses>& lines, int channel, float input, float input_gain)
{
    const float output = input_gain * input + lines.lpf_coef * lines.last_lpf_out[channel];
    lines.last_lpf_out[channel] = output;

    return output;
}

// Settings in both reverbs: `free_lines` frees the lines, and `init` runs Init. After Init succeeds, the next Process
// leaves its frame as it is.
template <typename FreeLines, typename Init>
bool Settings(uint32_t& active, const FreeLines& free_lines, const Init& init)
{
    free_lines();
    if (!init())
    {
        return false;
    }

    active = (active | kSkipFrame) & ~kStopped;

    return true;
}

// SettingsUpdate in both reverbs, once the reverb holds the new parameters: `clear` clears the lines, and `init_params`
// sets the reverb up again on them. The game's callback would run on null lines after a failed Init or a Shutdown. The
// model fails instead, where `has_lines` is false. If `init_params` fails, `free_lines` frees the lines. After a
// success, the next Process leaves its frame as it is.
template <typename FreeLines, typename Clear, typename InitParams>
bool SettingsUpdate(uint32_t& active, bool has_lines, const FreeLines& free_lines, const Clear& clear,
                    const InitParams& init_params)
{
    active |= kStopped;
    if (!has_lines)
    {
        return false;
    }

    clear();
    if (!init_params())
    {
        free_lines();
        return false;
    }

    active = (active | kSkipFrame) & ~kStopped;

    return true;
}

} // namespace ntdlib::rvl::axfx::reverb
