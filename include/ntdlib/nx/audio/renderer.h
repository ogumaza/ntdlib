// SPDX-License-Identifier: MIT

// The Switch's audio renderer, as a game's nn::audio sets it up: voices, the final mix and sub-mixes, and a device
// sink, and the frames it renders from them. This follows the build of the renderer for the CPU in WarioWare: Get It
// Together!'s sdk (nn::audio::server and nn::audio::dsp). Its delay, reverb and light limiter follow the build in
// WarioWare: Move It!'s sdk. That build renders the voices and mixes as the other build does, apart from a voice with
// both biquads on. The calls change the game's copy of the parameters. Each frame then sends them to the renderer. The
// renderer takes them in, reports the voices' state from two frames before, generates the frame's command list and runs
// it. An aux effect in a mix sends the mix's samples to the game and receives what the game makes of them. A delay, a
// reverb or a light limiter runs in the renderer. A voice's splitter follows the build in Skyward Sword HD's sdk
// (NintendoSDK 11.4.0, REV8). With splitters in use, that renderer orders the mixes by a topological sort
// (MixContext::TsortInfo). The model keeps the order by distance from the final mix. Not modelled yet: the renderer's
// I3DL2 reverb, compressor and other effects, a sub-mix's splitter, the topological order, voice dropping, a device
// sink's downmix to 2 channels (SetDownMixParameter), and the exchange of a 6-channel effect's buffers 2 and 3 with 4
// and 5 before REV11. That exchange leaves the front left and right as they are. A renderer at 32 kHz feeds its device
// sink through the upsampler to 48 kHz (UpsampleFrame). For a stereo output, the system mixes the device sink's 6
// channels down to 2 after the renderer (downmix.h).

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ntdlib/nx/audio/delay.h"
#include "ntdlib/nx/audio/dsp.h"
#include "ntdlib/nx/audio/light_limiter.h"
#include "ntdlib/nx/audio/reverb.h"
#include "ntdlib/nx/audio/voice.h"

namespace ntdlib::nx::audio
{

// nn::audio::VoiceType::PlayState.
enum class PlayState : uint8_t
{
    kStarted = 0,
    kStopped = 1,
    kPaused = 2,
};

// nn::audio::BiquadFilterParameter: the coefficients in Q14, the feedback ones added.
struct BiquadFilterParameter
{
    bool enabled = false;
    std::array<int16_t, 3> b{};
    std::array<int16_t, 2> a{};
};

// What AppendWaveBuffer takes (nn::audio::WaveBuffer): the samples and the range in them, and for DSP-ADPCM the context
// at the range's start, if any. GetReleasedWaveBuffer gives `tag` back once the renderer is done with the buffer.
//
// With `loop_fields` set, the buffer takes its loop range and loop count from the three fields after it. The sdk's
// AppendWaveBuffer does the same with WaveBuffer::MemberOptions (REV8). A looping buffer then plays [start, end)
// once and [loop_start, loop_end) after that, loop_count more times, or for ever for -1. Without them, the loop range
// is [start, end), and a looping buffer loops for ever.
struct WaveBufferInfo
{
    std::span<const uint8_t> buffer;
    int32_t start = 0;
    int32_t end = 0;
    bool loop = false;
    bool end_of_stream = false;
    const AdpcmContext* context = nullptr;
    const void* tag = nullptr;
    bool loop_fields = false;
    int32_t loop_start = 0;
    int32_t loop_end = 0;
    int32_t loop_count = 0;
};

// What AcquireVoiceSlot takes as nn::audio::VoiceType::BehaviorOptions (REV5): whether a looping buffer's played
// sample count starts again from 0 at each loop. nn::atk passes none. The renderer's option that skips the pitch and
// the sample-rate conversion isn't modelled.
struct VoiceBehavior
{
    bool reset_played_at_loop = false;
};

// The renderer's settings (nn::audio::AudioRendererParameter), and the mixing mode, which nn::audio lacks.
struct RendererParameter
{
    int32_t sample_rate = 48000;
    int32_t sample_count = 240;
    int32_t mix_buffer_count = 1;
    int32_t sub_mix_count = 0;
    int32_t voice_count = 1;

    // The splitters (splitterCount) and the destinations that they share (splitterSendChannelCount).
    int32_t splitter_count = 0;
    int32_t splitter_destination_count = 0;

    // The enhanced mixing mode (see Renderer). Off, the renderer follows the console to the bit.
    bool enhanced = false;
};

// Sees each call that the client makes on a renderer once the renderer has taken it: the arguments the client passed,
// and what the call gave back. A test makes the same calls on Nintendo's renderer through it. A renderer has no
// observer unless one is set.
class RendererObserver
{
public:
    virtual ~RendererObserver() = default;

    // The renderer as the client opened it (OpenAudioRenderer), when the observer is set.
    virtual void OpenAudioRenderer(const RendererParameter& parameter) = 0;

    virtual void AcquireVoiceSlot(int32_t sample_rate, int channel_count, SampleFormat format, int32_t priority,
                                  const AdpcmCoefficients* coefficients, const VoiceBehavior& behavior, int voice) = 0;
    virtual void ReleaseVoiceSlot(int voice) = 0;
    virtual void SetVoicePlayState(int voice, PlayState state) = 0;
    virtual void GetVoicePlayState(int voice, PlayState state) = 0;
    virtual void SetVoicePitch(int voice, float pitch) = 0;
    virtual void SetVoiceVolume(int voice, float volume) = 0;
    virtual void SetVoicePriority(int voice, int32_t priority) = 0;
    virtual void SetVoiceSrcQuality(int voice, SrcQuality quality) = 0;
    virtual void SetVoiceBiquadFilterParameter(int voice, int index, const BiquadFilterParameter& parameter) = 0;
    virtual void SetVoiceDestination(int voice, int mix) = 0;
    virtual void SetVoiceMixVolume(int voice, float volume, int channel, int buffer) = 0;
    virtual void AcquireSplitter(int32_t sample_rate, int channel_count, int destination_count, int splitter) = 0;
    virtual void ReleaseSplitter(int splitter) = 0;
    virtual void SetSplitterDestination(int splitter, int destination, int mix) = 0;
    virtual void SetSplitterMixVolume(int splitter, int destination, float volume, int channel, int buffer) = 0;
    virtual void SetVoiceSplitter(int voice, int splitter) = 0;
    virtual void AppendWaveBuffer(int voice, const WaveBufferInfo& buffer, bool appended) = 0;
    virtual void GetReleasedWaveBuffer(int voice, const void* tag) = 0;
    virtual void GetVoicePlayedSampleCount(int voice, uint64_t count) = 0;
    virtual void GetWaveBufferCount(int voice, int count) = 0;
    virtual void IsVoiceDroppedFlagOn(int voice, bool dropped) = 0;
    virtual void AcquireFinalMix(int buffer_count, bool acquired) = 0;
    virtual void AcquireSubMix(int32_t sample_rate, int buffer_count, int sub_mix) = 0;
    virtual void SetSubMixDestination(int sub_mix, int destination) = 0;
    virtual void SetSubMixMixVolume(int sub_mix, float volume, int source, int destination) = 0;
    virtual void AddDeviceSink(std::span<const int> inputs) = 0;
    virtual void AddAux(int mix, uint32_t buffer_size, int aux) = 0;
    virtual void SetAuxInputOutput(int aux, std::span<const int8_t> inputs, std::span<const int8_t> outputs) = 0;
    virtual void SetAuxEnabled(int aux, bool enabled) = 0;
    virtual void GetAuxSampleCount(int aux, int32_t count) = 0;
    virtual void GetAuxSampleRate(int aux, int32_t rate) = 0;

    // The client's buffer after the read, with how many samples the renderer gave. The samples the client wrote, with
    // how many the renderer took.
    virtual void ReadAuxSendBuffer(int aux, std::span<const int32_t> samples, int32_t read) = 0;
    virtual void WriteAuxReturnBuffer(int aux, std::span<const int32_t> samples, int32_t written) = 0;

    // The times are in nanoseconds (nn::TimeSpan).
    virtual void AddDelay(int mix, int64_t max_delay, int channel_count, int delay) = 0;
    virtual void SetDelayInputOutput(int delay, std::span<const int8_t> inputs, std::span<const int8_t> outputs) = 0;
    virtual void SetDelayTime(int delay, int64_t time) = 0;
    virtual void SetDelayInGain(int delay, float gain) = 0;
    virtual void SetDelayFeedbackGain(int delay, float gain) = 0;
    virtual void SetDelayDryGain(int delay, float gain) = 0;
    virtual void SetDelayChannelSpread(int delay, float spread) = 0;
    virtual void SetDelayLowPassAmount(int delay, float amount) = 0;
    virtual void SetDelayEnabled(int delay, bool enabled) = 0;
    virtual void AddReverb(int mix, int channel_count, int reverb) = 0;
    virtual void SetReverbInputOutput(int reverb, std::span<const int8_t> inputs, std::span<const int8_t> outputs) = 0;
    virtual void SetReverbEarlyMode(int reverb, int mode) = 0;
    virtual void SetReverbEarlyGain(int reverb, float gain) = 0;
    virtual void SetReverbPredelayTime(int reverb, int64_t time) = 0;
    virtual void SetReverbLateMode(int reverb, int mode) = 0;
    virtual void SetReverbLateGain(int reverb, float gain) = 0;
    virtual void SetReverbDecayTime(int reverb, int64_t time) = 0;
    virtual void SetReverbHighFrequencyDecayRatio(int reverb, float ratio) = 0;
    virtual void SetReverbColoration(int reverb, float coloration) = 0;
    virtual void SetReverbReverbGain(int reverb, float gain) = 0;
    virtual void SetReverbOutGain(int reverb, float gain) = 0;
    virtual void SetReverbDryGain(int reverb, float gain) = 0;
    virtual void SetReverbEnabled(int reverb, bool enabled) = 0;
    virtual void AddLightLimiter(int mix, int64_t max_look_ahead, int channel_count, int limiter) = 0;
    virtual void SetLightLimiterInputOutput(int limiter, std::span<const int8_t> inputs,
                                            std::span<const int8_t> outputs) = 0;
    virtual void SetLightLimiterAttackTime(int limiter, int64_t time) = 0;
    virtual void SetLightLimiterReleaseTime(int limiter, int64_t time) = 0;
    virtual void SetLightLimiterLookAheadTime(int limiter, int64_t time) = 0;
    virtual void SetLightLimiterThreshold(int limiter, float threshold) = 0;
    virtual void SetLightLimiterInputGain(int limiter, float gain) = 0;
    virtual void SetLightLimiterOutputGain(int limiter, float gain) = 0;
    virtual void SetLightLimiterEnabled(int limiter, bool enabled) = 0;

    // A frame: the update, and the command list's generation and processing.
    virtual void RenderFrame() = 0;
};

// In the enhanced mixing mode, the renderer keeps the console's voices, buffers, steps, gains and effects, and lifts
// the limits of its arithmetic:
//
// - A voice decodes as DecodeFromWaveBuffersEnhanced decodes it: the console's steps, with the band-limited
//   interpolator in place of the console's filters.
// - The voices, the mixes and the depop work in single-precision floating point, on the scale of 16-bit samples. A
//   biquad filters the voice's samples in floating point without the conversions around it. Nothing holds a sum to 32
//   bits.
// - The volume of a voice and each of its mix volumes ramp across the frame from their values in the last frame: sample
//   i takes from + i * ((to - from) / n) of a frame of n samples. Every channel of a voice ramps from the last frame's
//   volume.
// - A voice that stops leaves the last product of each of its mix ramps to its destination's depop. Each sample, the
//   depop multiplies by 31,529 / 32,768 at 48 kHz, or 30,923 / 32,768 at another rate, and adds to its buffer. A
//   depop below 2^-16 at the end of a frame becomes 0.
// - An aux effect's send ring takes each sample rounded to the nearest integer, halves away from zero, held to 32
//   bits. Its return ring's samples come back as they are. A delay and a light limiter take their samples rounded the
//   same way, and their output comes back as the console's integers.
// - DeviceOutputFloat gives the output divided by 32,768, without a limit. DeviceOutput gives it rounded to 16 bits
//   and held there.
//
// Each operation takes its operands in a fixed order, and every platform gives the same samples.
//
// A frame without mixing (SetMixing) moves every voice on through its wave buffers as a mixed frame does, in either
// mode. The voices' decoders, histories, buffers, counts of played samples and volumes end the frame as a mixed frame
// leaves them. Nothing is converted, filtered or mixed, the effects don't run, and the output and the meters are
// silent. The biquads' states, the effects' states and the aux rings stay as they were. The depop fades as in a mixed
// frame. The voices' last samples become 0. A voice that stops before a mixed frame has given them again leaves the
// depop nothing. A delay or a light limiter that a call has set to start again does start again, and one that a call
// has given new gains takes them. Once the frames mix again, a biquad's state takes a moment to settle, and the effects
// lack the tails of what they missed. A host runs such frames to reach a place in a sound quickly.
class Renderer
{
public:
    // A copy of the renderer's state between frames, for a snapshot of a model on it: everything but the observer.
    struct Snapshot;

    // A mix by its id: the final mix is 0, the sub-mixes 1 up.
    static constexpr int kFinalMix = 0;
    static constexpr int kNoMix = -1;
    static constexpr int kNoSplitter = -1;

    // The most channels a voice has, and the most buffers a mix has.
    static constexpr int kMaxChannels = 6;
    static constexpr int kMaxMixBuffers = 24;

    explicit Renderer(const RendererParameter& parameter);

    const RendererParameter& Parameter() const
    {
        return parameter_;
    }

    // Shows every call from now on to `observer`, or to none when it's null, starting with the parameter the renderer
    // was opened with. The observer has to outlive the renderer, or be replaced first.
    void SetObserver(RendererObserver* observer);

    // A voice is its slot, or -1 when there is no free slot or not enough channel resources (AcquireVoiceSlot). A new
    // voice is stopped, at pitch 1 and volume 1, with no destination.
    int AcquireVoiceSlot(int32_t sample_rate, int channel_count, SampleFormat format, int32_t priority,
                         const AdpcmCoefficients* coefficients, const VoiceBehavior& behavior = {});

    // ReleaseVoiceSlot: the renderer stops the voice at once, with no depop.
    void ReleaseVoiceSlot(int voice);

    void SetVoicePlayState(int voice, PlayState state);
    PlayState GetVoicePlayState(int voice) const;
    void SetVoicePitch(int voice, float pitch);
    void SetVoiceVolume(int voice, float volume);
    void SetVoicePriority(int voice, int32_t priority);
    void SetVoiceSrcQuality(int voice, SrcQuality quality);
    void SetVoiceBiquadFilterParameter(int voice, int index, const BiquadFilterParameter& parameter);
    void SetVoiceDestination(int voice, int mix);

    // The gain from a channel of the voice to a buffer of its destination.
    void SetVoiceMixVolume(int voice, float volume, int channel, int buffer);

    // Splitters (REV2). A splitter sends each channel of a voice to each of its destinations. A destination is a mix,
    // with mix volumes for each channel. AcquireSplitter returns the splitter, or kNoSplitter when no splitter is free
    // or too few destinations are (SplitterInfoManager::AcquireSplitterInfo, 0x45628 in Skyward Sword HD's sdk). It
    // takes the free splitter and the free destinations that were freed first. A new destination has no mix and mix
    // volumes of 0. ReleaseSplitter (0x457cc) frees them. The renderer then takes them out of use at the next frame,
    // unless a new splitter takes them first.
    int AcquireSplitter(int32_t sample_rate, int channel_count, int destination_count);
    void ReleaseSplitter(int splitter);

    // The mix of a destination, for each of the splitter's channels (SetSplitterDestination, 0x44db0).
    void SetSplitterDestination(int splitter, int destination, int mix);

    // The gain from a channel to a buffer of a destination's mix (SetSplitterMixVolume, 0x45070).
    void SetSplitterMixVolume(int splitter, int destination, float volume, int channel, int buffer);

    // The splitter as the voice's destination, in place of a mix (SetVoiceDestination, 0x465e0). SetVoiceDestination
    // takes a voice off its splitter.
    void SetVoiceSplitter(int voice, int splitter);

    // A host's mute of a voice. nn::audio has no such call. A muted voice plays on as before, and its output to every
    // buffer of its destination is silent. A change ramps the voice's volume to or from 0 across the next frame. A
    // change of volume ramps the same way. SetVoiceMuted makes no call to the observer. A new voice starts unmuted.
    // The first frame after a restore takes each voice's mute at once, with no ramp. A voice muted then leaves the
    // depop nothing to fade out. The voice sounds as though its mute had held since the snapshot.
    void SetVoiceMuted(int voice, bool muted);

    // A host's meters. nn::audio has no such meters. Each of the `count` meters adds up its voices' output to the
    // buffers of mix `mix`. Buffer b goes to the meter's channel channels[b], or to no channel for -1. SetVoiceMeter
    // gives a voice a meter, or none for -1. MeterPeaks gives the peak of each of a meter's channels in the last frame:
    // the largest magnitude of its sum, on the scale of the mix's samples. The depop of a voice that stops reaches no
    // meter. The meters change nothing in the output and make no call to the observer. A new voice has no meter. The
    // meters (SetMeters) aren't part of a snapshot. A restore brings back the voices' meters from the snapshot.
    void SetMeters(int count, int mix, std::span<const int> channels);
    void SetVoiceMeter(int voice, int meter);
    std::span<const int32_t> MeterPeaks(int meter) const;

    // A host's peaks of a mix's buffers. nn::audio has no such peaks. While MeterMix has them on, each frame finds the
    // peak of each of mix `mix`'s buffers after the mix's effects, before the mix's volumes: the largest magnitude, on
    // the scale of the mix's samples. MixPeaks gives the last frame's, one for each of the mix's buffers, or none for a
    // mix without them. Peaks turned on give 0 until the next frame. The peaks change nothing in the output and make no
    // call to the observer. They aren't part of a snapshot.
    void MeterMix(int mix, bool on);
    std::span<const int32_t> MixPeaks(int mix) const;

    // Queues a buffer: false when the voice already has 4 that it hasn't released.
    bool AppendWaveBuffer(int voice, const WaveBufferInfo& buffer);

    // The tag of the oldest buffer the renderer has finished with, or nullptr. Each one comes back once.
    const void* GetReleasedWaveBuffer(int voice);

    // The buffers appended that the client hasn't taken back with GetReleasedWaveBuffer (GetWaveBufferCount).
    int GetWaveBufferCount(int voice) const;

    // What the last frame's update reported, as of two frames before it.
    uint64_t GetVoicePlayedSampleCount(int voice) const;
    bool IsVoiceDroppedFlagOn(int voice) const;

    // The final mix (AcquireFinalMix) and the sub-mixes (AcquireSubMix). AcquireSubMix returns the id or kNoMix. A
    // sub-mix's sample rate sets its depop's decay.
    bool AcquireFinalMix(int buffer_count);
    int AcquireSubMix(int32_t sample_rate, int buffer_count);
    void SetSubMixDestination(int sub_mix, int destination);

    // The gain from a buffer of the sub-mix to a buffer of its destination.
    void SetSubMixMixVolume(int sub_mix, float volume, int source, int destination);

    // The device sink: the final mix's buffers it takes, one for each channel of the output (AddDeviceSink). The
    // enhanced mode has no upsampler. In that mode at 32 kHz, AddDeviceSink throws std::logic_error.
    void AddDeviceSink(std::span<const int> inputs);

    // The size of each of an aux effect's two buffers for `frames` frames of `channels` channels
    // (GetRequiredBufferSizeForAuxSendReturnBuffer, 0x3f5c0): a header of 0x80 bytes and the samples, in 0x40 bytes.
    static uint32_t GetRequiredBufferSizeForAuxSendReturnBuffer(const RendererParameter& parameter, int frames,
                                                                int channels);

    // An aux effect on a mix, after the mix's other effects (AddAux, 0x3f750, with buffers of `buffer_size` bytes).
    // Each frame, the renderer writes the effect's input buffers into a send ring for the game to read. The renderer
    // takes what the game wrote into a return ring, from the same place in the ring, and puts it into the effect's
    // output buffers. A ring holds whole frames of every channel. What the game writes back therefore comes out a
    // ring's worth of frames after it went in. Returns the effect.
    int AddAux(int mix, uint32_t buffer_size);

    // The mix's buffers that the effect takes and gives, one for each channel and at most the mix's buffer count
    // (SetAuxInputOutput, 0x3fb70).
    void SetAuxInputOutput(int aux, std::span<const int8_t> inputs, std::span<const int8_t> outputs);

    // A disabled effect passes its inputs through and starts its rings again (SetAuxEnabled, 0x3fa90).
    void SetAuxEnabled(int aux, bool enabled);

    // The samples a ring has room for (GetAuxSampleCount, 0x3fcc0), and the mix's rate.
    int32_t GetAuxSampleCount(int aux) const;
    int32_t GetAuxSampleRate(int aux) const;

    // The samples the renderer has sent since the last read, up to the span's size (ReadAuxSendBuffer, 0x3fc70, through
    // ReadAuxBuffer, 0x3d780). Returns how many.
    int32_t ReadAuxSendBuffer(int aux, std::span<int32_t> samples);

    // Samples for the renderer, as many as the ring has room for up to where the renderer reads next
    // (WriteAuxReturnBuffer, 0x3fc90, through WriteAuxBuffer, 0x3d950). Returns how many.
    int32_t WriteAuxReturnBuffer(int aux, std::span<const int32_t> samples);

    // The delay, the reverb and the light limiter follow Move It!'s sdk. Their times are in nanoseconds
    // (nn::TimeSpan). Each runs after the effects added to its mix before it, on the mix's buffers that
    // SetDelayInputOutput, SetReverbInputOutput or SetLightLimiterInputOutput gives it. A call that changes the delay's
    // channels or time, or that enables it, starts it again: its lines are cleared at the next frame. A call that
    // changes a gain gives it new gains at the next frame. The reverb starts again for a change of its channels or an
    // enable. Any other call works out its delays and gains again at the next frame, and starts its feedback lines
    // again from their first cells. A disabled delay, reverb or limiter passes its inputs through.

    // A delay on a mix of up to `max_delay` on `channel_count` channels (AddDelay, 0x1cf680). It starts enabled, with
    // no delay, no input and no feedback, and full dry and wet gains. Returns the delay.
    int AddDelay(int mix, int64_t max_delay, int channel_count);

    // The mix's buffers that the delay takes and gives, one for each channel, at most its channel count and 6
    // (SetDelayInputOutput, 0x1cfa50).
    void SetDelayInputOutput(int delay, std::span<const int8_t> inputs, std::span<const int8_t> outputs);

    // The delay in milliseconds, towards zero (SetDelayTime, 0x1cfb90).
    void SetDelayTime(int delay, int64_t time);

    // The gains in Q14, towards zero (SetDelayInGain, 0x1cfd40, and the setters after it).
    void SetDelayInGain(int delay, float gain);
    void SetDelayFeedbackGain(int delay, float gain);
    void SetDelayDryGain(int delay, float gain);
    void SetDelayChannelSpread(int delay, float spread);
    void SetDelayLowPassAmount(int delay, float amount);
    void SetDelayEnabled(int delay, bool enabled);

    // A reverb on a mix of `channel_count` channels (AddReverb, 0x1d2c10). Its sample rate is the mix's rate in kHz,
    // towards zero. It starts enabled, with no early reflections (early mode 4), late mode 0, a decay time and a
    // high-frequency decay ratio of 0x666 in Q14 (about 0.1), a full dry gain, and the other gains, the pre-delay and
    // the coloration at 0. Returns the reverb.
    int AddReverb(int mix, int channel_count);

    // The mix's buffers that the reverb takes and gives, one for each channel, at most its channel count and 6
    // (SetReverbInputOutput, 0x1d2f30).
    void SetReverbInputOutput(int reverb, std::span<const int8_t> inputs, std::span<const int8_t> outputs);

    // The modes, 0 to 4 (SetReverbEarlyMode, 0x1d35a0, and SetReverbLateMode, 0x1d3650). Nintendo's client stops on
    // another mode. The model throws std::invalid_argument.
    void SetReverbEarlyMode(int reverb, int mode);
    void SetReverbLateMode(int reverb, int mode);

    // The pre-delay: the time in microseconds towards zero, over 1,000 in single precision, in Q14 towards zero
    // (SetReverbPredelayTime, 0x1d30a0). The decay time: the time in milliseconds towards zero, over 1,000, in Q14
    // (SetReverbDecayTime, 0x1d31a0).
    void SetReverbPredelayTime(int reverb, int64_t time);
    void SetReverbDecayTime(int reverb, int64_t time);

    // The gains and ratios in Q14, towards zero (SetReverbReverbGain, 0x1d3020, and the setters after it).
    void SetReverbEarlyGain(int reverb, float gain);
    void SetReverbLateGain(int reverb, float gain);
    void SetReverbHighFrequencyDecayRatio(int reverb, float ratio);
    void SetReverbColoration(int reverb, float coloration);
    void SetReverbReverbGain(int reverb, float gain);
    void SetReverbOutGain(int reverb, float gain);
    void SetReverbDryGain(int reverb, float gain);
    void SetReverbEnabled(int reverb, bool enabled);

    // A light limiter on a mix with a look-ahead of up to `max_look_ahead` on `channel_count` channels
    // (AddLightLimiter, 0x1d1890). It starts enabled, with a look-ahead of a sample, instant attack and release, and a
    // threshold and gains of 0. Returns the limiter.
    int AddLightLimiter(int mix, int64_t max_look_ahead, int channel_count);

    // The mix's buffers that the limiter takes and gives (SetLightLimiterInputOutput, 0x1d1c70). The call starts the
    // limiter again. So do a change of its look-ahead and an enable. The other calls take effect at the next frame
    // without that.
    void SetLightLimiterInputOutput(int limiter, std::span<const int8_t> inputs, std::span<const int8_t> outputs);

    // The attack and release times, in microseconds towards zero, set the coefficients (SetLightLimiterAttackTime,
    // 0x1d1d80, and SetLightLimiterReleaseTime, 0x1d1e80). The look-ahead time sets the look-ahead in samples
    // (SetLightLimiterLookAheadTime, 0x1d1f80).
    void SetLightLimiterAttackTime(int limiter, int64_t time);
    void SetLightLimiterReleaseTime(int limiter, int64_t time);
    void SetLightLimiterLookAheadTime(int limiter, int64_t time);
    void SetLightLimiterThreshold(int limiter, float threshold);
    void SetLightLimiterInputGain(int limiter, float gain);
    void SetLightLimiterOutputGain(int limiter, float gain);
    void SetLightLimiterEnabled(int limiter, bool enabled);

    // A frame: the update, then the command list's generation and processing.
    void RenderFrame();

    // Whether the frames mix (see the class's comment). They mix unless the host turns the mixing off. A snapshot
    // doesn't hold the setting.
    void SetMixing(bool mixing)
    {
        mixing_ = mixing;
    }

    bool Mixing() const
    {
        return mixing_;
    }

    // The device sink's last frame: its channels interleaved, held to 16 bits. A renderer at 32 kHz gives 240 samples
    // of each channel at 48 kHz: the device sink's upsampler converts the final mix's 160 (UpsampleFrame).
    std::span<const int16_t> DeviceOutput() const
    {
        return output_;
    }

    // In the enhanced mode, the device sink's last frame in floating point: its channels interleaved, with full scale
    // at 1.0. Empty in the console's mode.
    std::span<const float> DeviceOutputFloat() const
    {
        return float_output_;
    }

    // A snapshot holds the voices and the channel resources up to the last that the renderer has used.
    Snapshot Save() const;

    // Returns the renderer to a snapshot that it saved. The wave buffers in a snapshot point into the renderer. Each
    // voice therefore goes back into the same place. The observer stays. The restore doesn't call it. The next frame
    // takes the voices' mutes at once (see SetVoiceMuted). Throws std::invalid_argument for a snapshot of another
    // renderer.
    void Restore(const Snapshot& snapshot);

private:
    struct ClientWaveBuffer
    {
        WaveBufferInfo info;
        bool sent = true; // an update has sent it
    };

    // A channel resource: the client's mix volumes, the renderer's mix volumes with the last frame's volumes, and two
    // copies of the channel's processing state. The commands use one copy. Updates report the other.
    struct ChannelResource
    {
        bool in_use = false;
        std::array<float, kMaxMixBuffers> client_mix{};
        std::array<float, kMaxMixBuffers> mix{};
        std::array<float, kMaxMixBuffers> previous_mix{};
        VoiceState dsp;
        VoiceState reported;
    };

    // A channel resource's state in the enhanced mode: its past samples, and the last product of each mix ramp, for
    // the depop.
    struct EnhancedChannel
    {
        std::array<float, kWideHistory> history{};
        std::array<float, kMaxMixBuffers> last{};
    };

    // A voice slot: what the client keeps, and the renderer's copy and state.
    struct Voice
    {
        // The client's fields.
        bool in_use = false;
        bool is_new = false;
        PlayState play_state = PlayState::kStopped;
        SampleFormat format = SampleFormat::kPcmInt16;
        int32_t sample_rate = 48000;
        int32_t priority = 0;
        int channel_count = 0;
        VoiceBehavior behavior;
        float pitch = 1.0f;
        float volume = 1.0f;
        bool muted = false; // the host's mute (SetVoiceMuted)
        int meter = -1;     // the host's meter (SetVoiceMeter)
        SrcQuality quality = SrcQuality::kMedium;
        std::array<BiquadFilterParameter, 2> biquads;
        AdpcmCoefficients coefficients{};
        int mix = kNoMix;
        int splitter = kNoSplitter;
        std::array<ClientWaveBuffer, kWaveBufferCount> buffers;
        int buffer_count = 0; // appended and not yet released
        int buffer_index = 0; // the oldest not yet released
        int released = 0;     // released since the voice was acquired
        std::array<int, kMaxChannels> resources{};
        uint64_t played = 0; // the statuses of the last update
        int consumed = 0;
        bool dropped = false;

        // The renderer's fields.
        struct Server
        {
            bool in_use = false;
            bool is_new = false;
            int state = 1;    // 0 started, 1 stopped, 2 stop requested, 3 paused
            int previous = 1; // the state before the last update
            SampleFormat format = SampleFormat::kPcmInt16;
            int32_t sample_rate = 48000;
            int32_t priority = 0;
            int32_t sort_order = 0;
            int channel_count = 0;
            VoiceBehavior behavior;
            float pitch = 1.0f;
            float volume = 1.0f;
            float previous_volume = 0.0f;
            bool muted = false;
            bool previous_muted = false;
            int meter = -1;
            SrcQuality quality = SrcQuality::kMedium;
            std::array<BiquadFilterParameter, 2> biquads;
            std::array<bool, 2> biquads_were_enabled{};
            int mix = kNoMix;
            int splitter = kNoSplitter;
            int buffer_count = 0;
            std::array<WaveBuffer, kWaveBufferCount> buffers;
            std::array<AdpcmContext, kWaveBufferCount> contexts{};
            std::array<bool, kWaveBufferCount> sent{true, true, true, true}; // false: taken, valid at the next frame
            bool stopping = false;
        } server;
    };

    // A splitter's destination, by its id: the client's mix and mix volumes, and the renderer's copy with the mix
    // volumes of the last frame that mixed the destination (nn::audio::SplitterDestinationData, 0x80 bytes, and
    // nn::audio::server::SplitterDestinationData, 0xe0 bytes). The next frame's update sends the client's values when
    // they've changed.
    struct SplitterDestination
    {
        int mix = kNoMix;
        std::array<float, kMaxMixBuffers> volumes{};
        bool in_use = false;
        bool changed = true;

        int server_mix = kNoMix;
        std::array<float, kMaxMixBuffers> server_volumes{};
        std::array<float, kMaxMixBuffers> previous_volumes{};
        bool server_in_use = false;
        bool mixed = false; // a frame has mixed it since the last update of its previous volumes
        int next = -1;      // the next destination in the renderer's list
    };

    // A splitter, by its id: the client's channels and destinations, and the renderer's list of the destinations
    // (nn::audio::SplitterInfo and its server's). The next frame's update sends the client's values when they've
    // changed. Destination 0 comes first, once for each channel, then destination 1, and so on. The renderer links its
    // list through the destinations themselves. It has no channel count.
    struct Splitter
    {
        bool in_use = false;
        bool changed = false;
        int channel_count = 0;
        int32_t sample_rate = 0;
        std::vector<int> destinations;
        int server_first = -1; // the first destination in the renderer's list
    };

    // An effect of a mix: its kind, and its index among the renderer's effects of that kind.
    enum class EffectKind : uint8_t
    {
        kAux,
        kDelay,
        kReverb,
        kLightLimiter,
    };

    struct Effect
    {
        EffectKind kind = EffectKind::kAux;
        int index = 0;
    };

    struct Mix
    {
        bool in_use = false;
        int32_t sample_rate = 48000;
        int buffer_count = 0;
        float volume = 1.0f;
        int destination = kNoMix;
        std::vector<std::array<float, kMaxMixBuffers>> mix_volumes; // [source buffer][destination buffer]
        int offset = 0;                                             // its first mix buffer
        std::vector<Effect> effects;                                // in their order
    };

    // An aux effect: the client's settings and the renderer's copy of them, and the two rings with their offsets, in
    // samples (the headers' fields at +0x0, +0x4, +0x40 and +0x44).
    struct Aux
    {
        int mix = kNoMix;
        bool enabled = false;
        std::vector<int8_t> inputs;
        std::vector<int8_t> outputs;
        uint32_t capacity = 0;        // samples a ring has room for
        uint32_t capacity_frames = 0; // frames of one channel
        uint32_t ring = 0;            // the samples the rings go round in: whole frames of every channel

        bool server_enabled = false;
        std::vector<int8_t> server_inputs;
        std::vector<int8_t> server_outputs;
        uint32_t server_ring = 0;

        std::vector<int32_t> send;
        std::vector<int32_t> receive;
        uint32_t send_read = 0;     // the client's offset
        uint32_t send_write = 0;    // the renderer's offset
        uint32_t receive_write = 0; // the client's offset
        uint32_t receive_read = 0;  // the renderer's offset
    };

    // The parameter state that the delay, the reverb and the light limiter keep: 0 starts the effect again, 1 gives it
    // new gains (and the reverb new delays), and 2 leaves it as it is.
    static constexpr uint8_t kStart = 0;
    static constexpr uint8_t kNewGains = 1;
    static constexpr uint8_t kUnchanged = 2;

    // The most channels a delay, a reverb or a light limiter has.
    static constexpr int kMaxEffectChannels = std::max({kDelayChannels, kReverbChannels, kLightLimiterChannels});

    // A delay, a reverb or a light limiter: the client's settings, and the renderer's copy of them with its state.
    // `parameter` holds the channels' count. The renderer has a copy once an update has taken the effect.
    template <typename Parameter, typename State, std::size_t kChannels>
    struct RendererEffect
    {
        bool enabled = true;
        bool is_new = true;
        std::array<int8_t, kChannels> inputs{};
        std::array<int8_t, kChannels> outputs{};
        Parameter parameter;
        uint8_t state = kStart;

        bool server_taken = false;
        bool server_enabled = false;
        std::array<int8_t, kChannels> server_inputs{};
        std::array<int8_t, kChannels> server_outputs{};
        Parameter server_parameter;
        uint8_t server_state = kStart;
        State dsp;
    };

    // A delay, with the times and gains as the client set them.
    struct Delay : RendererEffect<DelayParameter, DelayState, kDelayChannels>
    {
    };

    // A reverb, with the times, gains and ratios in Q14 that the client works out.
    struct Reverb : RendererEffect<ReverbParameter, ReverbState, kReverbChannels>
    {
    };

    // A light limiter, with the coefficients and the look-ahead in samples that the client works out from its times.
    struct LightLimiter : RendererEffect<LightLimiterParameter, LightLimiterState, kLightLimiterChannels>
    {
    };

    Voice& At(int voice);
    const Voice& At(int voice) const;
    void Insert(int voice);
    void Remove(int voice);

    // The resource of channel `channel` of `voice`, and mix `id` if the renderer has it in use, or nullptr.
    ChannelResource& Resource(const Voice& voice, int channel);
    const Mix* MixInUse(int id) const;

    // What the client calls of the same names do, before the observer sees them.
    int TakeVoiceSlot(int32_t sample_rate, int channel_count, SampleFormat format, int32_t priority,
                      const AdpcmCoefficients* coefficients, const VoiceBehavior& behavior);
    bool QueueWaveBuffer(int voice, const WaveBufferInfo& buffer);
    const void* TakeReleasedWaveBuffer(int voice);
    bool TakeFinalMix(int buffer_count);
    int TakeSubMix(int32_t sample_rate, int buffer_count);
    int32_t ReadAuxSend(int aux, std::span<int32_t> samples);
    int32_t WriteAuxReturn(int aux, std::span<const int32_t> samples);

    // The update: each voice's parameters and wave buffers, the splitters and the effects.
    void Update();
    void UpdateVoice(Voice& voice, int slot);
    void TakeWaveBuffers(Voice& voice);
    void MuteAtOnce(Voice& voice);
    void ReportStatuses(Voice& voice);

    void Generate();
    void GenerateWithoutMixing();
    void LayOutMixes();

    // A voice's commands: a new voice's start, the play state, then the processing in the console's mode, the enhanced
    // mode or a frame without mixing.
    void RenderVoice(Voice& voice);
    void StartNewVoice(Voice& voice);
    bool UpdatePlayState(Voice& voice);
    void RenderVoiceConsole(Voice& voice, const Mix* destination);
    void LeaveToDepop(const Voice::Server& server, VoiceState& state, const Mix* destination);
    DecodeFromWaveBuffersArgs DecodeArgs(const Voice& voice, VoiceState& state, int channel, int32_t* out) const;

    // The destinations of a voice's splitter, as the renderer has them: the destination and its mix, for each one in
    // use that has a mix in use. Channel c takes those from place c, the voice's channel count apart
    // (CommandGenerator::GenerateVoiceCommand, 0x5cd3c). The depop takes every channel's destinations, from place 0, 1
    // apart (GenerateDataSourceCommand, 0x5c0c0).
    std::vector<std::pair<SplitterDestination*, const Mix*>> SplitterMixes(const Voice::Server& server, int first,
                                                                           int step);

    // The client's destination of a splitter's channel (SplitterInfo::GetDestinationData, 0x45d10), and the
    // renderer's destination `index` places down a splitter's list, or -1 (SplitterContext::GetDestinationData,
    // 0x67704).
    SplitterDestination& ClientDestination(int splitter, int destination, int channel);
    int ServerDestination(int splitter, int index) const;

    // The splitters' update, and the destinations' previous mix volumes after the voices' commands.
    void UpdateSplitters();
    void FinishSplitters();

    // A voice channel's mix ramps from `work` into the buffers of `mix`, from the volumes `from` to `to`, with its last
    // samples for the depop and its meter's sums (GenerateVoiceMixCommand, 0x5c8c8).
    void MixVoiceChannel(const Voice::Server& server, VoiceState& state, const int32_t* work, const Mix& mix,
                         int mix_id, const std::array<float, kMaxMixBuffers>& from,
                         const std::array<float, kMaxMixBuffers>& to);
    void RenderVoiceEnhanced(Voice& voice, const Mix* destination);
    void AdvanceVoice(Voice& voice, const Mix* destination);

    // Copies the volume and the mute of a voice, and whether each biquad is on, for the next frame to start from.
    static void KeepFrameValues(Voice::Server& server);

    // Where the output of a metered voice to buffer `buffer` of mix `mix` goes in the meter sums: the index of the
    // first sample, or none.
    std::optional<std::size_t> MeterSum(const Voice::Server& server, int mix, int buffer) const;
    void AddToMeter(const Voice::Server& server, int mix, int buffer, const int32_t* work, int32_t start, int32_t end);
    void AddToMeterEnhanced(const Voice::Server& server, int buffer, const float* work, float start, float end);
    void RenderSubMix(int id);
    void RenderFinalMix();
    void MeasureMix(int id);
    void Depop(const Mix& mix);
    void RenderEffects(const Mix& mix);
    void RenderAux(const Mix& mix, Aux& aux);

    // The delays', the reverbs' and the limiters' updates, their commands, the steps of the commands before the
    // processing in a frame without mixing, and their states after the frame's commands.
    void UpdateRendererEffects();
    void RenderDelay(const Mix& mix, Delay& delay);
    void RenderReverb(const Mix& mix, Reverb& reverb);
    void RenderLightLimiter(const Mix& mix, LightLimiter& limiter);
    void StartRendererEffects();
    void FinishRendererEffects();
    static void PrepareCommand(Delay& delay);
    static void PrepareCommand(Reverb& reverb);
    static void PrepareCommand(LightLimiter& limiter);

    // Whether the command of a delay, a reverb or a light limiter runs on `mix`: an update has taken the effect, its
    // channel count is 1, 2, 4 or 6, and its buffers are all among the mix buffers. RenderCommand runs the command.
    // `apply` does the processing.
    template <typename EffectType>
    bool CommandRuns(const Mix& mix, const EffectType& effect) const;

    template <typename EffectType, typename Apply>
    void RenderCommand(const Mix& mix, EffectType& effect, Apply apply);

    // The delay or the reverb that a setter changes, with its state at 1 unless it's 0.
    Delay& ChangeDelay(int delay);
    Reverb& ChangeReverb(int reverb);

    // An effect's channels take inputs[k] to outputs[k] of the mix's buffers. A command whose buffers aren't all among
    // the mix buffers doesn't run. PassThrough copies each input to its output where they differ. TakeEffectBuffers
    // gives the channels' samples, the mix's buffers themselves in the console's mode, or rounded into
    // effect_samples_ in the enhanced mode. GiveEffectBuffers puts the enhanced mode's outputs back.
    bool InRange(const Mix& mix, std::span<const int8_t> inputs, std::span<const int8_t> outputs) const;
    void PassThrough(const Mix& mix, std::span<const int8_t> inputs, std::span<const int8_t> outputs);
    void TakeEffectBuffers(const Mix& mix, std::span<const int8_t> inputs, std::span<const int8_t> outputs,
                           std::span<const int32_t*> in, std::span<int32_t*> out);
    void GiveEffectBuffers(const Mix& mix, std::span<const int8_t> outputs, std::span<int32_t* const> out);
    int32_t* Buffer(int index);
    float* FloatBuffer(int index);

    // The meters' peaks from their sums, and the device sink's output.
    void MeasureMeters();
    void Output();

    RendererParameter parameter_;
    std::vector<Voice> voices_;
    std::vector<ChannelResource> resources_;
    std::vector<int> order_; // the client's voices in use, by priority, the newest first among equals
    std::vector<Mix> mixes_;
    std::vector<Splitter> splitters_;
    std::vector<SplitterDestination> splitter_destinations_;
    std::vector<int> free_splitters_;             // the first freed first
    std::vector<int> free_splitter_destinations_; // the first freed first
    std::vector<Aux> auxes_;
    std::vector<Delay> delays_;
    std::vector<Reverb> reverbs_;
    std::vector<LightLimiter> limiters_;
    bool mixes_changed_ = true;
    std::vector<int> sinks_;            // the device sink's inputs
    std::vector<Upsampler> upsamplers_; // one for each of the device sink's inputs, at 32 kHz only
    std::vector<int32_t> buffers_;      // the mix buffers, then a working buffer for each channel a voice may have
    std::vector<int32_t> depop_;        // a value for each mix buffer
    std::vector<int16_t> output_;
    RendererObserver* observer_ = nullptr;
    bool mutes_at_once_ = false; // a restore came after the last frame
    bool mixing_ = true;

    // The host's meters (SetMeters): their number, their mix and the channel of each of its buffers, then each
    // channel's sum in the frame and its peak.
    int meter_count_ = 0;
    int meter_mix_ = kNoMix;
    std::vector<int> meter_channels_;
    int meter_channel_count_ = 0;
    std::vector<int32_t> meter_sums_;  // by meter, channel and sample
    std::vector<int32_t> meter_peaks_; // by meter and channel

    // The host's peaks of the mixes' buffers (MeterMix), by mix and buffer. A mix without them has none.
    std::vector<std::vector<int32_t>> mix_peaks_;

    // The voice slots and the channel resources up to the last that a voice has taken. A voice takes the first free
    // slot and resources. Those past the marks have never changed.
    int voice_mark_ = 0;
    int resource_mark_ = 0;

    // The enhanced mode's state: each channel resource's, the mix buffers with a working buffer for each channel a
    // voice may have, the depop, the meters' sums and the output. Then room for a batch's input. Each is empty in the
    // console's mode.
    std::vector<EnhancedChannel> enhanced_;
    std::vector<float> float_buffers_;
    std::vector<float> float_depop_;
    std::vector<float> float_meter_sums_;
    std::vector<float> float_output_;
    std::vector<int16_t> decoded_;
    std::vector<float> input_;

    // The enhanced mode's room for a delay's or a limiter's samples as integers: each channel's input, then its output.
    std::vector<int32_t> effect_samples_;
};

// The voices and the channel resources go up to the marks.
struct Renderer::Snapshot
{
    const Renderer* owner = nullptr;
    std::vector<Voice> voices;
    std::vector<ChannelResource> resources;
    std::vector<int> order;
    std::vector<Mix> mixes;
    std::vector<Splitter> splitters;
    std::vector<SplitterDestination> splitter_destinations;
    std::vector<int> free_splitters;
    std::vector<int> free_splitter_destinations;
    std::vector<Aux> auxes;
    std::vector<Delay> delays;
    std::vector<Reverb> reverbs;
    std::vector<LightLimiter> limiters;
    bool mixes_changed = true;
    std::vector<int> sinks;
    std::vector<Upsampler> upsamplers;
    std::vector<int32_t> buffers;
    std::vector<int32_t> depop;
    std::vector<int16_t> output;
    std::vector<EnhancedChannel> enhanced;
    std::vector<float> float_depop;
    std::vector<float> float_output;
};

} // namespace ntdlib::nx::audio
