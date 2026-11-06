// SPDX-License-Identifier: MIT

// nn::atk's aux effects (EffectAux), which run in the game's process on what the renderer sends them from a bus, and
// the I3DL2 reverb that the reference game runs on its aux bus A that way. Each function's comment gives the address of
// the routine it follows in WarioWare: Get It Together!'s main. Also nn::atk's effects that the renderer runs
// (EffectBase): the delay and the light limiter of the version of nn::atk in WarioWare: Move It!'s main, and the reverb
// of the version in Clubhouse Games: 51 Worldwide Classics' main. Their comments give the addresses in those mains.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>

#include "ntdlib/nx/atk/util.h"
#include "ntdlib/nx/audio/i3dl2.h"
#include "ntdlib/nx/audio/renderer.h"

namespace ntdlib::nx::atk
{

// An effect on an aux bus (nn::atk::EffectAux, whose constructor is 0xc2b0d0). Each frame, the sound thread reads what
// the renderer has sent from the bus since the last frame, gives it to UpdateBuffer and writes it back. What the sound
// thread writes back comes out of the bus when the renderer comes round the ring again, 4 frames after it went in
// (0xc2b580).
class EffectAux
{
public:
    // The ring holds 4 frames of the bus's first 4 channels (+0x28 and +0x2c). A game can change both counts.
    static constexpr int kFrameCount = 4;
    static constexpr int kChannelCount = 4;

    // What UpdateBuffer takes (EffectAux::BufferUpdateInfo).
    struct BufferInfo
    {
        int32_t sample_count_per_frame = 0; // a channel's samples in a frame
        int32_t sample_rate = 0;
        int32_t frame_count = 0; // the most frames the samples hold
        int32_t channel_count = 0;
        int32_t read_sample_count = 0; // whole frames of every channel, channel after channel within each
        OutputMode output_mode = OutputMode::kStereo;
    };

    virtual ~EffectAux() = default;

    // Processes the samples that were read, in place (EffectAux's pure virtual function).
    virtual void UpdateBuffer(std::span<int32_t> samples, const BufferInfo& info) = 0;

    // A copy of the effect with its state, for a snapshot of the model.
    virtual std::unique_ptr<EffectAux> Clone() const = 0;
};

// The I3DL2 reverb that the reference game runs on an aux bus (its class at 0x8ac6b0, whose UpdateBuffer is 0x8ac8d0):
// Nintendo's reverb (audio::AuxI3dl2Reverb) at 48 kHz on the bus's front left and right, a frame at a time, with the
// parameters a scene gives it. The rear channels come back as they went in.
class I3dl2AuxEffect : public EffectAux
{
public:
    // The game's setters (0x8ac990 on) store each parameter as it is, but hold the high frequencies' decay ratio to 0.1
    // to 2 (0x8aca70).
    explicit I3dl2AuxEffect(const audio::I3dl2ReverbParameterSet& parameters);

    void UpdateBuffer(std::span<int32_t> samples, const BufferInfo& info) override;
    std::unique_ptr<EffectAux> Clone() const override;

private:
    audio::AuxI3dl2Reverb reverb_;
};

// An effect that the renderer runs on a bus (nn::atk::EffectBase). The game sets it up, then appends it to a bus. The
// append adds it to the renderer with its settings and gives it the bus's buffers. A setter called once the effect is
// added passes its value on to the renderer. Each effect holds the renderer it was added to, and its index there.
class EffectBase
{
public:
    // The channels that the effect takes from a bus: 1, 2, 4 or 6 (nn::atk::EffectBase::ChannelMode).
    enum class ChannelMode : uint8_t
    {
        k1Channel = 0,
        k2Channel = 1,
        k4Channel = 2,
        k6Channel = 3,
    };

    virtual ~EffectBase() = default;

    // Adds the effect to the renderer's mix with its settings (the effect's virtual function at +0x18).
    virtual void AddEffect(audio::Renderer& renderer, int mix) = 0;

    // The renderer's buffers for the effect's channels, from the bus's buffers, one for each of the bus's channels
    // (+0x20).
    virtual void SetEffectInputOutput(std::span<const int8_t> buffers) = 0;

    virtual void SetEnabled(bool enabled) = 0;

    // A copy of the effect's settings, for a snapshot of the model. The copy holds the same renderer's effect.
    virtual std::unique_ptr<EffectBase> Clone() const = 0;

    // The effect's channel count for its mode.
    int ChannelCount() const;

    // Whether the effect has been added.
    bool IsAdded() const
    {
        return renderer_ != nullptr;
    }

protected:
    // Holds a gain to 0 to 1, or 0 to `most`, like nn::atk: fmax with 0, then the most for a larger value.
    static float Held(float value, float most);

    audio::Renderer* renderer_ = nullptr;
    int index_ = -1;
    ChannelMode mode_ = ChannelMode::k4Channel;
    std::array<int, 6> channels_ = {0, 1, 2, 3, 4, 5}; // the bus's channel for each of the effect's
};

// nn::atk's delay (EffectDelay, constructor 0xdede34), on the renderer's delay. It starts with a longest delay of 2 s
// and a delay of 160 ms, an in gain of 0.5, a feedback gain of 0.4, a dry gain of 0.5, a low-pass amount of 0.8, a
// channel spread of 0.3, disabled, on 4 channels. The setters hold the delay to 0 to the longest delay, and each gain
// to 0 to 1. On 6 channels, the delay takes the bus's front left and right, centre, LFE, and rear left and right.
class EffectDelay : public EffectBase
{
public:
    static constexpr int64_t kDefaultMaxDelay = 2000000000;
    static constexpr int64_t kDefaultDelay = 160000000;

    // The renderer's AddDelay, then each setting from the delay to the enable (0xdedf58).
    void AddEffect(audio::Renderer& renderer, int mix) override;

    // The renderer's SetDelayInputOutput (0xdee0bc).
    void SetEffectInputOutput(std::span<const int8_t> buffers) override;

    // The longest delay and the channel mode change only before the effect is added, and return whether they changed
    // (0xdee42c and 0xdee2c4).
    bool SetDelayTimeMax(int64_t time);
    bool SetChannelMode(ChannelMode mode);

    void SetDelayTime(int64_t time); // 0xdee450
    void SetInGain(float gain);      // 0xdee4bc
    void SetFeedbackGain(float gain);
    void SetDryGain(float gain);
    void SetLowPassAmount(float amount);
    void SetChannelSpread(float spread);
    void SetEnabled(bool enabled) override; // 0xdee6ec

    std::unique_ptr<EffectBase> Clone() const override;

private:
    int64_t max_delay_ = kDefaultMaxDelay;
    int64_t delay_ = kDefaultDelay;
    float in_gain_ = 0.5f;
    float feedback_gain_ = 0.4f;
    float dry_gain_ = 0.5f;
    float low_pass_amount_ = 0.8f;
    float channel_spread_ = 0.3f;
    bool enabled_ = false;
};

// nn::atk's reverb (EffectReverb, constructor 0x544d90 in Clubhouse Games' main), on the renderer's reverb. It starts
// with early and late mode 0 (a small room), early and late gains of 0.7, a pre-delay of 15 ms, a decay time of 1.5 s,
// a high-frequency decay ratio of 0.8, a coloration of 0.7, a reverb gain of 0.2, an out gain of 1, a dry gain of 0.7,
// disabled, on 4 channels. The setters hold each gain and the coloration to 0 to 1, the ratio to 0.1 to 1, the
// pre-delay to 0 to 300 ms and the decay time to 0.1 to 20 s. They store a mode as it is. Nintendo's code stops when
// it passes a mode above 4 to the renderer, and the renderer's model throws. On 6 channels, the reverb takes the bus's
// channels in their order.
class EffectReverb : public EffectBase
{
public:
    static constexpr int64_t kDefaultPredelay = 15000000;
    static constexpr int64_t kDefaultDecay = 1500000000;
    static constexpr int64_t kMostPredelay = 300000000;
    static constexpr int64_t kLeastDecay = 100000000;
    static constexpr int64_t kMostDecay = 20000000000;

    // The renderer's AddReverb, then each setting from the early mode to the enable (the virtual function at +0x18,
    // 0x544ee0).
    void AddEffect(audio::Renderer& renderer, int mix) override;

    // The renderer's SetReverbInputOutput (0x5450b0).
    void SetEffectInputOutput(std::span<const int8_t> buffers) override;

    // The channel mode changes only before the effect is added, and returns whether it changed (0x5452e0).
    bool SetChannelMode(ChannelMode mode);

    void SetEarlyMode(int mode);                  // 0x545400
    void SetEarlyGain(float gain);                // 0x545480
    void SetPredelayTime(int64_t time);           // 0x5454f0
    void SetLateMode(int mode);                   // 0x545590
    void SetLateGain(float gain);                 // 0x545610
    void SetDecayTime(int64_t time);              // 0x545680
    void SetHighFrequencyDecayRatio(float ratio); // 0x545730
    void SetColoration(float coloration);         // 0x5457b0
    void SetReverbGain(float gain);               // 0x545820
    void SetOutGain(float gain);                  // 0x545890
    void SetDryGain(float gain);                  // 0x545900
    void SetEnabled(bool enabled) override;       // 0x545970

    std::unique_ptr<EffectBase> Clone() const override;

private:
    int early_mode_ = 0;
    float early_gain_ = 0.7f;
    int64_t predelay_ = kDefaultPredelay;
    int late_mode_ = 0;
    float late_gain_ = 0.7f;
    int64_t decay_ = kDefaultDecay;
    float high_frequency_decay_ratio_ = 0.8f;
    float coloration_ = 0.7f;
    float reverb_gain_ = 0.2f;
    float out_gain_ = 1.0f;
    float dry_gain_ = 0.7f;
    bool enabled_ = false;
};

// nn::atk's light limiter (EffectLightLimiter, constructor 0xdeff38), on the renderer's light limiter. It starts with
// an attack of 20 µs, a release of 300 ms, a longest look-ahead of 1 ms, a look-ahead of 0.5 ms, a threshold of 0.2,
// input and output gains of 1, disabled, on 6 channels. The setters hold the attack to 0 to 10 ms, the release to 0 to
// 5 s, the look-ahead to 0 to the longest look-ahead, the threshold to 0 to 1, and the gains to 0 to 4.
class EffectLightLimiter : public EffectBase
{
public:
    static constexpr int64_t kMostAttack = 10000000;
    static constexpr int64_t kMostRelease = 5000000000;

    EffectLightLimiter()
    {
        mode_ = ChannelMode::k6Channel;
    }

    // The renderer's AddLightLimiter, then each setting from the attack to the enable (0xdf0068).
    void AddEffect(audio::Renderer& renderer, int mix) override;

    // The renderer's SetLightLimiterInputOutput (0xdf01c0).
    void SetEffectInputOutput(std::span<const int8_t> buffers) override;

    // The longest look-ahead, in whole microseconds of up to 50 ms, and the channel mode change only before the
    // effect is added, and return whether they changed (0xdf05a8 and 0xdf03b8).
    bool SetLookAheadTimeMax(int64_t time);
    bool SetChannelMode(ChannelMode mode);

    void SetAttackTime(int64_t time);       // 0xdf04cc
    void SetReleaseTime(int64_t time);      // 0xdf053c
    void SetLookAheadTime(int64_t time);    // 0xdf0624
    void SetThreshold(float threshold);     // 0xdf0690
    void SetInputGain(float gain);          // 0xdf0700
    void SetOutputGain(float gain);         // 0xdf0770
    void SetEnabled(bool enabled) override; // 0xdf07e0

    std::unique_ptr<EffectBase> Clone() const override;

private:
    int64_t attack_ = 20000;
    int64_t release_ = 300000000;
    int64_t max_look_ahead_ = 1000000;
    int64_t look_ahead_ = 500000;
    float threshold_ = 0.2f;
    float input_gain_ = 1.0f;
    float output_gain_ = 1.0f;
    bool enabled_ = false;
};

} // namespace ntdlib::nx::atk
