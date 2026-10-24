// SPDX-License-Identifier: MIT

#include "ntdlib/wave/encode_ima.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

#include "ntdlib/wave/adpcm.h"

namespace ntdlib::wave
{
namespace
{

constexpr int kLastIndex = 88;              // the last entry of the table of steps
constexpr std::size_t kOpeningSamples = 64; // the samples that choose the first step

// The nibble whose decoded sample from `state` comes closest to `target`, and the state after it.
ImaAdpcmState Closest(ImaAdpcmState state, int32_t target, uint8_t& nibble)
{
    ImaAdpcmState best;
    int64_t best_error = std::numeric_limits<int64_t>::max();
    for (uint8_t n = 0; n < 16; n++)
    {
        const ImaAdpcmState next = ImaAdpcmNext(state, n);
        const int64_t error = int64_t{target - next.sample} * (target - next.sample);
        if (error < best_error)
        {
            best = next;
            best_error = error;
            nibble = n;
        }
    }

    return best;
}

// The squared error of the encoding of `samples` from `state`.
double ErrorFrom(ImaAdpcmState state, std::span<const int16_t> samples)
{
    double error = 0.0;
    uint8_t nibble = 0;
    for (const int16_t sample : samples)
    {
        state = Closest(state, sample, nibble);
        const double difference = static_cast<double>(sample) - state.sample;
        error += difference * difference;
    }

    return error;
}

} // namespace

std::size_t ImaAdpcmBytes(std::size_t samples)
{
    return (samples + 1) / 2;
}

ImaAdpcmChannel EncodeImaAdpcm(std::span<const int16_t> samples, uint32_t loop_start)
{
    if (samples.size() > std::numeric_limits<uint32_t>::max())
    {
        throw std::invalid_argument("too many samples for a wave");
    }

    // The first state: the first sample, with the step that suits the opening best.
    ImaAdpcmChannel channel;
    if (!samples.empty())
    {
        const std::span<const int16_t> opening = samples.first(std::min(kOpeningSamples, samples.size()));
        double least = std::numeric_limits<double>::infinity();
        for (int index = 0; index <= kLastIndex; index++)
        {
            const ImaAdpcmState state{samples[0], static_cast<uint8_t>(index)};
            const double error = ErrorFrom(state, opening);
            if (error < least)
            {
                least = error;
                channel.start = state;
            }
        }
    }

    // Each sample's nibble, and the state before the loop's first sample.
    channel.data.assign(ImaAdpcmBytes(samples.size()), 0);
    ImaAdpcmState state = channel.start;
    channel.loop = state;
    for (std::size_t i = 0; i < samples.size(); i++)
    {
        if (i == loop_start)
        {
            channel.loop = state;
        }

        uint8_t nibble = 0;
        state = Closest(state, samples[i], nibble);
        channel.data[i / 2] |= static_cast<uint8_t>(i % 2 == 0 ? nibble : nibble << 4);
    }
    if (loop_start >= samples.size())
    {
        channel.loop = state;
    }

    return channel;
}

} // namespace ntdlib::wave
