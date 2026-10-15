// SPDX-License-Identifier: MIT

// The definitions of channel.h's templates. The model's user includes this file in one source file, with the policy's
// types complete. That file builds the templates for the policy:
//
//     template class ntdlib::nwsnd::Channel<Policy>;
//     template class ntdlib::nwsnd::ChannelManager<Policy>;
//
// With a policy that gives the whole frame (UpdateChannel), the user writes no explicit instantiation and includes this
// file in each source file that uses the templates. An explicit instantiation would also build the template's frame,
// and the policy's voices may lack the methods that the frame calls.

#pragma once

#include <algorithm>
#include <cstdint>

#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/envelope.h"
#include "ntdlib/nwsnd/util.h"

namespace ntdlib::nwsnd
{

// ----------------------------------------------------------------------------------------------------------- Channel

template <typename Policy>
void Channel<Policy>::InitParam(int player, int track)
{
    cached_ratio = 1.0f;
    bend = 0.0f;
    tune = 1.0f;
    length = 0;
    key = 60;
    original_key = 60;
    biquad_value = 0.0f;
    user_pitch = 1.0f;
    track_volume = 1.0f;
    next_in_track = kNoChannel;
    release_priority_fix = false;
    owner_player = player;
    owner_track = track;
    source_player = player;
    source_track = track;
    cached_pitch = 0.0f;
    init_pan = 0.0f;
    lpf = 0.0f;
    biquad_type = 0;
    ignore_note_off = false;
    auto_sweep = true;
    loop_ = false;
    output_line = 1;

    const auto init_output = [](auto& param)
    {
        param.volume = 1.0f;
        param.mix_mode = 0;
        param.pan = 0.0f;
        param.span = 0.0f;
        param.main_send = 0.0f;
        param.fx_send = {};
    };
    Policy::ForEachOutput(init_output, outputs);

    silence_duration = 0;
    silence_counter = 0;
    silence_to = 255;
    sweep_pitch = 0.0f;
    silence_from = 255;
    sweep_counter = 0;
    sweep_length = 0;
    envelope.Initialize(CurveAdshr<Console>::kInitialDecibel);

    for (int i = 0; i < Console::kLfoCount; ++i)
    {
        lfos[i].param = CurveLfoParam{};
        lfo_types[i] = Console::kUntypedLfos ? kUntypedLfo : 0;
    }

    release_requested_ = false;
    updated_ = false;
    pan_curve = 0;
    key_group = 0;
    pan_mode = 0;

    // GetNwInterpolationTypeFromHardwareManager (0x022B8DA0): the 4-tap filters, the sound system's default.
    interpolation_type = 0;
    velocity_volume = 1.0f;
    init_volume = 1.0f;
}

template <typename Policy>
void Channel<Policy>::Start(Engine& engine, const WaveInfo& wave, int32_t note_length)
{
    length = note_length;
    for (auto& lfo : lfos)
    {
        lfo.Reset();
    }
    envelope.Reset(CurveAdshr<Console>::kInitialDecibel);
    sweep_counter = 0;

    auto& played = Voice(engine);
    played.SetSampleFormat(wave.format);
    played.SetSampleRate(wave.sample_rate);
    played.SetInterpolationType(interpolation_type);

    // AppendWaveBuffer (0x022B96FC): the voice queues the wave from its start to its end, and for a wave that loops,
    // the loop after that, for ever.
    loop_ = wave.loop;
    played.AppendWave(engine, wave);
    played.Start();
    active = true;
}

template <typename Policy>
float Channel<Policy>::GetSweepValue() const
{
    if (sweep_pitch == 0.0f || sweep_counter >= sweep_length)
    {
        return 0.0f;
    }

    return (sweep_pitch * static_cast<float>(sweep_length - sweep_counter)) / static_cast<float>(sweep_length);
}

template <typename Policy>
void Channel<Policy>::Update(Engine& engine, bool periodic)
{
    if constexpr (requires { Policy::UpdateChannel(*this, engine, periodic); })
    {
        Policy::UpdateChannel(*this, engine, periodic);
    }
    else
    {
        RunFrame(engine, periodic);
    }
}

template <typename Policy>
void Channel<Policy>::RunFrame(Engine& engine, bool periodic)
{
    if (!active)
    {
        return;
    }

    // The silence ramp, then the volume from the velocity, the region and the track.
    uint32_t silence = silence_to;
    if (silence_counter < silence_duration)
    {
        ++silence_counter;
        if (silence_counter < silence_duration)
        {
            const int32_t change = (static_cast<int32_t>(silence_to) - static_cast<int32_t>(silence_from)) *
                                   static_cast<int32_t>(silence_counter) / static_cast<int32_t>(silence_duration);
            silence = static_cast<uint8_t>(silence_from + change);
        }
    }
    float volume = Console::ChannelVolume(velocity_volume, init_volume, track_volume, silence);

    if (envelope.GetStatus() == AdshrStatus::kRelease && envelope.GetValue() < CurveAdshr<Console>::kInitialDecibel)
    {
        Stop(engine);
        return;
    }

    // The pitch: the key, the bend, the sweep and the pitch LFOs. The result is a ratio, kept for the next frame.
    Random& random = engine.Rng();
    const float key_distance = static_cast<float>(static_cast<int32_t>(key) - static_cast<int32_t>(original_key));
    float pitch = Console::NotePitch(key_distance, bend, GetSweepValue());
    for (int i = 0; i < Console::kLfoCount; ++i)
    {
        if (lfo_types[i] == 0)
        {
            pitch = pitch + lfos[i].GetValue(random);
        }
    }

    float ratio;
    if (pitch == cached_pitch)
    {
        ratio = cached_ratio * tune;
    }
    else
    {
        const float semitones = CalcPitchRatio(Console::ToInt32(pitch * 256.0f));
        ratio = semitones * tune;
        cached_pitch = pitch;
        cached_ratio = semitones;
    }
    ratio = ratio * user_pitch;

    // Each device's output parameters, with the note's pan and surround pan, and the pan LFOs. The pan LFOs run once
    // for each device.
    Outputs params = outputs;
    const auto pan_device = [&](auto& param)
    {
        float pan = param.pan + init_pan;
        for (int i = 0; i < Console::kLfoCount; ++i)
        {
            if (lfo_types[i] == 2)
            {
                pan = pan + lfos[i].GetValue(random);
            }
        }
        param.pan = pan;
        param.span = param.span + init_span;
    };
    Policy::ForEachOutput(pan_device, params);

    // A sound frame passes: 3 ms on the Wii U, 5 on the Switch and the 3DS. nn::atk's LFOs without a type don't step. A
    // note-off that nn::atk held back before the channel's first update takes effect now.
    if (periodic)
    {
        if (auto_sweep)
        {
            UpdateSweep(Console::kChannelFrameMs);
        }

        for (int i = 0; i < Console::kLfoCount; ++i)
        {
            if (!Console::kUntypedLfos || lfo_types[i] != kUntypedLfo)
            {
                lfos[i].Update(Console::kChannelFrameMs);
            }
        }

        envelope.Update(Console::kChannelFrameMs);

        if (Console::kDeferredNoteOff && release_requested_)
        {
            Release(engine);
            release_requested_ = false;
        }
    }

    const float lpf_freq = lpf + 1.0f;
    volume = volume * CalcVolumeRatio<Console>(envelope.GetValue());
    for (int i = 0; i < Console::kLfoCount; ++i)
    {
        if (lfo_types[i] == 1)
        {
            volume = volume * CalcVolumeRatio<Console>(lfos[i].GetValue(random) * 6.0f);
        }
    }

    if (voice != kNoVoice)
    {
        auto& played = Voice(engine);
        played.SetVolume(volume);
        played.SetPitch(ratio);
        played.SetPanMode(pan_mode);
        played.SetPanCurve(pan_curve);
        played.SetLpfFreq(lpf_freq);
        played.SetBiquadFilter(biquad_type, biquad_value);
        played.SetOutputLine(output_line);
        played.SetOutputs(params);
    }

    updated_ = true;
}

template <typename Policy>
void Channel<Policy>::Stop(Engine& engine)
{
    if (voice == kNoVoice)
    {
        return;
    }

    auto& played = Voice(engine);
    played.Stop(engine);
    played.Free(engine);
    voice = kNoVoice;
    active = false;
}

template <typename Policy>
void Channel<Policy>::Release(Engine& engine)
{
    if (envelope.GetStatus() != AdshrStatus::kRelease)
    {
        if (voice != kNoVoice && !release_priority_fix)
        {
            Voice(engine).SetPriority(engine, 1);
        }

        envelope.SetStatus(AdshrStatus::kRelease);
    }
}

template <typename Policy>
void Channel<Policy>::NoteOff(Engine& engine)
{
    if (ignore_note_off)
    {
        return;
    }

    // nn::atk (0xc23a70): before the channel's first update, the release waits for the next periodic update.
    if (Console::kDeferredNoteOff && !updated_)
    {
        release_requested_ = true;
    }
    else
    {
        Release(engine);
    }
}

template <typename Policy>
void Channel<Policy>::CallChannelCallback(Engine& engine, CallbackStatus status)
{
    if (owner_player >= 0)
    {
        engine.ChannelEvent(owner_player, owner_track, *this, status);
        Detach();
    }
}

template <typename Policy>
void Channel<Policy>::OnVoiceEvent(Engine& engine, VoiceCallbackStatus status)
{
    switch (status)
    {
    case VoiceCallbackStatus::kFinishWave:
        Voice(engine).Free(engine);
        CallChannelCallback(engine, CallbackStatus::kFinish);
        break;

    case VoiceCallbackStatus::kCancel:
        Voice(engine).Free(engine);
        CallChannelCallback(engine, CallbackStatus::kCancel);
        break;

    case VoiceCallbackStatus::kDropVoice:
    case VoiceCallbackStatus::kDropDsp:
        CallChannelCallback(engine, CallbackStatus::kDrop);
        break;

    default:
        CallChannelCallback(engine, CallbackStatus::kFinish);
        break;
    }

    voice = kNoVoice;
    allocated = false;
    active = false;

    // On the 3DS and the Wii, the channel leaves the engine's channels at once. Free destroys the channel. Nothing may
    // follow.
    if constexpr (Console::kVoiceEndFreesChannel)
    {
        engine.Channels().Free(*this);
    }
}

// ---------------------------------------------------------------------------------------------------- ChannelManager

template <typename Policy>
ChannelManager<Policy>::ChannelManager(int count) : count_(count)
{
}

template <typename Policy>
Channel<Policy>* ChannelManager<Policy>::AllocChannel(Engine& engine, int channels, uint32_t priority, int player,
                                                      int track)
{
    // A channel from the pool (ChannelManager::Alloc, 0x022B8A14), at the end of the list, then its voice. With no
    // freed place, the pool adds the next place down.
    if (free_.empty())
    {
        if (static_cast<int>(pool_.size()) >= count_)
        {
            return nullptr;
        }

        free_.push_back(count_ - 1 - static_cast<int>(pool_.size()));
        pool_.emplace_back();
    }

    const ChannelId place = free_.back();
    free_.pop_back();
    Channel<Policy>& channel = Entry(place).emplace();
    channel.place = place;
    active_.push_back(place);
    channel.allocated = true;

    const VoiceId voice = engine.Voices().AllocVoice(engine, channels, priority, place);
    if (voice == kNoVoice)
    {
        Free(channel);
        return nullptr;
    }

    channel.voice = voice;
    channel.InitParam(player, track);

    return &channel;
}

template <typename Policy>
void ChannelManager<Policy>::UpdateAllChannel(Engine& engine)
{
    for (auto it = active_.begin(); it != active_.end();)
    {
        Channel<Policy>& channel = At(*it++);
        channel.Update(engine, true);
        if (!channel.active)
        {
            channel.CallChannelCallback(engine, Channel<Policy>::CallbackStatus::kStopped);
            Free(channel);
        }
    }
}

template <typename Policy>
bool ChannelManager<Policy>::AnyChanging() const
{
    const auto changing = [this](ChannelId place)
    {
        return At(place).envelope.GetStatus() != AdshrStatus::kSustain;
    };

    return std::any_of(active_.begin(), active_.end(), changing);
}

template <typename Policy>
void ChannelManager<Policy>::Free(Channel<Policy>& channel)
{
    const ChannelId place = channel.place;
    if (!Holds(place) || !Entry(place) || &At(place) != &channel)
    {
        return;
    }

    active_.remove(place);
    free_.push_back(place);
    Entry(place).reset();
}

} // namespace ntdlib::nwsnd
