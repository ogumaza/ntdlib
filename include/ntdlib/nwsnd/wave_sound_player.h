// SPDX-License-Identifier: MIT

// The player of a wave sound (driver::WaveSoundPlayer in nw::snd and nn::atk): ntdlib's model of it, a template on the
// sequence model's policy (see sequence.h). The player plays one wave on a channel, as a track plays a note. The
// channel's voices, envelope, pan and volume work as a note's do. The comment on a method that follows one of the
// game's routines gives the routine's address in Animal Crossing: amiibo Festival's executable, then in WarioWare: Get
// It Together!'s main.
//
// A wave sound file holds tracks of note events and a table of notes, as a bank's regions hold notes. The sound
// libraries of the Wii U and the Switch read none of the events. The player plays the wave of the sound's first note
// (note 0), once, from its start: a looped wave loops until the game stops the sound. The note's key, volume, pan,
// pitch and envelope stay unread. The channel takes the wave sound's settings instead: its pitch, pan, surround pan,
// sends, filters and envelope. The sound's entry in the archive gives the volume, the pan mode and curve, and the
// channel's priority, plus 64.

#pragma once

#include <cstdint>
#include <optional>

#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/sar/formats.h"

namespace ntdlib::nwsnd
{

template <typename Policy>
class WaveSoundPlayer
{
public:
    using Engine = typename Policy::Engine;
    using Channel = typename Policy::Channel;
    using Receiver = typename Policy::Receiver;
    using WaveInfo = typename Policy::WaveInfo;

    // Player `id` of an engine, for the sound of `info` with the settings of `sound` from its wave sound file. `wave`
    // is the wave of the sound's first note, or nothing when the note or its wave is missing. The player's channel
    // outputs to `receiver` on a console whose policy has receivers.
    WaveSoundPlayer(int id, const typename Policy::SoundInfo& info, const sar::WaveSoundFile::WaveSound& sound,
                    std::optional<WaveInfo> wave, const Receiver* receiver = nullptr);

    // The player's place among the engine's players. The channel gives it as its owner, with track 0.
    int Id() const
    {
        return id_;
    }

    // The sound thread's start. The channel starts in the player's next update.
    void Start()
    {
        started_ = true;
    }

    // Each sound frame: the channel starts in the first, and takes the player's parameters in each. The player finishes
    // once its channel has ended, or when the channel can't start (Update, 0x022B9BB8 and 0xc29470).
    void Update(Engine& engine);

    // The channel's callback (0x022B910C and 0xc29a10): the player lets its channel go.
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
    // Allocates the channel, gives it the envelope and starts the wave (StartChannel, 0x022B9A00 and 0xc29620).
    // Returns whether the channel started.
    bool StartChannel(Engine& engine);

    // The player's parameters and the wave sound's settings reach the channel (UpdateChannel, 0x022B7888 and
    // 0xc297f0).
    void UpdateChannel(Engine& engine);

    int id_;
    std::optional<WaveInfo> wave_;
    sar::WaveSoundFile::WaveSound settings_; // without the tracks and the notes
    bool started_ = false;
    bool finished_ = false;
    bool channel_started_ = false; // +0x190 and +0xf0
    ChannelId channel_ = kNoChannel;

    // What BasicSound gives the player from the sound's info and what the game sets (BasicSoundPlayer's parameters), as
    // a sequence's player holds them.
    float volume_ = 1.0f;
    float pitch_ = 1.0f;
    float lpf_ = 0.0f;
    float biquad_value_ = 0.0f;
    int8_t biquad_type_ = -1; // replaces the sound's type when 0 or more
    bool front_bypass_ = false;
    uint32_t pan_mode_ = 0;
    uint32_t pan_curve_ = 0;
    uint32_t output_line_ = 1;
    typename Policy::Outputs outputs_;
    const Receiver* receiver_ = nullptr;
    float pan_range_ = 1.0f;
    uint8_t channel_priority_ = 64;
    bool release_priority_fix_ = false;
};

} // namespace ntdlib::nwsnd
