// SPDX-License-Identifier: MIT

// The steps of the lines that the Wii's and the Wii U's reverbs share (ax::axfx::ReverbLines): the allocation, the
// clear and the release of the lines, a sample through the pre-delay, the lengths of the combs and the allpasses, and
// the positions' step. The steps that round differently on the two consoles stay with each console's reverbs.

#pragma once

#include <cstdint>
#include <vector>

#include "ntdlib/ax/axfx.h"
#include "ntdlib/ax/reverb_lines.h"

namespace ntdlib::ax::axfx::reverb
{

// Gives each channel's lines their longest lengths, cleared: `pre_delay_max_length` samples for the pre-delay, and
// `longest`'s lengths for the combs and then the allpasses.
template <int kChannels, int kCombs, int kAllpasses>
void Allocate(ReverbLines<kChannels, kCombs, kAllpasses>& lines, uint32_t pre_delay_max_length, const uint32_t* longest)
{
    for (int channel = 0; channel < kChannels; channel++)
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

// Sets every sample of the lines to 0.
template <int kChannels, int kCombs, int kAllpasses>
void ClearLines(ReverbLines<kChannels, kCombs, kAllpasses>& lines)
{
    for (int channel = 0; channel < kChannels; channel++)
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
template <int kChannels, int kCombs, int kAllpasses>
void FreeLines(ReverbLines<kChannels, kCombs, kAllpasses>& lines)
{
    for (int channel = 0; channel < kChannels; channel++)
    {
        lines.pre_delay_line[channel] = {};
        lines.comb_line[channel] = {};
        lines.allpass_line[channel] = {};
    }
}

// A sample through a channel's pre-delay: the line's oldest sample comes out, and `input` goes in. A pre-delay of 0
// samples passes `input` as it is.
template <int kChannels, int kCombs, int kAllpasses>
float PreDelay(ReverbLines<kChannels, kCombs, kAllpasses>& lines, int channel, float input)
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

// Sets the positions of the combs and the allpasses to 0, and their lengths to the fused mode's `sizes`: the combs'
// lengths, then the allpasses'.
template <int kChannels, int kCombs, int kAllpasses>
void SetLengths(ReverbLines<kChannels, kCombs, kAllpasses>& lines, const uint32_t* sizes)
{
    for (int comb = 0; comb < kCombs; comb++)
    {
        lines.comb_pos[comb] = 0;
        lines.comb_length[comb] = sizes[comb];
    }

    for (int allpass = 0; allpass < kAllpasses; allpass++)
    {
        lines.allpass_pos[allpass] = 0;
        lines.allpass_length[allpass] = sizes[kCombs + allpass];
    }
}

// Moves the positions of the pre-delay, the combs and the allpasses on by a sample.
template <int kChannels, int kCombs, int kAllpasses>
void StepLines(ReverbLines<kChannels, kCombs, kAllpasses>& lines)
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

} // namespace ntdlib::ax::axfx::reverb
