// SPDX-License-Identifier: MIT

// The DSP as the ARM11 sees it while the audio firmware runs: its pipes, the semaphore towards it, its interrupts, DSP
// RAM and its audio output. Two devices give it. TeakraDsp (teakra_dsp.h) runs a firmware image on Teakra.
// FirmwareModel (firmware_model.h) models the audio firmware without an image, and also has an enhanced mixing mode.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "ntdlib/ctr/dsp/dsp.h"

namespace ntdlib::ctr::dsp
{

// A copy of a device's state, from that device's SaveState.
class DeviceState
{
public:
    virtual ~DeviceState() = default;
};

class Device
{
public:
    using InterruptHandler = std::function<void(Interrupt interrupt, Pipe pipe)>;
    using SampleSink = std::function<void(std::array<int16_t, 2> sample)>;
    using FrameSink = std::function<void(std::span<const float> frame)>;

    virtual ~Device() = default;

    // Sets bits of the ARM11's semaphore towards the DSP.
    virtual void SetSemaphore(uint16_t bits) = 0;

    // Returns the number of bytes waiting in the DSP-to-ARM11 ring of `pipe`.
    virtual std::size_t PipeReadable(Pipe pipe) const = 0;

    // Reads `size` bytes from the DSP-to-ARM11 ring of `pipe`, then tells the firmware. Only the low 16 bits of `size`
    // count, as in the dsp::DSP service. Throws if fewer bytes are waiting.
    virtual std::vector<uint8_t> ReadPipe(Pipe pipe, std::size_t size) = 0;

    // Writes `data` to the ARM11-to-DSP ring of `pipe`, then tells the firmware. Throws if the ring fills up first.
    virtual void WritePipe(Pipe pipe, std::span<const uint8_t> data) = 0;

    // Sets the function that gets the DSP's interrupts while the firmware runs. It's called while the DSP runs, and it
    // may replace itself.
    virtual void SetInterruptHandler(InterruptHandler handler) = 0;

    // Sets the function that gets each stereo sample of the DSP's audio output.
    virtual void SetSampleSink(SampleSink sink) = 0;

    // Starts the audio firmware from a DSP1 image. Does nothing if the firmware is already running.
    virtual void LoadComponent(std::span<const uint8_t> image) = 0;

    // Returns DSP RAM, kRamSize bytes of program memory and then data memory. It stays the same for the object's
    // lifetime.
    virtual uint8_t* Ram() = 0;

    // Runs the DSP for `cycles` cycles.
    virtual void Run(uint64_t cycles) = 0;

    // Returns the DSP cycles that have passed.
    virtual uint64_t Cycles() const = 0;

    // Takes a copy of the device's state, sharing what hasn't changed since `previous` (from the same kind of device)
    // where it can. Must not be called during a DSP run.
    virtual std::shared_ptr<const DeviceState> SaveState(const DeviceState* previous) = 0;

    // Restores a state from SaveState of the same kind of device. Throws if it comes from another kind. Must not be
    // called during a DSP run.
    virtual void RestoreState(const DeviceState& state) = 0;

    // A host's meters. The firmware has no such meters, and a device without them gives 0. Each of the `count` meters
    // adds up its sources' mix into the main bus, front and rear, left and right. The stereo output takes the bus's
    // channels in the same way.
    // SetSourceMeter gives source `source` a meter, or none for -1. MeterPeaks gives the peaks of a meter's left and
    // right in the last frame that the DSP mixed: the largest magnitude of each sum, on the scale of the output's
    // samples. The meters change nothing in the output, and they aren't part of a state.
    virtual void SetMeters(int /*count*/)
    {
    }

    virtual void SetSourceMeter(int /*source*/, int /*meter*/)
    {
    }

    virtual std::array<int32_t, 2> MeterPeaks(int /*meter*/) const
    {
        return {0, 0};
    }

    // The final samples of the last frame that the DSP mixed, left and right in turn, before they wait for the output.
    // A device that doesn't give them returns nothing.
    virtual std::span<const int16_t> MixedFrame() const
    {
        return {};
    }

    // Only the model of the firmware (FirmwareModel) has the enhanced mixing mode. The device then mixes more sources
    // than the firmware's kSources, and the sources' structures lie outside DSP RAM.
    // SourceCount gives the device's sources, and MaxSources the most that GrowSources can give it. A new source starts
    // as the firmware's sources start. SourceStructure gives the bytes of structure `structure` in region `region` for
    // every source, in the firmware's layout, or nullptr where the structure lies in DSP RAM. A structure is a place in
    // the list of addresses that the firmware sends: 1 for the sources' parameters, 2 for their reports and 3 for their
    // ADPCM coefficients. The bytes move when the device grows. None of this may be called during a DSP run.
    virtual int SourceCount() const
    {
        return kSources;
    }

    virtual int MaxSources() const
    {
        return SourceCount();
    }

    virtual void GrowSources(int /*count*/)
    {
    }

    virtual uint8_t* SourceStructure(int /*structure*/, int /*region*/)
    {
        return nullptr;
    }

    // In the enhanced mode, the aux buses' samples of region `region` lie outside DSP RAM too: bus A's front left,
    // front right, rear left and rear right channels of kEnhancedFrameSamples samples, and then bus B's. The firmware's
    // mixing keeps them in DSP RAM, in frames of kSamplesPerFrame, and gives nullptr here. The same rules hold as for
    // SourceStructure.
    virtual int32_t* AuxStructure(int /*region*/)
    {
        return nullptr;
    }

    // Sets the function that gets each frame of the enhanced mode's output as the DSP mixes it: kEnhancedFrameSamples
    // stereo samples, left and right in turn, with full scale at 1. A device without the mode never calls it.
    virtual void SetFrameSink(FrameSink /*sink*/)
    {
    }
};

} // namespace ntdlib::ctr::dsp
