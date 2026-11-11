// SPDX-License-Identifier: MIT

// What the renderer's delay, reverb and light limiter share: the channel counts that they process, and the copy that
// passes their inputs through.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ntdlib::nx::audio
{

// Whether an effect processes `channels` channels: 1, 2, 4 or 6. With another count, the delay and the reverb pass
// their inputs through, and the light limiter's command has no channels.
inline bool ProcessesChannels(int channels)
{
    return channels == 1 || channels == 2 || channels == 4 || channels == 6;
}

// Copies `count` samples of each of the first `channel_count` channels from inputs[k] to outputs[k] where the two
// differ, as far as both spans go.
inline void PassChannels(int channel_count, std::span<const int32_t* const> inputs, std::span<int32_t* const> outputs,
                         int count)
{
    const auto channels =
        std::min({static_cast<std::size_t>(std::max(channel_count, 0)), inputs.size(), outputs.size()});
    for (std::size_t k = 0; k < channels; ++k)
    {
        if (inputs[k] != outputs[k])
        {
            std::copy(inputs[k], inputs[k] + count, outputs[k]);
        }
    }
}

} // namespace ntdlib::nx::audio
