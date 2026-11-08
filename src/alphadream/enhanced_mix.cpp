// SPDX-License-Identifier: MIT

#include "enhanced_mix.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/alphadream/game_driver.h"
#include "ntdlib/wave/sinc.h"

namespace ntdlib::alphadream
{
namespace
{

// The points of a sample are unsigned. 128 is silence.
constexpr float kSilentPoint = 128.0f;

} // namespace

EnhancedMix::EnhancedMix(const Rom& rom, uint32_t period, uint64_t lead) : rom_(rom), period_(period)
{
    // The console's sample k plays from the timer's overflow k + 1.
    console_ = lead + 1;
    lead_ = size_t(Scaled(console_));
}

uint64_t EnhancedMix::Scaled(uint64_t console_samples) const
{
    return console_samples * period_ * uint64_t(agb::kEnhancedRate) / agb::kSecondCycles;
}

void EnhancedMix::Frame(const std::vector<MixChannel>& channels, bool heard)
{
    const uint64_t first = Scaled(console_);
    console_ += uint64_t(kFrameSamples);
    const size_t n = size_t(Scaled(console_) - first);
    for (std::vector<float>& side : out_)
    {
        side.assign(n, 0.0f);
    }
    parts_ = {};

    // A frame that isn't heard mixes no samples. Each channel still leaves the state that a heard frame leaves.
    std::array<bool, kSampleTracks> mixed = {};
    for (const MixChannel& channel : channels)
    {
        MixChannelInto(channel, heard ? n : 0);
        mixed[size_t(channel.channel)] = true;
    }

    // A channel that the frame didn't mix starts again at its next note.
    for (size_t c = 0; c < channels_.size(); c++)
    {
        channels_[c].known = channels_[c].known && mixed[c];
    }
}

void EnhancedMix::MixChannelInto(const MixChannel& channel, size_t n)
{
    const SampleHeader& sample = channel.sample;
    const float scale = float(channel.level) / 65536.0f;
    const std::array<float, 2> to =
        channel.silenced ? std::array<float, 2>{}
                         : std::array<float, 2>{float(channel.volumes[0]) * scale, float(channel.volumes[1]) * scale};
    ChannelState& state = channels_[size_t(channel.channel)];
    const bool goes_on = state.known && state.data == sample.data && state.next == channel.place;
    const std::array<float, 2> from = goes_on ? state.levels : to;
    state = {true, sample.data, channel.next, to};

    const uint64_t move = uint64_t(channel.step) * uint64_t(kFrameSamples);
    if (n == 0 || move == 0)
    {
        return;
    }

    // A point past the end goes round the loop, or is silent without one.
    const int64_t end = int64_t(sample.end);
    const int64_t loop = int64_t(sample.loop_start);
    const bool loops = sample.loops && end > loop;
    const auto point_of = [&](int64_t at)
    {
        if (at < 0)
        {
            return 0.0f;
        }

        if (at >= end)
        {
            if (!loops)
            {
                return 0.0f;
            }
            at = loop + (at - end) % (end - loop);
        }

        return float(rom_.U8(sample.data + uint32_t(at))) - kSilentPoint;
    };

    const wave::SincKernel& kernel = wave::SincKernel::ForStep(double(move) / (double(n) * 1024.0));
    const int64_t width = kernel.HalfWidth();
    const int64_t first = int64_t(channel.place >> 10) - (width - 1);
    const int64_t last = int64_t((uint64_t(channel.place) + move) >> 10) + width;
    std::vector<float> input(size_t(last - first + 1));
    for (int64_t k = first; k <= last; k++)
    {
        input[size_t(k - first)] = point_of(k);
    }

    // Places in 2^-26ths of a point.
    const uint64_t start = uint64_t(channel.place) << 16;
    const uint64_t advance = (move << 16) / n;
    const std::array<float, 2> slopes = {(to[0] - from[0]) / float(n), (to[1] - from[1]) / float(n)};
    std::array<float, 2>& part = parts_[size_t(channel.channel)];
    for (size_t i = 0; i < n; i++)
    {
        const uint64_t at = start + uint64_t(i) * advance;
        const int64_t whole = int64_t(at >> 26);
        const float value = kernel.At(&input[size_t(whole - (width - 1) - first)], uint32_t(at >> 10) & 0xFFFF);
        for (size_t side = 0; side < 2; side++)
        {
            const float x = value * (from[side] + slopes[side] * float(i));
            out_[side][i] += x;
            part[side] = std::max(part[side], std::fabs(x));
        }
    }
}

} // namespace ntdlib::alphadream
