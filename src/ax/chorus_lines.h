// SPDX-License-Identifier: MIT

// The steps that the choruses of the Wii and the Wii U share (ax::axfx::ChorusLines and ChorusLfo): the next offset of
// the LFO, the read of the lines for a sample, and the step of the positions. Each console keeps its interpolation. The
// two interpolations round differently.

#pragma once

#include <array>
#include <cstdint>

#include "ntdlib/ax/axfx.h"
#include "ntdlib/ax/chorus_lines.h"

namespace ntdlib::ax::axfx::chorus
{

// __CalcLFO's step for one sample: the LFO's offset of the read position, in 16.16 fixed point.
inline int32_t NextLfo(ChorusLfo& lfo)
{
    // At each step of the table, the LFO starts from the step's value and moves toward the next step's value in equal
    // parts.
    const uint32_t number = lfo.phase & 0xffff0000;
    int32_t value = 0;
    if (number != lfo.last_num)
    {
        lfo.last_num = number;
        const uint32_t index = number >> 16;
        const int32_t start = kChorusLfoSine[index];
        const int32_t end = kChorusLfoSine[(index + 1) & 127];
        lfo.grad = MultiplyShift24(static_cast<int32_t>(static_cast<uint32_t>(end) - static_cast<uint32_t>(start)),
                                   lfo.grad_factor);
        value = MultiplyShift24(start, lfo.depth_samp);
    }
    else
    {
        value = static_cast<int32_t>(static_cast<uint32_t>(lfo.last_value) + static_cast<uint32_t>(lfo.grad));
    }

    // The table holds half a period. The other half is the same, negated.
    lfo.last_value = value;
    if (lfo.sign != 0)
    {
        value = static_cast<int32_t>(0u - static_cast<uint32_t>(value));
    }

    lfo.phase += static_cast<uint32_t>(lfo.phase_add);
    if ((lfo.phase & 0xff800000) != 0)
    {
        lfo.phase &= 0x7fffff;
        lfo.sign ^= 1;
    }

    return value;
}

// Moves the read position to the output position moved by the LFO's `offset`, kept within the lines. The history takes
// the samples that the read position has passed since the last sample. Returns the taps of the interpolator for the
// fraction of the distance, oldest first. The caller weights the history with these taps.
template <int kChannels>
const std::array<float, 4>& Read(ChorusLines<kChannels>& lines, int32_t offset)
{
    uint32_t position = lines.out_pos + static_cast<uint32_t>(offset);
    if (static_cast<int32_t>(position) >= static_cast<int32_t>(lines.size_fp))
    {
        position -= lines.size_fp;
    }
    else if (static_cast<int32_t>(position) < 0)
    {
        position += lines.size_fp;
    }

    uint32_t distance = position - lines.last_pos;
    if (static_cast<int32_t>(distance) < 0)
    {
        distance += lines.size_fp;
    }

    uint32_t read = lines.last_pos >> 16;
    for (uint32_t steps = distance >> 16; steps > 0; steps--)
    {
        for (int channel = 0; channel < kChannels; channel++)
        {
            lines.history[channel][lines.hist_index] = lines.line[channel][read];
        }

        lines.hist_index = (lines.hist_index + 1) & 3;
        Step(read, lines.size);
    }

    lines.last_pos = position & 0xffff0000;

    return kChorusSrcTaps[(distance & 0xffff) >> 9];
}

// Moves the input position on by a sample, and the output position by a sample in 16.16 fixed point. Both wrap to 0 at
// the lines' end.
template <int kChannels>
void StepPositions(ChorusLines<kChannels>& lines)
{
    Step(lines.in_pos, lines.size);
    lines.out_pos += 0x10000;
    if (lines.out_pos >= lines.size_fp)
    {
        lines.out_pos = 0;
    }
}

} // namespace ntdlib::ax::axfx::chorus
