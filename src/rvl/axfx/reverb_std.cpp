// SPDX-License-Identifier: MIT

#include "ntdlib/rvl/axfx/reverb_std.h"

#include <vector>

#include "ntdlib/fused.h"
#include "ntdlib/rvl/axfx/util.h"

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

// The longest pre-delay line that the model allocates (see reverb_hi.cpp).
constexpr uint32_t kMaxPreDelayLength = 1u << 23;

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
    const uint32_t pre_delay_max_length = ToUnsigned(kSampleRate * params.pre_delay_time_max);
    if (params.pre_delay_time_max < 0.0f || pre_delay_max_length >= kMaxPreDelayLength)
    {
        FreeDelayLines();
        return false;
    }

    // __AllocDelayLine (0x80033954) allocates each line with the longest length that the tables give.
    // __BzeroDelayLines (0x80033a98) then clears it.
    const uint32_t* longest = kFilterSizes[kFusedModes];
    for (int channel = 0; channel < kChannelCount; channel++)
    {
        early_line_[channel].assign(kEarlySizes[kEarlyModes - 1], 0.0f);
        pre_delay_line_[channel].assign(pre_delay_max_length, 0.0f);
        for (int comb = 0; comb < kCombs; comb++)
        {
            comb_line_[channel][comb].assign(longest[comb], 0.0f);
        }

        for (int allpass = 0; allpass < kAllpasses; allpass++)
        {
            allpass_line_[channel][allpass].assign(longest[kCombs + allpass], 0.0f);
        }
    }

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
    FreeDelayLines();
    if (!Init(params))
    {
        return false;
    }

    active_ = (active_ | kSkipFrame) & ~kStopped;

    return true;
}

bool ReverbStdExp::SettingsUpdate(const Params& params)
{
    // The game's callback would run on null lines after a failed Init or a Shutdown. The model fails here instead.
    params_ = params;
    active_ |= kStopped;
    if (comb_line_[0][0].empty())
    {
        return false;
    }

    ClearDelayLines();
    if (!InitParams())
    {
        FreeDelayLines();
        return false;
    }

    active_ = (active_ | kSkipFrame) & ~kStopped;

    return true;
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
    const float lowpass_gain = 1.0f - lpf_coef_;
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

    // The pre-delay.
    float delayed = input;
    if (pre_delay_length_ != 0)
    {
        float& sample = pre_delay_line_[channel][pre_delay_pos_];
        delayed = sample;
        sample = input;
    }

    // Two combs in parallel.
    std::array<float, kCombs> combs{};
    for (int comb = 0; comb < kCombs; comb++)
    {
        float& sample = comb_line_[channel][comb][comb_pos_[comb]];
        combs[comb] = sample;
        sample = delayed + combs[comb] * comb_coef_[comb];
    }

    // An allpass, the low-pass and a second allpass.
    const float allpass0 = Allpass(allpass_line_[channel][0][allpass_pos_[0]], combs[0] + combs[1], allpass_coef_);
    const float lowpass = lowpass_gain * allpass0 + lpf_coef_ * last_lpf_out_[channel];
    last_lpf_out_[channel] = lowpass;
    const float allpass1 = Allpass(allpass_line_[channel][1][allpass_pos_[1]], lowpass, allpass_coef_);
    return early * early_gain + allpass1 * fused_gain;
}

void ReverbStdExp::AdvancePositions()
{
    Step(early_pos_, early_length_);
    if (pre_delay_length_ != 0)
    {
        Step(pre_delay_pos_, pre_delay_length_);
    }

    for (int comb = 0; comb < kCombs; comb++)
    {
        Step(comb_pos_[comb], comb_length_[comb]);
    }

    for (int allpass = 0; allpass < kAllpasses; allpass++)
    {
        Step(allpass_pos_[allpass], allpass_length_[allpass]);
    }
}

bool ReverbStdExp::InitParams()
{
    const Params& p = params_;
    if (p.early_mode >= kEarlyModes || p.pre_delay_time < 0.0f || p.pre_delay_time > p.pre_delay_time_max ||
        p.fused_mode >= kFusedModes || p.fused_time < 0.0f || OutOfRange(p.coloration) || OutOfRange(p.damping) ||
        OutOfRange(p.early_gain) || OutOfRange(p.fused_gain) || OutOfRange(p.out_gain) || OutOfRange(p.send_gain))
    {
        return false;
    }

    // Early modes 0 to 3 feed the early line back negated.
    early_pos_ = 0;
    early_length_ = kEarlySizes[p.early_mode];
    early_coef_ = p.early_mode <= 3 ? -0.33f : 0.33f;

    // The game's pre-delay can be longer than its line only for a NaN time. The game would then write past the line.
    // The model fails instead.
    pre_delay_pos_ = 0;
    pre_delay_length_ = ToUnsigned(kSampleRate * p.pre_delay_time);
    if (pre_delay_length_ > pre_delay_line_[0].size())
    {
        return false;
    }

    const uint32_t* sizes = kFilterSizes[p.fused_mode];
    for (int comb = 0; comb < kCombs; comb++)
    {
        comb_pos_[comb] = 0;
        comb_length_[comb] = sizes[comb];
        comb_coef_[comb] = CombCoef(sizes[comb], p.fused_time);
    }

    for (int allpass = 0; allpass < kAllpasses; allpass++)
    {
        allpass_pos_[allpass] = 0;
        allpass_length_[allpass] = sizes[kCombs + allpass];
    }

    // The coloration is the allpasses' gain. The damping sets the low-pass's weight.
    allpass_coef_ = p.coloration;
    lpf_coef_ = LowpassCoef(p.damping);
    last_lpf_out_.fill(0.0f);

    return true;
}

void ReverbStdExp::ClearDelayLines()
{
    ForEachLine(ClearLine);
}

void ReverbStdExp::FreeDelayLines()
{
    ForEachLine(ReleaseLine);
}

void ReverbStdExp::ForEachLine(void (*function)(std::vector<float>& line))
{
    for (int channel = 0; channel < kChannelCount; channel++)
    {
        function(early_line_[channel]);
        function(pre_delay_line_[channel]);
        for (std::vector<float>& line : comb_line_[channel])
        {
            function(line);
        }

        for (std::vector<float>& line : allpass_line_[channel])
        {
            function(line);
        }
    }
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
