// SPDX-License-Identifier: MIT

#include "ntdlib/rvl/axfx/reverb_std.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "ntdlib/fused.h"
#include "ntdlib/rvl/axfx/axfx.h"
#include "ntdlib/rvl/axfx/util.h"
#include "rvl/axfx/reverb_lines.h"

namespace ntdlib::rvl::axfx
{
namespace
{

// The early line's length for each early mode (__EarlySizeTable, 0x80533540 in Smooth Moves). Mode 7 has the longest
// line. Init allocates the line with that length.
constexpr uint32_t kEarlyModes = 8;
constexpr uint32_t kEarlySizes[kEarlyModes] = {163, 317, 479, 641, 797, 967, 1123, 1283};

// For each fused mode, the lengths of the two combs and the two allpasses (__FilterSizeTable, 0x80533560 in Smooth
// Moves). The last row holds the longest lengths. Init allocates the lines with them.
constexpr uint32_t kFusedModes = 6;
constexpr uint32_t kFilterSizes[kFusedModes + 1][4] = {
    {1789, 1999, 433, 149}, {149, 293, 251, 103},   {947, 1361, 433, 137},  {1279, 1531, 509, 149},
    {1531, 1847, 563, 179}, {1823, 2357, 571, 137}, {1823, 2357, 571, 179},
};

} // namespace

uint32_t ReverbStdExp::GetMemSize(const Params& params)
{
    // Each channel's early line, pre-delay, combs and allpasses at their longest. This sum converts the pre-delay with
    // fctiwz. Init uses the runtime's unsigned conversion.
    const uint32_t* longest = kFilterSizes[kFusedModes];
    uint32_t samples = kEarlySizes[kEarlyModes - 1] +
                       static_cast<uint32_t>(ntdlib::PowerPcToInt32(kSampleRate * params.pre_delay_time_max));
    for (int i = 0; i < kCombs + kAllpasses; i++)
    {
        samples += longest[i];
    }

    return samples * static_cast<uint32_t>(kChannelCount) * 4;
}

bool ReverbStdExp::Init(const Params& params)
{
    params_ = params;
    active_ = kStopped;
    const std::optional<uint32_t> pre_delay_max_length = reverb::PreDelayMaxLength(params.pre_delay_time_max);
    if (!pre_delay_max_length)
    {
        FreeDelayLines();
        return false;
    }

    // __AllocDelayLine (0x80033954) allocates each line with the longest length that the tables give.
    // __BzeroDelayLines (0x80033a98) then clears it.
    for (std::vector<float>& line : early_line_)
    {
        line.assign(kEarlySizes[kEarlyModes - 1], 0.0f);
    }

    reverb::Allocate(lines_, *pre_delay_max_length, kFilterSizes[kFusedModes]);

    if (!InitParams())
    {
        FreeDelayLines();
        return false;
    }

    active_ &= ~kStopped;

    return true;
}

bool ReverbStdExp::Settings(const Params& params)
{
    const auto free_lines = [this]
    {
        FreeDelayLines();
    };
    const auto init = [&]
    {
        return Init(params);
    };

    return reverb::Settings(active_, free_lines, init);
}

bool ReverbStdExp::SettingsUpdate(const Params& params)
{
    params_ = params;
    const auto free_lines = [this]
    {
        FreeDelayLines();
    };
    const auto clear = [this]
    {
        ClearDelayLines();
    };
    const auto init_params = [this]
    {
        return InitParams();
    };

    return reverb::SettingsUpdate(active_, !lines_.comb_line[0][0].empty(), free_lines, clear, init_params);
}

void ReverbStdExp::Shutdown()
{
    active_ |= kStopped;
    FreeDelayLines();
}

void ReverbStdExp::Process(AuxFrame& frame)
{
    if (active_ != 0)
    {
        active_ &= ~kSkipFrame;
        return;
    }

    // The gains that the callback computes once a frame.
    const float lowpass_gain = 1.0f - lines_.lpf_coef;
    const float early_gain = 0.6f * params_.early_gain;
    const float fused_gain = 0.6f * params_.fused_gain;
    for (int i = 0; i < kSamplesPerFrame; i++)
    {
        for (int channel = 0; channel < kChannelCount; channel++)
        {
            int32_t& sample = frame.channels[channel][i];
            const float output =
                ProcessChannel(channel, static_cast<float>(sample), early_gain, fused_gain, lowpass_gain);
            sample = ntdlib::PowerPcToInt32(output * params_.out_gain);
        }

        AdvancePositions();
    }
}

float ReverbStdExp::ProcessChannel(int channel, float input, float early_gain, float fused_gain, float lowpass_gain)
{
    // The early line.
    float& early_sample = early_line_[channel][early_pos_];
    const float early = early_sample;
    early_sample = input + early * early_coef_;

    // The pre-delay, two combs in parallel, an allpass, the low-pass and a second allpass.
    const float delayed = reverb::PreDelay(lines_, channel, input);
    const float comb0 = reverb::Comb(lines_, channel, 0, delayed);
    const float comb1 = reverb::Comb(lines_, channel, 1, delayed);
    const float allpass0 = reverb::Allpass(lines_, channel, 0, comb0 + comb1);
    const float lowpass = reverb::Lowpass(lines_, channel, allpass0, lowpass_gain);
    const float allpass1 = reverb::Allpass(lines_, channel, 1, lowpass);

    return early * early_gain + allpass1 * fused_gain;
}

void ReverbStdExp::AdvancePositions()
{
    Step(early_pos_, early_length_);
    reverb::StepLines(lines_);
}

bool ReverbStdExp::InitParams()
{
    const Params& p = params_;
    if (reverb::AnyOutOfRange(p, kEarlyModes, kFusedModes))
    {
        return false;
    }

    // Early modes 0 to 3 feed the early line back negated.
    early_pos_ = 0;
    early_length_ = kEarlySizes[p.early_mode];
    early_coef_ = p.early_mode <= 3 ? -0.33f : 0.33f;

    return reverb::SetUp(lines_, p, kFilterSizes[p.fused_mode]);
}

void ReverbStdExp::ClearDelayLines()
{
    for (std::vector<float>& line : early_line_)
    {
        Clear(line);
    }

    reverb::ClearLines(lines_);
}

void ReverbStdExp::FreeDelayLines()
{
    for (std::vector<float>& line : early_line_)
    {
        line = {};
    }

    reverb::FreeLines(lines_);
}

uint32_t ReverbStd::GetMemSize(const Params& params)
{
    return ReverbStdExp::GetMemSize(Convert(params));
}

bool ReverbStd::Init(const Params& params)
{
    return exp_.Init(Convert(params));
}

bool ReverbStd::Settings(const Params& params)
{
    return exp_.Settings(Convert(params));
}

void ReverbStd::Shutdown()
{
    exp_.Shutdown();
}

void ReverbStd::Process(AuxFrame& frame)
{
    exp_.Process(frame);
}

ReverbStdExp::Params ReverbStd::Convert(const Params& params)
{
    ReverbStdExp::Params exp;
    exp.early_mode = 5;
    exp.pre_delay_time_max = params.pre_delay;
    exp.pre_delay_time = params.pre_delay;
    exp.fused_mode = 0;
    exp.fused_time = params.time;
    exp.coloration = params.coloration;
    exp.damping = params.damping;
    exp.early_gain = 0.0f;
    exp.fused_gain = 1.0f;
    exp.out_gain = params.mix;
    exp.send_gain = 0.0f;

    return exp;
}

} // namespace ntdlib::rvl::axfx
