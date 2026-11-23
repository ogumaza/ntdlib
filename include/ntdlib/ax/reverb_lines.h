// SPDX-License-Identifier: MIT

// The lines and filters that the standard and the high-quality reverb of the Wii's and the Wii U's AXFX run after
// their early reflections: a pre-delay, combs in parallel, allpasses with one gain, and a one-pole low-pass. Each
// reverb contains them and chains them in its order. The libraries' four reverbs keep the same fields.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace ntdlib::ax::axfx
{

template <int kChannels, int kCombs, int kAllpasses>
struct ReverbLines
{
    // The pre-delay. A length of 0 leaves the input undelayed.
    std::array<std::vector<float>, kChannels> pre_delay_line;
    uint32_t pre_delay_pos = 0;
    uint32_t pre_delay_length = 0;

    // The combs, by channel and comb.
    std::array<std::array<std::vector<float>, kCombs>, kChannels> comb_line;
    std::array<uint32_t, kCombs> comb_pos{};
    std::array<uint32_t, kCombs> comb_length{};
    std::array<float, kCombs> comb_coef{};

    // The allpasses, by channel and allpass.
    std::array<std::array<std::vector<float>, kAllpasses>, kChannels> allpass_line;
    std::array<uint32_t, kAllpasses> allpass_pos{};
    std::array<uint32_t, kAllpasses> allpass_length{};
    float allpass_coef = 0.0f;

    // The low-pass: each channel's last output and the weight of it.
    std::array<float, kChannels> last_lpf_out{};
    float lpf_coef = 0.0f;
};

} // namespace ntdlib::ax::axfx
