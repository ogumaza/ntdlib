// SPDX-License-Identifier: MIT

// The wave list that a rip writes into the parameters of a driver for a JAudio game: the wave groups that the sound
// requires. The Z2 start-up's parameters hold one (z2_params.h), and so do other drivers' parameter blocks.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace ntdlib::flipper
{

// The most wave groups that a wave list holds.
inline constexpr uint32_t kMaxWaves = 64;

// The words of a wave list: the number of wave groups, then each group as its wave bank in the high half and its number
// in the low half, big-endian. Throws std::invalid_argument for more than kMaxWaves groups.
std::vector<uint8_t> WaveList(std::span<const uint32_t> waves);

} // namespace ntdlib::flipper
