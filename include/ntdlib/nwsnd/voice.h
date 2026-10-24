// SPDX-License-Identifier: MIT

// The sound library's MultiVoices: nw::snd's driver::MultiVoice and driver::MultiVoiceManager in NintendoWare for Cafe,
// and the same classes of nn::atk on the Switch. A MultiVoice plays each channel of a wave on a voice of the renderer.
// The MultiVoice keeps the parameters that its channel sets each frame, and works them out into each voice's parameters
// when they change. The manager allocates the MultiVoices by priority, and drops the lowest when none is free. The
// model is a template on a policy, like the channels (see channel.h). The console's policy selects the steps where
// nn::atk differs (see console.h). The engine's renderer holds the renderer's voices, the waves' buffers and the
// console's mix. The comment on a method that follows one of the game's routines gives the routine's address in Animal
// Crossing: amiibo Festival's executable. A step of nn::atk alone gives the address in WarioWare: Get It Together!'s
// main. A field comment gives the field's offset in the game's object.
//
// The policy supplies the types VoiceParam (what a voice plays with each frame, with the fields volume, pitch and
// interpolation), SampleFormat (with kPcm16) and Receiver (the mixes that voices output to, or void on a console
// without them), InitialVoiceParam() (a voice's parameters when it's allocated), and ForEachOutput (see channel.h). The
// receivers live outside the engine. A copy of the MultiVoices outputs to the same receivers.
//
// A MultiVoice calls back its owner: a channel (see channel.h), or a channel of a stream. The engine's
// VoiceEvent(owner, status) passes the callback of a stream's channel on to the stream's player. An engine without
// streams needn't have VoiceEvent.
//
// The engine's Renderer() supplies:
//
//     Acquire(priority, voice)                a renderer voice for a wave channel's voice, or -1. When the renderer
//                                             frees the voice or gives it to another, it sets voice.renderer to -1.
//     Free(renderer)                          frees a renderer voice
//     IsAvailable(renderer)                   whether the renderer still plays the voice
//     SetPriority(renderer, priority)
//     SetState(renderer, voice)               starts or stops a renderer voice, with the voice's state, format, rate
//                                             and coefficients
//     SetParam(renderer, param)               the voice's parameters for the frame
//     AppendWave(place, voices, wave)         queues a wave's buffers on the voices of the MultiVoice at `place`
//     IsPlayFinished(place)                   whether the last buffer of that MultiVoice has played
//     OutputVolume()                          the volume of the console's output
//     CalcMix(multi_voice), CalcLpf(multi_voice), CalcBiquadFilter(multi_voice)
//                                             the console's mix and filters for each of a MultiVoice's voices

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <list>
#include <span>

#include "ntdlib/nwsnd/channel.h"

namespace ntdlib::nwsnd
{

// A renderer voice's state (Voice::State).
enum class VoiceState : uint32_t
{
    kPlay = 0,
    kStop = 1,
};

template <typename Policy>
class MultiVoiceManager;

// What a MultiVoice calls back (the callback and its argument): the channel at `channel`, or else channel
// `stream_channel` of the stream of the player at `player` among the engine's players.
struct VoiceOwner
{
    friend bool operator==(const VoiceOwner&, const VoiceOwner&) = default;

    ChannelId channel = kNoChannel;
    int player = -1;
    int stream_channel = -1;
};

// A channel's voices (driver::MultiVoice): a renderer voice for each channel of the wave, up to two. MultiVoiceManager
// holds the MultiVoices.
template <typename Policy>
class MultiVoice
{
public:
    using Engine = typename Policy::Engine;
    using Console = typename Policy::Console;
    using Outputs = typename Policy::Outputs;
    using Receiver = typename Policy::Receiver;
    using SampleFormat = typename Policy::SampleFormat;
    using WaveInfo = typename Policy::WaveInfo;

    // A wave channel's voice (internal::Voice): the renderer's voice that it plays on, and what it passes to that
    // voice.
    struct Voice
    {
        uint32_t priority = 0;
        VoiceState state = VoiceState::kStop;
        typename Policy::VoiceParam param;
        SampleFormat format = SampleFormat::kPcm16;
        uint32_t sample_rate = 32000;
        std::array<int16_t, 16> coefs{};
        int renderer = -1;                  // the renderer's voice, or -1 when it has none
        const Receiver* receiver = nullptr; // nn::atk's +0xA8: the mix that the voice outputs to
    };

    // Start sets the voices to start at the next update (0x0229887C). Stop stops them now (0x02298B38).
    void Start();
    void Stop(Engine& engine);

    // Gives the voices back to the renderer and the MultiVoice back to its manager (0x02298E8C).
    void Free(Engine& engine);

    void SetSampleFormat(SampleFormat format); // 0x02298F38
    void SetSampleRate(uint32_t rate);         // 0x02298F70
    void SetInterpolationType(uint8_t type);   // 0x02298FA0

    // Queues each wave channel's buffers on its voice: the wave from its start to its end, and for a wave that loops,
    // the loop after that, for ever (Channel::AppendWaveBuffer, 0x022B96FC). The renderer holds the buffers.
    void AppendWave(Engine& engine, const WaveInfo& wave);

    void SetVolume(float volume);                        // 0x022AA3C8: held to 0 and up
    void SetPitch(float pitch);                          // 0x022AA3F4
    void SetPanMode(uint32_t mode);                      // 0x022AA414
    void SetPanCurve(uint32_t curve);                    // 0x022AA434
    void SetLpfFreq(float lpf);                          // 0x022AA454
    void SetBiquadFilter(int type, float value);         // 0x022AA474: the value held to 0 and 1
    void SetPriority(Engine& engine, uint32_t priority); // 0x022AA5D0
    void SetFrontBypass(bool bypass);                    // 0x022AA64C
    void SetOutputLine(uint32_t line);                   // 0x022AA6D0

    // Each output device's parameters (SetTvParam and SetDrcParam, 0x022AA844 and 0x022AA84C).
    void SetOutputs(const Outputs& outputs);

    // nn::atk's side of a stream's channel (0xc26430): 0 for a channel by itself, or 1 and 2 for the left and the right
    // channel of a pair. The console's mix places the channel by it. Allocation sets it to 0. A change takes effect
    // at the next calculation of the mix.
    void SetSide(uint32_t side)
    {
        side_ = side;
    }

    // nn::atk's mix for the voices to output to (SetOutputReceiver, 0xc25970). The MultiVoice keeps the mix from one
    // allocation to the next. Each of its voices holds the mix for the renderer.
    void SetOutputReceiver(const Receiver* receiver);

    int ChannelCount() const
    {
        return channel_count_;
    }

    uint32_t Priority() const
    {
        return priority_;
    }

    float Volume() const
    {
        return volume_;
    }

    float Pitch() const
    {
        return pitch_;
    }

    uint32_t PanMode() const
    {
        return pan_mode_;
    }

    uint32_t PanCurve() const
    {
        return pan_curve_;
    }

    float LpfFreq() const
    {
        return lpf_freq_;
    }

    uint8_t BiquadType() const
    {
        return biquad_type_;
    }

    float BiquadValue() const
    {
        return biquad_value_;
    }

    uint32_t OutputLine() const
    {
        return output_line_;
    }

    const Outputs& GetOutputs() const
    {
        return outputs_;
    }

    const Receiver* GetOutputReceiver() const
    {
        return receiver_;
    }

    uint32_t Side() const
    {
        return side_;
    }

    // The voices of the wave's channels, for the renderer's calculations.
    std::span<Voice> Voices()
    {
        return {voices_.data(), static_cast<std::size_t>(channel_count_)};
    }

private:
    friend class MultiVoiceManager<Policy>;

    // The changes that wait for the next update (+0x380): the start, and the calculations that a parameter change
    // requires.
    static constexpr uint16_t kSyncStart = 0x01;
    static constexpr uint16_t kSyncSrc = 0x04;
    static constexpr uint16_t kSyncMix = 0x08;
    static constexpr uint16_t kSyncLpf = 0x10;
    static constexpr uint16_t kSyncBiquad = 0x20;
    static constexpr uint16_t kSyncVe = 0x40;

    bool Alloc(Engine& engine, int channels, uint32_t priority, VoiceOwner owner); // 0x022AA2B0
    void InitParam(VoiceOwner owner);                                              // 0x022AA1C0

    // Each frame: first the calculations that the changes require (Calc, 0x0229D168). Then the start and the parameters
    // passed to the voices (Update, 0x0229D3AC).
    void Calc(Engine& engine);
    void Update(Engine& engine);

    // Each frame before the sound frame's other work: the end of the wave, or the renderer taking a voice away
    // (0x0229BCA0).
    void UpdateVoiceStatus(Engine& engine);
    bool IsPlayFinished(Engine& engine) const; // 0x0229BC2C

    // Calls the owner back (the callback that VoiceCallbackFunc receives).
    void CallOwner(Engine& engine, VoiceCallbackStatus status, VoiceOwner owner);

    void CalcSrc();              // 0x0229BF24
    void CalcVe(Engine& engine); // 0x0229C028

    void RunAllSdkVoice(Engine& engine);  // 0x0229D2D4
    void StopAllSdkVoice(Engine& engine); // 0x02298ACC

    void SetVoiceState(Engine& engine, Voice& voice, VoiceState state); // Voice::SetState, 0x0229897C
    void FreeVoice(Engine& engine, Voice& voice);                       // Voice::Free, 0x02298DCC

    // SetOutputParamImpl (0x022AA6F0).
    template <typename OutputParam>
    void SetOutputParam(const OutputParam& from, OutputParam& to);

    VoiceId place_ = kNoVoice; // the MultiVoice's place among the manager's MultiVoices
    std::array<Voice, 2> voices_;
    int channel_count_ = 0;
    VoiceOwner owner_;                   // what the callback calls back
    bool active_ = false;                // +0x374
    bool started_ = false;               // +0x375
    bool sdk_started_ = false;           // +0x376: the voices have been started on the renderer
    uint16_t sync_ = 0;                  // +0x380
    uint8_t biquad_type_ = 0;            // +0x382
    bool front_bypass_ = false;          // +0x384
    float volume_ = 1.0f;                // +0x388
    float pitch_ = 1.0f;                 // +0x38C
    uint32_t pan_mode_ = 0;              // +0x390
    uint32_t pan_curve_ = 0;             // +0x394
    float lpf_freq_ = 1.0f;              // +0x398
    float biquad_value_ = 0.0f;          // +0x39C
    uint32_t priority_ = 0;              // +0x3A0
    uint32_t output_line_ = 1;           // +0x3A4
    Outputs outputs_;                    // +0x3A8
    const Receiver* receiver_ = nullptr; // nn::atk's +0x280
    uint32_t side_ = 0;                  // nn::atk's +0x1c0
};

// The MultiVoices, a fixed number of them: those in use in a list from the lowest priority up, the newest last among
// equals, and the others in a queue. The queue starts with every MultiVoice, from the first up. A freed MultiVoice
// joins the end of the queue. The MultiVoice keeps its settings until the queue hands it out again. The model adds each
// MultiVoice the first time the queue hands it out. A deque holds the MultiVoices. A MultiVoice keeps its address while
// the deque grows. In the enhanced mixing mode, the number can grow: when every MultiVoice is in use, the queue takes
// the next one in place of a drop.
template <typename Policy>
class MultiVoiceManager
{
public:
    using Engine = typename Policy::Engine;
    using Console = typename Policy::Console;

    // `count` is nw::snd's voice count, the .data word at 0x101AA478 (96 unless the game changes it before the sound
    // system starts). Above `count`, `most` is the number that the queue can grow to.
    explicit MultiVoiceManager(int count, int most = 0);

    // The place of a MultiVoice for `channels` channels, or kNoVoice (AllocVoice, 0x022AAC0C). When every MultiVoice is
    // in use, the lowest one of a lower or equal priority is dropped. nn::atk drops the lowest ones that make room for
    // the channels instead (see console.h's kDropVoicesForChannels). The MultiVoice calls back `owner`, or the channel
    // at `owner`.
    VoiceId AllocVoice(Engine& engine, int channels, uint32_t priority, VoiceOwner owner);
    VoiceId AllocVoice(Engine& engine, int channels, uint32_t priority, ChannelId owner);

    // Drops the MultiVoice of the lowest priority, the oldest among equals. Nothing is dropped if that MultiVoice's
    // priority is above `priority`. Returns the dropped MultiVoice's channel count, or 0 if none was dropped
    // (0x022AAB50).
    int DropLowestPriorityVoice(Engine& engine, uint32_t priority);

    void UpdateAllVoiceStatus(Engine& engine); // 0x0229BE08
    void UpdateAllVoices(Engine& engine);      // 0x0229D528

    int ActiveCount() const
    {
        return static_cast<int>(active_.size());
    }

    // The MultiVoice at `place`.
    MultiVoice<Policy>& At(VoiceId place)
    {
        return voices_[static_cast<std::size_t>(place)];
    }

    const MultiVoice<Policy>& At(VoiceId place) const
    {
        return voices_[static_cast<std::size_t>(place)];
    }

    // The MultiVoices that the queue has tried to hand out, at the places from 0 up. A copy of the manager can put
    // them at other addresses: the deque's assignment frees the blocks that the copy doesn't fill.
    int AddedCount() const
    {
        return static_cast<int>(voices_.size());
    }

private:
    friend class MultiVoice<Policy>;

    // nn::atk's drop (part of AllocVoice, 0xc268d0): the MultiVoices of the lowest priority, the oldest first among
    // equals, until they had `channels` channels. Nothing is dropped unless each of them has a priority of at most
    // `priority` and below 255. Returns whether any was dropped.
    bool DropLowestPriorityVoices(Engine& engine, int channels, uint32_t priority);

    // Stops and frees `voice`, then calls back its owner with kDropVoice.
    void DropVoice(Engine& engine, MultiVoice<Policy>& voice);

    void AppendVoiceList(VoiceId place);     // 0x022AA4F8
    void RemoveVoiceList(VoiceId place);     // 0x02298E30
    void ChangeVoicePriority(VoiceId place); // 0x022AA584

    // Whether the queue is empty: every MultiVoice is in use.
    bool QueueEmpty() const
    {
        return unused_ >= count_ && free_.empty();
    }

    int count_;                             // the MultiVoices in the queue at the start, and those it has grown by
    int most_;                              // the number that the queue can grow to
    int unused_ = 0;                        // the first MultiVoice that the queue hasn't handed out
    std::deque<MultiVoice<Policy>> voices_; // from the first up, each MultiVoice that the queue has tried to hand out
    std::list<VoiceId> active_;
    std::deque<VoiceId> free_; // the freed MultiVoices, behind those that the queue hasn't handed out
};

} // namespace ntdlib::nwsnd
