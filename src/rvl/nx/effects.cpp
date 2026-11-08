// SPDX-License-Identifier: MIT

#include "ntdlib/rvl/nx/effects.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace ntdlib::rvl::nx
{

namespace
{

namespace audio = ntdlib::nx::audio;

// The game's reverbs (0x4cbd260): 8, each on 4 channels at 32 kHz. Each channel takes and gives its block of the bus's
// samples (0x1010a78).
constexpr std::size_t kReverbSlots = 8;
constexpr int32_t kReverbRate = 32000;
constexpr int kReverbChannels = 4;
constexpr std::array<int8_t, kReverbChannels> kReverbBlocks{0, 1, 2, 3};

// The late modes for the fused modes 0 to 5 (0x13a8130).
constexpr std::array<int32_t, 6> kLateModes{4, 2, 0, 0, 1, 3};

// AArch64's fmax: NaN if either value is NaN.
float Max(float a, float b)
{
    if (std::isnan(a) || std::isnan(b))
    {
        return std::numeric_limits<float>::quiet_NaN();
    }

    return a > b || (a == b && !std::signbit(a)) ? a : b;
}

// A value held to 0 and 1, as SetParam holds it: fmax with 0, and 1 for what's above 1.
float Unit(float value)
{
    const float low = Max(value, 0.0f);
    return value > 1.0f ? 1.0f : low;
}

} // namespace

audio::ReverbParameterSet ReverbParameters(const ReverbParam& param)
{
    // SetParam (0x10104e8).
    const float predelay_max = param.predelay_time_max;
    const float predelay = param.predelay_time > predelay_max ? predelay_max : Max(param.predelay_time, 0.0f);
    const float fused_time = Max(param.fused_time, 0.0f);

    // StartUp (0x1010aac to 0x1010b90).
    audio::ReverbParameterSet set;
    set.early_mode = static_cast<uint32_t>(param.early_mode) - 5u < 4u ? 3 : 2;
    set.early_gain = Unit(param.early_gain);
    set.predelay = predelay * 1000.0f;
    const auto fused_mode = static_cast<uint32_t>(param.fused_mode);
    set.late_mode = fused_mode < kLateModes.size() ? kLateModes[fused_mode] : 2;
    set.late_gain = Unit(param.fused_gain);
    set.decay_time = fused_time;
    set.high_frequency_decay_ratio = Unit(param.damping);
    set.coloration = Unit(Unit(param.coloration) / 0.6f);
    set.reverb_gain = 1.0f;
    set.out_gain = Unit(param.out_gain);
    set.dry_gain = 0.0f;

    return set;
}

void Effects::AppendReverb(ax::System& ax, Layer& layer, int bus, const ReverbParam& param)
{
    // AppendEffect (0x100e7e8): the bus's return to full volume, then StartUp (0x10108c4). A StartUp that finds no
    // free reverb fails, and the object stays off the list.
    const auto index = static_cast<std::size_t>(bus);
    ax.SetAuxReturnVolume(bus, ax::kMaxVolume);
    std::size_t slot = 0;
    while (slot < slot_used_.size() && slot_used_[slot])
    {
        slot++;
    }

    if (slot == kReverbSlots)
    {
        return;
    }

    if (slot < slots_.size())
    {
        slots_[slot].Reset();
    }
    else
    {
        slots_.emplace_back(kReverbRate, kReverbChannels);
        slots_.back().SetInputOutput(kReverbBlocks, kReverbBlocks);
        slot_used_.push_back(false);
    }

    slot_used_[slot] = true;
    slots_[slot].SetParameters(ReverbParameters(param));

    std::vector<std::size_t>& list = lists_.at(index);
    if (list.empty())
    {
        layer.SetAuxCallback(
            bus, [this, bus](std::span<int32_t, Layer::kAuxSamples> samples) { AuxCallback(bus, samples); });
        wait_counters_[index] = 2;
    }

    list.push_back(slot);
}

void Effects::AuxCallback(int bus, std::span<int32_t, Layer::kAuxSamples> samples)
{
    const auto index = static_cast<std::size_t>(bus);
    int& wait = wait_counters_[index];
    const std::vector<std::size_t>& list = lists_[index];
    if (wait > 0 || list.empty())
    {
        if (wait > 0)
        {
            wait--;
        }

        std::fill(samples.begin(), samples.end(), 0);

        return;
    }

    // UpdateBuffer (0x1010cb8): the reverb on the 160 samples of each channel, in place.
    constexpr uint32_t kFrameSamples = Layer::kAuxSamples / kReverbChannels;
    for (const std::size_t slot : list)
    {
        slots_[slot].Process(samples.data(), samples.data(), kFrameSamples);
    }
}

} // namespace ntdlib::rvl::nx
