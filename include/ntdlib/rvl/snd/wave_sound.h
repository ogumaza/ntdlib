// SPDX-License-Identifier: MIT

// NW4R SND's player of a wave sound (WsdPlayer): ntdlib's model of it on the Wii. The comment on a method that follows
// one of the game's routines gives the routine's address in Wii Sports' main.dol, then in Mario Kart Wii's.
//
// The player plays the first note of the wave sound (note 0) once, from its start. A looped wave loops until the game
// stops the sound. The channel takes the note's envelope, and in builds from 2008 the note's hold. It takes the wave
// sound's pitch, pan, surround pan and sends too. The note's key, volume, pan and pitch stay unread. The sound's entry
// in the archive gives the volume, the pan mode and curve, and the channel's priority, plus 64.
//
// The build of October 2006 plays the wave sound on a track of its player (WarioWare: Smooth Moves' main.dol,
// 0x800f34e0 and 0x800f3004). The track starts the same channel. The build's main send starts at 1, not 0. The voice
// then holds the send at its full level. The build sets no release priority fix.

#pragma once

#include <cstdint>
#include <optional>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/snd/driver.h"
#include "ntdlib/sound_build.h"

namespace ntdlib::rvl::snd
{

class Engine;

class WsdPlayer
{
public:
    // Player `id` of an engine, for the sound of `info` with wave sound `sound` of its file. `wave` is the wave of the
    // sound's first note, or nothing when the note or its wave is missing. The player follows `build` of NW4R SND.
    WsdPlayer(int id, const ntdlib::rsar::SoundInfo& info, const ntdlib::rsar::WaveSoundFile::WaveSound& sound,
              std::optional<WaveData> wave, const SndBuild& build);

    // The player's place among the engine's players. The channel gives it as its owner, with track 0.
    int Id() const
    {
        return id_;
    }

    // The sound thread's start (WsdPlayer::Start, 0x8004f868). The channel starts in the player's next update.
    void Start()
    {
        started_ = true;
    }

    // Each sound frame (Update, 0x8004fc1c and 0x800ae3f0): the channel starts in the first, and takes the player's
    // parameters in each. The player finishes once its channel has ended, or when the channel can't start.
    void Update(Engine& engine);

    // The channel's callback (ChannelCallbackFunc, 0x800504cc): the player lets its channel go.
    void ChannelEvent(Engine&, int, Channel&)
    {
        channel_ = kNoChannel;
    }

    bool IsFinished() const
    {
        return finished_;
    }

    // Whether the sound has started and hasn't ended.
    bool IsActive() const
    {
        return started_ && !finished_;
    }

    // The place of the channel that plays the wave, or kNoChannel before it starts and after it ends.
    ChannelId ChannelPlace() const
    {
        return channel_;
    }

private:
    // Allocates the channel, gives it the note's envelope and starts the wave (StartChannel, 0x8004fec8 and
    // 0x800ae6a0). Returns whether the channel started.
    bool StartChannel(Engine& engine);

    // The player's parameters and the wave sound's settings reach the channel (UpdateChannel, 0x8005008c and
    // 0x800ae860).
    void UpdateChannel(Engine& engine);

    int id_;
    std::optional<WaveData> wave_;
    ntdlib::rsar::WaveSoundFile::WaveSound settings_; // without the notes
    ntdlib::rsar::WaveSoundFile::Note note_;          // the first note, or the defaults without one
    SndBuild build_;
    bool started_ = false;
    bool finished_ = false;
    bool channel_started_ = false; // mWavePlayFlag
    ChannelId channel_ = kNoChannel;

    // What the archive player gives the sound from its entry (SoundArchivePlayer::PrepareWaveSoundImpl, 0x80045400),
    // as the player holds it. Archive mode leaves the game's parameters at their starting values.
    float volume_ = 1.0f;
    uint32_t pan_mode_ = 0;
    uint32_t pan_curve_ = 0;
    uint8_t channel_priority_ = 64;
    bool release_priority_fix_ = false;
};

} // namespace ntdlib::rvl::snd
