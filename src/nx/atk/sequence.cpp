// SPDX-License-Identifier: MIT

#include "ntdlib/nx/atk/sequence.h"

#include <array>
#include <cstddef>

#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/channel_impl.h"
#include "ntdlib/nwsnd/sequence.h"
#include "ntdlib/nwsnd/sequence_impl.h"
#include "ntdlib/nwsnd/voice.h"
#include "ntdlib/nwsnd/voice_impl.h"
#include "ntdlib/nx/atk/channel.h"
#include "ntdlib/nx/atk/engine.h"
#include "ntdlib/nx/atk/voice.h"
#include "ntdlib/nx/atk/voice_renderer.h"

namespace ntdlib::nx::atk
{

OutputParam SequencePolicy::CombineOutputs(const OutputParam& player, const OutputParam& track, float pan, float span,
                                           float main_send, const std::array<float, 3>& fx_send)
{
    OutputParam output = player;
    output.volume = track.volume * player.volume;
    for (std::size_t channel = 0; channel < 2; ++channel)
    {
        for (std::size_t i = 0; i < 6; ++i)
        {
            output.mix_parameter[channel][i] = track.mix_parameter[channel][i] * player.mix_parameter[channel][i];
        }
    }
    output.pan = (pan + player.pan) + track.pan;
    output.span = (span + player.span) + track.span;
    output.main_send = main_send + (track.main_send + player.main_send);
    for (std::size_t bus = 0; bus < 3; ++bus)
    {
        output.fx_send[bus] = fx_send[bus] + (track.fx_send[bus] + player.fx_send[bus]);
    }

    return output;
}

} // namespace ntdlib::nx::atk

template class ntdlib::nwsnd::Channel<ntdlib::nx::atk::SequencePolicy>;
template class ntdlib::nwsnd::ChannelManager<ntdlib::nx::atk::SequencePolicy>;
template class ntdlib::nwsnd::MultiVoice<ntdlib::nx::atk::SequencePolicy>;
template class ntdlib::nwsnd::MultiVoiceManager<ntdlib::nx::atk::SequencePolicy>;
template class ntdlib::nwsnd::SequenceTrack<ntdlib::nx::atk::SequencePolicy>;
template class ntdlib::nwsnd::SequenceSoundPlayer<ntdlib::nx::atk::SequencePolicy>;
