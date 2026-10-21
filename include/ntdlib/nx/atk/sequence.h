// SPDX-License-Identifier: MIT

// nn::atk's sequence playback (driver::SequenceTrack with its MmlParser, and driver::SequenceSoundPlayer): ntdlib's
// model of it, and of the channels and MultiVoices that play its notes. The policy here gives the model the Switch's
// arithmetic and steps. It also gives the model this module's engine, waves and voices, one output, and the mixes that
// voices output to.

#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "ntdlib/nwsnd/archive_files.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/sequence.h"
#include "ntdlib/nx/atk/channel.h"
#include "ntdlib/nx/atk/hardware.h"
#include "ntdlib/nx/atk/low_level_voice.h"
#include "ntdlib/nx/atk/voice.h"
#include "ntdlib/sar/formats.h"

namespace ntdlib::nx::atk
{

class Engine;

// The sequence model's policy: the Switch's arithmetic and steps, and this module's types.
struct SequencePolicy : ntdlib::nwsnd::NxPolicy
{
    using Channel = atk::Channel;
    using Engine = atk::Engine;
    using NoteOnInfo = atk::NoteOnInfo;
    using SoundInfo = sar::SoundInfo;
    using WaveInfo = atk::WaveInfo;
    using Outputs = OutputParam;
    using Receiver = OutputReceiver;
    using VoiceParam = atk::VoiceParam;
    using SampleFormat = atk::SampleFormat;

    // A channel's samples are where the wave archive's file has them. The renderer reads them there.
    static void PlaceSamples(WaveInfo::Channel& channel, std::span<const uint8_t> samples,
                             const ntdlib::nwsnd::LoadedWaveArchive&)
    {
        channel.data = samples;
    }

    // The one output device's parameters, from each of `outputs` in step.
    template <typename Function, typename... Each>
    static void ForEachOutput(const Function& function, Each&... outputs)
    {
        function(outputs...);
    }

    // A voice's parameters when it's allocated (Voice::AllocVoice, 0xc27bc0): gains of 1 to the main bus's front left
    // and right, and no filters.
    static VoiceParam InitialVoiceParam()
    {
        VoiceParam param;
        param.mix[0] = 1.0f;
        param.mix[1] = 1.0f;

        return param;
    }

    // The output parameters combine the player's, the track's, and the bytecode's pan, surround pan and sends. The
    // player's and the track's sends are added before the bytecode's sends.
    static OutputParam CombineOutputs(const OutputParam& player, const OutputParam& track, float pan, float span,
                                      float main_send, const std::array<float, 3>& fx_send);

    static void SetOutputs(Channel& channel, const OutputParam& output)
    {
        channel.outputs = output;
    }

    // nn::atk's tracks set no front bypass on their voices.
    static void SetFrontBypass(Engine&, Channel&, bool)
    {
    }

    // A channel's voices output to the player's mix.
    static void SetReceiver(NoteOnInfo& info, const OutputReceiver* receiver)
    {
        info.receiver = receiver;
    }

    // A stream's voice takes the player's pan mode and curve each frame (UpdateVoiceParams, 0xc10460). nn::atk's voices
    // have no front bypass.
    template <typename Voice>
    static void SetStreamVoiceMode(Voice& voice, uint32_t pan_mode, uint32_t pan_curve, bool)
    {
        voice.SetPanCurve(pan_curve);
        voice.SetPanMode(pan_mode);
    }

    // The output parameters of channel `index` of a stream's track of `count` channels. A channel by itself takes side
    // 0. The channels of a pair take sides 1 and 2, the left and the right (0xc26430). With the mix parameters, the
    // right channel's voice takes the second channel's parameters as its first (0xc10850). A track of more channels
    // gives its voices none.
    template <typename Voice>
    static void SetStreamVoiceOutputs(Voice& voice, const OutputParam& output, int count, int index)
    {
        if (count == 1)
        {
            voice.SetSide(0);
            voice.SetOutputs(output);
        }
        else if (count == 2)
        {
            voice.SetSide(index == 0 ? 1u : 2u);
            OutputParam param = output;
            if (index == 1 && param.mix_mode == 1)
            {
                param.mix_parameter[0] = param.mix_parameter[1];
                param.mix_parameter[1] = {};
            }
            voice.SetOutputs(param);
        }
    }
};

using SequenceTrack = ntdlib::nwsnd::SequenceTrack<SequencePolicy>;
using SequenceSoundPlayer = ntdlib::nwsnd::SequenceSoundPlayer<SequencePolicy>;

} // namespace ntdlib::nx::atk
