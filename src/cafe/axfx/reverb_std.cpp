// SPDX-License-Identifier: MIT

#include "ntdlib/cafe/axfx/reverb_std.h"

#include <cstdint>
#include <optional>
#include <vector>

#include "ntdlib/fused.h"

#include "cafe/axfx/reverb_lines.h"
#include "cafe/axfx/reverb_tables.h"
#include "cafe/axfx/util.h"

namespace ntdlib::cafe::axfx
{
namespace
{

using reverb::kEarlyModes;
using reverb::kFusedModes;
using reverb::kStdEarlySizes;
using reverb::kStdFilterSizes;

} // namespace

uint32_t ReverbStdExp::GetMemSize(const Params& params, uint32_t rate)
{
    // Each channel's lines at their longest. This sum converts the pre-delay with fctiwz, and Init with the library's
    // unsigned conversion.
    const uint32_t* longest = kStdFilterSizes[kFusedModes];
    uint32_t samples = static_cast<uint32_t>(ntdlib::PowerPcToInt32(params.pre_delay_time_max * ToFloat(rate)));
    samples += kStdEarlySizes[kEarlyModes - 1];
    for (int i = 0; i < kCombs + kAllpasses; i++)
    {
        samples += longest[i];
    }

    return samples * 12;
}

bool ReverbStdExp::Init(const Params& params, uint32_t rate)
{
    params_ = params;
    active_ = kOff;
    const std::optional<uint32_t> pre_delay_max_length = reverb::PreDelayMaxLength(params.pre_delay_time_max, rate);
    if (!pre_delay_max_length)
    {
        Off();
        return false;
    }

    // The library allocates each line with the longest length that the tables give (0x02016bc4), or no pre-delay line
    // for a longest pre-delay of 0 samples. The library then clears the lines (0x02016cfc). Each assign does both
    // steps.
    for (std::vector<float>& line : early_line_)
    {
        line.assign(kStdEarlySizes[kEarlyModes - 1], 0.0f);
    }

    reverb::Allocate(lines_, *pre_delay_max_length, kStdFilterSizes[kFusedModes]);
    if (!InitParams(rate))
    {
        Off();
        return false;
    }

    active_ &= ~kOff;

    return true;
}

bool ReverbStdExp::Settings(const Params& params, uint32_t rate)
{
    Off();
    if (!Init(params, rate))
    {
        Off();
        return false;
    }

    active_ = (active_ | kSkip) & ~kOff;

    return true;
}

bool ReverbStdExp::SettingsUpdate(const Params& params, uint32_t rate)
{
    // After a Shutdown or a failed Init, the library would set the reverb up on lines it has freed, and its callback
    // would then run on null lines. The model fails instead.
    params_ = params;
    active_ |= kOff;
    if (early_line_[0].empty())
    {
        Off();
        return false;
    }

    ClearLines();
    if (!InitParams(rate))
    {
        Off();
        return false;
    }

    active_ = (active_ | kSkip) & ~kOff;

    return true;
}

void ReverbStdExp::Shutdown()
{
    Off();
}

void ReverbStdExp::Process(const Buffers& frame, uint32_t samples, const Buffers& bus_in, const Buffers& bus_out)
{
    if (active_ != 0)
    {
        active_ &= ~kSkip;
        return;
    }

    // The gains that the callback computes once a frame.
    const float early_gain = params_.early_gain * 0.6f;
    const float lowpass_gain = 1.0f - lines_.lpf_coef;
    const float fused_gain = params_.fused_gain * 0.6f;
    for (uint32_t i = 0; i < samples; i++)
    {
        for (int channel = 0; channel < kChannelCount; channel++)
        {
            uint32_t sample = static_cast<uint32_t>(frame[channel][i]);
            if (bus_in[channel] != nullptr)
            {
                sample += static_cast<uint32_t>(bus_in[channel][i]);
            }

            const float input = ToFloat(static_cast<int32_t>(sample));

            // The early line feeds back into itself, as a comb does.
            const float early = reverb::CombStep(early_line_[channel][early_pos_], early_coef_, input);

            // The pre-delay, two combs in parallel, an allpass, the low-pass and a second allpass.
            const float delayed = reverb::PreDelay(lines_, channel, input);
            const float out0 = reverb::Comb(lines_, channel, 0, delayed);
            const float out1 = reverb::Comb(lines_, channel, 1, delayed);
            const float passed0 = reverb::Allpass(lines_, channel, 0, out0 + out1);
            const float lowpass = reverb::Lowpass(lines_, channel, passed0, lowpass_gain);
            const float passed1 = reverb::Allpass(lines_, channel, 1, lowpass);

            const float output = Fmadds(early, early_gain, passed1 * fused_gain);
            frame[channel][i] = ntdlib::PowerPcToInt32(output * params_.out_gain);
            if (bus_out[channel] != nullptr)
            {
                bus_out[channel][i] = ntdlib::PowerPcToInt32(output * params_.send_gain);
            }
        }

        Step(early_pos_, early_length_);
        reverb::StepLines(lines_);
    }
}

bool ReverbStdExp::InitParams(uint32_t rate)
{
    const Params& p = params_;
    if (reverb::AnyOutOfRange(p))
    {
        return false;
    }

    // Early modes 0 to 3 feed the early line back negated.
    early_pos_ = 0;
    early_length_ = kStdEarlySizes[p.early_mode];
    early_coef_ = p.early_mode <= 3 ? -0.33f : 0.33f;

    return reverb::SetUp(lines_, p, kStdFilterSizes[p.fused_mode], ToFloat(rate));
}

void ReverbStdExp::ClearLines()
{
    for (std::vector<float>& line : early_line_)
    {
        Clear(line);
    }

    reverb::ClearLines(lines_);
}

void ReverbStdExp::Off()
{
    active_ |= kOff;
    for (std::vector<float>& line : early_line_)
    {
        line = {};
    }

    reverb::FreeLines(lines_);
}

} // namespace ntdlib::cafe::axfx
