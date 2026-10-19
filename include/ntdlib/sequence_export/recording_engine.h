// SPDX-License-Identifier: MIT

// An engine for nw::snd's model (nwsnd/) whose voices play nothing. It plays a sound archive's sequence on the model,
// with the sound's banks, and a recording follows what the sequence plays through the model's hooks. A position model
// in place of a renderer determines when a one-shot wave has played out: AX's 16.16 position at 32 kHz on the Wii U and
// the Wii, the audio renderer's Q15 position at 48 kHz on the Switch, and the DSP's position at 32,728 Hz on the 3DS.
//
// The engine is a base class. The class that derives from it is the model's engine (Policy::Engine), and gives the
// hooks. The engine holds no pointers into itself. A copy of the derived class plays on apart from the original.

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <list>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/nwsnd/archive_files.h"
#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/channel_impl.h"
#include "ntdlib/nwsnd/sequence.h"
#include "ntdlib/nwsnd/sequence_impl.h"
#include "ntdlib/nwsnd/util.h"
#include "ntdlib/nwsnd/voice.h"
#include "ntdlib/nwsnd/voice_impl.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/instruments.h"

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
// gives its channels the bytecode's pan, surround pan and two sends as they are (SequenceTrack::UpdateChannelParam,
// 0x31de18).
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
    static Outputs CombineOutputs(const Outputs&, const Outputs&, float pan, float span, float main_send,
                                  const std::array<float, 3>& fx_send)
    {
        Outputs param;
        param.pan = pan;
        param.span = span;
        param.main_send = main_send;
        for (std::size_t bus = 0; bus < static_cast<std::size_t>(nwsnd::CtrPolicy::kFxSendCount); ++bus)
        {
            param.fx_send[bus] = fx_send[bus];
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

        float init_pan = static_cast<float>(static_cast<int>(region.pan) - 64) / 63.0f;
        init_pan += static_cast<float>(info.init_pan) / 63.0f;
        channel.init_pan = init_pan;
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

// The end of a voice's wave that a recording reads from the voice layer (RecordingEngine::PositionOf): whether the wave
// is a one-shot, and the frame in which it runs out at its current rate. A wave that doesn't run out at its rate has
// kNoEnd.
struct WaveEnd
{
    static constexpr uint64_t kNoEnd = std::numeric_limits<uint64_t>::max();

    bool one_shot = false;
    uint64_t frame = kNoEnd;
};

// The position model in place of the console's mixer: each one-shot wave's position. The engine reads it to find when
// the wave has played out. The mixer reads a sample each time the position passes a whole sample, from 0, and stops
// once it has read the last. IsPlayFinished gives the stop at the start of the frame after the mixer reports it.
template <typename Policy>
class PositionRenderer
{
public:
    using Mix = Mixer<typename Policy::Console>;
    using MultiVoice = nwsnd::MultiVoice<Policy>;
    using WaveInfo = typename Policy::WaveInfo;
    using VoiceParam = typename Policy::VoiceParam;

    // The frame that the engine processes.
    void SetFrame(uint64_t frame)
    {
        frame_ = frame;
    }

    int Acquire(uint32_t, typename MultiVoice::Voice&)
    {
        return next_++;
    }

    void Free(int)
    {
    }

    bool IsAvailable(int) const
    {
        return true;
    }

    void SetPriority(int, uint32_t)
    {
    }

    void SetState(int, const typename MultiVoice::Voice&)
    {
    }

    void SetParam(int renderer, const VoiceParam& param)
    {
        if (renderer < 0 || static_cast<std::size_t>(renderer) >= owners_.size())
        {
            return;
        }

        const nwsnd::VoiceId place = owners_[static_cast<std::size_t>(renderer)];
        if (place == nwsnd::kNoVoice || static_cast<std::size_t>(place) >= waves_.size())
        {
            return;
        }

        Wave& wave = waves_[static_cast<std::size_t>(place)];
        if (wave.one_shot && wave.first_voice == renderer)
        {
            wave.ChangeRate(frame_, UnitsPerFrame(param.pitch, wave.sample_rate));
        }
    }

    void AppendWave(nwsnd::VoiceId place, std::span<typename MultiVoice::Voice> voices, const WaveInfo& info)
    {
        if (waves_.size() <= static_cast<std::size_t>(place))
        {
            waves_.resize(static_cast<std::size_t>(place) + 1);
        }

        Wave& wave = waves_[static_cast<std::size_t>(place)];
        wave = Wave{};
        wave.one_shot = !info.loop;
        wave.sample_rate = info.sample_rate;
        wave.start_frame = frame_ + Mix::kPlayDelayFrames;
        wave.remaining = static_cast<uint64_t>(info.samples) << Mix::kFractionBits;

        for (const typename MultiVoice::Voice& voice : voices)
        {
            if (voice.renderer < 0)
            {
                continue;
            }

            if (owners_.size() <= static_cast<std::size_t>(voice.renderer))
            {
                owners_.resize(static_cast<std::size_t>(voice.renderer) + 1, nwsnd::kNoVoice);
            }
            owners_[static_cast<std::size_t>(voice.renderer)] = place;
            if (wave.first_voice < 0)
            {
                wave.first_voice = voice.renderer;
            }
        }
    }

    // The end of the wave that renderer voice `renderer` plays first, or nothing once another wave has taken the place
    // of the voice's MultiVoice. A renderer voice's number is never reused.
    std::optional<WaveEnd> PositionOf(int renderer) const
    {
        if (renderer < 0 || static_cast<std::size_t>(renderer) >= owners_.size())
        {
            return std::nullopt;
        }

        const nwsnd::VoiceId place = owners_[static_cast<std::size_t>(renderer)];
        if (place == nwsnd::kNoVoice || static_cast<std::size_t>(place) >= waves_.size() ||
            waves_[static_cast<std::size_t>(place)].first_voice != renderer)
        {
            return std::nullopt;
        }

        const Wave& wave = waves_[static_cast<std::size_t>(place)];
        return WaveEnd{wave.one_shot, wave.EndFrame()};
    }

    // The frame in which a one-shot wave of `samples` samples, started in frame `frame`, runs out at the pitch ratio
    // `ratio`, or WaveEnd::kNoEnd. A recording takes it as a note's end until the note's voice has its first rate.
    static uint64_t PredictEnd(uint64_t frame, uint32_t samples, float ratio, uint32_t sample_rate)
    {
        Wave wave;
        wave.one_shot = true;
        wave.start_frame = frame + Mix::kPlayDelayFrames;
        wave.per_frame = UnitsPerFrame(ratio, sample_rate);
        wave.remaining = static_cast<uint64_t>(samples) << Mix::kFractionBits;

        return wave.EndFrame();
    }

    // Whether the end of the one-shot wave of the MultiVoice at `place` shows in frame `f`, this frame: whether the
    // wave ended by the end of frame `f - 1 - Mix::kReportDelayFrames`.
    bool IsPlayFinished(nwsnd::VoiceId place) const
    {
        if (place < 0 || static_cast<std::size_t>(place) >= waves_.size() || frame_ < 1 + Mix::kReportDelayFrames)
        {
            return false;
        }

        return waves_[static_cast<std::size_t>(place)].EndedBy(frame_ - 1 - Mix::kReportDelayFrames);
    }

    float OutputVolume() const
    {
        return 1.0f;
    }

    void CalcMix(MultiVoice&) const
    {
    }

    void CalcLpf(MultiVoice&) const
    {
    }

    void CalcBiquadFilter(MultiVoice&) const
    {
    }

private:
    struct Wave
    {
        bool one_shot = false;
        uint32_t sample_rate = 32000;
        int first_voice = -1;
        uint64_t start_frame = 0;
        uint64_t per_frame = 0;
        uint64_t remaining = 0;

        // Whether the mixer has stopped the one-shot wave by the end of frame `frame`.
        bool EndedBy(uint64_t frame) const
        {
            return one_shot && frame >= start_frame && (frame + 1 - start_frame) * per_frame >= remaining;
        }

        // The first frame by whose end EndedBy holds at the current rate, or WaveEnd::kNoEnd.
        uint64_t EndFrame() const
        {
            if (per_frame == 0)
            {
                return remaining == 0 ? start_frame : WaveEnd::kNoEnd;
            }

            return start_frame + std::max<uint64_t>((remaining + per_frame - 1) / per_frame, 1) - 1;
        }

        // Plays on at `new_per_frame` from the first frame that plays a rate set in frame `frame`, after the frames
        // played at the old rate. A wave that has ended keeps its end. In frame 0 on a console without a play delay,
        // `from - 1` wraps. EndedBy then holds only for a wave of no samples. Such a wave has ended at once.
        void ChangeRate(uint64_t frame, uint64_t new_per_frame)
        {
            const uint64_t from = frame + Mix::kPlayDelayFrames;
            if (EndedBy(from - 1))
            {
                return;
            }

            if (from > start_frame)
            {
                remaining -= (from - start_frame) * per_frame;
                start_frame = from;
            }
            per_frame = new_per_frame;
        }
    };

    // The units of wave that the mixer plays in a sound frame at the voice's pitch ratio.
    static uint64_t UnitsPerFrame(float ratio, uint32_t sample_rate)
    {
        return Mix::kFrameSamples * Mix::UnitsPerSample(ratio, sample_rate);
    }

    uint64_t frame_ = 0;
    int next_ = 0;
    std::vector<nwsnd::VoiceId> owners_; // each renderer voice's MultiVoice
    std::vector<Wave> waves_;            // by MultiVoice place
};

// The voice layer of the Wii U, the Switch and the Wii: nw::snd's MultiVoices, with the position model in place of the
// mixer. A voice layer gives the engine its voices and their two steps in a frame. BeginFrame comes before the player's
// ticks. FinishFrame comes after the channels. On the Wii U and the Switch, BeginFrame takes the voices' ends, and
// FinishFrame updates the voices, then steps the random numbers once. On the Wii, FinishFrame steps the random numbers,
// then takes the voices' ends and updates the voices (Mixer::kEndsAtFrameEnd).
template <typename Policy>
class MultiVoiceLayer
{
public:
    using Mix = Mixer<typename Policy::Console>;

    explicit MultiVoiceLayer(int count) : voices_(count)
    {
    }

    nwsnd::MultiVoiceManager<Policy>& Voices()
    {
        return voices_;
    }

    PositionRenderer<Policy>& Renderer()
    {
        return renderer_;
    }

    // The key of the voice at `place` for PositionOf: the number of its MultiVoice's first renderer voice, or -1.
    int PositionKey(nwsnd::VoiceId place)
    {
        const auto voices = voices_.At(place).Voices();
        return voices.empty() ? -1 : voices[0].renderer;
    }

    std::optional<WaveEnd> PositionOf(int key) const
    {
        return renderer_.PositionOf(key);
    }

    static uint64_t PredictEnd(uint64_t frame, uint32_t samples, float ratio, uint32_t sample_rate)
    {
        return PositionRenderer<Policy>::PredictEnd(frame, samples, ratio, sample_rate);
    }

    template <typename Engine>
    void BeginFrame(Engine& engine, uint64_t frame)
    {
        renderer_.SetFrame(frame);
        if constexpr (!Mix::kEndsAtFrameEnd)
        {
            voices_.UpdateAllVoiceStatus(engine);
        }
    }

    template <typename Engine>
    void FinishFrame(Engine& engine, nwsnd::Random& random)
    {
        if constexpr (Mix::kEndsAtFrameEnd)
        {
            random.Next();
            voices_.UpdateAllVoiceStatus(engine);
            voices_.UpdateAllVoices(engine);
        }
        else
        {
            voices_.UpdateAllVoices(engine);
            random.Next();
        }
    }

private:
    nwsnd::MultiVoiceManager<Policy> voices_;
    PositionRenderer<Policy> renderer_;
};

// The voice layer of the 3DS: nw::snd's voices, with a model of the DSP's position in place of nn::snd and the DSP. The
// DSP plays 160 samples a sound frame at 32,728 Hz. nw::snd starts a voice on the DSP in its voice update at the end of
// the note's frame (Voice::UpdateState). The DSP plays it from the next frame. A rate that nw::snd sets in frame f
// plays from frame f + 1. The DSP marks a one-shot wave's last buffer done in the frame that reads its last sample.
// nw::snd reads the mark at the next frame's sync. That frame's voice update then frees the channel
// (Voice::CheckFinish). The voices keep nw::snd's list by priority. The list sets the order of those updates. A full
// pool drops the voice with the lowest priority (VoiceManager::AllocVoice, 0x187070). The model leaves out nn::snd's
// limit of 24 DSP voices, and its drops of voices that don't fit the DSP's cycles.
//
// The layer is also the voices' manager. BeginFrame is the DSP's sync. FinishFrame steps the random numbers once, then
// updates the voices: each one's end, then its rate, then each start (VoiceManager::UpdateAllVoices, 0x174540). The
// pool of voices hands out the last freed voice first. With no freed voice to hand out, the pool hands out the next
// voice down, from the last. The layer adds each voice the first time the pool hands it out. A deque holds the voices.
// A voice keeps its address while the pool grows.
template <typename Policy>
class CtrVoiceLayer
{
public:
    using Engine = typename Policy::Engine;

    // A voice of nw::snd (Voice), with the position of its one-shot wave on the DSP.
    class Voice
    {
    public:
        void SetSampleFormat(typename Policy::SampleFormat)
        {
        }

        void SetSampleRate(uint32_t sample_rate)
        {
            sample_rate_ = sample_rate;
        }

        void SetInterpolationType(uint8_t)
        {
        }

        // The wave plays from its start. A wave that doesn't loop ends after its last sample.
        void AppendWave(Engine&, const typename Policy::WaveInfo& wave)
        {
            sample_rate_ = wave.sample_rate;
            one_shot_ = !wave.loop;
            samples_ = wave.samples;
        }

        // The voice starts on the DSP in the frame's voice update (0x31c540).
        void Start()
        {
            started_ = true;
        }

        void Stop(Engine&)
        {
            playing_ = false;
            started_ = false;
        }

        void Free(Engine& engine)
        {
            engine.Voices().FreeVoice(place_);
        }

        void SetVolume(float volume)
        {
            volume_ = volume;
        }

        void SetPitch(float pitch)
        {
            pitch_ = pitch;
        }

        void SetPanMode(uint32_t)
        {
        }

        void SetPanCurve(uint32_t)
        {
        }

        void SetLpfFreq(float)
        {
        }

        void SetBiquadFilter(uint8_t, float)
        {
        }

        void SetOutputLine(uint32_t)
        {
        }

        void SetOutputs(const typename Policy::Outputs&)
        {
        }

        // A new priority moves the voice in the list.
        void SetPriority(Engine& engine, int priority)
        {
            priority_ = priority;
            engine.Voices().ChangePriority(place_);
        }

        // The volume and the pitch that the channel gave the voice this frame.
        float Volume() const
        {
            return volume_;
        }

        float Pitch() const
        {
            return pitch_;
        }

    private:
        friend class CtrVoiceLayer;

        // The samples of wave that the DSP plays in a frame. nn::snd turns the pitch into a rate with code.bin's
        // 1/32728 (0x1863b0).
        double SamplesPerFrame() const
        {
            constexpr float kInvDspRate = std::bit_cast<float>(0x3800280du);
            const float rate =
                std::max(pitch_, 0.0f) * (static_cast<float>(static_cast<int32_t>(sample_rate_)) * kInvDspRate);
            return kFrameSamples * static_cast<double>(rate);
        }

        // The wave starts in frame `frame + 1`. The DSP's position runs ahead of the sample that it reads by the rate,
        // less 1.
        void StartWave(uint64_t frame)
        {
            start_frame_ = frame + 1;
            per_frame_ = SamplesPerFrame();
            remaining_ = static_cast<double>(samples_) + per_frame_ / kFrameSamples - 1.0;
            playing_ = true;
        }

        // Whether the DSP has finished the one-shot wave by the end of frame `frame`.
        bool EndedBy(uint64_t frame) const
        {
            return one_shot_ && frame >= start_frame_ &&
                   static_cast<double>(frame + 1 - start_frame_) * per_frame_ >= remaining_;
        }

        // The first frame by whose end EndedBy holds at the current rate, or WaveEnd::kNoEnd. A wave that the DSP
        // hasn't started has no end yet. The loops settle the division's rounding against EndedBy's product.
        uint64_t EndFrame() const
        {
            if (!playing_ || !(per_frame_ > 0.0))
            {
                return playing_ && remaining_ <= 0.0 ? start_frame_ : WaveEnd::kNoEnd;
            }

            const double frames = std::ceil(remaining_ / per_frame_);
            if (!(frames < 1e15))
            {
                return WaveEnd::kNoEnd;
            }

            uint64_t played = static_cast<uint64_t>(std::max(frames, 1.0));
            while (played > 1 && static_cast<double>(played - 1) * per_frame_ >= remaining_)
            {
                played--;
            }
            while (static_cast<double>(played) * per_frame_ < remaining_)
            {
                played++;
            }

            return start_frame_ + played - 1;
        }

        // The rate that nw::snd sets in frame `frame` plays from frame `frame + 1`. The position's lead follows the
        // new rate. A wave that has ended keeps its end.
        void ChangeRate(uint64_t frame, double per_frame)
        {
            if (EndedBy(frame))
            {
                return;
            }

            if (frame >= start_frame_)
            {
                remaining_ -= static_cast<double>(frame + 1 - start_frame_) * per_frame_;
                start_frame_ = frame + 1;
            }
            remaining_ += (per_frame - per_frame_) / kFrameSamples;
            per_frame_ = per_frame;
        }

        static constexpr double kFrameSamples = 160.0;

        nwsnd::VoiceId place_ = nwsnd::kNoVoice;
        nwsnd::ChannelId owner_ = nwsnd::kNoChannel;
        int key_ = -1;
        int priority_ = 0;
        bool active_ = false;
        bool started_ = false;
        bool playing_ = false;
        bool one_shot_ = false;
        uint32_t sample_rate_ = 32000;
        uint32_t samples_ = 0;
        float volume_ = 1.0f;
        float pitch_ = 1.0f;
        uint64_t start_frame_ = 0;
        double per_frame_ = 0.0;
        double remaining_ = 0.0;
    };

    explicit CtrVoiceLayer(int count) : count_(count)
    {
    }

    // The layer is the voices' manager.
    CtrVoiceLayer& Voices()
    {
        return *this;
    }

    // The voice that the pool holds at `place`. The pool adds the places from the last down.
    Voice& At(nwsnd::VoiceId place)
    {
        return pool_[static_cast<std::size_t>(count_ - 1 - place)];
    }

    const Voice& At(nwsnd::VoiceId place) const
    {
        return pool_[static_cast<std::size_t>(count_ - 1 - place)];
    }

    // A voice for the channel at `owner`, after every voice of lower or equal priority in the list. With every voice in
    // use, the voice with the lowest priority makes room. It doesn't if it outranks the new one (0x187070).
    nwsnd::VoiceId AllocVoice(Engine& engine, int, uint32_t priority, nwsnd::ChannelId owner)
    {
        if (list_.size() >= static_cast<std::size_t>(count_) &&
            !DropLowestPriorityVoice(engine, static_cast<int>(priority)))
        {
            return nwsnd::kNoVoice;
        }

        // With no freed place, the pool adds the next place down.
        if (free_.empty())
        {
            free_.push_back(count_ - 1 - static_cast<nwsnd::VoiceId>(pool_.size()));
            pool_.emplace_back();
        }

        const nwsnd::VoiceId place = free_.back();
        free_.pop_back();
        Voice& voice = At(place);
        voice = Voice{};
        voice.place_ = place;
        voice.owner_ = owner;
        voice.key_ = next_key_++;
        voice.active_ = true;
        voice.priority_ = static_cast<int>(priority & 0xff);
        Insert(place);

        return place;
    }

    // 0x18b928.
    void FreeVoice(nwsnd::VoiceId place)
    {
        if (place == nwsnd::kNoVoice || !At(place).active_)
        {
            return;
        }

        list_.remove(place);
        free_.push_back(place);
        At(place).active_ = false;
    }

    void ChangePriority(nwsnd::VoiceId place)
    {
        list_.remove(place);
        Insert(place);
    }

    // The key of the voice at `place` for PositionOf: the number of its allocation, never reused, or -1.
    int PositionKey(nwsnd::VoiceId place) const
    {
        if (place == nwsnd::kNoVoice || !At(place).active_)
        {
            return -1;
        }

        return At(place).key_;
    }

    // The frame in which a one-shot wave of `samples` samples, started in frame `frame`, runs out at the pitch ratio
    // `ratio`, or WaveEnd::kNoEnd. The DSP starts it in the frame after the note's frame (StartWave).
    static uint64_t PredictEnd(uint64_t frame, uint32_t samples, float ratio, uint32_t sample_rate)
    {
        Voice voice;
        voice.one_shot_ = true;
        voice.sample_rate_ = sample_rate;
        voice.samples_ = samples;
        voice.pitch_ = ratio;
        voice.StartWave(frame);

        return voice.EndFrame();
    }

    // The end of the wave of the voice with key `key`, or nothing once that voice is free.
    std::optional<WaveEnd> PositionOf(int key) const
    {
        for (const nwsnd::VoiceId place : list_)
        {
            const Voice& voice = At(place);
            if (voice.key_ == key)
            {
                return WaveEnd{voice.one_shot_, voice.EndFrame()};
            }
        }

        return std::nullopt;
    }

    void BeginFrame(Engine&, uint64_t frame)
    {
        frame_ = frame;
    }

    void FinishFrame(Engine& engine, nwsnd::Random& random)
    {
        random.Next();

        // A one-shot wave that ended in the frame before calls its channel back.
        std::vector<nwsnd::VoiceId> voices(list_.begin(), list_.end());
        for (const nwsnd::VoiceId place : voices)
        {
            Voice& voice = At(place);
            if (voice.active_ && voice.playing_ && frame_ > 0 && voice.EndedBy(frame_ - 1))
            {
                engine.Channels().At(voice.owner_).OnVoiceEvent(engine, nwsnd::VoiceCallbackStatus::kFinishWave);
                voice.playing_ = false;
                voice.started_ = false;
            }
        }

        // The voices that play take their new rates. The voices that have started begin to play.
        voices.assign(list_.begin(), list_.end());
        for (const nwsnd::VoiceId place : voices)
        {
            Voice& voice = At(place);
            const double per_frame = voice.SamplesPerFrame();
            if (voice.playing_ && per_frame != voice.per_frame_)
            {
                voice.ChangeRate(frame_, per_frame);
            }
        }

        for (const nwsnd::VoiceId place : voices)
        {
            Voice& voice = At(place);
            if (voice.active_ && voice.started_ && !voice.playing_)
            {
                voice.StartWave(frame_);
            }
        }
    }

private:
    // Puts the voice after every voice of lower or equal priority.
    void Insert(nwsnd::VoiceId place)
    {
        const int priority = At(place).priority_;
        const auto outranks = [this, priority](nwsnd::VoiceId other)
        {
            return At(other).priority_ > priority;
        };
        const auto after = std::find_if(list_.begin(), list_.end(), outranks);
        list_.insert(after, place);
    }

    // Drops the first voice of the list if its priority isn't above `priority` (0x18dd54). Returns whether it did.
    bool DropLowestPriorityVoice(Engine& engine, int priority)
    {
        if (list_.empty() || At(list_.front()).priority_ > priority)
        {
            return false;
        }

        const nwsnd::VoiceId victim = list_.front();
        Voice& voice = At(victim);
        const nwsnd::ChannelId owner = voice.owner_;
        voice.Stop(engine);
        FreeVoice(victim);
        engine.Channels().At(owner).OnVoiceEvent(engine, nwsnd::VoiceCallbackStatus::kDropVoice);

        return true;
    }

    int count_;                        // the voices that the pool can hand out
    std::deque<Voice> pool_;           // the voices that the pool has handed out, the last place first
    std::vector<nwsnd::VoiceId> free_; // the freed places, the next to hand out last
    std::list<nwsnd::VoiceId> list_;   // by priority, the lowest first
    int next_key_ = 0;                 // the next allocation's key (PositionKey)
    uint64_t frame_ = 0;
};

// The engine. `Derived` derives from it and gives the hooks (see nwsnd/sequence.h). `Banks` gives each note's velocity
// region and wave: BankSet, or another type with BankSet's FindRegion and GetWave. `VoiceLayer` gives the console's
// voices and their steps in a frame (see MultiVoiceLayer). `count` channels and voices are enough that none is ever
// dropped. A model of a console's counts takes `channels` and `voices` apart.
template <typename Console, typename Derived, typename Banks = BankSet,
          template <typename> class VoiceLayer = MultiVoiceLayer>
class RecordingEngine
{
public:
    using Policy = RecordingPolicy<Console, Derived>;
    using Channel = typename Policy::Channel;
    using Player = nwsnd::SequenceSoundPlayer<Policy>;

    RecordingEngine(Banks& banks, int count) : RecordingEngine(banks, count, count)
    {
    }

    RecordingEngine(Banks& banks, int channels, int voices) : banks_(&banks), channels_(channels), layer_(voices)
    {
    }

    nwsnd::ChannelManager<Policy>& Channels()
    {
        return channels_;
    }

    const nwsnd::ChannelManager<Policy>& Channels() const
    {
        return channels_;
    }

    // The voices that the channels play on.
    auto& Voices()
    {
        return layer_.Voices();
    }

    // The position model, for a voice layer with one.
    auto& Renderer()
        requires requires(VoiceLayer<Policy>& layer) { layer.Renderer(); }
    {
        return layer_.Renderer();
    }

    // A recording pairs a note with the voice at `place` by the voice's key. The key is never reused. PositionOf gives
    // the end of that voice's wave, or nothing once the voice is gone.
    int PositionKey(nwsnd::VoiceId place)
    {
        return layer_.PositionKey(place);
    }

    std::optional<WaveEnd> PositionOf(int key) const
    {
        return layer_.PositionOf(key);
    }

    // The frame in which a one-shot wave runs out when its note starts in frame `frame` at the pitch ratio `ratio`,
    // before the note's voice has a rate (the voice layer's PredictEnd).
    static uint64_t PredictEnd(uint64_t frame, uint32_t samples, float ratio, uint32_t sample_rate)
    {
        return VoiceLayer<Policy>::PredictEnd(frame, samples, ratio, sample_rate);
    }

    nwsnd::Random& Rng()
    {
        return random_;
    }

    std::array<int16_t, 16>& GlobalVariables()
    {
        return globals_;
    }

    // The player of the sequence, once Play has made it.
    Player& GetPlayer()
    {
        return *player_;
    }

    const Player& GetPlayer() const
    {
        return *player_;
    }

    // The frame being processed, counted from the frame of the call to Play.
    uint64_t Frame() const
    {
        return frame_;
    }

    // Starts sound `info` with sequence `data`. The player starts `start_frames` frames later. The default of 1 is
    // nw::snd's timing: SoundArchivePlayer starts a sound in the sound thread's next frame.
    void Play(const sar::SoundInfo& info, std::span<const uint8_t> data, int start_frames = 1)
    {
        player_.emplace(0, info, data);
        start_frames_ = start_frames;
    }

    // A frame of the sound thread (SoundThread::FrameProcess): the voice layer's first step, the sound's start when
    // it's due, the player's ticks and the tracks' parameters, the channels, then the voice layer's last step.
    void RunFrame()
    {
        BeginFrame();
        if (player_)
        {
            player_->Update(static_cast<Derived&>(*this));
        }
        FinishFrame();
    }

    // A frame in parts, for an engine that runs the player's ticks itself: BeginFrame, the ticks in steps (see
    // SequenceSoundPlayer::BeginTick), the player's UpdateTrackParams, then FinishFrame.
    void BeginFrame()
    {
        layer_.BeginFrame(static_cast<Derived&>(*this), frame_);
        if (start_frames_ >= 0 && start_frames_-- == 0)
        {
            player_->Start();
        }
    }

    void FinishFrame()
    {
        Derived& engine = static_cast<Derived&>(*this);
        channels_.UpdateAllChannel(engine);
        layer_.FinishFrame(engine, random_);
        ++frame_;
    }

    // A channel's callback: the player's track that owns the channel. A channel whose voice went to another note
    // reports the drop to the derived engine first (OnChannelDropped), if it has the hook.
    void ChannelEvent(int, int track, Channel& channel, typename Channel::CallbackStatus status)
    {
        Derived& engine = static_cast<Derived&>(*this);
        if constexpr (requires { engine.OnChannelDropped(track, channel); })
        {
            if (status == Channel::CallbackStatus::kDrop)
            {
                engine.OnChannelDropped(track, channel);
            }
        }

        if (player_)
        {
            player_->ChannelEvent(engine, track, channel);
        }
    }

    // Bank::NoteOn: the region of the note's program, key and velocity in the bank of slot `slot`, a channel with a
    // voice for each of the wave's channels (two at most), and the channel's start. Returns nullptr if no region or
    // wave plays the note, or no channel is free. A note that finds no free channel calls the derived engine's
    // OnNoChannel with its track, if it has the hook.
    Channel* NoteOn(const std::vector<uint32_t>&, int slot, const typename Policy::NoteOnInfo& info)
    {
        const sar::VelocityRegion* region = banks_->FindRegion(slot, info.prg_no, info.key, info.velocity);
        if (!region)
        {
            return nullptr;
        }
        const sar::Pcm* pcm = banks_->GetWave(slot, region->wave_id_index);
        if (!pcm)
        {
            return nullptr;
        }

        Derived& engine = static_cast<Derived&>(*this);
        const int channel_count = std::min(static_cast<int>(pcm->channels.size()), 2);
        Channel* channel =
            channels_.AllocChannel(engine, channel_count, info.priority, info.owner_player, info.owner_track);
        if (!channel)
        {
            if constexpr (requires { engine.OnNoChannel(info.owner_track); })
            {
                engine.OnNoChannel(info.owner_track);
            }

            return nullptr;
        }

        ChannelRules<Console>::SetNote(engine, *channel, info, *region);

        typename Policy::WaveInfo wave;
        wave.sample_rate = pcm->sample_rate;
        wave.loop = pcm->loop;
        wave.samples = pcm->Length();
        channel->Start(engine, wave, info.length);

        return channel;
    }

private:
    Banks* banks_;
    nwsnd::ChannelManager<Policy> channels_;
    VoiceLayer<Policy> layer_;
    nwsnd::Random random_;
    std::array<int16_t, 16> globals_{};
    std::optional<Player> player_;
    int start_frames_ = -1;
    uint64_t frame_ = 0;
};

} // namespace ntdlib::sequence_export
