// SPDX-License-Identifier: MIT

#include "ntdlib/nx/audio/renderer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include "ntdlib/nx/audio/delay.h"
#include "ntdlib/nx/audio/light_limiter.h"
#include "ntdlib/nx/audio/reverb.h"
#include "ntdlib/rounding.h"
#include "nx/audio/arithmetic.h"
#include "nx/audio/effect_channels.h"

namespace ntdlib::nx::audio
{
namespace
{

// A time in nanoseconds in milliseconds and in microseconds, towards zero, in 32 bits.
int32_t Milliseconds(int64_t time)
{
    return static_cast<int32_t>(time / 1000000);
}

int32_t Microseconds(int64_t time)
{
    return static_cast<int32_t>(time / 1000);
}

// The client's count of an effect's channels from the buffers it was given (SetDelayInputOutput, 0x1cfa50, and
// SetLightLimiterInputOutput, 0x1d1c70): at most the effect's channel count and 6. The call copies the buffers, if
// any, and gives the effect that count.
template <std::size_t kChannels>
void SetEffectBuffers(std::array<int8_t, kChannels>& to_inputs, std::array<int8_t, kChannels>& to_outputs,
                      int& channel_count, int max_channel_count, std::span<const int8_t> inputs,
                      std::span<const int8_t> outputs)
{
    const int count = static_cast<int>(std::min(inputs.size(), outputs.size()));
    const int channels = std::min({max_channel_count, static_cast<int>(kChannels), count});
    for (int k = 0; k < channels; ++k)
    {
        to_inputs[static_cast<std::size_t>(k)] = inputs[static_cast<std::size_t>(k)];
        to_outputs[static_cast<std::size_t>(k)] = outputs[static_cast<std::size_t>(k)];
    }

    channel_count = channels;
}

} // namespace

uint32_t Renderer::GetRequiredBufferSizeForAuxSendReturnBuffer(const RendererParameter& parameter, int frames,
                                                               int channels)
{
    const int64_t samples = int64_t{channels} * frames * parameter.sample_count;
    return static_cast<uint32_t>((samples * 4 + 0xBF) & ~int64_t{0x3F});
}

int Renderer::AddAux(int mix, uint32_t buffer_size)
{
    // The rings' room, after their headers, in samples and in frames of one channel. They go round nowhere until the
    // effect has its channels.
    Aux aux;
    aux.mix = mix;
    aux.enabled = true;
    const uint32_t bytes = buffer_size - 0x80;
    aux.capacity = bytes / 4;
    aux.capacity_frames = bytes / (static_cast<uint32_t>(parameter_.sample_count) * 4);
    aux.send.assign(aux.capacity, 0);
    aux.receive.assign(aux.capacity, 0);
    auxes_.push_back(std::move(aux));

    const int id = static_cast<int>(auxes_.size()) - 1;
    mixes_.at(static_cast<std::size_t>(mix)).effects.push_back({EffectKind::kAux, id});
    if (observer_ != nullptr)
    {
        observer_->AddAux(mix, buffer_size, id);
    }

    return id;
}

void Renderer::SetAuxInputOutput(int aux, std::span<const int8_t> inputs, std::span<const int8_t> outputs)
{
    Aux& a = auxes_.at(static_cast<std::size_t>(aux));
    const int most = mixes_.at(static_cast<std::size_t>(a.mix)).buffer_count;
    const int count = std::min(most, static_cast<int>(std::min(inputs.size(), outputs.size())));
    a.inputs.assign(inputs.begin(), inputs.begin() + std::max(count, 0));
    a.outputs.assign(outputs.begin(), outputs.begin() + std::max(count, 0));
    a.ring = count < 1 ? 0
                       : a.capacity_frames / static_cast<uint32_t>(count) * static_cast<uint32_t>(count) *
                             static_cast<uint32_t>(parameter_.sample_count);
    if (observer_ != nullptr)
    {
        observer_->SetAuxInputOutput(aux, inputs, outputs);
    }
}

void Renderer::SetAuxEnabled(int aux, bool enabled)
{
    Aux& a = auxes_.at(static_cast<std::size_t>(aux));
    a.enabled = enabled;
    a.send_read = 0;
    a.receive_write = 0;
    if (observer_ != nullptr)
    {
        observer_->SetAuxEnabled(aux, enabled);
    }
}

int32_t Renderer::GetAuxSampleCount(int aux) const
{
    const auto count = static_cast<int32_t>(auxes_.at(static_cast<std::size_t>(aux)).capacity);
    if (observer_ != nullptr)
    {
        observer_->GetAuxSampleCount(aux, count);
    }

    return count;
}

int32_t Renderer::GetAuxSampleRate(int aux) const
{
    const int32_t rate = mixes_.at(static_cast<std::size_t>(auxes_.at(static_cast<std::size_t>(aux)).mix)).sample_rate;
    if (observer_ != nullptr)
    {
        observer_->GetAuxSampleRate(aux, rate);
    }

    return rate;
}

int32_t Renderer::ReadAuxSendBuffer(int aux, std::span<int32_t> samples)
{
    const int32_t count = ReadAuxSend(aux, samples);
    if (observer_ != nullptr)
    {
        observer_->ReadAuxSendBuffer(aux, samples, count);
    }

    return count;
}

int32_t Renderer::ReadAuxSend(int aux, std::span<int32_t> samples)
{
    // Everything the renderer has written since the last read, up to the span.
    Aux& a = auxes_.at(static_cast<std::size_t>(aux));
    const uint32_t ring = a.ring;
    if (ring == 0)
    {
        return 0;
    }

    const uint32_t written = (a.send_write < a.send_read ? ring : 0) - a.send_read + a.send_write;
    const uint32_t count = std::min(written, static_cast<uint32_t>(samples.size()));
    for (uint32_t i = 0; i < count; ++i)
    {
        samples[i] = a.send[(a.send_read + i) % ring];
    }
    a.send_read = (a.send_read + count) % ring;

    return static_cast<int32_t>(count);
}

int32_t Renderer::WriteAuxReturnBuffer(int aux, std::span<const int32_t> samples)
{
    const int32_t written = WriteAuxReturn(aux, samples);
    if (observer_ != nullptr)
    {
        observer_->WriteAuxReturnBuffer(aux, samples, written);
    }

    return written;
}

int32_t Renderer::WriteAuxReturn(int aux, std::span<const int32_t> samples)
{
    // As far as the renderer's next read, or a whole ring when the two are at the same place.
    Aux& a = auxes_.at(static_cast<std::size_t>(aux));
    const uint32_t ring = a.ring;
    if (ring == 0)
    {
        return 0;
    }

    const uint32_t room = (a.receive_read > a.receive_write ? 0 : ring) + a.receive_read - a.receive_write;
    const uint32_t count = std::min(room, static_cast<uint32_t>(samples.size()));
    for (uint32_t i = 0; i < count; ++i)
    {
        a.receive[(a.receive_write + i) % ring] = samples[i];
    }
    a.receive_write = (a.receive_write + count) % ring;

    return static_cast<int32_t>(count);
}

int Renderer::AddDelay(int mix, int64_t max_delay, int channel_count)
{
    // The client's DelayType (AddDelay): the channel counts in 16 bits, and the mix's rate in Q14.
    Delay delay;
    const int channels = static_cast<uint16_t>(channel_count);
    delay.parameter.channel_count = channels;
    delay.parameter.max_channel_count = channels;
    delay.parameter.max_delay = Milliseconds(max_delay);
    delay.parameter.sample_rate =
        static_cast<int32_t>(static_cast<uint32_t>(mixes_.at(static_cast<std::size_t>(mix)).sample_rate) << 14);
    delays_.push_back(std::move(delay));

    const int id = static_cast<int>(delays_.size()) - 1;
    mixes_[static_cast<std::size_t>(mix)].effects.push_back({EffectKind::kDelay, id});
    if (observer_ != nullptr)
    {
        observer_->AddDelay(mix, max_delay, channel_count, id);
    }

    return id;
}

void Renderer::SetDelayInputOutput(int delay, std::span<const int8_t> inputs, std::span<const int8_t> outputs)
{
    Delay& d = delays_.at(static_cast<std::size_t>(delay));
    d.state = kStart;
    SetEffectBuffers(d.inputs, d.outputs, d.parameter.channel_count, d.parameter.max_channel_count, inputs, outputs);
    if (observer_ != nullptr)
    {
        observer_->SetDelayInputOutput(delay, inputs, outputs);
    }
}

void Renderer::SetDelayTime(int delay, int64_t time)
{
    Delay& d = delays_.at(static_cast<std::size_t>(delay));
    d.state = kStart;
    d.parameter.delay = Milliseconds(time);
    if (observer_ != nullptr)
    {
        observer_->SetDelayTime(delay, time);
    }
}

void Renderer::SetDelayInGain(int delay, float gain)
{
    ChangeDelay(delay).parameter.in_gain = ToFixed(gain, 14);
    if (observer_ != nullptr)
    {
        observer_->SetDelayInGain(delay, gain);
    }
}

void Renderer::SetDelayFeedbackGain(int delay, float gain)
{
    ChangeDelay(delay).parameter.feedback_gain = ToFixed(gain, 14);
    if (observer_ != nullptr)
    {
        observer_->SetDelayFeedbackGain(delay, gain);
    }
}

void Renderer::SetDelayDryGain(int delay, float gain)
{
    ChangeDelay(delay).parameter.dry_gain = ToFixed(gain, 14);
    if (observer_ != nullptr)
    {
        observer_->SetDelayDryGain(delay, gain);
    }
}

void Renderer::SetDelayChannelSpread(int delay, float spread)
{
    ChangeDelay(delay).parameter.channel_spread = ToFixed(spread, 14);
    if (observer_ != nullptr)
    {
        observer_->SetDelayChannelSpread(delay, spread);
    }
}

void Renderer::SetDelayLowPassAmount(int delay, float amount)
{
    ChangeDelay(delay).parameter.low_pass_amount = ToFixed(amount, 14);
    if (observer_ != nullptr)
    {
        observer_->SetDelayLowPassAmount(delay, amount);
    }
}

void Renderer::SetDelayEnabled(int delay, bool enabled)
{
    // An enabled delay starts again. A disabled one takes its gains again once it's enabled.
    Delay& d = delays_.at(static_cast<std::size_t>(delay));
    d.enabled = enabled;
    d.state = enabled ? kStart : kNewGains;
    if (observer_ != nullptr)
    {
        observer_->SetDelayEnabled(delay, enabled);
    }
}

int Renderer::AddReverb(int mix, int channel_count)
{
    // The client's ReverbType (AddReverb): the channel counts in 16 bits, and the mix's rate in kHz in Q14.
    Reverb reverb;
    const int channels = static_cast<uint16_t>(channel_count);
    reverb.parameter.channel_count = channels;
    reverb.parameter.max_channel_count = channels;
    const int32_t kilohertz = mixes_.at(static_cast<std::size_t>(mix)).sample_rate / 1000;
    reverb.parameter.sample_rate = static_cast<int32_t>(static_cast<uint32_t>(kilohertz) << 14);
    reverbs_.push_back(std::move(reverb));

    const int id = static_cast<int>(reverbs_.size()) - 1;
    mixes_[static_cast<std::size_t>(mix)].effects.push_back({EffectKind::kReverb, id});
    if (observer_ != nullptr)
    {
        observer_->AddReverb(mix, channel_count, id);
    }

    return id;
}

void Renderer::SetReverbInputOutput(int reverb, std::span<const int8_t> inputs, std::span<const int8_t> outputs)
{
    Reverb& r = reverbs_.at(static_cast<std::size_t>(reverb));
    r.state = kStart;
    SetEffectBuffers(r.inputs, r.outputs, r.parameter.channel_count, r.parameter.max_channel_count, inputs, outputs);
    if (observer_ != nullptr)
    {
        observer_->SetReverbInputOutput(reverb, inputs, outputs);
    }
}

Renderer::Delay& Renderer::ChangeDelay(int delay)
{
    Delay& d = delays_.at(static_cast<std::size_t>(delay));
    d.state = d.state == kStart ? kStart : kNewGains;

    return d;
}

Renderer::Reverb& Renderer::ChangeReverb(int reverb)
{
    Reverb& r = reverbs_.at(static_cast<std::size_t>(reverb));
    r.state = r.state == kStart ? kStart : kNewGains;

    return r;
}

void Renderer::SetReverbEarlyMode(int reverb, int mode)
{
    if (mode < 0 || mode >= kReverbModes)
    {
        throw std::invalid_argument("SetReverbEarlyMode: no early mode " + std::to_string(mode));
    }

    ChangeReverb(reverb).parameter.early_mode = mode;
    if (observer_ != nullptr)
    {
        observer_->SetReverbEarlyMode(reverb, mode);
    }
}

void Renderer::SetReverbLateMode(int reverb, int mode)
{
    if (mode < 0 || mode >= kReverbModes)
    {
        throw std::invalid_argument("SetReverbLateMode: no late mode " + std::to_string(mode));
    }

    ChangeReverb(reverb).parameter.late_mode = mode;
    if (observer_ != nullptr)
    {
        observer_->SetReverbLateMode(reverb, mode);
    }
}

void Renderer::SetReverbPredelayTime(int reverb, int64_t time)
{
    const float milliseconds = static_cast<float>(time / 1000) / 1000.0f;
    ChangeReverb(reverb).parameter.predelay = ToFixed(milliseconds, 14);
    if (observer_ != nullptr)
    {
        observer_->SetReverbPredelayTime(reverb, time);
    }
}

void Renderer::SetReverbDecayTime(int reverb, int64_t time)
{
    const float seconds = static_cast<float>(time / 1000000) / 1000.0f;
    ChangeReverb(reverb).parameter.decay_time = ToFixed(seconds, 14);
    if (observer_ != nullptr)
    {
        observer_->SetReverbDecayTime(reverb, time);
    }
}

void Renderer::SetReverbEarlyGain(int reverb, float gain)
{
    ChangeReverb(reverb).parameter.early_gain = ToFixed(gain, 14);
    if (observer_ != nullptr)
    {
        observer_->SetReverbEarlyGain(reverb, gain);
    }
}

void Renderer::SetReverbLateGain(int reverb, float gain)
{
    ChangeReverb(reverb).parameter.late_gain = ToFixed(gain, 14);
    if (observer_ != nullptr)
    {
        observer_->SetReverbLateGain(reverb, gain);
    }
}

void Renderer::SetReverbHighFrequencyDecayRatio(int reverb, float ratio)
{
    ChangeReverb(reverb).parameter.high_frequency_decay_ratio = ToFixed(ratio, 14);
    if (observer_ != nullptr)
    {
        observer_->SetReverbHighFrequencyDecayRatio(reverb, ratio);
    }
}

void Renderer::SetReverbColoration(int reverb, float coloration)
{
    ChangeReverb(reverb).parameter.coloration = ToFixed(coloration, 14);
    if (observer_ != nullptr)
    {
        observer_->SetReverbColoration(reverb, coloration);
    }
}

void Renderer::SetReverbReverbGain(int reverb, float gain)
{
    ChangeReverb(reverb).parameter.reverb_gain = ToFixed(gain, 14);
    if (observer_ != nullptr)
    {
        observer_->SetReverbReverbGain(reverb, gain);
    }
}

void Renderer::SetReverbOutGain(int reverb, float gain)
{
    ChangeReverb(reverb).parameter.out_gain = ToFixed(gain, 14);
    if (observer_ != nullptr)
    {
        observer_->SetReverbOutGain(reverb, gain);
    }
}

void Renderer::SetReverbDryGain(int reverb, float gain)
{
    ChangeReverb(reverb).parameter.dry_gain = ToFixed(gain, 14);
    if (observer_ != nullptr)
    {
        observer_->SetReverbDryGain(reverb, gain);
    }
}

void Renderer::SetReverbEnabled(int reverb, bool enabled)
{
    // An enabled reverb starts again. A disabled one works out its delays and gains again once it's enabled.
    Reverb& r = reverbs_.at(static_cast<std::size_t>(reverb));
    r.enabled = enabled;
    r.state = enabled ? kStart : kNewGains;
    if (observer_ != nullptr)
    {
        observer_->SetReverbEnabled(reverb, enabled);
    }
}

int Renderer::AddLightLimiter(int mix, int64_t max_look_ahead, int channel_count)
{
    // The client's LightLimiterType (AddLightLimiter): the longest look-ahead in samples from the time in
    // microseconds, and a look-ahead of a sample.
    LightLimiter limiter;
    const int32_t rate = mixes_.at(static_cast<std::size_t>(mix)).sample_rate;
    const int channels = static_cast<uint16_t>(channel_count);
    limiter.parameter.channel_count = channels;
    limiter.parameter.max_channel_count = channels;
    limiter.parameter.sample_rate = rate;
    limiter.parameter.max_look_ahead = LightLimiterSamples(rate, max_look_ahead / 1000);
    limiter.parameter.look_ahead = LightLimiterSamples(rate, 0);
    limiters_.push_back(std::move(limiter));

    const int id = static_cast<int>(limiters_.size()) - 1;
    mixes_[static_cast<std::size_t>(mix)].effects.push_back({EffectKind::kLightLimiter, id});
    if (observer_ != nullptr)
    {
        observer_->AddLightLimiter(mix, max_look_ahead, channel_count, id);
    }

    return id;
}

void Renderer::SetLightLimiterInputOutput(int limiter, std::span<const int8_t> inputs, std::span<const int8_t> outputs)
{
    LightLimiter& l = limiters_.at(static_cast<std::size_t>(limiter));
    l.state = kStart;
    SetEffectBuffers(l.inputs, l.outputs, l.parameter.channel_count, l.parameter.max_channel_count, inputs, outputs);
    if (observer_ != nullptr)
    {
        observer_->SetLightLimiterInputOutput(limiter, inputs, outputs);
    }
}

void Renderer::SetLightLimiterAttackTime(int limiter, int64_t time)
{
    LightLimiter& l = limiters_.at(static_cast<std::size_t>(limiter));
    l.parameter.attack = LightLimiterCoefficient(l.parameter.sample_rate, Microseconds(time));
    if (observer_ != nullptr)
    {
        observer_->SetLightLimiterAttackTime(limiter, time);
    }
}

void Renderer::SetLightLimiterReleaseTime(int limiter, int64_t time)
{
    LightLimiter& l = limiters_.at(static_cast<std::size_t>(limiter));
    l.parameter.release = LightLimiterCoefficient(l.parameter.sample_rate, Microseconds(time));
    if (observer_ != nullptr)
    {
        observer_->SetLightLimiterReleaseTime(limiter, time);
    }
}

void Renderer::SetLightLimiterLookAheadTime(int limiter, int64_t time)
{
    LightLimiter& l = limiters_.at(static_cast<std::size_t>(limiter));
    l.state = kStart;
    l.parameter.look_ahead = LightLimiterSamples(l.parameter.sample_rate, time / 1000);
    if (observer_ != nullptr)
    {
        observer_->SetLightLimiterLookAheadTime(limiter, time);
    }
}

void Renderer::SetLightLimiterThreshold(int limiter, float threshold)
{
    limiters_.at(static_cast<std::size_t>(limiter)).parameter.threshold = threshold;
    if (observer_ != nullptr)
    {
        observer_->SetLightLimiterThreshold(limiter, threshold);
    }
}

void Renderer::SetLightLimiterInputGain(int limiter, float gain)
{
    limiters_.at(static_cast<std::size_t>(limiter)).parameter.input_gain = gain;
    if (observer_ != nullptr)
    {
        observer_->SetLightLimiterInputGain(limiter, gain);
    }
}

void Renderer::SetLightLimiterOutputGain(int limiter, float gain)
{
    limiters_.at(static_cast<std::size_t>(limiter)).parameter.output_gain = gain;
    if (observer_ != nullptr)
    {
        observer_->SetLightLimiterOutputGain(limiter, gain);
    }
}

void Renderer::SetLightLimiterEnabled(int limiter, bool enabled)
{
    LightLimiter& l = limiters_.at(static_cast<std::size_t>(limiter));
    l.enabled = enabled;
    l.state = enabled ? kStart : kNewGains;
    if (observer_ != nullptr)
    {
        observer_->SetLightLimiterEnabled(limiter, enabled);
    }
}

// The renderer's copies of the delays, the reverbs and the limiters. A delay's update leaves out a delay whose longest
// channel count isn't 1, 2, 4 or 6. It replaces another channel count with the longest. It keeps its state until a
// command has taken it (DelayInfo::Update, 0x2188b8). A reverb's update does the same (ReverbInfo::Update, 0x218b9c).
// A limiter's update takes the client's state each time (LightLimiterInfo::Update,
// 0x219614). A new effect starts again. The client's states then become 2.
void Renderer::UpdateRendererEffects()
{
    const auto update_processed = [](auto& effect)
    {
        if (ProcessesChannels(effect.parameter.max_channel_count))
        {
            const uint8_t kept = effect.server_state;
            effect.server_parameter = effect.parameter;
            if (!ProcessesChannels(effect.server_parameter.channel_count))
            {
                effect.server_parameter.channel_count = effect.server_parameter.max_channel_count;
            }

            effect.server_inputs = effect.inputs;
            effect.server_outputs = effect.outputs;
            effect.server_enabled = effect.enabled;
            effect.server_state = effect.is_new ? kStart : kept != kUnchanged ? kept : effect.state;
            effect.server_taken = true;
        }

        effect.state = kUnchanged;
        effect.is_new = false;
    };
    std::ranges::for_each(delays_, update_processed);
    std::ranges::for_each(reverbs_, update_processed);

    for (LightLimiter& limiter : limiters_)
    {
        limiter.server_parameter = limiter.parameter;
        limiter.server_inputs = limiter.inputs;
        limiter.server_outputs = limiter.outputs;
        limiter.server_enabled = limiter.enabled;
        limiter.server_state = limiter.is_new ? kStart : limiter.state;
        limiter.server_taken = true;
        limiter.state = kUnchanged;
        limiter.is_new = false;
    }
}

// A mix's effects in their order.
void Renderer::RenderEffects(const Mix& mix)
{
    for (const Effect& effect : mix.effects)
    {
        const auto index = static_cast<std::size_t>(effect.index);
        switch (effect.kind)
        {
        case EffectKind::kAux:
            RenderAux(mix, auxes_[index]);
            break;
        case EffectKind::kDelay:
            RenderDelay(mix, delays_[index]);
            break;
        case EffectKind::kReverb:
            RenderReverb(mix, reverbs_[index]);
            break;
        case EffectKind::kLightLimiter:
            RenderLightLimiter(mix, limiters_[index]);
            break;
        }
    }
}

// An aux's command for each channel writes the input buffer into the send ring and reads the output buffer from the
// return ring, both from the renderer's offsets plus the channel's place in the frame. The last channel's command moves
// both offsets on by a frame of every channel (GenerateAuxCommand, 0x5f4a0, and WriteAuxBufferDsp and ReadAuxBufferDsp,
// 0x6d2b0 and 0x6d0f0). A disabled aux starts its rings again and passes its inputs through.
void Renderer::RenderAux(const Mix& mix, Aux& aux)
{
    const auto count = static_cast<uint32_t>(parameter_.sample_count);
    const bool enhanced = parameter_.enhanced;
    const std::size_t channels = aux.server_inputs.size();
    if (!aux.server_enabled)
    {
        aux.send_write = 0;
        aux.receive_read = 0;
        PassThrough(mix, aux.server_inputs, aux.server_outputs);
        return;
    }

    const uint32_t ring = aux.server_ring;
    for (std::size_t k = 0; k < channels && ring != 0; ++k)
    {
        const uint32_t offset = static_cast<uint32_t>(k) * count;
        const int input = mix.offset + aux.server_inputs[k];
        const int output = mix.offset + aux.server_outputs[k];
        if (aux.send_write + offset <= ring)
        {
            for (uint32_t t = 0; t < count; ++t)
            {
                aux.send[(aux.send_write + offset + t) % ring] =
                    enhanced ? RoundToInt32(FloatBuffer(input)[t]) : Buffer(input)[t];
            }
        }

        if (aux.receive_read + offset <= ring)
        {
            for (uint32_t t = 0; t < count; ++t)
            {
                const int32_t x = aux.receive[(aux.receive_read + offset + t) % ring];
                if (enhanced)
                {
                    FloatBuffer(output)[t] = static_cast<float>(x);
                }
                else
                {
                    Buffer(output)[t] = x;
                }
            }
        }
    }

    if (ring != 0)
    {
        const auto frame = static_cast<uint32_t>(channels) * count;
        aux.send_write = (aux.send_write + frame) % ring;
        aux.receive_read = (aux.receive_read + frame) % ring;
    }
}

// The update of a delay or a reverb takes it only with 1, 2, 4 or 6 channels. A light limiter takes any count.
template <typename EffectType>
bool Renderer::CommandRuns(const Mix& mix, const EffectType& effect) const
{
    const int channels = effect.server_parameter.channel_count;
    if (!effect.server_taken || !ProcessesChannels(channels))
    {
        return false;
    }

    const auto count = static_cast<std::size_t>(channels);

    return InRange(mix, std::span(effect.server_inputs.data(), count), std::span(effect.server_outputs.data(), count));
}

// A disabled effect passes its inputs through. An enabled one takes the steps before the processing. `apply` then
// processes the channels.
template <typename EffectType, typename Apply>
void Renderer::RenderCommand(const Mix& mix, EffectType& effect, Apply apply)
{
    if (!CommandRuns(mix, effect))
    {
        return;
    }

    const auto& parameter = effect.server_parameter;
    const auto channels = static_cast<std::size_t>(parameter.channel_count);
    const std::span<const int8_t> inputs(effect.server_inputs.data(), channels);
    const std::span<const int8_t> outputs(effect.server_outputs.data(), channels);
    if (!effect.server_enabled)
    {
        PassThrough(mix, inputs, outputs);
        return;
    }

    PrepareCommand(effect);

    std::array<const int32_t*, kMaxEffectChannels> in{};
    std::array<int32_t*, kMaxEffectChannels> out{};
    TakeEffectBuffers(mix, inputs, outputs, in, out);
    apply(parameter, effect.dsp, std::span(in.data(), channels), std::span(out.data(), channels),
          parameter_.sample_count);
    GiveEffectBuffers(mix, outputs, std::span(out.data(), channels));
}

// A delay's command (the processing at 0x222000 in Move It!'s sdk). An enabled delay first starts again, or takes new
// gains, when its state asks.
void Renderer::RenderDelay(const Mix& mix, Delay& delay)
{
    RenderCommand(mix, delay, ApplyDelay);
}

// A reverb's command (GenerateReverbCommand, 0x20e368, and its processing). An enabled reverb first starts again, or
// works out its delays and gains again, when its state asks. The renderer's revision 3 and later give it the long
// pre-delay line.
void Renderer::RenderReverb(const Mix& mix, Reverb& reverb)
{
    RenderCommand(mix, reverb, ApplyReverb);
}

// A light limiter's command (the processing at 0x2226f0). A limiter whose channel count isn't 1, 2, 4 or 6 has a
// command without channels (GenerateLightLimiterEffectCommand, 0x20ec24). The model leaves that command out. An
// enabled limiter first starts again if its state says so.
void Renderer::RenderLightLimiter(const Mix& mix, LightLimiter& limiter)
{
    RenderCommand(mix, limiter, ApplyLightLimiter);
}

void Renderer::StartRendererEffects()
{
    // An effect that would run in a mixed frame takes the steps of its command before the processing.
    const auto start = [this](const Mix& mix, auto& effect)
    {
        if (effect.server_enabled && CommandRuns(mix, effect))
        {
            PrepareCommand(effect);
        }
    };

    for (const Mix& mix : mixes_)
    {
        if (!mix.in_use)
        {
            continue;
        }

        for (const Effect& effect : mix.effects)
        {
            const auto index = static_cast<std::size_t>(effect.index);
            switch (effect.kind)
            {
            case EffectKind::kAux:
                break;
            case EffectKind::kDelay:
                start(mix, delays_[index]);
                break;
            case EffectKind::kReverb:
                start(mix, reverbs_[index]);
                break;
            case EffectKind::kLightLimiter:
                start(mix, limiters_[index]);
                break;
            }
        }
    }
}

// The steps of an effect's command before its processing, as its state asks. An effect at 0 starts again. A delay at 1
// takes new gains, and a reverb at 1 new delays and gains.
void Renderer::PrepareCommand(Delay& delay)
{
    if (delay.server_state == kStart)
    {
        InitializeDelay(delay.server_parameter, delay.dsp);
    }
    else if (delay.server_state == kNewGains)
    {
        SetDelayGains(delay.server_parameter, delay.dsp);
    }
}

void Renderer::PrepareCommand(Reverb& reverb)
{
    if (reverb.server_state == kStart)
    {
        InitializeReverb(reverb.server_parameter, reverb.dsp, true);
    }
    else if (reverb.server_state == kNewGains)
    {
        UpdateReverb(reverb.server_parameter, reverb.dsp);
    }
}

void Renderer::PrepareCommand(LightLimiter& limiter)
{
    if (limiter.server_state == kStart)
    {
        InitializeLightLimiter(limiter.server_parameter, limiter.dsp);
    }
}

// After the frame's commands, each effect's state is 2 until an update brings a new one (UpdateForCommandGeneration,
// 0x2189d8 and 0x2196cc).
void Renderer::FinishRendererEffects()
{
    for (Delay& delay : delays_)
    {
        delay.server_state = kUnchanged;
    }

    for (Reverb& reverb : reverbs_)
    {
        reverb.server_state = kUnchanged;
    }

    for (LightLimiter& limiter : limiters_)
    {
        limiter.server_state = kUnchanged;
    }
}

bool Renderer::InRange(const Mix& mix, std::span<const int8_t> inputs, std::span<const int8_t> outputs) const
{
    const auto valid = [&](int8_t buffer)
    {
        const int index = mix.offset + buffer;
        return index >= 0 && index < parameter_.mix_buffer_count;
    };

    return std::all_of(inputs.begin(), inputs.end(), valid) && std::all_of(outputs.begin(), outputs.end(), valid);
}

void Renderer::PassThrough(const Mix& mix, std::span<const int8_t> inputs, std::span<const int8_t> outputs)
{
    const auto count = static_cast<std::size_t>(parameter_.sample_count);
    for (std::size_t k = 0; k < inputs.size() && k < outputs.size(); ++k)
    {
        const int input = mix.offset + inputs[k];
        const int output = mix.offset + outputs[k];
        if (input != output && parameter_.enhanced)
        {
            std::copy(FloatBuffer(input), FloatBuffer(input) + count, FloatBuffer(output));
        }
        else if (input != output)
        {
            std::copy(Buffer(input), Buffer(input) + count, Buffer(output));
        }
    }
}

void Renderer::TakeEffectBuffers(const Mix& mix, std::span<const int8_t> inputs, std::span<const int8_t> outputs,
                                 std::span<const int32_t*> in, std::span<int32_t*> out)
{
    const auto count = static_cast<std::size_t>(parameter_.sample_count);
    for (std::size_t k = 0; k < inputs.size(); ++k)
    {
        if (!parameter_.enhanced)
        {
            in[k] = Buffer(mix.offset + inputs[k]);
            out[k] = Buffer(mix.offset + outputs[k]);
            continue;
        }

        int32_t* samples = effect_samples_.data() + k * count;
        const float* from = FloatBuffer(mix.offset + inputs[k]);
        for (std::size_t t = 0; t < count; ++t)
        {
            samples[t] = RoundToInt32(from[t]);
        }

        in[k] = samples;
        out[k] = effect_samples_.data() + (static_cast<std::size_t>(kMaxEffectChannels) + k) * count;
    }
}

void Renderer::GiveEffectBuffers(const Mix& mix, std::span<const int8_t> outputs, std::span<int32_t* const> out)
{
    if (!parameter_.enhanced)
    {
        return;
    }

    const auto count = static_cast<std::size_t>(parameter_.sample_count);
    for (std::size_t k = 0; k < outputs.size(); ++k)
    {
        float* to = FloatBuffer(mix.offset + outputs[k]);
        for (std::size_t t = 0; t < count; ++t)
        {
            to[t] = static_cast<float>(out[k][t]);
        }
    }
}

} // namespace ntdlib::nx::audio
