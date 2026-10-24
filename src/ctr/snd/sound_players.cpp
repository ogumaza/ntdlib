// SPDX-License-Identifier: MIT

#include "ntdlib/ctr/snd/sound_players.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "ntdlib/ctr/snd/engine.h"
#include "ntdlib/ctr/snd/sequence.h"
#include "ntdlib/ctr/snd/voice.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"

namespace ntdlib::ctr::snd
{
namespace
{

// The DSP's highest priority, which the stream's voices take (0x18062c). nw::snd's voice manager keeps its low byte.
constexpr int kStreamVoicePriority = 0x7fff;

// A pan from 0 to 127, with the centre at 64, as a distance from the centre in 63rds: 0 and 1 count from 63, the
// rest from 64 (0x31ed80 and 0x31f6d0). 0 and 127 then lie 63 steps from the centre.
int CentredPan(uint8_t pan)
{
    return pan <= 1 ? pan - 63 : pan - 64;
}

// `base` plus a surround pan from 0 to 127: up to 63 in 63rds, and above 63 one step more in 64ths, which gives 2 at
// 127. The product rounds before the sum (vmla).
float PlusSpan(float base, uint8_t span)
{
    if (span <= 63)
    {
        return base + static_cast<float>(span) * Policy::kOneOver63;
    }

    return base + static_cast<float>(span + 1) * (1.0f / 64.0f);
}

} // namespace

// ---------------------------------------------------------------------------------------------- WaveSoundPlayer
WaveSoundPlayer::WaveSoundPlayer(int id, const ntdlib::sar::SoundInfo& info,
                                 const ntdlib::sar::WaveSoundFile::WaveSound& sound, std::optional<WaveInfo> wave)
    : id_(id), wave_(std::move(wave)), settings_(sound)
{
    settings_.tracks = {};
    settings_.notes = {};

    // What the archive player gives a sound from its info: the volume in 127ths, and the pan mode and curve. A wave
    // sound's entry also gives the channel's priority and the release priority fix.
    parameters_.volume = Policy::PlayerVolume(info.volume);
    parameters_.pan_mode = info.pan_mode;
    parameters_.pan_curve = info.pan_curve;
    channel_priority_ = info.channel_priority;
    release_priority_fix_ = info.release_priority_fix;
}

void WaveSoundPlayer::Update(Engine& engine)
{
    if (!started_ || finished_)
    {
        return;
    }

    // The loader that reads the files from the sound heap has nothing to wait for: the archive holds them.
    if (!channel_started_)
    {
        if (!StartChannel(engine))
        {
            finished_ = true;
            return;
        }
    }
    else if (channel_ == ntdlib::nwsnd::kNoChannel)
    {
        finished_ = true;
        return;
    }

    UpdateChannel(engine);
}

bool WaveSoundPlayer::StartChannel(Engine& engine)
{
    if (!wave_)
    {
        return false;
    }

    // The channel's priority: the sound's channel priority plus 64. The channel plays the wave once, from its start,
    // with the default key, original key, velocity and tuning. The game can give a start offset. The model's is 0.
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
    Policy::SetFrontBypass(engine, *channel, parameters_.front_bypass);
    channel->Start(engine, *wave_, -1);
    channel_ = channel->place;
    channel_started_ = true;

    return true;
}

void WaveSoundPlayer::UpdateChannel(Engine& engine)
{
    if (channel_ == ntdlib::nwsnd::kNoChannel)
    {
        return;
    }

    // The wave sound's pan, surround pan and sends, added to the player's. Each product rounds before its sum. The
    // main send is the send less 1, as a track's is.
    Outputs outputs;
    outputs.pan = parameters_.pan + pan_range_ * (static_cast<float>(CentredPan(settings_.pan)) * Policy::kOneOver63);
    outputs.span = parameters_.span + PlusSpan(0.0f, settings_.span);
    outputs.main_send = parameters_.main_send + (static_cast<float>(settings_.main_send) * Policy::kOneOver127 - 1.0f);
    for (std::size_t bus = 0; bus < outputs.fx_send.size(); ++bus)
    {
        outputs.fx_send[bus] =
            parameters_.fx_send[bus] + static_cast<float>(settings_.aux_sends[bus]) * Policy::kOneOver127;
    }

    // The filters are the player's alone. The player's LFO reaches the channel's too, as the values that the channel
    // starts with.
    Channel& channel = engine.Channels().At(channel_);
    channel.pan_mode = parameters_.pan_mode;
    channel.pan_curve = parameters_.pan_curve;
    channel.track_volume = parameters_.volume;
    channel.user_pitch = settings_.pitch * parameters_.pitch;
    channel.lpf = parameters_.lpf;
    channel.biquad_type = parameters_.biquad_type;
    channel.biquad_value = parameters_.biquad_value;
    Policy::SetOutputs(channel, outputs);
}

// ---------------------------------------------------------------------------------------------- StreamTracksOf
StreamTracks StreamTracksOf(const ntdlib::sar::StreamSoundInfo& sound, const ntdlib::sar::StreamFile& file)
{
    // 0x180898: the tracks up to the last valid one, 4 at most. ReadStreamSoundInfo (0x48f64c) reads at most 2
    // channels of a track.
    StreamTracks result;
    result.valid = sound.valid_tracks;
    std::size_t count = 0;
    for (uint32_t bits = sound.valid_tracks; bits != 0; bits >>= 1)
    {
        ++count;
    }
    count = std::min(count, StreamSoundPlayer::kMaxTracks);

    // A track past the table holds the reader's defaults: no channels.
    result.tracks.resize(count);
    for (ntdlib::sar::StreamTrackInfo& track : result.tracks)
    {
        track.volume = 0;
        track.pan = 0;
    }

    const bool from_file = sound.tracks.empty() && file.tracks;
    const std::size_t given = from_file ? file.tracks->size() : sound.tracks.size();
    for (std::size_t i = 0; i < std::min(given, count); ++i)
    {
        ntdlib::sar::StreamTrackInfo& track = result.tracks[i];
        if (from_file)
        {
            const ntdlib::sar::StreamFile::Track& from = (*file.tracks)[i];
            track = {};
            track.volume = from.volume;
            track.pan = from.pan;
            track.span = from.span;
            track.flags = from.flags;
            track.channels = from.channels;
        }
        else
        {
            track = sound.tracks[i];
        }

        if (track.channels.size() > 2)
        {
            track.channels.resize(2);
        }
    }

    return result;
}

// ---------------------------------------------------------------------------------------------- StreamSoundPlayer
StreamSoundPlayer::StreamSoundPlayer(int id, const ntdlib::sar::SoundInfo& info, const StreamTracks& tracks,
                                     std::vector<WaveInfo> channels)
    : id_(id), waves_(std::move(channels)), voices_(waves_.size(), kNoVoice)
{
    for (std::size_t i = 0; i < tracks.tracks.size(); ++i)
    {
        Track track;
        track.active = (tracks.valid & (1u << i)) != 0;
        track.info = tracks.tracks[i];
        tracks_.push_back(track);
    }

    // The archive player gives the sound its volume in 127ths. The voices keep the pan mode and curve 0: the player
    // gives them neither.
    parameters_.volume = Policy::PlayerVolume(info.volume);
}

void StreamSoundPlayer::Start(Engine& engine)
{
    started_ = true;

    // 0x180128: a voice of one channel for each channel. When one can't be allocated, the others are freed. The voice's
    // callback reaches the player through the engine: a restore can put another player object in the player's place.
    for (std::size_t channel = 0; channel < waves_.size(); ++channel)
    {
        const int player = id_;
        const int index = static_cast<int>(channel);
        const VoiceId voice = engine.Voices().AllocVoice(engine, 1, kStreamVoicePriority,
                                                         [&engine, player, index](VoiceId, VoiceCallbackStatus status)
                                                         { engine.VoiceEvent(player, index, status); });
        if (voice == kNoVoice)
        {
            FinishPlayer(engine);
            return;
        }

        voices_[channel] = voice;
    }

    // The voices of each track's channels then take the track's flags, whether the track plays or not.
    for (const Track& track : tracks_)
    {
        for (const uint8_t channel : track.info.channels)
        {
            if (channel >= voices_.size())
            {
                continue;
            }

            Voice& voice = engine.Voices().At(voices_[channel]);
            switch (track.info.flags)
            {
            case 0:
                voice.SetFrontBypass(false);
                voice.SetTwoChannelBus(false);
                break;
            case 1:
                voice.SetFrontBypass(true);
                voice.SetTwoChannelBus(false);
                break;
            case 2:
                voice.SetFrontBypass(false);
                voice.SetTwoChannelBus(true);
                break;
            default:
                break;
            }
        }
    }

    // StartPlayer (0x31f9dc): each channel of each track that plays takes the stream's format and rate, and starts. The
    // game's loader has queued the first blocks by then. The model's voice queues the channel's samples as one wave.
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

            Voice& voice = engine.Voices().At(voices_[channel]);
            if (!queued[channel])
            {
                voice.AppendWave(engine, waves_[channel]);
                queued[channel] = true;
            }

            voice.Start();
        }
    }
}

void StreamSoundPlayer::Update(Engine& engine)
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

void StreamSoundPlayer::VoiceEvent(Engine& engine, int channel, VoiceCallbackStatus status)
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
            engine.Voices().FreeVoice(voice);
        }

        voice = kNoVoice;
        break;

    case VoiceCallbackStatus::kDropVoice:
    case VoiceCallbackStatus::kDropDsp:
        voice = kNoVoice;
        break;
    }
}

void StreamSoundPlayer::UpdateVoiceParams(Engine& engine, const Track& track)
{
    if (!track.active)
    {
        return;
    }

    // The voices' parameters: the track's volume in 127ths times the player's, the track's pan and surround pan added
    // to the player's, and the player's pitch, filters and sends. Each product rounds before its sum.
    const ntdlib::sar::StreamTrackInfo& info = track.info;
    const float volume = track.volume * ((static_cast<float>(info.volume) * Policy::kOneOver127) * parameters_.volume);
    const float pan = track.pan + (parameters_.pan + static_cast<float>(CentredPan(info.pan)) * Policy::kOneOver63);
    const float span = parameters_.span + PlusSpan(track.span, info.span);
    const float lpf = parameters_.lpf + 1.0f;

    // A stereo track's channels take the pan less 1 and plus 1, and a mono track's the pan.
    Outputs outputs;
    outputs.span = span;
    outputs.main_send = parameters_.main_send;
    outputs.fx_send = parameters_.fx_send;
    const std::size_t count = info.channels.size();
    for (std::size_t index = 0; index < count; ++index)
    {
        const uint8_t channel = info.channels[index];
        if (channel >= voices_.size() || voices_[channel] == kNoVoice)
        {
            continue;
        }

        Voice& voice = engine.Voices().At(voices_[channel]);
        voice.SetVolume(volume);
        voice.SetPitch(parameters_.pitch);
        voice.SetLpfFreq(lpf);
        voice.SetBiquadFilter(parameters_.biquad_type, parameters_.biquad_value);
        outputs.pan = count == 1 ? pan : index == 0 ? pan - 1.0f : pan + 1.0f;
        voice.SetOutputs(outputs);
    }
}

void StreamSoundPlayer::FinishPlayer(Engine& engine)
{
    // Stop (0x31f8e4) stops the voices, and the game frees them once the sound's handle lets the sound go (0x31fcc8).
    // The model frees them at once. nn::snd stops a voice that it frees.
    for (VoiceId& voice : voices_)
    {
        if (voice != kNoVoice)
        {
            engine.Voices().FreeVoice(voice);
            voice = kNoVoice;
        }
    }

    finished_ = true;
}

} // namespace ntdlib::ctr::snd
