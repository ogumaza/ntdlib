// SPDX-License-Identifier: MIT

// A model of the GBA's sound hardware: the four PSG channels of the Game Boy's sound, the two DirectSound FIFOs that
// the timers empty, and the output stage. GBATEK describes the GBA's parts, and Pan Docs the Game Boy's channels. The
// model runs on the CPU's cycles. Stores to the sound registers and samples for the FIFOs come in between runs.
//
// A PSG channel gives a level of 0 to 15 at each moment, and 0 while it's off. The levels of a side's channels add up,
// times the side's master volume plus 1, times 2, times the PSG's share (25%, 50% or 100%). A FIFO's sample counts 2 or
// 4 times (50% or 100%). The sum on each side, plus the bias, clips to the 10-bit range of 0 to 1023. The output keeps
// the top bits that the sound bias register's resolution allows: 9 at 32,768 Hz, 8 at 65,536 Hz, 7 and 6 at the higher
// rates. The model gives each output sample less the bias, times 64, as a 16-bit sample. A channel at level 15 with
// master volume 7 and the PSG's share at 100% then spans 240 of the 1,024 steps. GBATEK gives each PSG channel a
// quarter of the range, and each FIFO the whole range. mGBA 0.11.0 gives the same steps for the PSG and the FIFOs.
//
// The PSG and the FIFOs are sampled where each output sample falls. The rates above 65,536 Hz put out their samples at
// 65,536 Hz, with their resolution.
//
// In the enhanced mixing mode (StartEnhanced), the model keeps the PSG's channels with their timers, lengths, sweep and
// envelopes, and lifts the limits of the output stage. The output comes in floating point, kEnhancedFrameSamples
// samples for each frame of kFrameCycles cycles:
//
// - Each PSG channel's level changes at the cycle where its timer steps. The changes go through band-limited steps
//   (wave::BandLimitedSteps) in place of the sampling at the output's rate. A side's channels add up at their volumes
//   without the shift's rounding: each step of a channel's level counts 128 * (master volume + 1) >> share steps of a
//   16-bit sample.
// - The FIFOs take their samples at the enhanced mode's rate, in floating point, on the scale of their 8-bit samples
//   (QueueEnhanced). Each output sample takes one from each FIFO, at 2 or 4 times as in the console's mode.
// - Nothing adds the bias, clips the sum or keeps the resolution's bits. The output is the sum on the scale of 16-bit
//   samples divided by 32,768, without a limit.
// - The band-limited steps run wave::BandLimitedSteps::kHalfWidth samples late. Each FIFO starts with as many samples
//   of silence. The output therefore runs 32 samples behind the console's timing.
//
// A run without output (Run(cycles)) in the enhanced mode moves the PSG's channels on without the band-limited steps.
// The FIFOs drop the samples of the time that passes. The next run with output starts the steps again at the levels of
// its start.
//
// A driver that grows its channels (mp2k::Sequencer::GrowChannels) gives the model PSG units past the console's one,
// at kGrownPsgRegisters. A store there makes the unit. A grown unit plays its four channels as the console's PSG plays
// them, at the frame sequencer's steps. Its NR51 sends its channels to the sides. The console's NR50, SOUNDCNT_H and
// SOUNDCNT_X give its volumes, its share and its power. Its other registers, NR50 and those past NR51 up to the wave
// RAM, do nothing.

#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/wave/blep.h"

namespace ntdlib::agb
{

// The output's rate at the sound bias register's default resolution of 9 bits.
constexpr int kBaseOutputRate = 32768;

// The highest rate that the model puts out.
constexpr int kMaxOutputRate = 65536;

// The cycles between steps of the frame sequencer. The sequencer steps the length counters at 256 Hz, the sweep at
// 128 Hz and the envelopes at 64 Hz.
constexpr uint32_t kSequencerCycles = 32768;

// The bits of the channels for Apu::SetMuted().
constexpr uint8_t kApuSquare1 = 0x01;
constexpr uint8_t kApuSquare2 = 0x02;
constexpr uint8_t kApuWave = 0x04;
constexpr uint8_t kApuNoise = 0x08;
constexpr uint8_t kApuFifoA = 0x10;
constexpr uint8_t kApuFifoB = 0x20;

class Apu
{
public:
    // The registers as the BIOS leaves them: the bias at 0x200 and the rest 0. The sound is off until a store to
    // SOUNDCNT_X turns it on.
    Apu();

    // The registers from kRegSound1CntL up to kRegFifoA as `registers` holds them, with every channel off. The sound
    // is on when SOUNDCNT_X's bit 7 is set. The stores of a game's init leave the registers in such a state. Throws
    // std::invalid_argument unless `registers` holds kSoundRegisterBytes bytes.
    explicit Apu(std::span<const uint8_t> registers);

    // Stores the `size` bytes of `value` from `address` in the CPU's order: the lowest first. Each byte from
    // kRegSound1CntL up to kRegFifoA takes effect at the model's present cycle. The model ignores other bytes. The
    // FIFOs take their samples through Queue().
    void Store(uint32_t address, int size, uint32_t value);

    // Returns the byte that the last store left at `address`, from kRegSound1CntL up to kRegFifoA. SOUNDCNT_X's bits 0
    // to 3 give the channels that play. The wave RAM reads the bank that the CPU reaches.
    uint8_t Register(uint32_t address) const;

    // Sets timer `timer` (0 or 1) to overflow every `period` cycles, the first time `first` cycles from now. A period
    // of 0 stops it. Each overflow takes a sample from each FIFO that SOUNDCNT_H puts on the timer.
    void SetTimer(int timer, uint32_t period, uint32_t first);

    // Adds samples to FIFO `fifo` (0 for A, 1 for B). On the GBA, the DMA adds them. The model's FIFOs have no limit.
    // An empty FIFO goes on with its last sample.
    void Queue(int fifo, std::span<const int8_t> samples);

    // Puts the model in the enhanced mixing mode from its present cycle on. The FIFOs' samples in the console's mode
    // stay, and no longer reach the output.
    void StartEnhanced();
    bool Enhanced() const
    {
        return enhanced_.has_value();
    }

    // In the enhanced mode, adds samples to FIFO `fifo` at the enhanced mode's rate, on the scale of 8-bit samples.
    // An empty FIFO gives 0. Throws std::logic_error in the console's mode.
    void QueueEnhanced(int fifo, std::span<const float> samples);

    // Returns the number of samples that FIFO `fifo` holds.
    size_t Queued(int fifo) const
    {
        return fifos_[size_t(fifo)].samples.size();
    }

    // Leaves the channels in `mask` (kApuSquare1 to kApuFifoB) out of the output. Their state goes on as before.
    void SetMuted(uint8_t mask);

    // Leaves the channels of the grown PSG units out of the output in the same way: entry u - 1 of `units` gives unit
    // u's channels, kApuSquare1 to kApuNoise. The units past the entries play unmuted.
    void SetMutedUnits(std::span<const uint8_t> units);

    // The PSG units: the console's, and the grown units that stores have made since.
    int PsgUnits() const
    {
        return int(psgs_.size());
    }

    // Sets the frame sequencer's phase: its step `step` (0 to 7) comes `cycles` (1 to kSequencerCycles) from now. When
    // the sound is switched on, step 0 comes kSequencerCycles later. The phase at a song's start depends on when the
    // game switched the sound on. Throws std::invalid_argument for a step or a number of cycles out of range.
    void SetSequencer(int step, uint32_t cycles);

    // Returns the output's rate in Hz, from the sound bias register's resolution.
    int Rate() const;

    // Runs the hardware for `cycles` of the CPU's cycles, and adds the output samples that fall in them to `out`, left
    // then right. Throws std::logic_error in the enhanced mode.
    void Run(uint64_t cycles, std::vector<int16_t>& out);

    // In the enhanced mode, runs the hardware for `cycles` of the CPU's cycles, and adds the samples whose times lie
    // before the run's end to `out`, left then right. Throws std::logic_error in the console's mode.
    void Run(uint64_t cycles, std::vector<float>& out);

    // Runs the hardware in the same way without the output samples. The model ends in the state that Run(cycles, out)
    // leaves. The channels' peaks stay as they were.
    void Run(uint64_t cycles);

    // From the next run with output on, keeps each channel's peaks when `on`: the largest part that the channel
    // gives an output sample on each side, on the scale of 16-bit samples. A channel's part is its level, or the
    // FIFO's sample, at the output's volumes, before the bias, the clipping and the resolution. A muted channel's part
    // is 0. In the enhanced mode, a PSG channel's part is its level at the enhanced mode's volumes. ClearPeaks() sets
    // the peaks to 0.
    void MeterChannels(bool on)
    {
        metering_ = on;
    }
    void ClearPeaks()
    {
        peaks_ = {};
        for (Psg& psg : psgs_)
        {
            psg.peaks = {};
        }
    }

    // The peaks of channel `channel` (0 to 5: the PSG's four, then FIFO A and B), left then right.
    std::array<int32_t, 2> ChannelPeaks(int channel) const
    {
        return peaks_[size_t(channel)];
    }

    // The peaks of channel `channel` (0 to 3) of grown PSG unit `unit`, left then right.
    std::array<int32_t, 2> UnitPeaks(int unit, int channel) const
    {
        return psgs_[size_t(unit)].peaks[size_t(channel)];
    }

    // Returns the cycles that the model has run.
    uint64_t Cycle() const
    {
        return cycle_;
    }

private:
    struct Envelope
    {
        uint8_t level = 0;
        uint8_t period = 0;
        bool up = false;
        uint8_t ticks = 0;
    };

    struct Square
    {
        bool on = false;
        uint16_t frequency = 0;
        uint8_t duty = 0;
        uint8_t position = 0;   // the step of the duty's 8
        uint32_t countdown = 0; // cycles until the next step
        uint16_t length = 0;
        Envelope envelope;
    };

    struct Wave
    {
        bool on = false;
        uint16_t frequency = 0;
        uint8_t position = 0; // the digit of the 32 or 64
        uint32_t countdown = 0;
        uint16_t length = 0;
        uint8_t held = 0; // the level that a stopped channel holds
        std::array<std::array<uint8_t, 16>, 2> banks{};
    };

    struct Noise
    {
        bool on = false;
        uint16_t lfsr = 0;
        // The counter's last carry. GBATEK's output is high with a carry. That's the Game Boy's analogue output. The
        // output falls as the level rises. The channel gives its level without a carry. mGBA 0.11.0 does the same.
        bool carry = false;
        uint32_t countdown = 0;
        uint16_t length = 0;
        Envelope envelope;
    };

    struct Sweep
    {
        bool on = false;
        uint16_t shadow = 0;
        uint8_t ticks = 0;
    };

    struct Fifo
    {
        std::deque<int8_t> samples;
        int8_t output = 0;
    };

    struct Timer
    {
        uint32_t period = 0;
        uint32_t countdown = 0;
    };

    // A PSG unit: its registers, laid out from kRegSound1CntL, and its channels. The console's unit holds the console's
    // sound registers.
    struct Psg
    {
        std::array<uint8_t, kSoundRegisterBytes> registers{};
        std::array<Square, 2> squares;
        Wave wave;
        Noise noise;
        Sweep sweep;
        uint8_t muted = 0;                             // a grown unit's mutes (SetMutedUnits)
        std::array<std::array<int32_t, 2>, 4> peaks{}; // a grown unit's peaks
    };

    // The enhanced mode's state.
    struct EnhancedState
    {
        EnhancedState();

        std::array<wave::BandLimitedSteps, 2> steps; // the PSG's output on the left, then the right
        std::array<std::deque<float>, 2> fifos;
        // Each unit's PSG channels' levels that the steps hold, and each side's steps of a 16-bit sample for each
        // channel's level.
        std::vector<std::array<int, 4>> levels;
        std::vector<std::array<std::array<int32_t, 4>, 2>> gains;
        uint64_t samples = 0; // the samples whose times lie before the present cycle
        bool stale = false;   // a run without output left the steps behind the channels
    };

    // The console's sound registers.
    std::array<uint8_t, kSoundRegisterBytes>& Registers()
    {
        return psgs_[0].registers;
    }
    const std::array<uint8_t, kSoundRegisterBytes>& Registers() const
    {
        return psgs_[0].registers;
    }

    // Unit `unit`'s mutes, and its peak of channel `channel` on side `side`.
    uint8_t UnitMuted(size_t unit) const;
    int32_t& Peak(size_t unit, int channel, size_t side);

    // Makes the grown units up to `unit`.
    void GrowUnits(size_t unit);

    void StoreByte(Psg& psg, bool console, uint32_t offset, uint8_t value);
    void PowerOff();
    void StartSquare(Psg& psg, int n);
    void StartWave(Psg& psg);
    void StopWave(Psg& psg);
    void StartNoise(Psg& psg);
    static uint16_t SweepTarget(const Psg& psg);
    void StepSequencer();
    void StepUnit(Psg& psg);
    void OverflowTimers();
    void Advance(Psg& psg, uint32_t cycles);
    void AdvanceEnhanced(size_t unit, uint32_t cycles);
    void RunCycles(uint64_t cycles, std::vector<int16_t>* out, std::vector<float>* enhanced_out);
    static int Level(const Psg& psg, int channel);
    int16_t Output(int side);
    void SetEnhancedGains();
    void SettleEnhanced();
    void ChangeLevel(size_t unit, int channel, uint64_t cycle);
    void TakeEnhancedSamples(std::vector<float>* out);

    std::vector<Psg> psgs_ = std::vector<Psg>(1); // the console's unit, then the grown units
    std::array<Fifo, 2> fifos_;
    std::array<Timer, 2> timers_;
    uint8_t muted_ = 0;
    bool metering_ = false;
    std::array<std::array<int32_t, 2>, 6> peaks_ = {};
    uint8_t step_ = 0;                      // the frame sequencer's next step, 0 to 7
    uint32_t sequencer_ = kSequencerCycles; // cycles until the frame sequencer's next step
    uint32_t until_output_ = 0;             // cycles until the next output sample
    uint64_t cycle_ = 0;
    std::optional<EnhancedState> enhanced_;
};

} // namespace ntdlib::agb
