// SPDX-License-Identifier: MIT

// The DS's sound unit: 16 channels, the mixer and the output. The model follows GBATEK's description. The ARM7 reaches
// the unit through the registers at 0x04000400 to 0x0400051F.
//
// Each channel plays signed 8-bit PCM, 16-bit PCM or IMA-ADPCM from memory. Channels 8 to 13 can also play a
// rectangular wave, and channels 14 and 15 noise. A channel's timer counts at half the system clock and steps the
// channel to its next sample each time it overflows. The mixer scales each channel by its volume and pan in fixed
// point, adds the channels, scales the sum by the master volume, adds the bias and clips it to 10 bits.
//
// The unit makes one output sample each 1024 system clocks, 32,728.5 a second. Each output sample holds every channel's
// sample at its end: the channels' samples aren't interpolated.
//
// The two capture units record the mixer's left and right sums into main memory, before the master volume. Capture 0
// counts with channel 1's timer, and capture 1 with channel 3's. GBATEK gives the registers and the samples' format.
// It doesn't say when a capture takes its sample, when it writes memory, how far ahead a channel reads its samples, or
// what a one-shot capture does at its buffer's end. The model takes a capture's sample at each overflow of its timer,
// after the channels have stepped, and writes it at once. A channel that plays PCM or IMA-ADPCM reads its words
// through a queue of 8 words, which it fills as it starts and again as each word ends. A channel that plays a capture's
// buffer with the same timer therefore plays what the capture recorded one pass of the buffer earlier, as the SDK's
// reverb requires. A one-shot capture clears its busy bit when its buffer is full. A capture of a channel (bit 1 of
// SNDCAPxCNT) and the addition of channel 1 or 3 to channel 0 or 2 (bit 0) aren't modelled: the capture still takes
// the mixer, and the channels play as such.
//
// A driver that grows its channels (snd::Driver::GrowChannels) gives the unit channels past the console's 16, at
// kGrownChannelRegisters. A grown channel plays as its twin does: a rectangular wave in the PSG format on a twin from 8
// to 13, and noise on a twin of 14 or 15. It goes to the mixer and to the captures. SOUNDCNT's choices of channel 1 or
// 3 alone leave it out.
//
// In the enhanced mixing mode (StartEnhanced), the channels and the captures step as in the console's mode, and the
// mixer lifts the console's limits. The output comes in floating point, kEnhancedFrameSamples samples in each
// kEnhancedFrameTicks ticks:
//
// - A channel that plays PCM or IMA-ADPCM gives the band-limited interpolator (wave::SincKernel) its wave's samples in
//   the order it plays them, its loops included. The interpolator takes them at the channel's place: the sample that
//   plays, and the share of the timer's period that has passed.
// - A rectangular wave or noise changes its level at each overflow of its timer. The changes go through band-limited
//   steps (wave::BandLimitedSteps).
// - A channel's volume, divider and pan, and the master volume, give gains in floating point. A changed gain moves to
//   its new value over kEnhancedFrameSamples samples. A channel that starts takes its gains at once.
// - Nothing adds the bias, cuts the fixed point's bits or clips the sum. The output is the sum on the scale of 16-bit
//   samples divided by 32,768, without a limit.
// - A channel that stops, and a one-shot wave at its end, ring out through the interpolator. The samples after the stop
//   are silent. With the hold flag, they hold the last sample.
// - The band-limited steps run wave::BandLimitedSteps::kHalfWidth samples late. The waves pass through a delay of as
//   many samples. The output therefore runs 32 samples behind the console's timing.
// - The captures record the mixer as in the console's mode.
//
// A run without output (Run(ticks)) in the enhanced mode moves the channels on without the mix. The next run with
// output starts the band-limited steps again at the channels' levels and the delay empty, and gives the gains their
// values at once.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/wave/adpcm.h"
#include "ntdlib/wave/blep.h"
#include "ntdlib/wave/sinc.h"

namespace ntdlib::ntr
{

// The ARM7's system clock, in Hz.
constexpr uint32_t kSystemClockHz = 33513982;

// A channel's timer counts once each 2 system clocks. The unit makes an output sample each 512 counts.
constexpr uint32_t kTicksPerSample = 512;

// The output's rate in Hz: one sample each 1024 system clocks.
constexpr double kOutputRate = kSystemClockHz / 1024.0;

// The enhanced mixing mode's samples in each frame of the driver (snd::kFrameClocks system clocks), and its rate in
// Hz: about 47,989.
constexpr uint32_t kEnhancedFrameTicks = 87296;
constexpr uint32_t kEnhancedFrameSamples = 250;
constexpr double kEnhancedRate = kSystemClockHz / 2.0 * kEnhancedFrameSamples / kEnhancedFrameTicks;

// The registers' addresses.
constexpr uint32_t kSoundRegisters = 0x04000400;
constexpr uint32_t kSoundRegistersEnd = 0x04000520;

// The registers of the channels past the console's 16 that a driver grows in the enhanced mixing mode
// (snd::Driver::GrowChannels). Channel n's lie at kGrownChannelRegisters + (n - 16) * kGrownChannelBytes, laid out as
// a channel's at kSoundRegisters. The word at kGrownChannelTwin then gives its twin: the console's channel whose part
// it takes. The console has no such registers.
constexpr uint32_t kGrownChannelRegisters = 0x04F00000;
constexpr uint32_t kGrownChannelBytes = 0x20;
constexpr uint32_t kGrownChannelTwin = 0x10;

// The channels' formats, by bits 29 and 30 of SOUNDxCNT.
enum class ChannelFormat
{
    kPcm8,
    kPcm16,
    kImaAdpcm,
    kPsg, // a rectangular wave on channels 8 to 13, noise on channels 14 and 15, and silence on the others
};

// The DS's IMA-ADPCM decoder's state after the nibble `nibble` (0 to 15) from `state`. The DS clamps a step down to
// -0x7FFF and a step up to 0x7FFF. A sample of -0x8000 from a block's first word therefore stays until a step moves
// it. An index past 88 steps by 0, and the next index comes from it clamped to 0 to 88.
wave::ImaAdpcmState AdpcmStep(wave::ImaAdpcmState state, uint8_t nibble);

// The decoder's state that a block of IMA-ADPCM's first word gives: the sample in bits 0 to 15, the index in bits 16
// to 22.
wave::ImaAdpcmState AdpcmHeader(uint32_t word);

// An access to the unit's registers.
struct RegisterAccess
{
    friend bool operator==(const RegisterAccess&, const RegisterAccess&) = default;

    uint32_t address = 0;
    int size = 0;
    uint32_t value = 0;
    bool write = false;
};

// One output sample: the left and the right side, from 0 to 0x3FF, with the bias as the middle.
struct SoundOutput
{
    friend bool operator==(const SoundOutput&, const SoundOutput&) = default;

    uint16_t left = 0;
    uint16_t right = 0;
};

// Appends `samples` to `out` as 16-bit values, the left and the right side in turn: the unit's 10 bits, less the bias,
// in the top bits. 0x200 gives 0, 0 gives -32768 and 0x3FF gives 32704.
inline void AppendPcm16(const std::vector<SoundOutput>& samples, std::vector<int16_t>& out)
{
    for (const SoundOutput& sample : samples)
    {
        out.push_back(static_cast<int16_t>((sample.left - 0x200) * 64));
        out.push_back(static_cast<int16_t>((sample.right - 0x200) * 64));
    }
}

class SoundUnit
{
public:
    // A unit that plays from `main_memory`, the memory at 0x02000000, and captures into it. The memory repeats up to
    // 0x02FFFFFF, every `main_memory.size()` bytes. Channels that play from another address read zeros, and captures
    // into another address write nothing. The memory must outlive the unit.
    explicit SoundUnit(std::span<uint8_t> main_memory);

    // Reads and writes a register: `size` bytes (1, 2 or 4) at `address`, from kSoundRegisters on. `address` is a
    // multiple of `size`. A channel starts when a write sets bit 31 of its SOUNDxCNT, and stops when a write clears it.
    // A capture starts and stops in the same way with bit 7 of its SNDCAPxCNT. The write-only registers read as 0.
    uint32_t Read(uint32_t address, int size) const;
    void Write(uint32_t address, int size, uint32_t value);

    // Runs the channels' and the captures' timers for `ticks` counts, and appends an output sample to `out` each time
    // the count passes a multiple of kTicksPerSample.
    void Run(uint64_t ticks, std::vector<SoundOutput>& out);

    // Runs the timers in the same way, without the output. The channels and the captures end in the same state, the
    // captures write the same samples, and the meters stay as they were.
    void Run(uint64_t ticks);

    // Turns the enhanced mixing mode on from the present tick. The mode stays on.
    void StartEnhanced();
    bool Enhanced() const;

    // Runs the timers in the enhanced mode, and appends the output samples whose times pass to `out`: the left and the
    // right side in turn. Sample n of the mode lies kEnhancedFrameTicks * n / kEnhancedFrameSamples ticks after
    // StartEnhanced. Throws std::logic_error outside the mode.
    void Run(uint64_t ticks, std::vector<float>& out);

    // The channels: the console's 16, and the channels that a driver has grown since (kGrownChannelRegisters).
    int ChannelCount() const;

    // A host's mutes and meters. The console has neither. The mixer leaves out the channels whose bits `channels` sets,
    // and so do the captures. The channels play on as before. The grown channels play unmuted.
    void SetMutedChannels(uint16_t channels);

    // Mutes in the same way each channel whose entry in `channels` isn't 0, a grown channel included. The channels
    // past the entries play unmuted.
    void SetMutedChannels(std::span<const uint8_t> channels);

    // Gives each channel a meter from 0 to 15, or none for -1, and sets each meter's peaks to 0. Each output sample
    // then adds up each meter's channels as the mixer adds them, and scales the sum by the master volume. A muted
    // channel reaches no meter. MeterPeaks gives each meter's peaks since the call, left and right: the largest
    // magnitude of its sum, on the scale of 16-bit samples (the output's 10 bits times 64). The meters change nothing
    // in the output.
    void SetMeters(const std::array<int8_t, 16>& meters);

    // Gives each channel the meter of its entry in `meters` in the same way, a grown channel included. The channels
    // past the entries reach no meter.
    void SetMeters(std::span<const int8_t> meters);
    const std::array<std::array<int32_t, 2>, 16>& MeterPeaks() const;

    // Whether channel `channel` plays: bit 31 of its SOUNDxCNT.
    bool Busy(int channel) const;

    // The counts since the last output sample, from 0 to kTicksPerSample - 1.
    uint32_t Phase() const;

    // Where a channel plays, for a length analysis: whether it's busy, whether what it plays repeats, and the place it
    // has reached. A wave's place is its sample, and the wave repeats when it loops. A rectangular wave repeats every 8
    // steps, and noise every 32,767. Their places count the steps of the cycle.
    struct ChannelPlace
    {
        bool playing = false;
        bool loops = false;
        uint32_t position = 0;
    };

    ChannelPlace PlaceOf(int channel) const;

private:
    static constexpr int kQueueWords = 8;

    struct Channel
    {
        bool busy = false;
        bool playing = false; // whether the output holds a sample of the wave. Off once a one-shot wave ends
        uint32_t counter = 0; // the timer: the channel steps when it reaches 0x10000
        int delay = 0;        // the timer's overflows until the channel plays its first sample
        uint32_t position = 0;
        int16_t sample = 0;
        wave::ImaAdpcmState adpcm = {};
        wave::ImaAdpcmState loop_adpcm = {};
        uint16_t noise = 0;
        // A wave's next words, in the order it plays them. The head holds the word of the current sample.
        std::array<uint32_t, kQueueWords> queue{};
        uint32_t head = 0;
        uint32_t fetch = 0; // the word that the queue reads next, counted from SOUNDxSAD
    };

    struct Capture
    {
        bool busy = false;
        uint32_t counter = 0; // the timer: the capture takes a sample when it reaches 0x10000
        uint32_t written = 0; // the bytes written since the buffer's start
    };

    // A channel's part of the enhanced mode's mix.
    struct Voice
    {
        bool sounding = false; // whether the voice's wave adds to the mix
        bool fresh = false;    // whether the gains take their values at once
        bool psg = false;      // whether the channel plays the PSG format

        // A wave's samples in the order the channel plays them, from sample `first` on. The samples before the wave's
        // first are 0. `decoded` is the next sample to decode, `wave_sample` its sample of the wave, and `adpcm` the
        // decoder's state before it. A sample from `end` on is silent, or `held` with the hold flag. `last` is the
        // sample decoded last.
        std::vector<float> input;
        int64_t first = 0;
        int64_t decoded = 0;
        uint32_t wave_sample = 0;
        wave::ImaAdpcmState adpcm = {};
        wave::ImaAdpcmState loop_adpcm = {};
        int64_t end = INT64_MAX;
        bool holds = false;
        float held = 0.0f;
        float last = 0.0f;

        // The timer's overflows since the start, and the overflows before the first sample.
        uint64_t overflows = 0;
        int delay = 0;

        // After a stop: the place at `stopped_at`, in kEnhancedFrameSamples-ths of a tick from StartEnhanced, and the
        // timer's period then.
        bool stopped = false;
        double place = 0.0;
        uint64_t stopped_at = 0;
        uint32_t period = 1;

        // The interpolator's kernel, for the timer's period `kernel_period`.
        const wave::SincKernel* kernel = nullptr;
        uint32_t kernel_period = 0;

        // A rectangular wave's or noise's level as the steps last took it, and the steps. Idle steps hold 0 from
        // sample `settled` on, and aren't read.
        float level = 0.0f;
        std::optional<wave::BandLimitedSteps> steps;
        bool idle = false;
        uint64_t settled = 0;
        std::vector<std::array<float, 2>> run_gains; // the gains of each sample of the run, for the steps

        // The gains on the left and the right, their targets, the ramp's step and the samples left in the ramp.
        std::array<float, 2> gains = {};
        std::array<float, 2> targets = {};
        std::array<float, 2> slopes = {};
        uint32_t ramp = 0;
    };

    // The enhanced mode's state.
    struct EnhancedState
    {
        uint64_t ticks = 0;   // the ticks since StartEnhanced
        uint64_t samples = 0; // the samples whose times have passed
        bool stale = true;    // whether the last run had no output
        std::vector<Voice> voices;
        float master = 0.0f;
        float master_target = 0.0f;
        float master_slope = 0.0f;
        uint32_t master_ramp = 0;
        bool master_fresh = true;

        // The run with output: its first sample, and for each sample the waves' sums (the mixer's, channel 1's and
        // channel 3's, left and right), the master volume and the meters' sums.
        uint64_t run_first = 0;
        std::vector<std::array<float, 6>> run_waves;
        std::vector<float> run_master;
        std::vector<std::array<float, 32>> run_meters;

        // The waves' sums of the last wave::BandLimitedSteps::kHalfWidth samples.
        std::array<std::vector<float>, 6> delayed;
    };

    uint32_t Control(int channel) const;
    uint32_t ChannelWord(int channel, uint32_t offset) const;
    uint8_t* ChannelBytes(int channel);
    int Twin(int channel) const;
    bool Muted(int channel) const;
    uint32_t Word(uint32_t offset) const;
    uint8_t Byte(uint32_t address) const;
    uint32_t MemoryWord(uint32_t address) const;
    void WriteByte(uint32_t address, uint8_t value);
    void WriteGrown(uint32_t address, int size, uint32_t value);
    void Grow(int channels);
    void Start(int channel);
    void Stop(int channel);
    void Step(int channel);
    void StepWave(int channel, uint32_t control, bool advanced);
    int16_t WaveSample(int channel, uint32_t control);
    void NextWord(int channel);
    uint32_t Fetch(int channel);
    void StepCapture(int capture);
    void Advance(uint64_t ticks, std::vector<SoundOutput>* out, std::vector<float>* enhanced_out);
    bool Sides(int channel, std::array<int32_t, 2>& sides) const;
    std::array<int32_t, 2> Mix();

    // The enhanced mode's parts. Times `at` and `now` count kEnhancedFrameSamples-ths of a tick from StartEnhanced.
    void StartVoice(int channel);
    void StopVoice(int channel, uint64_t at, uint64_t now);
    void StepLevel(int channel, uint64_t tick);
    void Overflowed(int channel, uint64_t tick, uint32_t late);
    uint32_t Period(int channel) const;
    double VoicePlace(int channel, uint64_t at, uint64_t now) const;
    float WaveAt(int channel, double place);
    float DecodeNext(int channel);
    std::array<float, 2> Gains(int channel) const;
    void BeginEnhanced();
    void MixDue(bool output);
    void MixEnhanced();
    void FinishEnhanced(std::vector<float>& out);

    std::span<uint8_t> main_memory_;
    std::array<uint8_t, kSoundRegistersEnd - kSoundRegisters> registers_{};
    std::vector<std::array<uint8_t, kGrownChannelBytes>> grown_registers_;
    std::vector<Channel> channels_ = std::vector<Channel>(16);
    std::array<Capture, 2> captures_{};
    uint32_t phase_ = 0;
    std::vector<uint8_t> muted_ = std::vector<uint8_t>(16);
    bool metered_ = false;
    std::vector<int8_t> meters_ = std::vector<int8_t>(16, -1);
    std::array<std::array<int32_t, 2>, 16> peaks_{};
    std::optional<EnhancedState> enhanced_;
};

} // namespace ntdlib::ntr
