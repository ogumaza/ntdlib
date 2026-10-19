// SPDX-License-Identifier: MIT

// The definitions of voice.h's templates. The model's user includes this file in one source file, with the policy's
// types complete. That file builds the templates for the policy:
//
//     template class ntdlib::nwsnd::MultiVoice<Policy>;
//     template class ntdlib::nwsnd::MultiVoiceManager<Policy>;

#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>

#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/voice.h"

namespace ntdlib::nwsnd
{

// ------------------------------------------------------------------------------------------------------- MultiVoice

template <typename Policy>
bool MultiVoice<Policy>::Alloc(Engine& engine, int channels, uint32_t priority, ChannelId owner)
{
    const int count = channels > 2 ? 2 : (channels < 1 ? 1 : channels);
    channel_count_ = 0;
    int allocated = 0;
    for (; allocated < count; ++allocated)
    {
        // Voice::AllocVoice (0x022A5970) and Voice::Initialize (0x022A5920): the renderer's voice, stopped, with the
        // parameters that VoiceParam::Initialize (0x022A254C) sets: PCM16 at 32 kHz. When the renderer takes the voice
        // away or it's freed, the Voice loses it (AxVoiceDisposeCallback, 0x022A5908).
        Voice& voice = voices_[static_cast<std::size_t>(allocated)];
        const int renderer = engine.Renderer().Acquire(priority, voice);
        if (renderer < 0)
        {
            break;
        }

        voice.renderer = renderer;
        voice.priority = priority;
        voice.state = VoiceState::kStop;
        voice.param = Policy::InitialVoiceParam();
        voice.format = SampleFormat::kPcm16;
        voice.sample_rate = 32000;
        voice.receiver = nullptr;
    }

    if (allocated != count)
    {
        for (int i = 0; i < allocated; ++i)
        {
            FreeVoice(engine, voices_[static_cast<std::size_t>(i)]);
        }

        return false;
    }

    channel_count_ = count;
    InitParam(owner);
    active_ = true;

    return true;
}

template <typename Policy>
void MultiVoice<Policy>::InitParam(ChannelId owner)
{
    owner_ = owner;
    biquad_value_ = 0.0f;
    volume_ = 1.0f;
    pitch_ = 1.0f;
    lpf_freq_ = 1.0f;
    biquad_type_ = 0;
    front_bypass_ = false;
    started_ = false;
    sync_ = 0;
    sdk_started_ = false;
    pan_mode_ = 0;
    pan_curve_ = 0;

    const auto init_output = [](auto& param)
    {
        param.volume = 1.0f;
        param.mix_mode = 0;
        param.pan = 0.0f;
        param.span = 0.0f;
        param.main_send = 0.0f;
        param.fx_send = {};
    };
    Policy::ForEachOutput(init_output, outputs_);
}

template <typename Policy>
void MultiVoice<Policy>::Start()
{
    started_ = true;
    sync_ |= kSyncStart;
}

template <typename Policy>
void MultiVoice<Policy>::Stop(Engine& engine)
{
    if (sdk_started_)
    {
        StopAllSdkVoice(engine);
        sdk_started_ = false;
    }

    started_ = false;
}

template <typename Policy>
void MultiVoice<Policy>::Free(Engine& engine)
{
    if (!active_)
    {
        return;
    }

    for (int i = 0; i < channel_count_; ++i)
    {
        FreeVoice(engine, voices_[static_cast<std::size_t>(i)]);
    }

    channel_count_ = 0;
    engine.Voices().RemoveVoiceList(place_);
    active_ = false;
}

template <typename Policy>
void MultiVoice<Policy>::SetSampleFormat(SampleFormat format)
{
    for (int i = 0; i < channel_count_; ++i)
    {
        voices_[static_cast<std::size_t>(i)].format = format;
    }
}

template <typename Policy>
void MultiVoice<Policy>::SetSampleRate(uint32_t rate)
{
    for (int i = 0; i < channel_count_; ++i)
    {
        voices_[static_cast<std::size_t>(i)].sample_rate = rate;
    }
}

template <typename Policy>
void MultiVoice<Policy>::SetInterpolationType(uint8_t type)
{
    for (int i = 0; i < channel_count_; ++i)
    {
        voices_[static_cast<std::size_t>(i)].param.interpolation = type;
    }
}

template <typename Policy>
void MultiVoice<Policy>::AppendWave(Engine& engine, const WaveInfo& wave)
{
    engine.Renderer().AppendWave(place_, Voices(), wave);
}

template <typename Policy>
void MultiVoice<Policy>::SetOutputReceiver(const Receiver* receiver)
{
    if (channel_count_ < 1)
    {
        return;
    }

    receiver_ = receiver;
    for (int i = 0; i < channel_count_; ++i)
    {
        voices_[static_cast<std::size_t>(i)].receiver = receiver;
    }
}

template <typename Policy>
void MultiVoice<Policy>::SetVolume(float volume)
{
    volume = Console::AtLeastZero(volume);
    if (!(volume == volume_))
    {
        volume_ = volume;
        sync_ |= kSyncVe;
    }
}

template <typename Policy>
void MultiVoice<Policy>::SetPitch(float pitch)
{
    if (!(pitch == pitch_))
    {
        pitch_ = pitch;
        sync_ |= kSyncSrc;
    }
}

template <typename Policy>
void MultiVoice<Policy>::SetPanMode(uint32_t mode)
{
    if (mode != pan_mode_)
    {
        pan_mode_ = mode;
        sync_ |= kSyncMix;
    }
}

template <typename Policy>
void MultiVoice<Policy>::SetPanCurve(uint32_t curve)
{
    if (curve != pan_curve_)
    {
        pan_curve_ = curve;
        sync_ |= kSyncMix;
    }
}

template <typename Policy>
void MultiVoice<Policy>::SetLpfFreq(float lpf)
{
    if (!(lpf == lpf_freq_))
    {
        lpf_freq_ = lpf;
        sync_ |= kSyncLpf;
    }
}

template <typename Policy>
void MultiVoice<Policy>::SetBiquadFilter(int type, float value)
{
    value = value > 1.0f ? 1.0f : Console::AtLeastZero(value);
    const bool type_changed = static_cast<uint32_t>(type) != biquad_type_;
    if (type_changed)
    {
        biquad_type_ = static_cast<uint8_t>(type);
    }

    if (!(value == biquad_value_))
    {
        biquad_value_ = value;
        sync_ |= kSyncBiquad;
    }
    else if (type_changed)
    {
        sync_ |= kSyncBiquad;
    }
}

template <typename Policy>
void MultiVoice<Policy>::SetPriority(Engine& engine, uint32_t priority)
{
    priority_ = priority;
    engine.Voices().ChangeVoicePriority(place_);

    for (int i = 0; i < channel_count_; ++i)
    {
        // Voice::SetPriority (0x022A5AB0).
        Voice& voice = voices_[static_cast<std::size_t>(i)];
        voice.priority = priority_;
        if (voice.renderer >= 0)
        {
            engine.Renderer().SetPriority(voice.renderer, priority_);
        }
    }
}

template <typename Policy>
void MultiVoice<Policy>::SetFrontBypass(bool bypass)
{
    // The voices keep it in their parameters too. Only surround output would use it there.
    front_bypass_ = bypass;
}

template <typename Policy>
void MultiVoice<Policy>::SetOutputLine(uint32_t line)
{
    if (line != output_line_)
    {
        output_line_ = line;
        sync_ |= kSyncMix;
    }
}

template <typename Policy>
void MultiVoice<Policy>::SetOutputs(const Outputs& outputs)
{
    const auto set = [this](const auto& from, auto& to)
    {
        this->SetOutputParam(from, to);
    };
    Policy::ForEachOutput(set, outputs, outputs_);
}

template <typename Policy>
template <typename OutputParam>
void MultiVoice<Policy>::SetOutputParam(const OutputParam& from, OutputParam& to)
{
    const auto take = [this](float value, float& kept)
    {
        if (!(value == kept))
        {
            kept = value;
            sync_ |= kSyncMix;
        }
    };

    take(Console::AtLeastZero(from.volume), to.volume);
    if (from.mix_mode != to.mix_mode)
    {
        to.mix_mode = from.mix_mode;
        sync_ |= kSyncMix;
    }

    // nn::atk takes the mix parameters only in mix mode 1 and the pans only in mode 0.
    if (!Console::kOutputParamByMixMode || to.mix_mode == 1)
    {
        for (std::size_t channel = 0; channel < from.mix_parameter.size(); ++channel)
        {
            for (std::size_t i = 0; i < from.mix_parameter[channel].size(); ++i)
            {
                take(from.mix_parameter[channel][i], to.mix_parameter[channel][i]);
            }
        }
    }
    if (!Console::kOutputParamByMixMode || to.mix_mode == 0)
    {
        take(from.pan, to.pan);
        take(from.span, to.span);
    }

    take(from.main_send, to.main_send);
    for (std::size_t bus = 0; bus < from.fx_send.size(); ++bus)
    {
        take(from.fx_send[bus], to.fx_send[bus]);
    }
}

template <typename Policy>
void MultiVoice<Policy>::SetVoiceState(Engine& engine, Voice& voice, VoiceState state)
{
    // Starting gives the renderer's voice the wave's format, rate and coefficients. Stopping drops its buffers.
    voice.state = state;
    if (voice.renderer < 0)
    {
        return;
    }

    engine.Renderer().SetState(voice.renderer, voice);
}

template <typename Policy>
void MultiVoice<Policy>::FreeVoice(Engine& engine, Voice& voice)
{
    voice.state = VoiceState::kStop;
    if (voice.renderer >= 0)
    {
        engine.Renderer().Free(voice.renderer);
        voice.renderer = -1;
    }
}

template <typename Policy>
void MultiVoice<Policy>::RunAllSdkVoice(Engine& engine)
{
    for (int i = 0; i < channel_count_; ++i)
    {
        SetVoiceState(engine, voices_[static_cast<std::size_t>(i)], VoiceState::kPlay);
    }
}

template <typename Policy>
void MultiVoice<Policy>::StopAllSdkVoice(Engine& engine)
{
    for (int i = 0; i < channel_count_; ++i)
    {
        SetVoiceState(engine, voices_[static_cast<std::size_t>(i)], VoiceState::kStop);
    }
}

template <typename Policy>
bool MultiVoice<Policy>::IsPlayFinished(Engine& engine) const
{
    if (channel_count_ <= 0)
    {
        return false;
    }

    return engine.Renderer().IsPlayFinished(place_);
}

template <typename Policy>
void MultiVoice<Policy>::CallOwner(Engine& engine, VoiceCallbackStatus status, ChannelId owner)
{
    if (owner != kNoChannel)
    {
        engine.Channels().At(owner).OnVoiceEvent(engine, status);
    }
}

template <typename Policy>
void MultiVoice<Policy>::UpdateVoiceStatus(Engine& engine)
{
    if (!active_ || !sdk_started_)
    {
        return;
    }

    if (IsPlayFinished(engine))
    {
        CallOwner(engine, VoiceCallbackStatus::kFinishWave, owner_);
        sdk_started_ = false;
        started_ = false;
        return;
    }

    // Voice::IsAvailable (0x0229BC68): the renderer may have taken a voice away for one of higher priority.
    bool available = true;
    for (int i = 0; i < channel_count_; ++i)
    {
        const int renderer = voices_[static_cast<std::size_t>(i)].renderer;
        if (renderer < 0 || !engine.Renderer().IsAvailable(renderer))
        {
            available = false;
            break;
        }
    }
    if (available)
    {
        return;
    }

    Free(engine);
    CallOwner(engine, VoiceCallbackStatus::kDropDsp, owner_);
    started_ = false;
    sdk_started_ = false;
}

template <typename Policy>
void MultiVoice<Policy>::Calc(Engine& engine)
{
    if (!started_)
    {
        return;
    }

    if (sync_ & kSyncSrc)
    {
        CalcSrc();
        sync_ &= ~kSyncSrc;
    }
    if (sync_ & kSyncVe)
    {
        CalcVe(engine);
        sync_ &= ~kSyncVe;
    }
    if (sync_ & kSyncMix)
    {
        engine.Renderer().CalcMix(*this);
        sync_ &= ~kSyncMix;
    }
    if (sync_ & kSyncLpf)
    {
        engine.Renderer().CalcLpf(*this);
        sync_ &= ~kSyncLpf;
    }
    if (sync_ & kSyncBiquad)
    {
        engine.Renderer().CalcBiquadFilter(*this);
        sync_ &= ~kSyncBiquad;
    }

    // The Wii Remotes' filter (0x80, CalcRemoteFilter) isn't modelled.
    sync_ &= ~0x80;
}

template <typename Policy>
void MultiVoice<Policy>::Update(Engine& engine)
{
    if (!active_)
    {
        return;
    }

    // The first update after Start starts the voices on the renderer.
    const bool start = (sync_ & kSyncStart) && started_ && !sdk_started_;
    if (start)
    {
        CalcSrc();
        engine.Renderer().CalcMix(*this);
        CalcVe(engine);
        sdk_started_ = true;
        sync_ &= ~(kSyncStart | kSyncSrc | kSyncMix);
    }

    // Voice::UpdateParam (0x0229D24C): each voice's parameters reach the renderer's voice.
    for (int i = 0; i < channel_count_; ++i)
    {
        const Voice& voice = voices_[static_cast<std::size_t>(i)];
        if (voice.renderer >= 0)
        {
            engine.Renderer().SetParam(voice.renderer, voice.param);
        }
    }

    if (start)
    {
        RunAllSdkVoice(engine);
    }
}

template <typename Policy>
void MultiVoice<Policy>::CalcSrc()
{
    for (int i = 0; i < channel_count_; ++i)
    {
        voices_[static_cast<std::size_t>(i)].param.pitch = pitch_;
    }
}

template <typename Policy>
void MultiVoice<Policy>::CalcVe(Engine& engine)
{
    const float volume = volume_ * engine.Renderer().OutputVolume();
    for (int i = 0; i < channel_count_; ++i)
    {
        voices_[static_cast<std::size_t>(i)].param.volume = volume;
    }
}

// ----------------------------------------------------------------------------------------------- MultiVoiceManager

template <typename Policy>
MultiVoiceManager<Policy>::MultiVoiceManager(int count) : count_(count)
{
}

template <typename Policy>
VoiceId MultiVoiceManager<Policy>::AllocVoice(Engine& engine, int channels, uint32_t priority, ChannelId owner)
{
    if constexpr (Console::kDropVoicesForChannels)
    {
        if (QueueEmpty() && !DropLowestPriorityVoices(engine, channels, priority))
        {
            return kNoVoice;
        }
        if (QueueEmpty())
        {
            return kNoVoice;
        }
    }
    else if (QueueEmpty() && DropLowestPriorityVoice(engine, priority) == 0)
    {
        return kNoVoice;
    }

    // The front of the queue: the first MultiVoice that the queue hasn't handed out, or else the first freed. The model
    // adds a MultiVoice when the queue first tries to hand it out. A MultiVoice whose allocation fails stays at the
    // front.
    const bool unused = unused_ < count_;
    const VoiceId place = unused ? unused_ : free_.front();
    if (place == static_cast<VoiceId>(voices_.size()))
    {
        voices_.emplace_back().place_ = place;
    }

    MultiVoice<Policy>& voice = At(place);
    bool allocated = voice.Alloc(engine, channels, priority, owner);
    if constexpr (Console::kDropVoicesForChannels)
    {
        // nn::atk (0xc268d0): when the MultiVoice can't get the renderer's voices that its channels require, voices are
        // dropped once more. The allocation then runs again.
        if (!allocated && DropLowestPriorityVoices(engine, channels, priority))
        {
            allocated = voice.Alloc(engine, channels, priority, owner);
        }
    }
    if (!allocated)
    {
        return kNoVoice;
    }

    voice.priority_ = priority & 0xFF;
    if (unused)
    {
        ++unused_;
    }
    else
    {
        free_.pop_front();
    }

    AppendVoiceList(place);

    return place;
}

template <typename Policy>
int MultiVoiceManager<Policy>::DropLowestPriorityVoice(Engine& engine, uint32_t priority)
{
    if (active_.empty())
    {
        return 0;
    }

    MultiVoice<Policy>& voice = At(active_.front());
    if (static_cast<int32_t>(voice.priority_) > static_cast<int32_t>(priority))
    {
        return 0;
    }

    const int channels = voice.channel_count_;
    DropVoice(engine, voice);

    return channels;
}

template <typename Policy>
bool MultiVoiceManager<Policy>::DropLowestPriorityVoices(Engine& engine, int channels, uint32_t priority)
{
    if (active_.empty() || channels < 1)
    {
        return false;
    }

    // The MultiVoices of the lowest priority, taken in order until they have the channels, must all be at most
    // `priority` and below 255.
    int found = 0;
    for (auto it = active_.begin(); it != active_.end() && found < channels; ++it)
    {
        const MultiVoice<Policy>& voice = At(*it);
        const int32_t other = static_cast<int32_t>(voice.priority_);
        if (other == 255 || other > static_cast<int32_t>(priority))
        {
            return false;
        }

        found += voice.channel_count_;
    }
    if (found < channels)
    {
        return false;
    }

    int dropped = 0;
    while (dropped < channels && !active_.empty())
    {
        MultiVoice<Policy>& voice = At(active_.front());
        dropped += voice.channel_count_;
        DropVoice(engine, voice);
    }

    return dropped != 0;
}

template <typename Policy>
void MultiVoiceManager<Policy>::DropVoice(Engine& engine, MultiVoice<Policy>& voice)
{
    const ChannelId owner = voice.owner_;
    voice.Stop(engine);
    voice.Free(engine);
    voice.CallOwner(engine, VoiceCallbackStatus::kDropVoice, owner);
}

template <typename Policy>
void MultiVoiceManager<Policy>::UpdateAllVoiceStatus(Engine& engine)
{
    for (auto it = active_.begin(); it != active_.end();)
    {
        const VoiceId place = *it++;
        At(place).UpdateVoiceStatus(engine);
    }
}

template <typename Policy>
void MultiVoiceManager<Policy>::UpdateAllVoices(Engine& engine)
{
    for (auto it = active_.begin(); it != active_.end();)
    {
        const VoiceId place = *it++;
        At(place).Calc(engine);
    }

    for (auto it = active_.begin(); it != active_.end();)
    {
        const VoiceId place = *it++;
        At(place).Update(engine);
    }
}

template <typename Policy>
void MultiVoiceManager<Policy>::AppendVoiceList(VoiceId place)
{
    // After every voice of a lower or equal priority.
    const int32_t priority = static_cast<int32_t>(At(place).priority_);
    auto it = active_.end();
    while (it != active_.begin())
    {
        const auto previous = std::prev(it);
        if (static_cast<int32_t>(At(*previous).priority_) <= priority)
        {
            break;
        }

        it = previous;
    }

    active_.insert(it, place);
}

template <typename Policy>
void MultiVoiceManager<Policy>::RemoveVoiceList(VoiceId place)
{
    active_.remove(place);
    free_.push_back(place);
}

template <typename Policy>
void MultiVoiceManager<Policy>::ChangeVoicePriority(VoiceId place)
{
    active_.remove(place);
    AppendVoiceList(place);
}

} // namespace ntdlib::nwsnd
