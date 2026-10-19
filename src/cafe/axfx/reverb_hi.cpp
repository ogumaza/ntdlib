// SPDX-License-Identifier: MIT

#include "ntdlib/cafe/axfx/reverb_hi.h"

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
using reverb::kHiEarlyCoefs;
using reverb::kHiEarlySizes;
using reverb::kHiFilterSizes;

} // namespace

uint32_t ReverbHiExp::GetMemSize(const Params& params, uint32_t rate)
{
    // Each channel's lines at their longest, and each channel's last allpass. This sum converts the pre-delay with
    // fctiwz, and Init with the library's unsigned conversion.
    const uint32_t* longest = kHiFilterSizes[kFusedModes];
    uint32_t samples = static_cast<uint32_t>(ntdlib::PowerPcToInt32(params.pre_delay_time_max * ToFloat(rate)));
    samples += kHiEarlySizes[kEarlyModes - 1][kTaps - 1];
    for (int i = 0; i < kCombs + kAllpasses; i++)
    {
        samples += longest[i];
    }

    samples *= 3;
    for (int channel = 0; channel < kChannelCount; channel++)
    {
        samples += longest[kCombs + kAllpasses + channel];
    }

    return samples * 4;
}

bool ReverbHiExp::Init(const Params& params, uint32_t rate)
{
    params_ = params;
    active_ = kOff;
    const std::optional<uint32_t> pre_delay_max_length = reverb::PreDelayMaxLength(params.pre_delay_time_max, rate);
    if (!pre_delay_max_length)
    {
        Off();
        return false;
    }

    // The library allocates each line with the longest length that the tables give (0x02014834), or no pre-delay line
    // for a longest pre-delay of 0 samples. The library then clears the lines (0x02014998). Each assign does both
    // steps.
    const uint32_t* longest = kHiFilterSizes[kFusedModes];
    for (int channel = 0; channel < kChannelCount; channel++)
    {
        early_line_[channel].assign(kHiEarlySizes[kEarlyModes - 1][kTaps - 1], 0.0f);
        last_allpass_line_[channel].assign(longest[kCombs + kAllpasses + channel], 0.0f);
    }

    reverb::Allocate(lines_, *pre_delay_max_length, longest);
    if (!InitParams(rate))
    {
        Off();
        return false;
    }

    active_ &= ~kOff;

    return true;
}

bool ReverbHiExp::Settings(const Params& params, uint32_t rate)
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

bool ReverbHiExp::SettingsUpdate(const Params& params, uint32_t rate)
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

void ReverbHiExp::Shutdown()
{
    Off();
}

void ReverbHiExp::Process(const Buffers& frame, uint32_t samples, const Buffers& bus_in, const Buffers& bus_out)
{
    if (active_ != 0)
    {
        active_ &= ~kSkip;
        return;
    }

    // The gains that the callback computes once a frame.
    const float fused_gain = params_.fused_gain * 0.6f;
    const float lowpass_gain = 1.0f - lines_.lpf_coef;
    const float crosstalk = params_.crosstalk * 0.5f;
    for (uint32_t i = 0; i < samples; i++)
    {
        std::array<float, kChannelCount> out{};
        for (int channel = 0; channel < kChannelCount; channel++)
        {
            uint32_t sample = static_cast<uint32_t>(frame[channel][i]);
            if (bus_in[channel] != nullptr)
            {
                sample += static_cast<uint32_t>(bus_in[channel][i]);
            }

            out[channel] = ProcessChannel(channel, ToFloat(static_cast<int32_t>(sample)), fused_gain, lowpass_gain);
        }

        // The crosstalk mixes the other two channels into each one.
        const float left = Fmadds(out[kRight] + out[kSurround], crosstalk, out[kLeft]);
        const float right = Fmadds(out[kLeft] + out[kSurround], crosstalk, out[kRight]);
        const float surround = Fmadds(out[kLeft] + out[kRight], crosstalk, out[kSurround]);
        frame[kLeft][i] = ntdlib::PowerPcToInt32(left * params_.out_gain);
        frame[kRight][i] = ntdlib::PowerPcToInt32(right * params_.out_gain);
        frame[kSurround][i] = ntdlib::PowerPcToInt32(surround * params_.out_gain);
        if (bus_out[kLeft] != nullptr)
        {
            bus_out[kLeft][i] = ntdlib::PowerPcToInt32(left * params_.send_gain);
        }

        if (bus_out[kRight] != nullptr)
        {
            bus_out[kRight][i] = ntdlib::PowerPcToInt32(right * params_.send_gain);
        }

        if (bus_out[kSurround] != nullptr)
        {
            bus_out[kSurround][i] = ntdlib::PowerPcToInt32(surround * params_.send_gain);
        }

        for (uint32_t& position : early_pos_)
        {
            Step(position, early_length_);
        }

        reverb::StepLines(lines_);
    }
}

float ReverbHiExp::ProcessChannel(int channel, float input, float fused_gain, float lowpass_gain)
{
    // The early reflections: three taps of the line. The input goes into the line at the last tap.
    std::vector<float>& early_line = early_line_[channel];
    const float tap1 = early_line[early_pos_[1]] * early_coef_[1];
    const float taps01 = Fmadds(early_line[early_pos_[0]], early_coef_[0], tap1);
    const float tap2 = early_line[early_pos_[2]];
    early_line[early_pos_[2]] = input;
    const float early = Fmadds(tap2, early_coef_[2], taps01);

    // The pre-delay, three combs in parallel and two allpasses.
    const float delayed = reverb::PreDelay(lines_, channel, input);
    float combs = 0.0f;
    for (int comb = 0; comb < kCombs; comb++)
    {
        combs = combs + reverb::Comb(lines_, channel, comb, delayed);
    }

    float passed = combs;
    for (int allpass = 0; allpass < kAllpasses; allpass++)
    {
        passed = reverb::Allpass(lines_, channel, allpass, passed);
    }

    // The low-pass and the channel's last allpass. The last allpass's position moves on here, a channel at a time.
    const float lowpass = reverb::Lowpass(lines_, channel, passed, lowpass_gain);
    uint32_t& position = last_allpass_pos_[channel];
    const float output = reverb::AllpassStep(last_allpass_line_[channel][position], lines_.allpass_coef, lowpass);
    Step(position, last_allpass_length_[channel]);

    return Fmadds(output, fused_gain, early);
}

bool ReverbHiExp::InitParams(uint32_t rate)
{
    const Params& p = params_;
    if (reverb::AnyOutOfRange(p) || OutOfRange(p.crosstalk))
    {
        return false;
    }

    // The early line is as long as the last tap's delay. Each tap starts at that length less its delay.
    const uint32_t* early_sizes = kHiEarlySizes[p.early_mode];
    early_length_ = early_sizes[kTaps - 1];
    for (int tap = 0; tap < kTaps; tap++)
    {
        early_pos_[tap] = early_length_ - early_sizes[tap];
        early_coef_[tap] = (kHiEarlyCoefs[p.early_mode][tap] * p.early_gain) * 0.6f;
    }

    const uint32_t* sizes = kHiFilterSizes[p.fused_mode];
    if (!reverb::SetUp(lines_, p, sizes, ToFloat(rate)))
    {
        return false;
    }

    for (int channel = 0; channel < kChannelCount; channel++)
    {
        last_allpass_pos_[channel] = 0;
        last_allpass_length_[channel] = sizes[kCombs + kAllpasses + channel];
    }

    return true;
}

void ReverbHiExp::ClearLines()
{
    for (int channel = 0; channel < kChannelCount; channel++)
    {
        Clear(early_line_[channel]);
        Clear(last_allpass_line_[channel]);
    }

    reverb::ClearLines(lines_);
}

void ReverbHiExp::Off()
{
    active_ |= kOff;
    for (int channel = 0; channel < kChannelCount; channel++)
    {
        early_line_[channel] = {};
        last_allpass_line_[channel] = {};
    }

    reverb::FreeLines(lines_);
}

} // namespace ntdlib::cafe::axfx
