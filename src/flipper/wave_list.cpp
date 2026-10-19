// SPDX-License-Identifier: MIT

#include "ntdlib/flipper/wave_list.h"

#include <cstddef>
#include <stdexcept>
#include <string>

#include "xsflib/bytes.h"

namespace ntdlib::flipper
{

std::vector<uint8_t> WaveList(std::span<const uint32_t> waves)
{
    if (waves.size() > kMaxWaves)
    {
        throw std::invalid_argument("a sound names " + std::to_string(waves.size()) + " waves, more than " +
                                    std::to_string(kMaxWaves));
    }

    std::vector<uint8_t> words(4 + waves.size() * 4);
    xsflib::StoreBe32(words.data(), static_cast<uint32_t>(waves.size()));
    for (std::size_t i = 0; i < waves.size(); i++)
    {
        xsflib::StoreBe32(words.data() + 4 + i * 4, waves[i]);
    }

    return words;
}

} // namespace ntdlib::flipper
