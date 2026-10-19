// SPDX-License-Identifier: MIT

// The lines and filters that the standard and the high-quality reverb both run after their early reflections: a
// pre-delay, combs in parallel, allpasses with one gain, and a one-pole low-pass. Each reverb holds them and chains
// them in its order. The library's two reverbs keep the same fields.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "ntdlib/cafe/axfx/axfx.h"

namespace ntdlib::cafe::axfx::detail
{

template <int kCombs, int kAllpasses>
struct ReverbLines
{
    // The pre-delay. A length of 0 leaves the input undelayed.
    std::array<std::vector<float>, kChannelCount> pre_delay_line;
    uint32_t pre_delay_pos = 0;
    uint32_t pre_delay_length = 0;

    // The combs, by channel and comb.
    std::array<std::array<std::vector<float>, kCombs>, kChannelCount> comb_line;
    std::array<uint32_t, kCombs> comb_pos{};
    std::array<uint32_t, kCombs> comb_length{};
    std::array<float, kCombs> comb_coef{};

    // The allpasses, by channel and allpass.
    std::array<std::array<std::vector<float>, kAllpasses>, kChannelCount> allpass_line;
    std::array<uint32_t, kAllpasses> allpass_pos{};
    std::array<uint32_t, kAllpasses> allpass_length{};
    float allpass_coef = 0.0f;

    // The low-pass: each channel's last output and the weight of it.
    std::array<float, kChannelCount> last_lpf_out{};
    float lpf_coef = 0.0f;
};

} // namespace ntdlib::cafe::axfx::detail
