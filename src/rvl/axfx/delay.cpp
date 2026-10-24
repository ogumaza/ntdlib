// SPDX-License-Identifier: MIT

#include "ntdlib/rvl/axfx/delay.h"

#include "ntdlib/fused.h"
#include "ntdlib/rvl/axfx/util.h"

namespace ntdlib::rvl::axfx
{
namespace
{

// A Wii can't allocate a line larger than its 64 MiB of MEM2. The game's allocation fails for a delay above 524,288 ms.
// The model's Init fails there too. From a delay of 2^25 ms, the game's sizes wrap around. Its lines would then overrun
// their memory.
constexpr uint32_t kMaxDelay = (64u << 20) / 128;

// The conversion in __InitParams of a gain in percent to a factor in 128ths: in single precision (fmuls and fdivs),
// then truncated (fctiwz).
int32_t Gain(uint32_t percent)
{
    return ntdlib::PowerPcToInt32(128.0f * static_cast<float>(percent) / 100.0f);
}

// The callback's product of a sample and a factor in 128ths: the low 32 bits of the product (mullw), then a shift right
// by 7 that keeps the sign (srawi).
int32_t Scale(int32_t sample, int32_t factor)
{
    return static_cast<int32_t>(static_cast<uint32_t>(sample) * static_cast<uint32_t>(factor)) >> 7;
}

} // namespace

uint32_t Delay::GetMemSize(const Params& params)
{
    return (params.delay[0] + params.delay[1] + params.delay[2]) << 7;
}

bool Delay::Init(const Params& params)
{
    params_ = params;
    active_ = kStopped;
    for (int channel = 0; channel < kChannelCount; channel++)
    {
        if (params_.delay[channel] == 0 || params_.delay[channel] > kMaxDelay)
        {
            FreeLines();
            return false;
        }

        length_[channel] = params_.delay[channel] << 5;
    }

    // The game allocates the lines through the AXFX hooks. __InitParams clears them.
    for (int channel = 0; channel < kChannelCount; channel++)
    {
        line_[channel].assign(length_[channel], 0);
    }

    if (!InitParams())
    {
        FreeLines();
        return false;
    }

    active_ = (active_ | kSkipFrame) & ~kStopped;

    return true;
}

bool Delay::Settings(const Params& params)
{
    // The game's Settings then sets the flags that Init has set.
    FreeLines();

    return Init(params);
}

void Delay::Shutdown()
{
    FreeLines();
}

void Delay::Process(AuxFrame& frame)
{
    if (active_ != 0)
    {
        active_ &= ~kSkipFrame;
        return;
    }

    // The game interleaves the three channels' steps. Each channel's steps depend only on that channel.
    for (int i = 0; i < kSamplesPerFrame; i++)
    {
        for (int channel = 0; channel < kChannelCount; channel++)
        {
            std::vector<int32_t>& line = line_[channel];
            uint32_t& position = cur_pos_[channel];
            int32_t& sample = frame.channels[channel][i];
            const int32_t delayed = line[position];
            const uint32_t feedback = static_cast<uint32_t>(Scale(delayed, feedback_gain_[channel]));
            line[position] = static_cast<int32_t>(static_cast<uint32_t>(sample) + feedback);
            Step(position, length_[channel]);
            sample = Scale(delayed, out_gain_[channel]);
        }
    }
}

bool Delay::InitParams()
{
    for (int channel = 0; channel < kChannelCount; channel++)
    {
        if (params_.feedback[channel] >= 100 || params_.output[channel] > 100)
        {
            return false;
        }

        cur_pos_[channel] = 0;
        feedback_gain_[channel] = Gain(params_.feedback[channel]);
        out_gain_[channel] = Gain(params_.output[channel]);
    }

    return true;
}

void Delay::FreeLines()
{
    active_ |= kStopped;
    for (std::vector<int32_t>& line : line_)
    {
        line = std::vector<int32_t>();
    }
}

} // namespace ntdlib::rvl::axfx
