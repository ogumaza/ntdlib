// SPDX-License-Identifier: MIT

#include "ntdlib/rvl/axfx/reverb_hi.h"

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

// The early mode's three taps (__EarlySizeTable, 0x803ae840 in Wii Sports): each tap's delay. The last tap's delay is
// the early line's length. Mode 7 has the longest line. Init allocates the line with that length.
constexpr uint32_t kEarlyModes = 8;
constexpr uint32_t kEarlySizes[kEarlyModes][3] = {
    {157, 479, 829},   {317, 809, 1117},  {479, 941, 1487},   {641, 1259, 1949},
    {797, 1667, 2579}, {967, 1901, 2903}, {1123, 2179, 3413}, {1279, 2477, 3889},
};

// The early mode's gains for its three taps (__EarlyCoefTable, 0x803ae8a0 in Wii Sports).
constexpr float kEarlyCoefs[kEarlyModes][3] = {
    {0.4f, -1.0f, 0.3f}, {0.5f, -0.95f, 0.3f}, {0.6f, -0.9f, 0.3f}, {0.75f, -0.85f, 0.3f},
    {-0.9f, 0.8f, 0.3f}, {-1.0f, 0.7f, 0.3f},  {-1.0f, 0.7f, 0.3f}, {-1.0f, 0.7f, 0.3f},
};

// For each fused mode, the lengths of the three combs, the two allpasses and the three channels' last allpasses
// (__FilterSizeTable, 0x803ae900 in Wii Sports). The last row holds the longest lengths. Init allocates the lines with
// them.
constexpr uint32_t kFusedModes = 6;
constexpr uint32_t kFilterSizes[kFusedModes + 1][8] = {
    {1789, 1999, 2333, 433, 149, 47, 73, 67}, {149, 293, 449, 251, 103, 47, 73, 67},
    {947, 1361, 1531, 433, 137, 47, 73, 67},  {1279, 1531, 1973, 509, 149, 47, 73, 67},
    {1531, 1847, 2297, 563, 179, 47, 73, 67}, {1823, 2357, 2693, 571, 137, 47, 73, 67},
    {1823, 2357, 2693, 571, 179, 47, 73, 67},
};

} // namespace

ReverbHiExp::ReverbHiExp(Build build) : build_(build)
{
}

uint32_t ReverbHiExp::GetMemSize(const Params& params)
{
    // Each channel's early line, pre-delay, combs and allpasses at their longest, plus the last allpasses. This sum
    // converts the pre-delay with fctiwz. Init uses the runtime's unsigned conversion.
    const uint32_t* longest = kFilterSizes[kFusedModes];
    uint32_t samples = kEarlySizes[kEarlyModes - 1][kTaps - 1] +
                       static_cast<uint32_t>(ntdlib::PowerPcToInt32(kSampleRate * params.pre_delay_time_max));
    for (int i = 0; i < kCombs + kAllpasses; i++)
    {
        samples += longest[i];
    }

    samples *= static_cast<uint32_t>(kChannelCount);
    for (int channel = 0; channel < kChannelCount; channel++)
    {
        samples += longest[kCombs + kAllpasses + channel];
    }

    return samples * 4;
}

bool ReverbHiExp::Init(const Params& params)
{
    params_ = params;
    active_ = kStopped;
    const std::optional<uint32_t> pre_delay_max_length = reverb::PreDelayMaxLength(params.pre_delay_time_max);
    if (!pre_delay_max_length)
    {
        FreeDelayLines();
        return false;
    }

    // __AllocDelayLine (0x80108080) allocates each line with the longest length that the tables give.
    // __BzeroDelayLines then clears it.
    const uint32_t* longest = kFilterSizes[kFusedModes];
    for (int channel = 0; channel < kChannelCount; channel++)
    {
        if (build_ == Build::kSeptember2008)
        {
            early_line_[channel].assign(kEarlySizes[kEarlyModes - 1][kTaps - 1], 0.0f);
        }

        last_allpass_line_[channel].assign(longest[kCombs + kAllpasses + channel], 0.0f);
    }

    reverb::Allocate(lines_, *pre_delay_max_length, longest);

    if (!InitParams())
    {
        FreeDelayLines();
        return false;
    }

    active_ &= ~kStopped;

    return true;
}

bool ReverbHiExp::Settings(const Params& params)
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

bool ReverbHiExp::SettingsUpdate(const Params& params)
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

void ReverbHiExp::Shutdown()
{
    active_ |= kStopped;
    FreeDelayLines();
}

void ReverbHiExp::Process(AuxFrame& frame)
{
    if (active_ != 0)
    {
        active_ &= ~kSkipFrame;
        return;
    }

    // The gains that the callback computes once a frame.
    const float lowpass_gain = 1.0f - lines_.lpf_coef;
    const float fused_gain = 0.6f * params_.fused_gain;
    const float crosstalk = 0.5f * params_.crosstalk;
    for (int i = 0; i < kSamplesPerFrame; i++)
    {
        std::array<float, kChannelCount> out{};
        for (int channel = 0; channel < kChannelCount; channel++)
        {
            const float input = static_cast<float>(frame.channels[channel][i]);
            out[channel] = ProcessChannel(channel, input, fused_gain, lowpass_gain);
        }

        // The crosstalk mixes the other two channels into each one.
        const float left = out[kLeft] + (out[kRight] + out[kSurround]) * crosstalk;
        const float right = out[kRight] + (out[kLeft] + out[kSurround]) * crosstalk;
        const float surround = out[kSurround] + (out[kLeft] + out[kRight]) * crosstalk;
        frame.channels[kLeft][i] = ntdlib::PowerPcToInt32(left * params_.out_gain);
        frame.channels[kRight][i] = ntdlib::PowerPcToInt32(right * params_.out_gain);
        frame.channels[kSurround][i] = ntdlib::PowerPcToInt32(surround * params_.out_gain);
        AdvancePositions();
    }
}

float ReverbHiExp::ProcessChannel(int channel, float input, float fused_gain, float lowpass_gain)
{
    // The early reflections. The builds of 2006 never write the early line. Each tap then reads a zero.
    std::array<float, kTaps> taps{};
    if (build_ == Build::kSeptember2008)
    {
        std::vector<float>& line = early_line_[channel];
        for (int tap = 0; tap < kTaps; tap++)
        {
            taps[tap] = line[early_pos_[tap]];
        }

        line[early_pos_[kTaps - 1]] = input;
    }

    const float early = early_coef_[2] * taps[2] + (early_coef_[0] * taps[0] + early_coef_[1] * taps[1]);

    // The pre-delay and three combs in parallel.
    const float delayed = reverb::PreDelay(lines_, channel, input);
    float combs = 0.0f;
    for (int comb = 0; comb < kCombs; comb++)
    {
        combs += reverb::Comb(lines_, channel, comb, delayed);
    }

    // The two allpasses, the low-pass and the channel's last allpass.
    const float allpass0 = reverb::Allpass(lines_, channel, 0, combs);
    const float allpass1 = reverb::Allpass(lines_, channel, 1, allpass0);
    const float lowpass = reverb::Lowpass(lines_, channel, allpass1, lowpass_gain);
    float& last = last_allpass_line_[channel][last_allpass_pos_[channel]];
    const float output = Allpass(last, lowpass, lines_.allpass_coef);

    return output * fused_gain + early;
}

void ReverbHiExp::AdvancePositions()
{
    for (uint32_t& position : early_pos_)
    {
        Step(position, early_length_);
    }

    reverb::StepLines(lines_);
    for (int channel = 0; channel < kChannelCount; channel++)
    {
        Step(last_allpass_pos_[channel], last_allpass_length_[channel]);
    }
}

bool ReverbHiExp::InitParams()
{
    const Params& p = params_;
    if (reverb::AnyOutOfRange(p, kEarlyModes, kFusedModes) || OutOfRange(p.crosstalk))
    {
        return false;
    }

    // The early line is as long as the last tap's delay. Each tap starts at that length less its delay.
    const uint32_t* early_sizes = kEarlySizes[p.early_mode];
    early_length_ = early_sizes[kTaps - 1];
    for (int tap = 0; tap < kTaps; tap++)
    {
        early_pos_[tap] = early_length_ - early_sizes[tap];
        early_coef_[tap] = 0.6f * (p.early_gain * kEarlyCoefs[p.early_mode][tap]);
    }

    const uint32_t* sizes = kFilterSizes[p.fused_mode];
    if (!reverb::SetUp(lines_, p, sizes))
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

void ReverbHiExp::ClearDelayLines()
{
    for (int channel = 0; channel < kChannelCount; channel++)
    {
        Clear(early_line_[channel]);
        Clear(last_allpass_line_[channel]);
    }

    reverb::ClearLines(lines_);
}

void ReverbHiExp::FreeDelayLines()
{
    for (int channel = 0; channel < kChannelCount; channel++)
    {
        early_line_[channel] = {};
        last_allpass_line_[channel] = {};
    }

    reverb::FreeLines(lines_);
}

ReverbHi::ReverbHi(Build build) : exp_(build)
{
}

uint32_t ReverbHi::GetMemSize(const Params& params)
{
    return ReverbHiExp::GetMemSize(Convert(params));
}

bool ReverbHi::Init(const Params& params)
{
    return exp_.Init(Convert(params));
}

bool ReverbHi::Settings(const Params& params)
{
    return exp_.Settings(Convert(params));
}

void ReverbHi::Shutdown()
{
    exp_.Shutdown();
}

void ReverbHi::Process(AuxFrame& frame)
{
    exp_.Process(frame);
}

ReverbHiExp::Params ReverbHi::Convert(const Params& params)
{
    ReverbHiExp::Params exp;
    exp.early_mode = 5;
    exp.pre_delay_time_max = params.pre_delay;
    exp.pre_delay_time = params.pre_delay;
    exp.fused_mode = 0;
    exp.fused_time = params.time;
    exp.coloration = params.coloration;
    exp.damping = params.damping;
    exp.crosstalk = params.crosstalk;
    exp.early_gain = 0.0f;
    exp.fused_gain = 1.0f;
    exp.out_gain = params.mix;
    exp.send_gain = 0.0f;

    return exp;
}

} // namespace ntdlib::rvl::axfx
