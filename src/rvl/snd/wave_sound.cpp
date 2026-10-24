// SPDX-License-Identifier: MIT

#include "ntdlib/rvl/snd/wave_sound.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/sound_values.h"
#include "ntdlib/rvl/snd/engine.h"

namespace ntdlib::rvl::snd
{
namespace
{

// A wave sound's surround pan from 0 to 127, as the player gives it to the channel (UpdateChannel, 0x80050200 and
// 0x800ae9d4): a 63rd of the value plus 1 up to 1, and of the value from 2. A 64th of the value comes on top.
float SurroundPanOf(uint8_t value)
{
    float span = 0.0f;
    if (value <= 1)
    {
        span += static_cast<float>(value + 1) / 63.0f;
    }
    else
    {
        span += static_cast<float>(value) / 63.0f;
    }
    span += static_cast<float>(value) * (1.0f / 64.0f);

    return span;
}

} // namespace

WsdPlayer::WsdPlayer(int id, const ntdlib::rsar::SoundInfo& info, const ntdlib::rsar::WaveSoundFile::WaveSound& sound,
                     std::optional<WaveData> wave, const SndBuild& build)
    : id_(id), wave_(std::move(wave)), settings_(sound), build_(build)
{
    if (!settings_.notes.empty())
    {
        note_ = settings_.notes.front();
    }
    settings_.notes = {};

    // PrepareWaveSoundImpl: the volume, a division by 127, the channel's priority, the release priority fix, and the
    // pan mode and curve. The build of October 2006 has no release priority fix.
    volume_ = ntdlib::nwsnd::RevolutionPolicy::PlayerVolume(info.volume);
    pan_mode_ = info.pan_mode;
    pan_curve_ = info.pan_curve;
    channel_priority_ = info.channel_priority;
    release_priority_fix_ = info.release_priority_fix && !build.October2006();
}

void WsdPlayer::Update(Engine& engine)
{
    if (!started_ || finished_)
    {
        return;
    }

    // The archive holds the files, and the player never pauses. A channel that has ended finishes the player.
    if (channel_started_ && channel_ == kNoChannel)
    {
        finished_ = true;
        return;
    }
    if (!channel_started_ && !StartChannel(engine))
    {
        finished_ = true;
        return;
    }

    UpdateChannel(engine);
}

bool WsdPlayer::StartChannel(Engine& engine)
{
    if (!wave_)
    {
        return false;
    }

    // The channel's priority: the sound's channel priority plus 64. The channel plays at most two of the wave's
    // channels, once, from the start, with the default key, original key, velocity and tuning.
    Channel* channel = engine.Channels().AllocChannel(engine, std::min(wave_->channel_count, 2),
                                                      static_cast<uint32_t>(channel_priority_) + 64u, id_, 0);
    if (channel == nullptr)
    {
        return false;
    }

    channel->envelope.SetAttack(note_.attack);
    if (build_.SinceMarch2008())
    {
        channel->envelope.SetHold(note_.hold);
    }
    channel->envelope.SetDecay(note_.decay);
    channel->envelope.SetSustain(note_.sustain);
    channel->envelope.SetRelease(note_.release);
    channel->release_priority_fix = release_priority_fix_;

    channel->Start(engine, *wave_, -1);
    channel_ = channel->place;
    channel_started_ = true;

    return true;
}

void WsdPlayer::UpdateChannel(Engine& engine)
{
    if (channel_ == kNoChannel)
    {
        return;
    }

    // The pitch: the player's, 1 in archive mode, times the wave sound's.
    float pitch = 1.0f;
    pitch *= settings_.pitch;

    // The pan: 0 and 1 count from 63, the others from 64, each in 63rds, times the player's pan range of 1. The
    // surround pan, the main send and the sends follow (see SurroundPanOf). The build of October 2006 starts the main
    // send at 1 (0x800f3238 in Smooth Moves' main.dol). The later builds start it at 0.
    const float pan = ntdlib::nwsnd::PanOfSoundValue(settings_.pan);
    const float span = SurroundPanOf(settings_.surround_pan);
    float main_send = build_.October2006() ? 1.0f : 0.0f;
    main_send += ntdlib::nwsnd::FractionOfSoundValue(settings_.main_send) - 1.0f;
    std::array<float, ax::kAuxBusCount> fx_send{};
    for (std::size_t bus = 0; bus < fx_send.size(); ++bus)
    {
        fx_send[bus] = ntdlib::nwsnd::FractionOfSoundValue(settings_.fx_sends[bus]);
    }

    // The player also gives the channel its low-pass filter and its LFO, which only the game's handle moves from the
    // values that the channel starts with. The model leaves out the remotes.
    Channel& channel = engine.Channels().At(channel_);
    channel.pan_mode = pan_mode_;
    channel.pan_curve = pan_curve_;
    channel.track_volume = volume_;
    channel.user_pitch = pitch;
    channel.outputs.pan = pan;
    channel.outputs.span = span;
    channel.outputs.main_send = main_send;
    for (std::size_t bus = 0; bus < fx_send.size(); ++bus)
    {
        channel.outputs.fx_send[bus] = fx_send[bus];
    }
}

} // namespace ntdlib::rvl::snd
