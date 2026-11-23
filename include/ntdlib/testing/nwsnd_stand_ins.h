// SPDX-License-Identifier: MIT

// The parts of a console policy that the tests of ntdlib/nwsnd/channel.h replace with stand-ins: the waves that a
// channel starts and the outputs that it mixes to.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ntdlib::testing::nwsnd_stand_ins
{

// A test policy derives from this, with the type of a wave's format, the rate of a wave at unison and the number of
// effect sends of its console.
template <typename Format, uint32_t kSampleRate, std::size_t kFxSends>
struct PolicyParts
{
    struct WaveInfo
    {
        Format format = 0;
        uint32_t sample_rate = kSampleRate;
        bool loop = false;
    };

    // The track's main out volume, pan, surround pan and sends.
    struct Outputs
    {
        float volume = 1.0f;
        uint32_t mix_mode = 0;
        float pan = 0.0f;
        float span = 0.0f;
        float main_send = 0.0f;
        std::array<float, kFxSends> fx_send{};
    };

    // One output: the function takes each of `outputs` once.
    template <typename Function, typename... Each>
    static void ForEachOutput(const Function& function, Each&... outputs)
    {
        function(outputs...);
    }
};

} // namespace ntdlib::testing::nwsnd_stand_ins
