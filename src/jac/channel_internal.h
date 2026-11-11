// SPDX-License-Identifier: MIT

// Definitions that the parts of jac's channels share (channel.cpp, channel_dsp.cpp, channel_lists.cpp,
// channel_levels.cpp and channel_oneshot.cpp). The five files build as one translation unit. The definitions
// therefore keep internal linkage. Only those files include this header.

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>

#include "ntdlib/jac/channel.h"
#include "ntdlib/jaudio2/calc.h"

namespace ntdlib::jac
{
namespace
{

using jaudio2::DspChannelBlock;
using jaudio2::ToInt32;

// Two words of a DSP channel's block that DspChannelBlock doesn't name: the frames since the DSP channel started
// (DSP_PlayStart clears them, and UpdateDSPchannelAll counts them), and the wave's count of samples.
constexpr std::size_t kPlayedFrames = 0x10c;
constexpr std::size_t kSampleCount = 0x11c;

// The pitch of the DSP's oscillators at key 60 (0x4682c008: 16,736.016), and a third (0x3eaaaaab), the weight of each
// source of a pan at a note's start.
constexpr float kOscillatorPitch = std::bit_cast<float>(0x4682c008u);
constexpr float kThird = std::bit_cast<float>(0x3eaaaaabu);

// A level within 0 to 1. A NaN stays NaN: the game's compares then fail.
[[maybe_unused]] float ClampUnit(float value)
{
    if (value < 0.0f)
    {
        return 0.0f;
    }

    return value > 1.0f ? 1.0f : value;
}

// The writes of dspinterface (0x276d70 to 0x277290 in main) that differ from JASDsp's. The rest are DspBlock's.

[[maybe_unused]] std::size_t MixerOffset(uint32_t mixer)
{
    return DspChannelBlock::kMixers + (mixer & 0xff) * 8;
}

[[maybe_unused]] void ClearBlock(DspBlock& block, std::size_t start, std::size_t end)
{
    for (std::size_t offset = start; offset < end; offset += 2)
    {
        block.SetU16(offset, 0);
    }
}

} // namespace

} // namespace ntdlib::jac
