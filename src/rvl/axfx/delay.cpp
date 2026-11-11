// SPDX-License-Identifier: MIT

#include "ntdlib/rvl/axfx/delay.h"

#include <algorithm>
#include <cmath>

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

// The longest line that DelayExp's Init allocates: 64 MiB of samples. Above it, the model's Init fails.
constexpr uint32_t kMaxLineSamples = (64u << 20) / 4;

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

uint32_t DelayExp::GetMemSize(const Params& params)
{
    return static_cast<uint32_t>(ntdlib::PowerPcToInt32(32.0f * params.max_delay)) * 12;
}

bool DelayExp::Init(const Params& params)
{
    params_ = params;
    active_ = kStopped;
    if (!(params_.max_delay > 0.0f))
    {
        FreeLines();
        return false;
    }

    // The lines hold 32 samples for each ms, and at least one. The game allocates them through the AXFX hooks.
    max_length_ = ToUnsigned(32.0f * params_.max_delay);
    if (max_length_ == 0)
    {
        max_length_ = 1;
    }

    if (max_length_ > kMaxLineSamples)
    {
        FreeLines();
        return false;
    }

    for (std::vector<int32_t>& line : line_)
    {
        line.assign(max_length_, 0);
    }

    if (!InitParams())
    {
        FreeLines();
        return false;
    }

    active_ = (active_ | kSkipFrame) & ~kStopped;

    return true;
}

bool DelayExp::Settings(const Params& params)
{
    FreeLines();

    return Init(params);
}

bool DelayExp::SettingsUpdate(const Params& params)
{
    params_ = params;
    active_ |= kStopped;
    if (!InitParams())
    {
        FreeLines();
        return false;
    }

    active_ = (active_ | kSkipFrame) & ~kStopped;

    return true;
}

void DelayExp::Shutdown()
{
    FreeLines();
}

void DelayExp::Process(AuxFrame& frame)
{
    Process(frame, nullptr, nullptr);
}

void DelayExp::Process(AuxFrame& frame, const AuxFrame* bus_in, AuxFrame* bus_out)
{
    if (active_ != 0)
    {
        active_ &= ~kSkipFrame;
        return;
    }

    // The filter's new output is (128 - lpfGain) 128ths of its input plus lpfGain 128ths of its last output. Both
    // products and their sum keep the low 32 bits. The game interleaves the three channels' steps. Each channel's steps
    // depend only on that channel and the shared position.
    const uint32_t keep = static_cast<uint32_t>(lpf_gain_);
    const uint32_t take = 128u - keep;
    for (int i = 0; i < kSamplesPerFrame; i++)
    {
        for (int channel = 0; channel < kChannelCount; channel++)
        {
            int32_t& sample = frame.channels[channel][i];
            uint32_t input = static_cast<uint32_t>(sample);
            if (bus_in != nullptr)
            {
                input += static_cast<uint32_t>(bus_in->channels[channel][i]);
            }

            int32_t& state = lpf_state_[channel];
            state = static_cast<int32_t>(take * input + keep * static_cast<uint32_t>(state)) >> 7;
            int32_t& stored = line_[channel][cur_pos_];
            const int32_t delayed = stored;
            stored = static_cast<int32_t>(static_cast<uint32_t>(state) +
                                          static_cast<uint32_t>(Scale(delayed, feedback_gain_)));
            sample = Scale(delayed, out_gain_);
            if (bus_out != nullptr)
            {
                bus_out->channels[channel][i] = Scale(delayed, send_gain_);
            }
        }

        Step(cur_pos_, length_);
    }
}

bool DelayExp::InitParams()
{
    // The delay is at least one sample. The game's checks pass a NaN delay. They also pass a delay longer than the
    // lines, from a SettingsUpdate with a longer longest delay than Init had. The game's position would then leave the
    // lines. The model refuses such a delay.
    const Params& p = params_;
    const uint32_t length = std::max(ToUnsigned(32.0f * p.delay), 1u);
    if (std::isnan(p.delay) || p.delay > p.max_delay || length > max_length_ || p.feedback < 0.0f ||
        p.feedback >= 1.0f || OutOfRange(p.lpf) || OutOfRange(p.out_gain) || OutOfRange(p.send_gain))
    {
        return false;
    }

    for (const std::vector<int32_t>& line : line_)
    {
        if (line.empty())
        {
            return false;
        }
    }

    for (std::vector<int32_t>& line : line_)
    {
        std::fill(line.begin(), line.end(), 0);
    }

    // The gains are in 128ths, truncated (fctiwz). The filter's weight of its last output is 1 - lpf, capped at 0.95.
    length_ = length;
    cur_pos_ = 0;
    feedback_gain_ = ntdlib::PowerPcToInt32(128.0f * p.feedback);
    lpf_gain_ = ntdlib::PowerPcToInt32(128.0f * LowpassCoef(p.lpf));
    lpf_state_ = {};
    out_gain_ = ntdlib::PowerPcToInt32(128.0f * p.out_gain);
    send_gain_ = ntdlib::PowerPcToInt32(128.0f * p.send_gain);

    return true;
}

void DelayExp::FreeLines()
{
    active_ |= kStopped;
    for (std::vector<int32_t>& line : line_)
    {
        line = std::vector<int32_t>();
    }
}

} // namespace ntdlib::rvl::axfx
