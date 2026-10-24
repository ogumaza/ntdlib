// SPDX-License-Identifier: MIT

// NW4R SND's driver layer: the AX voices (AxVoice), the voices that group them (Voice), the channels that play notes on
// voices (Channel) and their managers. The model follows Wii Sports' main.dol (see util.h). The game runs these in its
// sound thread and in AX's interrupt, under interrupt locks. The model makes the same calls in the same order within a
// frame. Archive mode plays sequences on the TV only. Each channel therefore has one output (a voice out count of 1) on
// the main line. The Wii Remote mixes and filters stay off.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <vector>

#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/channel_impl.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/ax/pb.h"
#include "ntdlib/rvl/ax/voice.h"
#include "ntdlib/types.h"

namespace ntdlib::rvl::snd
{

class Engine;

// A wave as NW4R SND plays it (WaveData, from WaveFileReader::ReadWaveParam at main.dol 0x8004eca8). It includes the
// addresses of the samples in main memory.
struct WaveData
{
    struct Channel
    {
        ntdlib::PAddr data = 0;
        ntdlib::rsar::DspAdpcmInfo adpcm;
    };

    ntdlib::rsar::WaveEncoding format = ntdlib::rsar::WaveEncoding::kPcm16;
    bool loop = false;
    int channel_count = 1;
    uint32_t sample_rate = 32000;
    uint32_t loop_start = 0; // in samples
    uint32_t loop_end = 0;   // in samples, past the last one played
    std::array<Channel, 2> channels;
};

// AX's sample format for a wave's encoding.
ax::SampleFormat ToAxFormat(ntdlib::rsar::WaveEncoding format);

// How the channels of a stereo wave pan (PanMode). In dual mode, the pan moves both channels, each from its side. In
// balance mode, each channel stays at its side.
enum class PanMode : uint8_t
{
    kDual = 0,
    kBalance = 1,
};

// One AX voice playing one channel of a wave (AxVoice, with its AxVoiceParamBlock).
class AxVoice
{
public:
    // The manager calls it from FreeAllReservedAxVoice after AX drops the voice: at the start of the next sound frame,
    // or earlier when the manager next acquires an AX voice.
    using Callback = std::function<void(AxVoice* voice)>;

    // The sample rate converter's settings (AxVoice::SrcType). With 4-tap, the polyphase coefficients are chosen
    // automatically for the pitch.
    enum class SrcType
    {
        kNone,
        kLinear,
        k4Tap8k,
        k4Tap12k,
        k4Tap16k,
        k4TapAuto,
    };

    // The mix volumes of AxVoice::MixParam: L, R and S for the main bus and each aux bus.
    struct MixParam
    {
        bool operator==(const MixParam&) const = default;

        uint16_t l = 0;
        uint16_t r = 0;
        uint16_t s = 0;
        uint16_t aux_a_l = 0;
        uint16_t aux_a_r = 0;
        uint16_t aux_a_s = 0;
        uint16_t aux_b_l = 0;
        uint16_t aux_b_r = 0;
        uint16_t aux_b_s = 0;
        uint16_t aux_c_l = 0;
        uint16_t aux_c_r = 0;
        uint16_t aux_c_s = 0;
    };

    explicit AxVoice(Engine& engine) : engine_(&engine)
    {
    }

    // Setup (0x8003265c): sets the wave whose samples this voice plays.
    void Setup(ntdlib::PAddr wave, ax::SampleFormat format, int sample_rate);

    // IsPlayFinished (0x800326dc): whether the voice has played to its end. At the end, the microcode leaves it in the
    // zero buffer.
    bool IsPlayFinished() const;

    // SetAddr (0x800332a0): sets where to loop and end, in samples from `wave`. The voice starts at the wave's start.
    // The game's SetAddr also takes a start part way into the wave. A sequence's notes never use one.
    void SetAddr(bool loop, ntdlib::PAddr wave, uint32_t loop_start, uint32_t loop_end);

    // SetSrcType (0x80033654).
    void SetSrcType(SrcType type, float pitch);

    // SetAdpcm (0x80033734) and SetAdpcmLoop (0x80033884). SetAdpcm starts the decoder at the wave's start, with the
    // info's predictor and history. The game's SetAdpcm takes them apart, for a start part way into the wave.
    void SetAdpcm(const ntdlib::rsar::DspAdpcmInfo& info);
    void SetAdpcmLoop(const ntdlib::rsar::DspAdpcmInfo& info);

    // SetMix (0x8003395c): ramps the mix from where the last frame left it towards `mix` over the next frame. Returns
    // whether the mix changed. If it did, the voice must ramp again in the next frame.
    bool SetMix(const MixParam& mix);

    // SetSrc (0x80033fd0): sets the pitch as a ratio to the wave's sample rate.
    void SetSrc(float ratio, bool initial);

    // SetVe (0x80034178): sets the volume to reach by the end of the next frame. Also sets the starting volume if the
    // voice hasn't started.
    void SetVe(float volume, float init_volume);

    // AXSetVoiceVe of October 2006 (0x80031404 in Smooth Moves' main.dol): sets the envelope's volume and its step per
    // sample. The build of October 2006 sets the envelope this way, in place of SetVe and Sync.
    void SetVeOctober2006(uint16_t volume, int16_t delta);

    // SetLpf (0x80034254): sets the low-pass filter's cutoff in Hz. A cutoff of 16 kHz or above turns the filter off.
    void SetLpf(uint16_t freq);

    // SetBiquad (Wii Music, 0x800afcbc): sets the biquad filter of `type` at `value` (see BiquadFilterCoefs). A type
    // without coefficients turns the filter off.
    void SetBiquad(int type, float value);

    void SetPriority(uint32_t priority);   // 0x8003307c
    void SetVoiceType(ax::VoiceType type); // 0x80033090
    void EnableRemote(bool enable);        // 0x800330f0

    // AxVoiceParamBlock::Sync (0x800346b0): sets the volume envelope's ramp for the next frame and passes the frame's
    // changes to AX.
    void Sync();

    void Run();  // AxVoiceParamBlock::SetVoiceStateRun
    void Stop(); // AxVoiceParamBlock::SetVoiceStateStop

    // ResetDelta (0x80033150): ends the ramps of the envelope and the mix where this frame takes them.
    void ResetDelta();

    bool IsAvailable() const
    {
        return vpb_ != nullptr;
    }

    bool IsRun() const
    {
        return IsAvailable() && vpb_->IsRunning();
    }

    // The index of the AX voice (ax::Voice::index). The voice has to be available.
    uint32_t AxIndex() const
    {
        return vpb_->index;
    }

    // The voice's address in the units of its format, for `samples` from `base` (GetDspAddressBySample).
    // SampleByDspAddress gives the sample at `address`. That address counts in the same units from the wave's start
    // (GetSampleByDspAddress, 0x80032fb4).
    static uint32_t DspAddressBySample(ntdlib::PAddr base, uint32_t samples, ax::SampleFormat format);
    static uint32_t SampleByDspAddress(uint32_t address, ax::SampleFormat format);

private:
    friend class AxVoiceManager;

    // AxVoiceParamBlock's setters that the voice uses.
    void SetVoiceMix(const ax::PbMix& mix, bool sync_now);
    void UpdateDelta();

    Engine* engine_;

    // AxVoiceParamBlock (AxVoice + 0x0).
    ax::Voice* vpb_ = nullptr;              // + 0x0
    uint32_t sync_ = 0;                     // + 0x4: changes to pass to AX at the next Sync
    uint16_t prev_volume_ = ax::kMaxVolume; // + 0x8: the volume envelope at the start of the next frame
    int16_t prev_delta_ = 0;                // + 0xa
    bool first_ve_update_ = false;          // + 0xc
    uint16_t volume_ = ax::kMaxVolume;      // + 0xe: the volume to reach

    ntdlib::PAddr wave_data_ = 0;                        // + 0x10: 0 for none
    ax::SampleFormat format_ = ax::SampleFormat::kPcm16; // + 0x14
    int sample_rate_ = 0;                                // + 0x18
    bool first_mix_update_ = false;                      // + 0x1c
    bool reserve_for_free_ = false;                      // + 0x1d
    MixParam mix_prev_;                                  // + 0x1e
    Callback callback_;                                  // + 0x38
};

// The pool of AxVoices (AxVoiceManager): 96 for AX's voices and 16 more. The extra ones let a voice that AX has dropped
// wait to be freed while its AX voice is reused.
class AxVoiceManager
{
public:
    // The pool's state, for a snapshot of the engine: a copy of each AxVoice, and the lists that hold them.
    struct Snapshot
    {
        std::vector<AxVoice> voices;
        std::list<AxVoice*> free;
        std::list<AxVoice*> active;
        std::list<AxVoice*> reserved;
    };

    static constexpr int kVoiceCount = ax::kVoiceCount + 16;

    explicit AxVoiceManager(Engine& engine);

    // The pool's state between sound frames, and its restoration. A snapshot can only be restored to the pool it came
    // from. Its lists point at that pool's AxVoices.
    Snapshot Save() const;
    void Restore(const Snapshot& snapshot);

    // AcquireAxVoice (0x800353d0): returns an AxVoice with an AX voice at `priority`, or nullptr.
    AxVoice* AcquireAxVoice(uint32_t priority, AxVoice::Callback callback);

    // FreeAxVoice (0x800356a0).
    void FreeAxVoice(AxVoice* voice);

    // ReserveForFreeAxVoice (0x8003576c): AX dropped the voice. FreeAllReservedAxVoice calls the owner's callback later
    // (see AxVoice::Callback).
    void ReserveForFreeAxVoice(AxVoice* voice);

    // FreeAllReservedAxVoice (0x80035800), the first step of the sound frame.
    void FreeAllReservedAxVoice();

private:
    AxVoice* Alloc();
    void Free(AxVoice* voice);

    Engine& engine_;
    std::array<std::unique_ptr<AxVoice>, kVoiceCount> pool_;
    std::list<AxVoice*> free_;
    std::list<AxVoice*> active_;
    std::list<AxVoice*> reserved_;
};

// A voice's place among the engine's voices (VoiceManager), and a channel's among the engine's channels
// (ChannelManager). kNoVoice and kNoChannel mark none.
using ntdlib::nwsnd::ChannelId;
using ntdlib::nwsnd::kNoChannel;
using ntdlib::nwsnd::kNoVoice;
using ntdlib::nwsnd::VoiceId;

// A voice (Voice): the AX voices that play one note, one for each channel of its wave.
class Voice
{
public:
    // Why the voice calls its channel back: kFinishWave when its wave has played to its end, kDropVoice when NW4R SND's
    // voice manager drops it, kDropDsp when AX does.
    using Status = ntdlib::nwsnd::VoiceCallbackStatus;

    // The voice's changes still to make to its AX voices (Voice::SyncFlag).
    static constexpr uint16_t kSyncSrcInitial = 1u << 0;
    static constexpr uint16_t kSyncSrc = 1u << 2;
    static constexpr uint16_t kSyncVe = 1u << 3;
    static constexpr uint16_t kSyncMix = 1u << 4;
    static constexpr uint16_t kSyncLpf = 1u << 5;
    static constexpr uint16_t kSyncBiquad = 1u << 8; // builds from June 2008

    static constexpr int kPriorityMax = 255;

    // The priority of a voice in its release. A channel gives its voice this priority when the note releases (ntdlib's
    // channel template).
    static constexpr int kPriorityRelease = 1;

    Voice(Engine& engine, VoiceId place) : engine_(&engine), place_(place)
    {
    }

    // Acquire (0x8004c14c): takes an AX voice for each of `channels`. Drops lower-priority voices to make room. The
    // voice reports its events to the channel at `owner`. The game passes a callback and its data instead.
    bool Acquire(int channels, int priority, ChannelId owner);

    // Free (0x8004c388).
    void Free();

    // StopFinished (0x8004bb24), Calc (0x8004bbc0) and Update (0x8004be10): the voice manager's three passes.
    void StopFinished();
    void Calc();
    void Update();

    // Setup (0x8004c454): sets the wave to play from its start. The game's Setup also takes a start part way into the
    // wave. ntdlib's channel always starts at the wave's start.
    void Setup(const WaveData& data);

    void Start(); // 0x8004c704
    void Stop();  // 0x8004c724

    // The calls of ntdlib's channel. Setup sets the wave's format, rate and loop. AppendWave plays the wave from its
    // start. The interpolation is AX's.
    void SetSampleFormat(ntdlib::rsar::WaveEncoding)
    {
    }

    void SetSampleRate(uint32_t)
    {
    }

    void SetInterpolationType(uint8_t)
    {
    }

    void AppendWave(Engine&, const WaveData& wave)
    {
        Setup(wave);
    }

    void Stop(Engine&)
    {
        Stop();
    }

    void Free(Engine&)
    {
        Free();
    }

    void SetPriority(Engine&, uint32_t priority)
    {
        SetPriority(static_cast<int>(priority));
    }

    void SetVolume(float volume);                // 0x8004c830
    void SetVeVolume(float target, float init);  // 0x8004c874
    void SetPitch(float pitch);                  // 0x8004c918
    void SetPanMode(PanMode mode);               // 0x8004c938
    void SetPanCurve(uint8_t curve);             // 0x8004c958
    void SetPan(float pan);                      // 0x8004c978
    void SetSurroundPan(float pan);              // 0x8004c998
    void SetLpfFreq(float freq);                 // 0x8004c9b8
    void SetBiquadFilter(int type, float value); // Wii Music, 0x800cc028
    void SetMainOutVolume(float volume);         // 0x8004ca54
    void SetMainSend(float send);                // 0x8004ca98
    void SetFxSend(int bus, float send);         // 0x8004cae0

    // The values of the channel's frame, set through the calls above in the order of Channel::Update (0x800378c8).
    void SetParam(const ntdlib::nwsnd::RevolutionPolicy::ChannelVoiceParam& param);

    // SetPriority (0x8004cc14): re-sorts the voice. A releasing voice (priority 1) gets a lower AX priority.
    void SetPriority(int priority);

    // UpdateVoicesPriority (0x8004ccb4).
    void UpdateVoicesPriority();

    int Priority() const
    {
        return priority_;
    }

    int AxVoiceCount() const
    {
        return channel_count_;
    }

    // The indices of the voice's AX voices (ax::Voice::index), in the order of its wave's channels. A channel without
    // an AX voice is left out.
    std::vector<uint32_t> AxVoiceIndices() const
    {
        std::vector<uint32_t> indices;
        for (int i = 0; i < channel_count_; i++)
        {
            if (ax_voices_[i] != nullptr && ax_voices_[i]->IsAvailable())
            {
                indices.push_back(ax_voices_[i]->AxIndex());
            }
        }

        return indices;
    }

    VoiceId Place() const
    {
        return place_;
    }

private:
    friend class VoiceManager;

    // Calls `step` on each of the voice's AX voices, in the order of the wave's channels. A channel without an AX voice
    // is skipped. The game's functions loop over the channels in the same way.
    template <typename Step>
    void ForEachAxVoice(Step step)
    {
        for (int i = 0; i < channel_count_; i++)
        {
            if (ax_voices_[i] != nullptr)
            {
                step(*ax_voices_[i]);
            }
        }
    }

    bool IsPlayFinished() const;
    void CalcAxSrc(bool initial);

    // CalcAxVe returns true in the build of October 2006 while the envelope ramps. The voice then sets it again in the
    // next frame. The ramp then lands on its target.
    bool CalcAxVe();

    bool CalcAxMix();
    void CalcAxLpf();
    void CalcAxBiquadFilter();
    void CalcMixParam(int channel, AxVoice::MixParam& mix) const;
    void SyncAxVoice();
    void RunAllAxVoice();
    void StopAllAxVoice();
    void ResetDelta();

    // AxVoiceCallbackFunc (0x8004d2a0): called when AX has dropped one of the voice's AX voices.
    void OnAxVoiceDropped(AxVoice* dropped);

    Engine* engine_;
    VoiceId place_;
    std::array<AxVoice*, 2> ax_voices_{};           // + 0xc, one for each channel
    int channel_count_ = 0;                         // + 0x9c
    ChannelId owner_ = kNoChannel;                  // + 0xa4, the callback
    bool active_ = false;                           // + 0xac
    bool starting_ = false;                         // + 0xad
    bool started_ = false;                          // + 0xae
    uint16_t sync_flag_ = 0;                        // + 0xb1
    int priority_ = 0;                              // + 0xb4
    float pan_ = 0.0f;                              // + 0xb8
    float surround_pan_ = 0.0f;                     // + 0xbc
    float lpf_freq_ = 1.0f;                         // + 0xc0
    float main_out_volume_ = 1.0f;                  // + 0xc8
    float main_send_ = 1.0f;                        // + 0xcc
    std::array<float, ax::kAuxBusCount> fx_send_{}; // + 0xd0
    float pitch_ = 1.0f;                            // + 0x10c
    float volume_ = 1.0f;                           // + 0x110
    float ve_init_volume_ = 0.0f;                   // + 0x114
    float ve_target_volume_ = 1.0f;                 // + 0x118
    PanMode pan_mode_ = PanMode::kDual;             // + 0x11c
    uint8_t pan_curve_ = 0;                         // + 0x120
    uint8_t biquad_type_ = 0;                       // builds from June 2008
    float biquad_value_ = 0.0f;                     // builds from June 2008
    bool first_ve_update_ = true;                   // the build of October 2006: whether CalcAxVe hasn't run yet
    float prev_ve_volume_ = 1.0f;                   // the build of October 2006: the volume that CalcAxVe last used
};

// The voices in use, by priority (VoiceManager): 96, one for each AX voice.
class VoiceManager
{
public:
    // The pool's state, for a snapshot of the engine: a copy of each Voice, and the lists that hold them.
    struct Snapshot
    {
        std::vector<Voice> voices;
        std::list<VoiceId> free;
        std::list<VoiceId> prio_list;
    };

    static constexpr int kVoiceCount = ax::kVoiceCount;

    explicit VoiceManager(Engine& engine);

    // The pool's state between sound frames, and its restoration, as for AxVoiceManager.
    Snapshot Save() const;
    void Restore(const Snapshot& snapshot);

    // AllocVoice (0x8004e358): returns a voice for `channels` at `priority`. If all voices are in use, drops the
    // lowest-priority voice first.
    Voice* AllocVoice(int channels, int priority, ChannelId owner);

    // The same, as ntdlib's channel asks for it: the voice's place, or kNoVoice.
    VoiceId AllocVoice(Engine& engine, int channels, uint32_t priority, ChannelId owner);

    // FreeVoice (0x8004e514).
    void FreeVoice(Voice* voice);

    // UpdateAllVoices (0x8004e5ac), the last step of the sound frame.
    void UpdateAllVoices();

    // ChangeVoicePriority (0x8004e6c4).
    void ChangeVoicePriority(Voice* voice);

    // NotifyVoiceUpdate (0x8004e65c): AX's frame callback. It ends each voice's ramps where the frame took them. The
    // sound frame may not set new ramps.
    void NotifyVoiceUpdate();

    // The voice at `place`.
    Voice& At(VoiceId place)
    {
        return *pool_[place];
    }

    const Voice& At(VoiceId place) const
    {
        return *pool_[place];
    }

    // The voices in use, lowest priority first and oldest first among equals.
    const std::list<VoiceId>& PrioVoices() const
    {
        return prio_list_;
    }

private:
    friend class Voice;

    // DropLowestPriorityVoice (inlined in AllocVoice): when no voice is free, drops the lowest-priority voice unless it
    // outranks `priority`. Returns the number of AX voices that the dropped voice had, or 0 if no voice was dropped.
    int DropLowestPriorityVoice(int priority);

    void AppendVoiceList(Voice* voice);
    void RemoveVoiceList(Voice* voice);
    void UpdateEachVoicePriority(std::list<VoiceId>::iterator begin);

    std::array<std::unique_ptr<Voice>, kVoiceCount> pool_;
    std::list<VoiceId> free_;
    std::list<VoiceId> prio_list_;
};

// The Wii's policy of ntdlib's channels: NW4R SND's channel on the model's voices, with RevolutionPolicy's frame
// (UpdateChannel). The track sets the channel's pan, surround pan and sends in its one output, the TV's stereo mix.
struct ChannelPolicy : ntdlib::nwsnd::RevolutionPolicy
{
    using Console = ntdlib::nwsnd::RevolutionPolicy;
    using Engine = ntdlib::rvl::snd::Engine;
    using WaveInfo = WaveData;

    // The channel's output: the main out volume, the pan, the surround pan and the sends.
    struct Outputs
    {
        float volume = 1.0f;
        uint32_t mix_mode = 0;
        float pan = 0.0f;
        float span = 0.0f;
        float main_send = 0.0f;
        std::array<float, kFxSendCount> fx_send{};
    };

    template <typename Function, typename... Each>
    static void ForEachOutput(const Function& function, Each&... outputs)
    {
        function(outputs...);
    }
};

// A channel (Channel): one note of a wave, with its envelope, LFO and sweep, played on a voice.
using Channel = ntdlib::nwsnd::Channel<ChannelPolicy>;

// The pool of channels and the channels in use (ChannelManager): 97, one for each AX voice and one more
// (RevolutionPolicy::kChannelCount). A channel that stops goes back to the pool at once.
using ChannelManager = ntdlib::nwsnd::ChannelManager<ChannelPolicy>;

} // namespace ntdlib::rvl::snd
