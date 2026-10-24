// SPDX-License-Identifier: MIT

// NW4R SND's sequence playback (SeqTrack with its MML parser, and SeqPlayer): ntdlib's model of it, on the Wii's
// policy. The policy here adds the model's channels and engine, a note's request to the bank and the sound's settings
// from the archive.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "ntdlib/nwsnd/sequence.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/snd/driver.h"

namespace ntdlib::rvl::snd
{

// A note's request to the bank (NoteOnInfo).
struct NoteOnInfo
{
    uint32_t prg_no = 0;
    int key = 60;
    int velocity = 127;
    int32_t length = -1; // in ticks, or -1 for a note that lasts until it's released
    int init_pan = 0;
    int priority = 64;

    // The player and the track that own the channel.
    int owner_player = -1;
    int owner_track = -1;
};

// What a sequence player takes from its sound's entry in the archive (SoundArchivePlayer's PrepareSeqImpl, and the
// volume from BasicSound::Update). The Wii has no bank select. A sequence plays its sound's one bank, in slot 0.
struct SequenceInfo
{
    explicit SequenceInfo(const ntdlib::rsar::SoundInfo& sound)
        : volume(sound.volume),
          pan_mode(sound.pan_mode),
          pan_curve(sound.pan_curve),
          channel_priority(sound.channel_priority),
          release_priority_fix(sound.release_priority_fix),
          banks{sound.bank},
          allocate_track_flags(sound.allocate_track_flags),
          start_offset(sound.start_offset)
    {
    }

    uint8_t volume;
    uint8_t pan_mode;
    uint8_t pan_curve;
    uint8_t channel_priority;
    bool release_priority_fix;
    std::vector<uint32_t> banks;
    uint32_t allocate_track_flags;
    uint32_t start_offset;
};

// The Wii's policy of ntdlib's sequence model: NW4R SND's steps (RevolutionPolicy) and the model's channels. A track
// sets its channels' pan, surround pan and sends in their one output. The Wii has no receivers and no front bypass.
struct SequencePolicy : ChannelPolicy
{
    using Channel = snd::Channel;
    using NoteOnInfo = snd::NoteOnInfo;
    using SoundInfo = SequenceInfo;
    using Receiver = void;

    // The track's pan, surround pan and sends (SeqTrack::UpdateChannelParam, 0x80046d0c). The game can add pans and
    // sends for the player and the track. Archive mode leaves them at 0, and the model leaves them out.
    static Outputs CombineOutputs(const Outputs&, const Outputs&, float pan, float span, float main_send,
                                  const std::array<float, kFxSendCount>& fx_send)
    {
        Outputs outputs;
        outputs.pan = pan;
        outputs.span = span;
        outputs.main_send = main_send;
        outputs.fx_send = fx_send;

        return outputs;
    }

    static void SetOutputs(Channel& channel, const Outputs& outputs)
    {
        channel.outputs.pan = outputs.pan;
        channel.outputs.span = outputs.span;
        channel.outputs.main_send = outputs.main_send;
        channel.outputs.fx_send = outputs.fx_send;
    }

    static void SetFrontBypass(Engine&, Channel&, bool)
    {
    }

    static void SetReceiver(NoteOnInfo&, const Receiver*)
    {
    }
};

// A sequence sound's player. The engine passes itself into the calls that require it. A copy of a player plays on apart
// from the original. A snapshot of the engine holds such copies.
using SeqPlayer = ntdlib::nwsnd::SequenceSoundPlayer<SequencePolicy>;

} // namespace ntdlib::rvl::snd
