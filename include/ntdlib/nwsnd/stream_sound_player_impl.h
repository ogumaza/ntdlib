// SPDX-License-Identifier: MIT

// The definitions of stream_sound_player.h. A console's source file includes this header and instantiates the player
// for its policy.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/sound_values.h"
#include "ntdlib/nwsnd/stream_sound_player.h"
#include "ntdlib/nwsnd/voice.h"
#include "ntdlib/sar/formats.h"

namespace ntdlib::nwsnd
{

template <typename Policy>
StreamSoundPlayer<Policy>::StreamSoundPlayer(int id, const typename Policy::SoundInfo& info, const StreamTracks& tracks,
                                             std::vector<WaveInfo> channels, const Receiver* receiver)
    : id_(id), waves_(std::move(channels)), voices_(waves_.size(), kNoVoice), receiver_(receiver)
{
    for (std::size_t i = 0; i < tracks.tracks.size(); ++i)
    {
        Track track;
        track.active = (tracks.valid & (1u << i)) != 0;
        track.info = tracks.tracks[i];
        tracks_.push_back(track);
    }

    // What the archive player gives a sound from its info (SoundArchivePlayer::SetCommonSoundParam, 0x02295C98): the
    // volume, a division by 127, and the pan mode and curve. The stream sound's details give its pitch and sends.
    volume_ = Policy::PlayerVolume(info.volume);
    pan_mode_ = info.pan_mode;
    pan_curve_ = info.pan_curve;
    item_pitch_ = info.stream.pitch;
    item_main_send_ = FractionOfSoundValue(info.stream.main_send) - 1.0f;
    for (std::size_t bus = 0; bus < item_fx_send_.size(); ++bus)
    {
        item_fx_send_[bus] = FractionOfSoundValue(info.stream.aux_sends[bus]);
    }
}

template <typename Policy>
void StreamSoundPlayer<Policy>::Start(Engine& engine)
{
    started_ = true;

    // AllocVoices (0x022C6E90, and part of 0xc0e6e0): a MultiVoice of one channel for each of the stream's channels, at
    // priority 255. When one can't be allocated, the others are freed.
    for (std::size_t channel = 0; channel < waves_.size(); ++channel)
    {
        const VoiceId voice =
            engine.Voices().AllocVoice(engine, 1, 255, VoiceOwner{kNoChannel, id_, static_cast<int>(channel)});
        if (voice == kNoVoice)
        {
            FinishPlayer(engine);
            return;
        }
        voices_[channel] = voice;
    }

    // StartPlayer (0x022B4508 and 0xc0ee10): each channel of each track that plays takes the stream's format and rate,
    // and starts. The voice queues the channel's samples as one wave.
    std::vector<bool> queued(waves_.size(), false);
    for (const Track& track : tracks_)
    {
        if (!track.active)
        {
            continue;
        }

        for (const uint8_t channel : track.info.channels)
        {
            if (channel >= voices_.size() || voices_[channel] == kNoVoice)
            {
                continue;
            }

            auto& voice = engine.Voices().At(voices_[channel]);
            const WaveInfo& wave = waves_[channel];
            voice.SetSampleFormat(wave.format);
            voice.SetSampleRate(wave.sample_rate);
            if constexpr (!std::is_void_v<Receiver>)
            {
                voice.SetOutputReceiver(receiver_);
            }
            if (!queued[channel])
            {
                voice.AppendWave(engine, wave);
                queued[channel] = true;
            }
            voice.Start();
        }
    }
}

template <typename Policy>
void StreamSoundPlayer<Policy>::Update(Engine& engine)
{
    if (!started_ || finished_)
    {
        return;
    }

    for (const VoiceId voice : voices_)
    {
        if (voice == kNoVoice)
        {
            FinishPlayer(engine);
            return;
        }
    }

    for (const Track& track : tracks_)
    {
        UpdateVoiceParams(engine, track);
    }
}

template <typename Policy>
void StreamSoundPlayer<Policy>::VoiceEvent(Engine& engine, int channel, VoiceCallbackStatus status)
{
    if (channel < 0 || channel >= ChannelCount())
    {
        return;
    }

    VoiceId& voice = voices_[static_cast<std::size_t>(channel)];
    switch (status)
    {
    case VoiceCallbackStatus::kFinishWave:
    case VoiceCallbackStatus::kCancel:
        if (voice != kNoVoice)
        {
            engine.Voices().At(voice).Free(engine);
        }
        voice = kNoVoice;
        break;

    case VoiceCallbackStatus::kDropVoice:
    case VoiceCallbackStatus::kDropDsp:
        voice = kNoVoice;
        break;
    }
}

template <typename Policy>
void StreamSoundPlayer<Policy>::UpdateVoiceParams(Engine& engine, const Track& track)
{
    if (!track.active)
    {
        return;
    }

    // TrackData::Set (0x022B4A44): the track's settings. The low-pass is in 64ths, 64 filtering nothing.
    const sar::StreamTrackInfo& info = track.info;
    const float data_volume = FractionOfSoundValue(info.volume);
    const float data_lpf = static_cast<float>(info.low_pass) * (1.0f / 64.0f);
    int biquad_type = static_cast<int8_t>(info.biquad_type);
    float biquad_value = FractionOfSoundValue(info.biquad_value);
    const float data_pan = PanOfSoundValue(info.pan);
    const float data_span = SpanOfSoundValue(info.span);
    const float data_main_send = FractionOfSoundValue(info.main_send) - 1.0f;
    std::array<float, 3> data_fx_send{};
    for (std::size_t bus = 0; bus < data_fx_send.size(); ++bus)
    {
        data_fx_send[bus] = FractionOfSoundValue(info.aux_sends[bus]);
    }

    // The voices' parameters: the player's with the track's. The player's biquad filter replaces the track's when the
    // game sets one.
    const float volume = volume_ * data_volume * track.volume;
    const float pitch = pitch_ * item_pitch_;
    const float lpf = data_lpf + lpf_;
    if (biquad_type_ != -1)
    {
        biquad_type = biquad_type_;
        biquad_value = biquad_value_;
    }
    const uint32_t output_line = track.output_line != -1 ? static_cast<uint32_t>(track.output_line) : output_line_;

    // Each device's output parameters (SetOutputParam, 0x022B4C78): the player's times the track's, with the track's
    // pan and surround pan added to the player's, and the track's and the sound's sends added to the player's.
    typename Policy::Outputs outputs = outputs_;
    Policy::ForEachOutput(
        [&](auto& param, const auto& from_track)
        {
            param.volume = param.volume * from_track.volume;
            for (std::size_t channel = 0; channel < param.mix_parameter.size(); ++channel)
            {
                for (std::size_t i = 0; i < param.mix_parameter[channel].size(); ++i)
                {
                    param.mix_parameter[channel][i] =
                        param.mix_parameter[channel][i] * from_track.mix_parameter[channel][i];
                }
            }
            param.pan = param.pan + (data_pan + from_track.pan);
            param.span = param.span + (data_span + from_track.span);
            param.main_send = (param.main_send + from_track.main_send) + (data_main_send + item_main_send_);
            for (std::size_t bus = 0; bus < param.fx_send.size(); ++bus)
            {
                param.fx_send[bus] =
                    (param.fx_send[bus] + from_track.fx_send[bus]) + (data_fx_send[bus] + item_fx_send_[bus]);
            }
        },
        outputs, track.outputs);

    const int count = static_cast<int>(info.channels.size());
    for (int index = 0; index < count; ++index)
    {
        const uint8_t channel = info.channels[static_cast<std::size_t>(index)];
        if (channel >= voices_.size() || voices_[channel] == kNoVoice)
        {
            continue;
        }

        auto& voice = engine.Voices().At(voices_[channel]);
        voice.SetVolume(volume);
        voice.SetPitch(pitch);
        voice.SetLpfFreq(lpf);
        voice.SetBiquadFilter(biquad_type, biquad_value);
        voice.SetOutputLine(output_line);
        Policy::SetStreamVoiceMode(voice, pan_mode_, pan_curve_, info.flags != 0);
        Policy::SetStreamVoiceOutputs(voice, outputs, count, index);
    }
}

template <typename Policy>
void StreamSoundPlayer<Policy>::FinishPlayer(Engine& engine)
{
    for (VoiceId& voice : voices_)
    {
        if (voice != kNoVoice)
        {
            engine.Voices().At(voice).Free(engine);
            voice = kNoVoice;
        }
    }
    finished_ = true;
}

} // namespace ntdlib::nwsnd
