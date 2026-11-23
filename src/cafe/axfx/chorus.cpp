// SPDX-License-Identifier: MIT

#include "ntdlib/cafe/axfx/chorus.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ax/chorus_lines.h"
#include "cafe/axfx/util.h"
#include "ntdlib/ax/axfx.h"
#include "ntdlib/cafe/axfx/axfx.h"
#include "ntdlib/fused.h"

namespace ntdlib::cafe::axfx
{
uint32_t ChorusExp::GetMemSize(uint32_t rate)
{
    return rate / 10 * 12;
}

bool ChorusExp::Init(const Params& params, uint32_t rate)
{
    // The library allocates three lines of 100 ms through the hooks (0x0200bd48). ResetLines clears them.
    params_ = params;
    active_ = kOff;
    lines_.size = rate / 10;
    for (std::vector<float>& line : lines_.line)
    {
        line.assign(lines_.size, 0.0f);
    }

    if (!ResetLines(rate) || !InitParams(rate))
    {
        Off();
        return false;
    }

    active_ &= ~kOff;

    return true;
}

bool ChorusExp::Settings(const Params& params, uint32_t rate)
{
    const auto off = [this]
    {
        Off();
    };
    const auto init = [&]
    {
        return Init(params, rate);
    };

    return axfx::Settings(active_, off, init);
}

bool ChorusExp::SettingsUpdate(const Params& params, uint32_t rate)
{
    params_ = params;
    active_ |= kOff;
    if (!ResetLines(rate) || !InitParams(rate))
    {
        Off();
        return false;
    }

    active_ = (active_ | kSkip) & ~kOff;

    return true;
}

void ChorusExp::Shutdown()
{
    Off();
}

void ChorusExp::Process(const Buffers& frame, uint32_t samples, const Buffers& bus_in, const Buffers& bus_out)
{
    if (active_ != 0)
    {
        active_ &= ~kSkip;
        return;
    }

    for (uint32_t i = 0; i < samples; i++)
    {
        // The read position: the output position moved by the LFO. Each channel's output is the history, oldest
        // first, through the taps for the fraction of the distance.
        const std::array<float, 4>& taps =
            ntdlib::ax::axfx::chorus::Read(lines_, ntdlib::ax::axfx::chorus::NextLfo(lfo_));
        for (int channel = 0; channel < kChannelCount; channel++)
        {
            const std::array<float, 4>& history = lines_.history[channel];
            float output = 0.0f;
            for (uint32_t tap = 0; tap < 4; tap++)
            {
                output = Fmadds(history[(lines_.hist_index + tap) & 3], taps[tap], output);
            }

            uint32_t sample = static_cast<uint32_t>(frame[channel][i]);
            if (bus_in[channel] != nullptr)
            {
                sample += static_cast<uint32_t>(bus_in[channel][i]);
            }

            lines_.line[channel][lines_.in_pos] =
                Fmadds(output, params_.feedback, ToFloat(static_cast<int32_t>(sample)));
            frame[channel][i] = ntdlib::PowerPcToInt32(output * params_.out_gain);
            if (bus_out[channel] != nullptr)
            {
                bus_out[channel][i] = ntdlib::PowerPcToInt32(output * params_.send_gain);
            }
        }

        ntdlib::ax::axfx::chorus::StepPositions(lines_);
    }
}

bool ChorusExp::ResetLines(uint32_t rate)
{
    if (lines_.line[0].empty())
    {
        return false;
    }

    for (std::vector<float>& line : lines_.line)
    {
        Clear(line);
    }

    // The output starts the delay behind the input, in 16.16 fixed point.
    lines_.in_pos = 0;
    const uint32_t delay = ntdlib::CafeToUint32(params_.delay_time * ToFloat(SamplesPerMs(rate)));
    lines_.out_pos = (lines_.size - delay) << 16;
    lines_.last_pos = lines_.out_pos;
    lines_.size_fp = lines_.size << 16;

    return true;
}

bool ChorusExp::InitParams(uint32_t rate)
{
    // The library's tests pass a NaN delay, depth or rate. The read position then leaves the lines. The model refuses
    // a NaN there.
    const Params& p = params_;
    if (p.delay_time < 0.1f || p.delay_time > 50.0f || p.depth < 0.0f || p.depth > 1.0f || p.rate < 0.1f ||
        p.rate > 2.0f || p.feedback < 0.0f || !(p.feedback < 1.0f) || OutOfRange(p.out_gain) ||
        OutOfRange(p.send_gain) || std::isnan(p.delay_time) || std::isnan(p.depth) || std::isnan(p.rate))
    {
        return false;
    }

    // The LFO's swing is `depth` times the delay, less a sample at full depth. One step of the table takes `step`
    // samples at 32 kHz.
    const float delay_samples = p.delay_time * ToFloat(SamplesPerMs(rate));
    float depth_samples = delay_samples * p.depth;
    if (!(depth_samples < delay_samples))
    {
        depth_samples = depth_samples - 1.0f;
        if (!(depth_samples >= 0.0f))
        {
            depth_samples = 0.0f;
        }
    }

    const float step = 32000.0f / p.rate * 0.00390625f;
    lfo_ = ntdlib::ax::axfx::ChorusLfo{};
    lfo_.phase_add = ntdlib::PowerPcToInt32(p.rate * 256.0f / 32000.0f * 65536.0f);
    lfo_.depth_samp = ntdlib::PowerPcToInt32(depth_samples * 65536.0f);
    lfo_.grad_factor = ntdlib::PowerPcToInt32(depth_samples / step * 65536.0f);
    lfo_.last_num = 0xffffffff;
    lines_.history = {};
    lines_.hist_index = 0;

    return true;
}

void ChorusExp::Off()
{
    active_ |= kOff;
    Free(lines_.line);
}

} // namespace ntdlib::cafe::axfx
