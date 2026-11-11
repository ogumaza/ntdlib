// SPDX-License-Identifier: MIT

// nw::snd's sequence playback on the 3DS: ntdlib's model of it (SequenceTrack with its MmlParser, SequenceSoundPlayer,
// and the channels that play the notes), with the 3DS's policy and this module's engine, waves and outputs.

#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "ntdlib/ctr/snd/starting_tracks.h"
#include "ntdlib/ctr/snd/voice.h"
#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/sequence.h"
#include "ntdlib/sar/formats.h"

namespace ntdlib::ctr::snd
{

class Engine;

// What a track gives the engine for a note (nw::snd's NoteOnInfo, built by SequenceTrack::NoteOn, 0x31e374).
struct NoteOnInfo
{
    uint32_t prg_no = 0;
    int key = 60;
    int velocity = 127;
    int32_t length = -1;
    int init_pan = 0;
    int priority = 64;
    int owner_player = -1;
    int owner_track = -1;
};

// The sequence model's policy: the 3DS's arithmetic and rules, and this module's types.
struct Policy : ntdlib::nwsnd::CtrPolicy
{
    using Engine = snd::Engine;
    using Channel = ntdlib::nwsnd::Channel<Policy>;
    using NoteOnInfo = snd::NoteOnInfo;
    using SoundInfo = ntdlib::sar::SoundInfo;
    using Outputs = snd::Outputs;
    using WaveInfo = snd::WaveInfo;
    using Receiver = void;

    template <typename Function, typename... Each>
    static void ForEachOutput(const Function& function, Each&... outputs)
    {
        function(outputs...);
    }

    // The 3DS's players and tracks give no output parameters beyond the track's pan, surround pan and sends, and the
    // player's sends. The pan and the surround pan go to the channels as they are. Each send is the track's plus the
    // player's (SequenceTrack::UpdateChannelParam, 0x31de18; the sends at 0x31e03c to 0x31e068).
    static Outputs CombineOutputs(const Outputs& player, const Outputs& track, float pan, float span, float main_send,
                                  const std::array<float, 3>& fx_send);

    static void SetOutputs(Channel& channel, const Outputs& outputs)
    {
        channel.outputs = outputs;
    }

    // The track's front bypass goes to the channel's voice.
    static void SetFrontBypass(Engine& engine, Channel& channel, bool front_bypass);

    // The 3DS's channels have no receivers.
    static void SetReceiver(NoteOnInfo&, const Receiver*)
    {
    }
};

using Channel = Policy::Channel;
using ChannelManager = ntdlib::nwsnd::ChannelManager<Policy>;
using SequenceSoundPlayer = ntdlib::nwsnd::SequenceSoundPlayer<Policy>;

// Sets the variables of frame 0 for `player`, and schedules the later ones (Engine::ScheduleVariable). It skips the
// entries that set no variable (SequenceVariable::kNone).
void ApplyVariables(Engine& engine, SequenceSoundPlayer& player, std::span<const SequenceVariable> variables);

// Starts `player` as `start` gives, as a game does through the sound's handle. It sets the variables of frame 0 and
// schedules the later ones (Engine::ScheduleVariable). With a seed, it sets the engine's random generator. It quiets
// the muted tracks in the mode that keeps their channels sounding (SEQ_MUTE_NO_STOP), and the silenced tracks at once.
// It gives the other tracks their volumes (the player's SetTrackVolume). It sets the tempo ratio, the pitch and the
// player's sends on the aux buses. Call it after the sequence's start and before its first tick. Neither kind of
// quieted track then starts a note that sounds. The silenced tracks' notes take their voices. The start's banks go to
// Engine::StartSequence. The caller applies the start's volume to the output.
void ApplySequenceStart(Engine& engine, SequenceSoundPlayer& player, const SequenceStart& start);

} // namespace ntdlib::ctr::snd
