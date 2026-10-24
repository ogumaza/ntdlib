// SPDX-License-Identifier: MIT

// A model of AX's microcode in the build that Wii Sports links (December 2006): what the DSP does with each frame's
// command list. Renderer runs in place of the DSP and the microcode (see DspLink). It reads the command list, the
// voices' PBs and samples and the aux buses' processed frames from main memory. It writes what the microcode writes
// there: the PBs, the aux buses' frames, the surround buffer, the Wii Remotes' samples and the frame's 96 stereo
// samples.
//
// The model keeps the microcode's data memory (DRAM), and each routine works on it as the microcode does: the buses'
// 32-bit samples, the copy of the PB, the voice's samples and the microcode's state between frames. The comments give
// the routines' addresses in the microcode. The arithmetic follows the DSP's: products and sums in 40-bit accumulators,
// and middle words saturated to 16 bits in 40-bit mode (ntdlib/dsp/accumulator.h). The accelerator reads and decodes
// the samples as the DSP's does (ntdlib/dsp/accelerator.h).
//
// The model covers the commands of AX's stereo command list: SETUP, ADD_TO_LR, PROCESS, MIX_AUX_A to C, COMPRESSOR,
// WM_OUTPUT, OUTPUT and END. AX's other modes use other commands. RunFrame refuses them.
//
// The model also takes the command lists and PBs of AX's build of October 2006 (Build::kOctober2006). WarioWare:
// Smooth Moves links that build. There, PROCESS loads the first PB, and command 5 mixes the voices. The later commands
// are numbered one higher than in the build of December 2006. OUTPUT takes no master volume, and the PBs have another
// layout (LoadPbOctober2006). MIX_AUX_A to C don't double the products of the return volume: a volume of 0x8000
// returns half the bus. The model mixes the voices of that build as the microcode of December 2006 does. The
// microcode of October 2006 works through each voice in three parts of 32 samples. That microcode's output can
// therefore differ a little from the model's.
//
// A host can also measure what the voices and the aux buses add to the main bus (Meters). The microcode has no such
// meters.

#pragma once

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "ntdlib/dsp/accelerator.h"
#include "ntdlib/dsp/memory.h"
#include "ntdlib/rvl/ax/pb.h"

namespace ntdlib::rvl::ax
{

// A host's meters of a frame. Each meter adds up its voices' mix into the main bus, left and right. `pbs` gives the
// voices' meters, by the physical addresses of their PBs in main memory. A frame gives `peaks` each meter's peaks of
// its left and right: the largest magnitude of its sum, on the scale of the buses' 16-bit samples. It gives `aux_peaks`
// the peaks of what each aux bus added to the main bus's left and right, at the bus's return volume, on the same
// scale. A bus that the frame didn't mix back gives 0. What a bus returns into the surround channel reaches the left
// and right in the next frame: OUTPUT keeps the surround channel, and ADD_TO_LR adds it to them. `aux_surround` keeps
// each bus's return into the surround channel from a frame for the next one. A meter leaves out its voices' mix into
// the surround channel: a voice of NW4R SND's stereo mode sends nothing there.
struct Meters
{
    using Surround = std::array<std::array<int64_t, kSamplesPerFrame>, kAuxBusCount>;

    std::vector<std::pair<uint32_t, int>> pbs;
    std::vector<std::array<int32_t, 2>> peaks;
    std::array<std::array<int32_t, 2>, kAuxBusCount> aux_peaks{};
    Surround aux_surround{};
};

class Renderer
{
public:
    // The microcode's DRAM: 4,096 words.
    using Dram = std::array<uint16_t, 0x1000>;

    // The state in which the microcode starts (0x0010): the master volume and the aux buses' return volumes at 1.0.
    // `build` gives the build of AX whose command lists and PBs the model takes.
    explicit Renderer(Build build = Build::kDecember2006);

    // Runs the `size`-byte command list at `command_list` (the cached address that AX mails), as the microcode does.
    // Returns false for a command that the model lacks, and for a PB that selects a sample rate converter or a filter
    // table that the microcode lacks. With `meters`, the frame also fills in the meters' peaks. The meters change
    // nothing in the output, and they aren't part of the model's state. A host that returns the model to an earlier
    // state returns the meters' `aux_surround` with it (see snd::ArchiveModel::Snapshot).
    bool RunFrame(ntdlib::dsp::WiiMemory& memory, uint32_t command_list, uint32_t size, Meters* meters = nullptr);

    // The DRAM as the model leaves it.
    const Dram& DramWords() const
    {
        return dram_;
    }

private:
    void FromMain(uint32_t address, uint16_t to, uint32_t bytes);
    void ToMain(uint16_t from, uint32_t address, uint32_t bytes);
    uint32_t Arg32(uint16_t& at) const;

    void Setup(uint32_t studio);
    void AddToLr(uint32_t surround, bool subtract_from_l, const Meters::Surround& returns);
    bool Process(uint32_t first);
    bool ProcessVoice();
    void LoadAccelerator();
    int16_t ReadSample();
    void AtEnd();
    void StoreAccelerator();
    void FourTap(uint16_t filter);
    void Linear();
    void Copy();
    void Envelope();
    void LowPass(uint16_t state, uint16_t from, uint16_t to);
    void Biquad(uint16_t state, uint16_t from, uint16_t to);
    void MixBus(int bus, uint32_t bits);
    uint16_t MixChannel(uint16_t source, uint16_t volume, bool ramp, uint16_t bus, int samples,
                        int64_t* meter = nullptr);
    void Remote();
    void MixAux(int bus, uint16_t volume, uint32_t write, uint32_t read);
    void Compressor(uint16_t threshold, uint16_t release, uint32_t table);
    void WmOutput(const std::array<uint32_t, 4>& remotes);
    void Output(uint16_t volume, uint32_t surround, uint32_t output);

    void LoadPb(uint32_t address);
    void StorePb(uint32_t address);

    Build build_ = Build::kDecember2006;
    ntdlib::dsp::WiiMemory* memory_ = nullptr;
    Dram dram_{};

    // The accelerator's registers. The microcode loads them for each voice. The input register keeps its value.
    ntdlib::dsp::Accelerator accelerator_;
    std::array<uint16_t, 16> coefs_{};

    // Set once the voice reaches its end without a loop. The microcode's handler then points its sample reads at a
    // zero in DRAM (0x0f61).
    bool reads_zero_ = false;

    // The frame's meters, the meter of the PB that PROCESS mixes, and each meter's sums of the frame's left and right,
    // then those of the aux buses.
    using MeterSums = std::array<std::array<int64_t, kSamplesPerFrame>, 2>;
    Meters* meters_ = nullptr;
    int meter_ = -1;
    std::vector<MeterSums> meter_sums_;
};

} // namespace ntdlib::rvl::ax
