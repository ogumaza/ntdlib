// SPDX-License-Identifier: MIT

#include "ntdlib/cafe/snd/sequence.h"

#include <array>

#include "ntdlib/cafe/snd/channel.h"
#include "ntdlib/cafe/snd/engine.h"
#include "ntdlib/cafe/snd/voice.h"
#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/channel_impl.h"
#include "ntdlib/nwsnd/sequence.h"
#include "ntdlib/nwsnd/sequence_impl.h"
#include "ntdlib/nwsnd/voice.h"
#include "ntdlib/nwsnd/voice_impl.h"

namespace ntdlib::cafe::snd
{

Outputs SequencePolicy::CombineOutputs(const Outputs& player, const Outputs& track, float pan, float span,
                                       float main_send, const std::array<float, 3>& fx_send)
{
    const auto combine = [&](const OutputParam& from_player, const OutputParam& from_track)
    {
        OutputParam param = from_player;
        param.volume = from_player.volume * from_track.volume;
        for (int channel = 0; channel < 2; ++channel)
        {
            for (int i = 0; i < 6; ++i)
            {
                param.mix_parameter[channel][i] =
                    from_player.mix_parameter[channel][i] * from_track.mix_parameter[channel][i];
            }
        }

        param.pan = (from_player.pan + pan) + from_track.pan;
        param.span = (from_player.span + span) + from_track.span;
        param.main_send = (from_player.main_send + main_send) + from_track.main_send;
        for (int bus = 0; bus < 3; ++bus)
        {
            param.fx_send[bus] = (from_player.fx_send[bus] + fx_send[bus]) + from_track.fx_send[bus];
        }

        return param;
    };

    Outputs outputs;
    outputs.tv = combine(player.tv, track.tv);
    outputs.drc = {combine(player.drc[0], track.drc[0]), combine(player.drc[1], track.drc[1])};

    return outputs;
}

void SequencePolicy::SetOutputs(Channel& channel, const Outputs& outputs)
{
    channel.outputs = outputs;
}

void SequencePolicy::SetFrontBypass(Engine& engine, Channel& channel, bool front_bypass)
{
    if (channel.voice != ntdlib::nwsnd::kNoVoice)
    {
        channel.Voice(engine).SetFrontBypass(front_bypass);
    }
}

} // namespace ntdlib::cafe::snd

template class ntdlib::nwsnd::Channel<ntdlib::cafe::snd::SequencePolicy>;
template class ntdlib::nwsnd::ChannelManager<ntdlib::cafe::snd::SequencePolicy>;
template class ntdlib::nwsnd::MultiVoice<ntdlib::cafe::snd::SequencePolicy>;
template class ntdlib::nwsnd::MultiVoiceManager<ntdlib::cafe::snd::SequencePolicy>;
template class ntdlib::nwsnd::SequenceTrack<ntdlib::cafe::snd::SequencePolicy>;
template class ntdlib::nwsnd::SequenceSoundPlayer<ntdlib::cafe::snd::SequencePolicy>;
