// SPDX-License-Identifier: MIT

#include "ntdlib/ctr/snd/sequence.h"

#include <array>

#include "ntdlib/ctr/snd/engine.h"
#include "ntdlib/ctr/snd/voice.h"
#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/channel_impl.h"
#include "ntdlib/nwsnd/sequence.h"
#include "ntdlib/nwsnd/sequence_impl.h"

namespace ntdlib::ctr::snd
{

Outputs Policy::CombineOutputs(const Outputs&, const Outputs&, float pan, float span, float main_send,
                               const std::array<float, 3>& fx_send)
{
    Outputs outputs;
    outputs.pan = pan;
    outputs.span = span;
    outputs.main_send = main_send;
    outputs.fx_send = {fx_send[0], fx_send[1]};

    return outputs;
}

void Policy::SetFrontBypass(Engine& engine, Channel& channel, bool front_bypass)
{
    if (channel.voice != ntdlib::nwsnd::kNoVoice)
    {
        channel.Voice(engine).SetFrontBypass(front_bypass);
    }
}

} // namespace ntdlib::ctr::snd

template class ntdlib::nwsnd::Channel<ntdlib::ctr::snd::Policy>;
template class ntdlib::nwsnd::ChannelManager<ntdlib::ctr::snd::Policy>;
template class ntdlib::nwsnd::SequenceTrack<ntdlib::ctr::snd::Policy>;
template class ntdlib::nwsnd::SequenceSoundPlayer<ntdlib::ctr::snd::Policy>;
