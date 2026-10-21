// SPDX-License-Identifier: MIT

// nw::snd's sequence playback in NintendoWare for Cafe: ntdlib's model of it (driver::SequenceTrack with its MmlParser,
// and driver::SequenceSoundPlayer), and of the channels that play its notes. The policy here gives the model the Wii
// U's arithmetic, and this module's engine, waves, voices and outputs.

#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "ntdlib/cafe/snd/ax_voice.h"
#include "ntdlib/cafe/snd/channel.h"
#include "ntdlib/cafe/snd/voice.h"
#include "ntdlib/nwsnd/archive_files.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/sequence.h"
#include "ntdlib/sar/formats.h"

namespace ntdlib::cafe::snd
{

class Engine;

// The sequence model's policy: the Wii U's arithmetic, and this module's types.
struct SequencePolicy : ntdlib::nwsnd::CafePolicy
{
    using Channel = snd::Channel;
    using Engine = snd::Engine;
    using NoteOnInfo = snd::NoteOnInfo;
    using SoundInfo = sar::SoundInfo;
    using WaveInfo = snd::WaveInfo;
    using Outputs = snd::Outputs;
    using Receiver = void;
    using VoiceParam = snd::VoiceParam;
    using SampleFormat = snd::SampleFormat;

    // A channel's samples are where the wave archive's file has them, in AX's memory. The engine maps the whole file
    // there when a note first uses the wave archive.
    static void PlaceSamples(WaveInfo::Channel& channel, std::span<const uint8_t> samples,
                             const ntdlib::nwsnd::LoadedWaveArchive& archive)
    {
        channel.address = archive.address + static_cast<uint32_t>(samples.data() - archive.file.data());
    }

    // Each device's output parameters, the TV's first, then each GamePad's, from each of `outputs` in step.
    template <typename Function, typename... Each>
    static void ForEachOutput(const Function& function, Each&... outputs)
    {
        function(outputs.tv...);
        function(outputs.drc[0]...);
        function(outputs.drc[1]...);
    }

    // A voice's parameters when it's allocated (VoiceParam::Initialize, 0x022A254C): the TV's front channels at unity
    // on the main bus.
    static VoiceParam InitialVoiceParam()
    {
        VoiceParam param{};
        param.tv[0][0] = 1.0f;
        param.tv[0][1] = 1.0f;

        return param;
    }

    // Each device's output parameters combine the player's parameters, the track's parameters, and the bytecode's pan,
    // surround pan and sends.
    static Outputs CombineOutputs(const Outputs& player, const Outputs& track, float pan, float span, float main_send,
                                  const std::array<float, 3>& fx_send);

    static void SetOutputs(Channel& channel, const Outputs& outputs);

    // The track's front bypass goes to the channel's voices.
    static void SetFrontBypass(Engine& engine, Channel& channel, bool front_bypass);

    // The Wii U's channels have no receivers.
    static void SetReceiver(NoteOnInfo&, const Receiver*)
    {
    }

    // A stream's voice takes the track's front bypass each frame: any of the track's flags sets it (UpdateVoiceParams,
    // 0x022B5024). The voices keep the pan mode and curve that their allocation gives them.
    template <typename Voice>
    static void SetStreamVoiceMode(Voice& voice, uint32_t, uint32_t, bool front_bypass)
    {
        voice.SetFrontBypass(front_bypass);
    }

    // The output parameters of channel `index` of a stream's track of `count` channels (MixSettingForOutputParam,
    // 0x022B4D8C). A channel by itself takes the track's. The channels of a pair mix by the TV's mix mode on every
    // device. With the pan, the left channel's pan moves 1 to the left and the right channel's 1 to the right. With the
    // mix parameters, the right channel's voice takes the second channel's parameters as its first. A track of more
    // channels gives its voices none.
    template <typename Voice>
    static void SetStreamVoiceOutputs(Voice& voice, const Outputs& outputs, int count, int index)
    {
        if (count == 1)
        {
            voice.SetOutputs(outputs);
        }
        else if (count == 2)
        {
            Outputs pair = outputs;
            const uint32_t mode = outputs.tv.mix_mode;
            const auto mix_setting = [mode, index](OutputParam& param)
            {
                if (mode == 0)
                {
                    param.pan = index == 0 ? param.pan - 1.0f : param.pan + 1.0f;
                }
                else if (mode == 1 && index == 1)
                {
                    param.mix_parameter[0] = param.mix_parameter[1];
                    param.mix_parameter[1] = {};
                }
            };
            ForEachOutput(mix_setting, pair);
            voice.SetOutputs(pair);
        }
    }
};

using SequenceTrack = ntdlib::nwsnd::SequenceTrack<SequencePolicy>;
using SequenceSoundPlayer = ntdlib::nwsnd::SequenceSoundPlayer<SequencePolicy>;

} // namespace ntdlib::cafe::snd
