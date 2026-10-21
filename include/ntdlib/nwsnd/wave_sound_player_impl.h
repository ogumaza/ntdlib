// SPDX-License-Identifier: MIT

// The definitions of wave_sound_player.h. A console's source file includes this header and instantiates the player
// for its policy.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/sound_values.h"
#include "ntdlib/nwsnd/wave_sound_player.h"
#include "ntdlib/sar/formats.h"

namespace ntdlib::nwsnd
{

template <typename Policy>
WaveSoundPlayer<Policy>::WaveSoundPlayer(int id, const typename Policy::SoundInfo& info,
                                         const sar::WaveSoundFile::WaveSound& sound, std::optional<WaveInfo> wave,
                                         const Receiver* receiver)
    : id_(id), wave_(std::move(wave)), settings_(sound), receiver_(receiver)
{
    settings_.tracks = {};
    settings_.notes = {};

    // What the archive player gives a sound from its info (SoundArchivePlayer::SetCommonSoundParam, 0x02295C98): the
    // volume, a division by 127, and the pan mode and curve. A wave sound's entry also gives the channel's priority.
    volume_ = Policy::PlayerVolume(info.volume);
    pan_mode_ = info.pan_mode;
    pan_curve_ = info.pan_curve;
    channel_priority_ = info.channel_priority;
    release_priority_fix_ = info.release_priority_fix;
}

template <typename Policy>
void WaveSoundPlayer<Policy>::Update(Engine& engine)
{
    if (!started_ || finished_)
    {
        return;
    }

    // The loader that reads the files from the sound heap has nothing to wait for: the archive holds them. A start's
    // delay is 0.
    if (!channel_started_)
    {
        if (!StartChannel(engine))
        {
            finished_ = true;
            return;
        }
    }
    else if (channel_ == kNoChannel)
    {
        finished_ = true;
        return;
    }

    UpdateChannel(engine);
}

template <typename Policy>
bool WaveSoundPlayer<Policy>::StartChannel(Engine& engine)
{
    if (!wave_)
    {
        return false;
    }

    // The channel's priority: the sound's channel priority plus 64. The channel plays the wave once, from its start,
    // with the default key, original key, velocity and tuning.
    Channel* channel = engine.Channels().AllocChannel(engine, std::min(wave_->channel_count, 2),
                                                      static_cast<uint32_t>(channel_priority_) + 64u, id_, 0);
    if (channel == nullptr)
    {
        return false;
    }

    channel->envelope.SetAttack(settings_.adshr.attack);
    channel->envelope.SetHold(settings_.adshr.hold);
    channel->envelope.SetDecay(settings_.adshr.decay);
    channel->envelope.SetSustain(settings_.adshr.sustain);
    channel->envelope.SetRelease(settings_.adshr.release);
    channel->release_priority_fix = release_priority_fix_;

    // nw::snd gives the voice the player's front bypass. nn::atk gives it the player's update type instead, which
    // stays 0: the channel updates in the sound frame. On a console with receivers, the voice outputs to the player's.
    Policy::SetFrontBypass(engine, *channel, front_bypass_);
    if constexpr (!std::is_void_v<Receiver>)
    {
        if (channel->voice != kNoVoice)
        {
            channel->Voice(engine).SetOutputReceiver(receiver_);
        }
    }

    channel->Start(engine, *wave_, -1);
    channel_ = channel->place;
    channel_started_ = true;

    return true;
}

template <typename Policy>
void WaveSoundPlayer<Policy>::UpdateChannel(Engine& engine)
{
    if (channel_ == kNoChannel)
    {
        return;
    }

    // The filters: the sound's low-pass in 64ths from 64, which filters nothing, and its biquad filter unless the
    // player's replaces it.
    const float lpf = static_cast<float>(settings_.low_pass - 64) * (1.0f / 64.0f) + lpf_;
    int biquad_type = settings_.biquad_type;
    float biquad_value = FractionOfSoundValue(settings_.biquad_value);
    if (biquad_type_ != -1)
    {
        biquad_type = biquad_type_;
        biquad_value = biquad_value_;
    }

    // The output parameters: the player's, with the sound's pan, surround pan and sends added, as a track's are added
    // (see the policy's CombineOutputs).
    const float pan = PanOfSoundValue(settings_.pan) * pan_range_;
    const float span = SpanOfSoundValue(settings_.span);
    const float main_send = FractionOfSoundValue(settings_.main_send) - 1.0f;
    std::array<float, 3> fx_send{};
    for (std::size_t bus = 0; bus < fx_send.size(); ++bus)
    {
        fx_send[bus] = FractionOfSoundValue(settings_.aux_sends[bus]);
    }
    const auto outputs = Policy::CombineOutputs(outputs_, typename Policy::Outputs{}, pan, span, main_send, fx_send);

    // The player also gives the channel's first LFO the player's LFO, which only the game's handle moves from the
    // values that the channel starts with. On the Wii U, the player's remote filter and remote outputs reach the
    // channel too. The model leaves out the remotes.
    Channel& channel = engine.Channels().At(channel_);
    channel.pan_mode = pan_mode_;
    channel.pan_curve = pan_curve_;
    channel.track_volume = volume_;
    channel.user_pitch = pitch_ * settings_.pitch;
    channel.lpf = lpf;
    channel.biquad_type = static_cast<uint8_t>(biquad_type);
    channel.biquad_value = biquad_value;
    channel.output_line = output_line_;
    Policy::SetOutputs(channel, outputs);
}

} // namespace ntdlib::nwsnd
