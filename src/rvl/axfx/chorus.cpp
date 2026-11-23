// SPDX-License-Identifier: MIT

#include "ntdlib/rvl/axfx/chorus.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ax/chorus_lines.h"
#include "ntdlib/ax/axfx.h"
#include "ntdlib/fused.h"
#include "ntdlib/rvl/axfx/axfx.h"
#include "ntdlib/rvl/axfx/util.h"

namespace ntdlib::rvl::axfx
{
namespace
{

// The lines' length in samples: 100 ms.
constexpr uint32_t kLineSize = 0xc80;

// AXFXChorusInit's and AXFXChorusSettings' conversion of AXFX_CHORUS's parameters to AXFX_CHORUS_EXP's.
ChorusExp::Params ExpParams(const Chorus::Params& params)
{
    const float base_delay = static_cast<float>(params.base_delay);
    ChorusExp::Params result;
    result.delay_time = base_delay;
    result.depth = static_cast<float>(params.variation) / base_delay;
    result.rate = 1000.0f / static_cast<float>(params.period);
    result.feedback = 0.0f;
    result.out_gain = 1.0f;
    result.send_gain = 0.0f;

    return result;
}

} // namespace

ChorusExp::ChorusExp(Build build) : build_(build)
{
}

uint32_t ChorusExp::GetMemSize()
{
    return kChannelCount * kLineSize * 4;
}

bool ChorusExp::Init(const Params& params)
{
    params_ = params;
    active_ = build_ == Build::kFebruary2009 ? kStopped : active_ | kStopped;
    lines_.size = kLineSize;
    for (std::vector<float>& line : lines_.line)
    {
        line.assign(lines_.size, 0.0f);
    }

    if (!Start())
    {
        return false;
    }

    active_ &= ~kStopped;

    return true;
}

bool ChorusExp::Settings(const Params& params)
{
    if (build_ == Build::kFebruary2009)
    {
        if (!Restart(params))
        {
            return false;
        }
    }
    else
    {
        FreeLines();
        if (!Init(params))
        {
            return false;
        }
    }

    active_ = (active_ | kSkipFrame) & ~kStopped;

    return true;
}

void ChorusExp::Shutdown()
{
    FreeLines();
}

void ChorusExp::Process(AuxFrame& frame)
{
    Process(frame, nullptr, nullptr);
}

void ChorusExp::Process(AuxFrame& frame, const AuxFrame* bus_in, AuxFrame* bus_out)
{
    if (active_ != 0)
    {
        active_ &= ~kSkipFrame;
        return;
    }

    const std::array<int32_t, kSamplesPerFrame> offsets = CalcLfo();
    for (int i = 0; i < kSamplesPerFrame; i++)
    {
        // The read position: the output position moved by the LFO. Each channel's output is the history, oldest
        // first, through the taps for the fraction of the distance.
        const std::array<float, 4>& taps = ntdlib::ax::axfx::chorus::Read(lines_, offsets[i]);
        for (int channel = 0; channel < kChannelCount; channel++)
        {
            const std::array<float, 4>& history = lines_.history[channel];
            float output = 0.0f;
            output = output + taps[0] * history[lines_.hist_index];
            output = output + taps[1] * history[(lines_.hist_index + 1) & 3];
            output = output + taps[2] * history[(lines_.hist_index + 2) & 3];
            output = output + taps[3] * history[(lines_.hist_index + 3) & 3];

            int32_t& sample = frame.channels[channel][i];
            uint32_t input = static_cast<uint32_t>(sample);
            if (bus_in != nullptr)
            {
                input += static_cast<uint32_t>(bus_in->channels[channel][i]);
            }

            lines_.line[channel][lines_.in_pos] =
                static_cast<float>(static_cast<int32_t>(input)) + output * params_.feedback;
            sample = ntdlib::PowerPcToInt32(output * params_.out_gain);
            if (bus_out != nullptr)
            {
                bus_out->channels[channel][i] = ntdlib::PowerPcToInt32(output * params_.send_gain);
            }
        }

        ntdlib::ax::axfx::chorus::StepPositions(lines_);
    }
}

bool ChorusExp::Restart(const Params& params)
{
    params_ = params;
    active_ |= kStopped;
    const auto is_freed = [](const std::vector<float>& line)
    {
        return line.empty();
    };
    if (std::any_of(lines_.line.begin(), lines_.line.end(), is_freed))
    {
        FreeLines();
        return false;
    }

    for (std::vector<float>& line : lines_.line)
    {
        Clear(line);
    }

    return Start();
}

bool ChorusExp::Start()
{
    // The output starts the delay time behind the input, in 16.16 fixed point.
    lines_.in_pos = 0;
    const uint32_t delay = ToUnsigned(32.0f * params_.delay_time);
    lines_.out_pos = (lines_.size - delay) << 16;
    lines_.last_pos = lines_.out_pos;
    lines_.size_fp = lines_.size << 16;
    if (!InitParams())
    {
        FreeLines();
        return false;
    }

    return true;
}

bool ChorusExp::InitParams()
{
    // The game's checks pass a NaN delay time, depth or rate. The read position then leaves the lines. The model
    // refuses a NaN there.
    const Params& p = params_;
    if (!(p.delay_time >= 0.1f && p.delay_time <= 50.0f) || !(p.depth >= 0.0f && p.depth <= 1.0f) ||
        !(p.rate >= 0.1f && p.rate <= 2.0f) || p.feedback < 0.0f || p.feedback >= 1.0f || p.out_gain < 0.0f ||
        p.out_gain > 1.0f || p.send_gain < 0.0f || p.send_gain > 1.0f)
    {
        return false;
    }

    // The LFO's swing is `depth` times the delay, less a sample at full depth. One step of the table takes `step`
    // samples.
    const float delay_samples = 32.0f * p.delay_time;
    float depth_samples = delay_samples * p.depth;
    if (depth_samples >= delay_samples)
    {
        depth_samples = depth_samples - 1.0f;
        if (depth_samples < 0.0f)
        {
            depth_samples = 0.0f;
        }
    }

    const float step = kSampleRate / p.rate * 0.00390625f;
    lfo_ = ntdlib::ax::axfx::ChorusLfo{};
    lfo_.phase_add = ntdlib::PowerPcToInt32(65536.0f * (256.0f * p.rate / kSampleRate));
    lfo_.depth_samp = ntdlib::PowerPcToInt32(65536.0f * depth_samples);
    lfo_.grad_factor = ntdlib::PowerPcToInt32(65536.0f * (depth_samples / step));
    lfo_.last_num = 0xffffffff;
    lines_.history = {};
    lines_.hist_index = 0;

    return true;
}

void ChorusExp::FreeLines()
{
    active_ |= kStopped;
    Free(lines_.line);
}

std::array<int32_t, kSamplesPerFrame> ChorusExp::CalcLfo()
{
    std::array<int32_t, kSamplesPerFrame> offsets{};
    for (int32_t& offset : offsets)
    {
        offset = ntdlib::ax::axfx::chorus::NextLfo(lfo_);
    }

    return offsets;
}

uint32_t Chorus::GetMemSize()
{
    return ChorusExp::GetMemSize();
}

bool Chorus::Init(const Params& params)
{
    return exp_.Init(ExpParams(params));
}

bool Chorus::Settings(const Params& params)
{
    return exp_.Settings(ExpParams(params));
}

void Chorus::Shutdown()
{
    exp_.Shutdown();
}

void Chorus::Process(AuxFrame& frame)
{
    exp_.Process(frame);
}

} // namespace ntdlib::rvl::axfx
