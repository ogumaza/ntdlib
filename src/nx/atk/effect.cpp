// SPDX-License-Identifier: MIT

#include "ntdlib/nx/atk/effect.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "ntdlib/nx/atk/util.h"
#include "ntdlib/nx/audio/i3dl2.h"
#include "ntdlib/nx/audio/renderer.h"

namespace ntdlib::nx::atk
{

I3dl2AuxEffect::I3dl2AuxEffect(const audio::I3dl2ReverbParameterSet& parameters) : reverb_(48000)
{
    // The constructor takes the reverb's first two blocks of a frame for input and output (0x8ac7f8). The game then
    // sets every parameter, the ratio held with fmin then fmax.
    reverb_.SetInputOutput({0, 1}, {0, 1});
    audio::I3dl2ReverbParameterSet held = parameters;
    held.late_reverb_hf_decay_ratio = util::Fmax(util::Fmin(parameters.late_reverb_hf_decay_ratio, 2.0f), 0.1f);
    reverb_.SetParameters(held);
}

void I3dl2AuxEffect::UpdateBuffer(std::span<int32_t> samples, const BufferInfo& info)
{
    // Whole frames of every channel, no more than the ring holds; the game stops otherwise.
    const int32_t frame = info.sample_count_per_frame * info.channel_count;
    if (frame <= 0 || info.read_sample_count % frame != 0 || info.read_sample_count / frame > info.frame_count)
    {
        return;
    }

    const auto count = static_cast<uint32_t>(info.sample_count_per_frame);
    for (int32_t f = 0; f < info.read_sample_count / frame; ++f)
    {
        int32_t* block = samples.data() + static_cast<std::ptrdiff_t>(f) * frame;
        reverb_.Process(block, block, count);
    }
}

std::unique_ptr<EffectAux> I3dl2AuxEffect::Clone() const
{
    return std::make_unique<I3dl2AuxEffect>(*this);
}

namespace
{

// The channel count of each channel mode (0x1874ba0).
constexpr std::array<int, 4> kModeChannels = {1, 2, 4, 6};

// The renderer's buffers for an effect's channels: the bus's buffer for each.
std::vector<int8_t> EffectBuffers(std::span<const int8_t> buffers, const std::array<int, 6>& channels, int count)
{
    std::vector<int8_t> mapped;
    for (int k = 0; k < count; ++k)
    {
        const auto channel = static_cast<std::size_t>(channels[static_cast<std::size_t>(k)]);
        mapped.push_back(channel < buffers.size() ? buffers[channel] : int8_t{0});
    }

    return mapped;
}

// A time held to at least 0 and at most `most`, like nn::atk.
int64_t HeldTime(int64_t time, int64_t most)
{
    return most < time ? most : std::max<int64_t>(time, 0);
}

} // namespace

int EffectBase::ChannelCount() const
{
    return kModeChannels[static_cast<std::size_t>(mode_)];
}

float EffectBase::Held(float value, float most)
{
    return value > most ? most : util::Fmax(value, 0.0f);
}

void EffectDelay::AddEffect(audio::Renderer& renderer, int mix)
{
    if (IsAdded())
    {
        return;
    }

    renderer_ = &renderer;
    index_ = renderer.AddDelay(mix, max_delay_, ChannelCount());
    renderer.SetDelayTime(index_, delay_);
    renderer.SetDelayInGain(index_, in_gain_);
    renderer.SetDelayFeedbackGain(index_, feedback_gain_);
    renderer.SetDelayDryGain(index_, dry_gain_);
    renderer.SetDelayLowPassAmount(index_, low_pass_amount_);
    renderer.SetDelayChannelSpread(index_, channel_spread_);
    renderer.SetDelayEnabled(index_, enabled_);
}

void EffectDelay::SetEffectInputOutput(std::span<const int8_t> buffers)
{
    if (!IsAdded())
    {
        return;
    }

    const std::vector<int8_t> mapped = EffectBuffers(buffers, channels_, ChannelCount());
    renderer_->SetDelayInputOutput(index_, mapped, mapped);
}

bool EffectDelay::SetDelayTimeMax(int64_t time)
{
    if (IsAdded())
    {
        return false;
    }

    max_delay_ = time;

    return true;
}

bool EffectDelay::SetChannelMode(ChannelMode mode)
{
    // On 6 channels, the bus's centre and LFE come before its rear channels.
    if (static_cast<int>(mode) > 3 || IsAdded())
    {
        return false;
    }

    mode_ = mode;
    channels_ =
        mode == ChannelMode::k6Channel ? std::array<int, 6>{0, 1, 4, 5, 2, 3} : std::array<int, 6>{0, 1, 2, 3, 4, 5};

    return true;
}

void EffectDelay::SetDelayTime(int64_t time)
{
    delay_ = HeldTime(time, max_delay_);
    if (IsAdded())
    {
        renderer_->SetDelayTime(index_, delay_);
    }
}

void EffectDelay::SetInGain(float gain)
{
    in_gain_ = Held(gain, 1.0f);
    if (IsAdded())
    {
        renderer_->SetDelayInGain(index_, in_gain_);
    }
}

void EffectDelay::SetFeedbackGain(float gain)
{
    feedback_gain_ = Held(gain, 1.0f);
    if (IsAdded())
    {
        renderer_->SetDelayFeedbackGain(index_, feedback_gain_);
    }
}

void EffectDelay::SetDryGain(float gain)
{
    dry_gain_ = Held(gain, 1.0f);
    if (IsAdded())
    {
        renderer_->SetDelayDryGain(index_, dry_gain_);
    }
}

void EffectDelay::SetLowPassAmount(float amount)
{
    low_pass_amount_ = Held(amount, 1.0f);
    if (IsAdded())
    {
        renderer_->SetDelayLowPassAmount(index_, low_pass_amount_);
    }
}

void EffectDelay::SetChannelSpread(float spread)
{
    channel_spread_ = Held(spread, 1.0f);
    if (IsAdded())
    {
        renderer_->SetDelayChannelSpread(index_, channel_spread_);
    }
}

void EffectDelay::SetEnabled(bool enabled)
{
    enabled_ = enabled;
    if (IsAdded())
    {
        renderer_->SetDelayEnabled(index_, enabled_);
    }
}

std::unique_ptr<EffectBase> EffectDelay::Clone() const
{
    return std::make_unique<EffectDelay>(*this);
}

void EffectLightLimiter::AddEffect(audio::Renderer& renderer, int mix)
{
    if (IsAdded())
    {
        return;
    }

    renderer_ = &renderer;
    index_ = renderer.AddLightLimiter(mix, max_look_ahead_, ChannelCount());
    renderer.SetLightLimiterAttackTime(index_, attack_);
    renderer.SetLightLimiterReleaseTime(index_, release_);
    renderer.SetLightLimiterLookAheadTime(index_, look_ahead_);
    renderer.SetLightLimiterThreshold(index_, threshold_);
    renderer.SetLightLimiterInputGain(index_, input_gain_);
    renderer.SetLightLimiterOutputGain(index_, output_gain_);
    renderer.SetLightLimiterEnabled(index_, enabled_);
}

void EffectLightLimiter::SetEffectInputOutput(std::span<const int8_t> buffers)
{
    if (!IsAdded())
    {
        return;
    }

    const std::vector<int8_t> mapped = EffectBuffers(buffers, channels_, ChannelCount());
    renderer_->SetLightLimiterInputOutput(index_, mapped, mapped);
}

bool EffectLightLimiter::SetLookAheadTimeMax(int64_t time)
{
    if (IsAdded())
    {
        return false;
    }

    max_look_ahead_ = std::min<int64_t>(std::max<int64_t>(time / 1000, 0), 50000) * 1000;

    return true;
}

bool EffectLightLimiter::SetChannelMode(ChannelMode mode)
{
    // Every mode takes the bus's channels in their order.
    if (static_cast<int>(mode) > 3 || IsAdded())
    {
        return false;
    }

    mode_ = mode;

    return true;
}

void EffectLightLimiter::SetAttackTime(int64_t time)
{
    attack_ = HeldTime(time, kMostAttack);
    if (IsAdded())
    {
        renderer_->SetLightLimiterAttackTime(index_, attack_);
    }
}

void EffectLightLimiter::SetReleaseTime(int64_t time)
{
    release_ = HeldTime(time, kMostRelease);
    if (IsAdded())
    {
        renderer_->SetLightLimiterReleaseTime(index_, release_);
    }
}

void EffectLightLimiter::SetLookAheadTime(int64_t time)
{
    look_ahead_ = HeldTime(time, max_look_ahead_);
    if (IsAdded())
    {
        renderer_->SetLightLimiterLookAheadTime(index_, look_ahead_);
    }
}

void EffectLightLimiter::SetThreshold(float threshold)
{
    threshold_ = Held(threshold, 1.0f);
    if (IsAdded())
    {
        renderer_->SetLightLimiterThreshold(index_, threshold_);
    }
}

void EffectLightLimiter::SetInputGain(float gain)
{
    input_gain_ = Held(gain, 4.0f);
    if (IsAdded())
    {
        renderer_->SetLightLimiterInputGain(index_, input_gain_);
    }
}

void EffectLightLimiter::SetOutputGain(float gain)
{
    output_gain_ = Held(gain, 4.0f);
    if (IsAdded())
    {
        renderer_->SetLightLimiterOutputGain(index_, output_gain_);
    }
}

void EffectLightLimiter::SetEnabled(bool enabled)
{
    enabled_ = enabled;
    if (IsAdded())
    {
        renderer_->SetLightLimiterEnabled(index_, enabled_);
    }
}

std::unique_ptr<EffectBase> EffectLightLimiter::Clone() const
{
    return std::make_unique<EffectLightLimiter>(*this);
}

} // namespace ntdlib::nx::atk
