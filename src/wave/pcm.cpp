// SPDX-License-Identifier: MIT

#include "ntdlib/wave/pcm.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ntdlib::wave
{

std::vector<int16_t> DecodePcm8(std::span<const uint8_t> data, uint32_t samples)
{
    std::vector<int16_t> out;
    out.reserve(std::min<std::size_t>(samples, data.size()));
    for (uint32_t i = 0; i < samples && i < data.size(); i++)
    {
        out.push_back(static_cast<int16_t>(static_cast<int8_t>(data[i]) * 256));
    }

    return out;
}

std::vector<int16_t> DecodePcm16(std::span<const uint8_t> data, uint32_t samples, bool big_endian)
{
    std::vector<int16_t> out;
    out.reserve(std::min<std::size_t>(samples, data.size() / 2));
    for (uint32_t i = 0; i < samples && 2 * std::size_t{i} + 1 < data.size(); i++)
    {
        const uint8_t first = data[2 * std::size_t{i}], second = data[2 * std::size_t{i} + 1];
        out.push_back(static_cast<int16_t>(big_endian ? first << 8 | second : second << 8 | first));
    }

    return out;
}

} // namespace ntdlib::wave
