// SPDX-License-Identifier: MIT

// AX's renderer, the part that the DSP does on the console. The system's sound library uploads a microcode to the DSP
// for it. The model follows the microcode's arithmetic step by step. The microcode is sndcore2's 32 kHz image in
// version 15702 of the OS (0x1240 bytes at 0x10007D20). The addresses in these files are in that image. All of it is
// integer arithmetic. A voice's samples stay 16-bit through the envelope and the filters. The buses hold 32-bit sums.
//
// Each frame, the DSP renders the running voices into the devices' buses (0x01E5). The accelerator fetches and decodes
// a voice's samples. The microcode converts them to the renderer's rate (see resample.h) and applies the volume
// envelope, the low-pass filter and the biquad filter, in that order. The microcode then adds the samples into each bus
// and channel that the voice's masks name. After the voices, the DSP adds each aux bus's return into the device's main
// bus (0x042A). Last, the DSP scales each device's main bus by the device's volume (0x0517). The envelope and a mix's
// volume start at their old values with the first sample. The aux returns and the devices' volumes reach their new
// values with the last sample.
//
// sndcore2's 48 kHz renderer has a microcode of its own. That microcode renders a frame in two halves. The model
// renders a 48 kHz frame with the 32 kHz microcode's arithmetic. The aux returns and the devices' volumes ramp by the
// 48 kHz microcode's step.
//
// In the enhanced mixing mode (System::InitEnhanced), the renderer keeps the console's voices, steps, gains and
// effects, and lifts the limits of its arithmetic:
//
// - The renderer runs at 48 kHz, in frames of 144 samples.
// - A voice reads its samples as the accelerator reads them, at the console's steps. Each output comes from the
//   band-limited interpolator (wave::SincKernel) at the console's place: between the third and second newest samples
//   for the 4-tap filters, and between the fourth and third newest for linear interpolation. A voice without a
//   conversion steps two thirds of a sample at a time, from its newest sample. Its samples then play at 32 kHz, as the
//   32 kHz renderer plays them. The interpolator's later inputs come from a copy of the voice's accelerator. A voice
//   that stops gives zeros after its end.
// - The envelope, the filters and the mixes work in single-precision floating point, on the scale of 16-bit samples.
//   Nothing holds a sample to 16 bits or a sum to 32 bits. The low-pass takes a0 as unsigned 1.15 and b0 as signed
//   1.15. The biquad takes its coefficients as 2.14.
// - The envelope and each mix ramp across the frame from their gains at the end of the last frame to their gains at
//   the end of this one: sample i takes from + i * ((to - from) / 144). A gain at the end of a frame is the block's
//   volume after the frame, as the console steps it. A voice that starts takes its gains at once.
// - A host's mute ramps across the frame in equal steps, and the last sample takes the new gain.
//
// System gives the rules of the aux buses and the output in the enhanced mode. Each operation takes its operands in a
// fixed order, and every platform gives the same samples.

#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "ntdlib/cafe/ax/cemu_departures.h"
#include "ntdlib/cafe/ax/memory.h"
#include "ntdlib/cafe/ax/pb.h"

namespace ntdlib::cafe::ax
{

// One frame of the devices' buses as the DSP keeps them: 32-bit samples in 16-bit scale, indexed
// [bus][channel][sample].
struct Buses
{
    using Channel = std::array<int32_t, kMaxSamplesPerFrame>;

    std::array<std::array<Channel, kTvChannels>, kBusCount> tv;
    std::array<std::array<std::array<Channel, kDrcChannels>, kBusCount>, kDrcCount> drc;
};

// One frame of the devices' buses in the enhanced mode, indexed as Buses: floats on the scale of 16-bit samples.
struct FloatBuses
{
    using Channel = std::array<float, kMaxSamplesPerFrame>;

    std::array<std::array<Channel, kTvChannels>, kBusCount> tv;
    std::array<std::array<std::array<Channel, kDrcChannels>, kBusCount>, kDrcCount> drc;
};

// What a host adds to a voice's mix. AX has neither part (see System::SetVoiceMuted and System::SetVoiceMeter). A
// muted voice's mix into every bus is silent. Between frames with and without the mute, a gain ramps the mix across the
// frame in equal steps. A meter's sums of left and right take the voice's mix into the TV's main bus, front left and
// right. The enhanced mode adds them up in `float_meter`.
struct HostMix
{
    bool was_muted = false; // the mix starts the frame silent
    bool muted = false;     // the mix ends the frame silent
    std::array<Buses::Channel, 2>* meter = nullptr;
    std::array<FloatBuses::Channel, 2>* float_meter = nullptr;
};

// A voice's state between frames in the enhanced mode: whether it starts, the gains that its ramps reached, the
// filters' last values and the samples that it read last.
struct EnhancedVoice
{
    // The samples that an output can take before the newest: the widest kernel's left side, at the farthest place.
    static constexpr int kHistory = 132;

    bool starts = true; // the next frame takes the gains at once
    float ve = 0.0f;
    std::array<std::array<float, kBusCount>, kTvChannels> tv{};
    std::array<std::array<std::array<float, kBusCount>, kDrcChannels>, kDrcCount> drc{};
    float lpf = 0.0f;                        // the low-pass's last output
    std::array<float, 4> biquad{};           // the biquad's last two inputs, then its last two outputs
    std::array<int16_t, kHistory> history{}; // oldest first
};

// Renders `samples` samples of the voice whose renderer's copy is `pb` into `buses`. `pb` is updated as the voice
// plays. A voice that isn't running at the start of the frame is skipped. Its block keeps its values. `cemu` makes the
// renderer do what Cemu does in the places where Cemu departs from the console (cemu_departures.h). `host` changes
// nothing in `pb`. Without a mute, it changes nothing in `buses` either.
void RenderVoice(Pb& pb, const Memory& memory, int samples, Buses& buses, const CemuDepartures& cemu = {},
                 const HostMix& host = {});

// Renders a frame of 144 samples of the voice in the enhanced mode, as RenderVoice does. `pb` is updated as the console
// updates it, and `voice` holds the rest of the voice's state. Cemu's departures don't apply.
void RenderVoiceEnhanced(Pb& pb, EnhancedVoice& voice, const Memory& memory, FloatBuses& buses,
                         const HostMix& host = {});

// Moves the voice on by a frame without mixing it (see System::SetMixing). Its accelerator, its position and its last
// samples move on as RenderVoice moves them, and so do its envelope's and its mixes' volumes. A voice with its low-pass
// or its biquad on runs its samples through them as RenderVoice does. The filters' states then move on too.
void AdvanceVoice(Pb& pb, const Memory& memory, int samples, const CemuDepartures& cemu = {});

// The same in the enhanced mode, as RenderVoiceEnhanced moves the voice on. The voice keeps the gains that its ramps
// reach.
void AdvanceVoiceEnhanced(Pb& pb, EnhancedVoice& voice, const Memory& memory);

// Sets the enhanced mode's past samples of a voice from a block's last four, as when the game sets the voice's SRC:
// zeros, then the four.
void ResetHistory(EnhancedVoice& voice, const std::array<int16_t, 4>& last);

// Adds an aux bus's return into one channel of the device's main bus. The volume goes from `from` to `to` across the
// frame (0x04C5). Returns the largest magnitude that it adds, for a host's meter.
int32_t ReturnAux(std::span<const int32_t> returned, uint16_t from, uint16_t to, std::span<int32_t> main);

// One channel of a device's output, in 24-bit scale: the main bus at the device's volume (0x0517). The volume goes from
// `from` to `to` across the frame.
void OutputBus(std::span<const int32_t> bus, uint16_t from, uint16_t to, std::span<int32_t> out);

} // namespace ntdlib::cafe::ax
