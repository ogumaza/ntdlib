// SPDX-License-Identifier: MIT

// The rules of a recording engine (recording_engine.h) on each console: how nw::snd sets a channel up for a note and
// gives it the track's outputs (ChannelRules), the policy that the engine derives from the console's (RecordingPolicy),
// and the console's mix of a voice's outputs (Mixer).

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "ntdlib/nwsnd/archive_files.h"
#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/channel_impl.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/sequence.h"
#include "ntdlib/nwsnd/sequence_impl.h"
#include "ntdlib/nwsnd/util.h"
#include "ntdlib/nwsnd/voice.h"
#include "ntdlib/nwsnd/voice_impl.h"
#include "ntdlib/sar/formats.h"

namespace ntdlib::sequence_export
{

// How nw::snd sets a channel up for a note (Bank::NoteOn), and the outputs that a track gives its channels
// (SequenceTrack::UpdateChannelParam). On the Wii U and the Switch, a channel keeps the velocity and the region's
// volume apart. Each is multiplied by 1/127. SetNote sets the channel up as the models of those consoles' engines do
// (nwsnd::SetUpNoteChannel). A channel's outputs add the bytecode's values to the outputs of the player and of the
// track. A console whose library differs specializes ChannelRules on its policy. The specialization takes what it
// shares from here.
struct NwSndChannelRules
{
    template <typename Engine, typename Channel, typename NoteOnInfo>
    static void SetNote(Engine&, Channel& channel, const NoteOnInfo& info, const sar::VelocityRegion& region)
    {
        nwsnd::SetUpNoteChannel<typename Engine::Policy>(channel, region, info);
    }

    template <typename Outputs>
    static Outputs CombineOutputs(const Outputs& player, const Outputs& track, float pan, float span, float main_send,
                                  const std::array<float, 3>& fx_send)
    {
        Outputs param = player;
        param.volume = player.volume * track.volume;
        for (std::size_t channel = 0; channel < 2; ++channel)
        {
            for (std::size_t i = 0; i < 6; ++i)
            {
                param.mix_parameter[channel][i] = player.mix_parameter[channel][i] * track.mix_parameter[channel][i];
            }
        }

        param.pan = (player.pan + pan) + track.pan;
        param.span = (player.span + span) + track.span;
        param.main_send = (player.main_send + main_send) + track.main_send;

        for (std::size_t bus = 0; bus < 3; ++bus)
        {
            param.fx_send[bus] = (player.fx_send[bus] + fx_send[bus]) + track.fx_send[bus];
        }

        return param;
    }
};

// A console's rules for a channel's setup and outputs: the rules of nw::snd, or those of a specialization for the
// console.
template <typename Console>
struct ChannelRules : NwSndChannelRules
{
};

// The 3DS keeps a note's whole volume in velocity_volume: the velocity squared times the region's volume, times
// code.bin's 1/127^3 (Bank::NoteOn, 0x48f954). Its init_volume stays 1. The pan multiplies by code.bin's 1/63. A track
// gives its channels the bytecode's pan and surround pan as they are, and the bytecode's two sends plus the player's
// (SequenceTrack::UpdateChannelParam, 0x31de18; the sends at 0x31e03c to 0x31e068).
template <>
struct ChannelRules<nwsnd::CtrPolicy> : NwSndChannelRules
{
    template <typename Engine, typename Channel, typename NoteOnInfo>
    static void SetNote(Engine& engine, Channel& channel, const NoteOnInfo& info, const sar::VelocityRegion& region)
    {
        NwSndChannelRules::SetNote(engine, channel, info, region);
        channel.velocity_volume = static_cast<float>(info.velocity * info.velocity * static_cast<int>(region.volume)) *
                                  nwsnd::CtrPolicy::kOneOver127Cubed;
        channel.init_volume = 1.0f;
        channel.init_pan =
            static_cast<float>(static_cast<int>(region.pan) + info.init_pan - 64) * nwsnd::CtrPolicy::kOneOver63;
    }

    template <typename Outputs>
    static Outputs CombineOutputs(const Outputs& player, const Outputs&, float pan, float span, float main_send,
                                  const std::array<float, 3>& fx_send)
    {
        Outputs param;
        param.pan = pan;
        param.span = span;
        param.main_send = main_send;
        for (std::size_t bus = 0; bus < static_cast<std::size_t>(nwsnd::CtrPolicy::kFxSendCount); ++bus)
        {
            param.fx_send[bus] = player.fx_send[bus] + fx_send[bus];
        }

        return param;
    }
};

// The Wii keeps a note's whole volume in velocity_volume: the velocity over 127, squared, times the region's volume
// over 127. Its init_volume stays 1. The pan adds the region's pan and the note's initial pan, each over 63, and the
// surround pan starts at 0. Builds from 2008 set the region's hold, note-off type and key group too (Mario Kart Wii's
// Bank::NoteOn, 0x8008d41c and 0x8008d490). Earlier builds keep a new channel's defaults. The engine supplies Build().
// A track combines its outputs as nw::snd does. The player's and the track's outputs add nothing to the bytecode's
// values.
template <>
struct ChannelRules<nwsnd::RevolutionPolicy> : NwSndChannelRules
{
    template <typename Engine, typename Channel, typename NoteOnInfo>
    static void SetNote(Engine& engine, Channel& channel, const NoteOnInfo& info, const sar::VelocityRegion& region)
    {
        channel.key = static_cast<uint8_t>(info.key);
        channel.original_key = region.original_key;
        channel.velocity_volume =
            nwsnd::RevolutionPolicy::VelocityVolume(info.velocity) * (static_cast<float>(region.volume) / 127.0f);
        channel.tune = region.pitch;
        channel.envelope.SetAttack(region.adshr.attack);
        channel.envelope.SetDecay(region.adshr.decay);
        channel.envelope.SetSustain(region.adshr.sustain);
        channel.envelope.SetRelease(region.adshr.release);
        if (engine.Build().SinceMarch2008())
        {
            channel.envelope.SetHold(region.adshr.hold);
            channel.ignore_note_off = region.ignore_note_off;
            channel.key_group = region.key_group;
        }

        channel.init_pan = nwsnd::RevolutionPolicy::InitialPan(region.pan, info.init_pan);
        channel.init_span = 0.0f;
    }
};

// The model's policy for a recording: the console's arithmetic, and the recording engine's types. `Derived` is the
// class that derives from RecordingEngine.
template <typename Console, typename Derived>
struct RecordingPolicy : Console
{
    using Engine = Derived;
    using Channel = nwsnd::Channel<RecordingPolicy>;
    using SoundInfo = sar::SoundInfo;
    using Receiver = void;

    enum class SampleFormat : uint32_t
    {
        kPcm8 = 0,
        kPcm16 = 1,
        kDspAdpcm = 2,
    };

    // A wave: its format, rate and loop, and its length in samples.
    struct WaveInfo
    {
        SampleFormat format = SampleFormat::kPcm16;
        uint32_t sample_rate = 32000;
        bool loop = false;
        uint32_t samples = 0;
    };

    // One output device. A recording reads the tracks' settings, not the mix.
    struct Outputs
    {
        float volume = 1.0f;
        uint32_t mix_mode = 0;
        std::array<std::array<float, 6>, 2> mix_parameter{};
        float pan = 0.0f;
        float span = 0.0f;
        float main_send = 0.0f;
        std::array<float, 3> fx_send{};
    };

    struct VoiceParam
    {
        float volume = 1.0f;
        float pitch = 1.0f;
        uint8_t interpolation = 0;
    };

    struct NoteOnInfo
    {
        uint32_t prg_no = 0;
        int key = 60;
        int velocity = 127;
        int32_t length = 0;
        int init_pan = 0;
        uint32_t priority = 0;
        int owner_player = -1;
        int owner_track = -1;
    };

    template <typename Function, typename... Each>
    static void ForEachOutput(const Function& function, Each&... outputs)
    {
        function(outputs...);
    }

    static VoiceParam InitialVoiceParam()
    {
        return {};
    }

    // Each device's parameters, from the player's, the track's and the bytecode's parameters. nw::snd's SequenceTrack
    // combines them the same way.
    static Outputs CombineOutputs(const Outputs& player, const Outputs& track, float pan, float span, float main_send,
                                  const std::array<float, 3>& fx_send)
    {
        return ChannelRules<Console>::CombineOutputs(player, track, pan, span, main_send, fx_send);
    }

    static void SetOutputs(Channel& channel, const Outputs& outputs)
    {
        channel.outputs = outputs;
    }

    static void SetFrontBypass(Engine&, Channel&, bool)
    {
    }

    static void SetReceiver(NoteOnInfo&, const Receiver*)
    {
    }
};

// How a console's mixer plays a voice's wave: the samples it mixes in a sound frame, the bits of a position's fraction,
// the frames between the sound frame that sets a wave or a rate and the first frame that plays it, and the frames by
// which the mixer reports a wave's end late. UnitsPerSample gives the position's step at a pitch ratio. kEndsAtFrameEnd
// is true where the sound library takes the voices' ends at the end of a frame, after the channels and the random step.
// It's false where the library takes them at the start of the next frame.
template <typename Console>
struct Mixer;

// AX on the Wii U mixes 96 samples a sound frame at 32 kHz and keeps a voice's position in 16.16. It sets a voice up at
// the end of the frame after its note and plays it from the frame after that. A rate that nw::snd sets in frame `f`
// plays from frame `f + 2`.
template <>
struct Mixer<nwsnd::CafePolicy>
{
    static constexpr uint64_t kFrameSamples = 96;
    static constexpr int kFractionBits = 16;
    static constexpr uint64_t kPlayDelayFrames = 2;
    static constexpr uint64_t kReportDelayFrames = 0;
    static constexpr bool kEndsAtFrameEnd = false;

    // The ratio times the sample rate over 32000, in 16.16, and no more than 8.
    static uint64_t UnitsPerSample(float ratio, uint32_t sample_rate)
    {
        const float src = ratio * static_cast<float>(sample_rate) / 32000.0f;
        return static_cast<uint64_t>(std::clamp(nwsnd::CafePolicy::ToInt32(src * 65536.0f), 0, 0x80000));
    }
};

// The Switch's audio renderer mixes 240 samples a sound frame at 48 kHz (HardwareManager::Initialize, 0xc05340) and
// keeps a voice's position in Q15 (nn::audio::DecodeFromWaveBuffers). nn::atk passes its voices to the renderer after
// the frame's channel updates (LowLevelVoiceAllocator::UpdateAllVoice, 0xc27950). The renderer plays a new voice and a
// new rate in that frame's output. The renderer reports a finished wave two frames late. nn::atk frees the channel in
// the frame after the report, before its ticks (MultiVoice::UpdateVoiceStatus, 0xc24820).
template <>
struct Mixer<nwsnd::NxPolicy>
{
    static constexpr uint64_t kFrameSamples = 240;
    static constexpr int kFractionBits = 15;
    static constexpr uint64_t kPlayDelayFrames = 0;
    static constexpr uint64_t kReportDelayFrames = 2;
    static constexpr bool kEndsAtFrameEnd = false;

    // The voice holds the ratio between 1/16 and 16 (LowLevelVoice::UpdateParam, 0xc270b0). The renderer takes the
    // ratio in Q15 and multiplies it by the wave's sample rate over 48000. Each conversion goes toward zero.
    static uint64_t UnitsPerSample(float ratio, uint32_t sample_rate)
    {
        const float held = ratio > 16.0f ? 16.0f : std::max(ratio, 0.0625f);
        const float q15 = static_cast<float>(nwsnd::NxPolicy::ToInt32(held * 32768.0f));
        const float step = static_cast<float>(sample_rate) / 48000.0f * q15;
        return static_cast<uint64_t>(std::max(nwsnd::NxPolicy::ToInt32(step), 0));
    }
};

// AX on the Wii mixes 96 samples a sound frame at 32 kHz and keeps a voice's position in 16.16. It plays a voice from
// the frame after its note, and a rate that NW4R SND sets in frame `f` from frame `f + 1`. AX takes a voice's position
// back from the DSP at the next frame's sync (__AXServiceVPB, Wii Sports' main.dol 0x80106814). NW4R SND takes the
// voices' ends at the end of a frame, after the channels and the random step (VoiceManager::UpdateAllVoices,
// 0x8004e5ac). It finds a wave's end in the frame after the one in which the wave ran out (Voice::StopFinished,
// 0x8004bb24).
template <>
struct Mixer<nwsnd::RevolutionPolicy>
{
    static constexpr uint64_t kFrameSamples = 96;
    static constexpr int kFractionBits = 16;
    static constexpr uint64_t kPlayDelayFrames = 1;
    static constexpr uint64_t kReportDelayFrames = 0;
    static constexpr bool kEndsAtFrameEnd = true;

    // The ratio times the sample rate over 32000, in 16.16 (AxVoice::SetSrc, 0x80033fd0).
    static uint64_t UnitsPerSample(float ratio, uint32_t sample_rate)
    {
        const float src = ratio * static_cast<float>(sample_rate) / 32000.0f;
        return nwsnd::RevolutionPolicy::ToUnsigned(65536.0f * src);
    }
};

} // namespace ntdlib::sequence_export
