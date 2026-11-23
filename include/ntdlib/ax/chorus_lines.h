// SPDX-License-Identifier: MIT

// The state that the chorus of the Wii's and the Wii U's AXFX keeps: the LFO, the lines with their positions, and the
// history of the samples that the read position has passed. The two libraries' choruses keep the same fields.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace ntdlib::ax::axfx
{

// AXFX_CHORUS_EXP_LFO: the LFO's step, depth, phase and interpolation, in 16.16 fixed point. The libraries also store
// the table's address, and a step in samples (stepSamp) that nothing reads.
struct ChorusLfo
{
    int32_t phase_add = 0;
    int32_t depth_samp = 0;
    uint32_t phase = 0;
    uint32_t sign = 0;
    uint32_t last_num = 0;
    int32_t last_value = 0;
    int32_t grad = 0;
    int32_t grad_factor = 0;
};

// The lines of each of `kChannels` channels, the positions in them, and each channel's last 4 samples that the read
// position has passed.
template <int kChannels>
struct ChorusLines
{
    std::array<std::vector<float>, kChannels> line;
    uint32_t in_pos = 0;
    uint32_t out_pos = 0;  // 16.16
    uint32_t last_pos = 0; // 16.16
    uint32_t size_fp = 0;  // 16.16
    uint32_t size = 0;
    std::array<std::array<float, 4>, kChannels> history{};
    uint32_t hist_index = 0;
};

} // namespace ntdlib::ax::axfx
