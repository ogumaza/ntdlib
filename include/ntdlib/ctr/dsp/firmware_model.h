// SPDX-License-Identifier: MIT

// A model of the 3DS's DSP audio firmware, as Pokémon X's dspaudio.cdc builds it: what the firmware does for nn::snd
// with each frame of 160 samples, without a firmware image and without Teakra. It gives the same output samples as
// Teakra running the firmware, and writes the same words of the shared structures that nn::snd reads back: the frame
// counters, the sources' reports and the parameters' dirty bits, the status, the final samples and the aux buses.
//
// The model keeps the firmware's data memory in Ram(), at the firmware's addresses, and each routine works on it as the
// firmware does. The arithmetic follows Teak's: 40-bit accumulators, products shifted by the product shift mode, and
// saturation where the firmware turns it on. The wave data comes from FCRAM as the firmware's DMA reads it.
//
// The firmware runs its audio thread from interrupts: the output's FIFO, which asks for 8 more samples each time it
// runs empty, and a semaphore from the ARM11. The model runs the output's FIFO cycle for cycle. It does each step of
// the thread's work at once, at the time that the firmware starts the step on Teakra. On Teakra, each step takes time:
// a frame's mix takes up to about 340,000 cycles, more with more voices. The model's interrupt that ends a frame comes
// up to about 20,000 cycles from Teakra's. nn::snd reads the same words all the same, and each sample comes out of the
// FIFO in the same place in the output.
//
// The model covers what nn::snd drives through DspLink: the start-up handshake, the frames, every source's formats,
// interpolations, filters and mixes, the aux buses, the mono and stereo output modes, the clipping modes and the master
// volume. It differs from the firmware in these places:
// - The surround mode of the output (mode 2) mixes the output as in stereo. The mixer's dirty bit 30, which loads the
//   surround's preset filters from the firmware's program memory, does nothing.
// - A source in the fourth encoding, which the firmware never comes back from, stays silent.
// - The firmware's stop and sleep commands, its fade of the output and its built-in effects of the aux buses aren't
//   modelled. The mixer's dirty bits 10 to 13, which set up those effects, do nothing.
// - The model has only the audio pipe. The debug pipe (0) and the binary pipe (3), which takes the codec's requests,
//   carry nothing.
// - A frame whose mix runs into the next frame's refill, and a poll that runs for more than a few frames, take only
//   about the firmware's times.
//
// The model follows the build of the firmware in Pokémon X's dspaudio.cdc. Ocarina of Time 3D's code holds a build of
// 2011. Its output comes a DSP frame later and inverted.
//
// In the enhanced mixing mode (`enhanced`), the model keeps the firmware's steps through each source's buffers, its
// reports and its times. It lifts the limits of the firmware's mixing:
//
// - The model mixes up to kEnhancedSources sources. It starts with the firmware's 24, and GrowSources adds more. The
//   sources' parameters, reports and ADPCM coefficients lie outside DSP RAM (SourceStructure).
// - The output runs at 48 kHz: 235 stereo samples in each frame of the firmware's 160, about 48,070 Hz. The frame sink
//   gets each frame as the model mixes it. The firmware's output then stays silent: the sample sink gets zeros.
// - A source's samples come from the firmware's decoders, in the firmware's steps through its buffers. Each output
//   comes from the band-limited interpolator (wave::SincKernel). It lies where the firmware's resampler would take an
//   output: between the third and second samples before the frame's first new sample, moved on by the source's phase
//   and rate. The interpolator's later inputs come from a copy of the source. A source that stops gives zeros after its
//   end.
// - The filters and the gains work in single-precision floating point, on the scale of 16-bit samples. The one-pole
//   takes b0 and a1 as 1.15, and the biquad its coefficients as 2.14. nw::snd works out the coefficients for 48 kHz in
//   the mode (ntdlib/ctr/snd/engine.h).
// - Where the firmware ramps a gain across a frame, sample i takes from + i * ((to - from) / 235), from the gain before
//   the frame to the new one. Otherwise each sample takes the new gain.
// - The output is the main bus, with the rear at the rear ratio, in mono or in stereo, at the master volume and divided
//   by 32,768. Neither the limiter nor the clipping applies. The surround mode mixes as stereo. The console's mode does
//   the same in the model.
// - The aux buses mix in floating point too, in frames of 235 samples. The ARM11 gets each region's buses outside DSP
//   RAM (AuxStructure), as 32-bit samples on the scale of the firmware's sums: each one rounded to the nearest integer,
//   halves away from zero. The buses that the ARM11 gives back join the main bus at the return volume two frames after
//   their mix, as in the firmware. A bus's bypass changes nothing. It matters only to the surround mode, which mixes
//   as stereo here.
// - The declick of the sources that stop isn't mixed. nn::snd's archive mode turns it off. A two-channel source stays
//   silent. The firmware's mono and stereo outputs leave it out too.
//
// Each operation takes its operands in a fixed order, and every platform gives the same samples.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "ntdlib/ctr/dsp/device.h"
#include "ntdlib/ctr/dsp/dsp.h"
#include "ntdlib/ctr/dsp/fcram.h"

namespace ntdlib::ctr::dsp
{

namespace firmware
{
class Audio;
} // namespace firmware

class FirmwareModel : public Device
{
public:
    // `fcram` outlives the object. With `enhanced`, the model runs in the enhanced mixing mode.
    explicit FirmwareModel(Fcram& fcram, bool enhanced = false);
    ~FirmwareModel() override;

    FirmwareModel(const FirmwareModel&) = delete;
    FirmwareModel& operator=(const FirmwareModel&) = delete;

    void SetSemaphore(uint16_t bits) override;
    std::size_t PipeReadable(Pipe pipe) const override;
    std::vector<uint8_t> ReadPipe(Pipe pipe, std::size_t size) override;
    void WritePipe(Pipe pipe, std::span<const uint8_t> data) override;
    void SetInterruptHandler(InterruptHandler handler) override;
    void SetSampleSink(SampleSink sink) override;
    void SetFrameSink(FrameSink sink) override;

    // The enhanced mixing mode, and its sources (see Device).
    bool Enhanced() const;
    int SourceCount() const override;
    int MaxSources() const override;
    void GrowSources(int count) override;
    uint8_t* SourceStructure(int structure, int region) override;
    int32_t* AuxStructure(int region) override;

    // Starts the model of the firmware. The image isn't used: the model needs none. Does nothing if the model already
    // runs.
    void LoadComponent(std::span<const uint8_t> image) override;

    uint8_t* Ram() override;
    void Run(uint64_t cycles) override;
    uint64_t Cycles() const override;
    std::shared_ptr<const DeviceState> SaveState(const DeviceState* previous) override;
    void RestoreState(const DeviceState& state) override;

    // The host's meters, and the frame that the model mixed last (see Device).
    void SetMeters(int count) override;
    void SetSourceMeter(int source, int meter) override;
    std::array<int32_t, 2> MeterPeaks(int meter) const override;
    std::span<const int16_t> MixedFrame() const override;

    // What the model keeps besides DSP RAM: the times of the firmware's pending work, the output's FIFO, the pipes and
    // the audio thread's state.
    struct Schedule;

private:
    void RunTo(uint64_t end);
    void Emit(uint64_t time);
    void Refill(uint64_t time);
    void TakeCommand(uint64_t time);
    void SendStartUp(bool addresses);
    void HandleSemaphore(uint64_t time);
    void ResumeFromPoll(uint64_t time);
    void PollTimer(uint64_t time);
    void RunLoop(uint64_t time);
    void SendCommand(uint16_t value);
    void Raise(Interrupt interrupt, Pipe pipe);

    std::vector<uint8_t> ram_;
    std::unique_ptr<firmware::Audio> audio_;
    std::unique_ptr<Schedule> schedule_;
    InterruptHandler interrupt_handler_;
    SampleSink sample_sink_;
    FrameSink frame_sink_;
};

} // namespace ntdlib::ctr::dsp
