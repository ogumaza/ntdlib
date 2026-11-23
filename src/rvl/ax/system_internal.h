// SPDX-License-Identifier: MIT

// Definitions that the parts of System share (system.cpp and system_frame.cpp): the microcode's cycle budget and costs,
// the defaults of a voice's PB, the builds' traits, the depop's fade, and the steps of __AXServiceVPB that copy a
// voice's changes into the PB. The two files build as one translation unit. The definitions therefore keep internal
// linkage. Only those files include this header.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

#include "ntdlib/ax/ax.h"
#include "ntdlib/flipper/machine.h"
#include "ntdlib/rvl/ax/pb.h"
#include "ntdlib/rvl/ax/system.h"
#include "ntdlib/types.h"

namespace ntdlib::rvl::ax
{
namespace
{

// AX allows the microcode 1/667 of the bus clock per frame.
constexpr uint32_t kMaxDspCycles = ntdlib::flipper::kWiiBusClockHz / 667;

// The parts of a voice's PB that __AXSetPBDefault (0x8010707c) marks as changed.
constexpr uint32_t kDefaultSync =
    sync::kState | sync::kItd | sync::kLpf | sync::kBiquad | sync::kRemote | sync::kRmtSrc | sync::kRmtIir;

// The DSP cycles that the microcode spends on a voice's mixes (__AXMixCycles, __AXRmtMixCycles). kMixCycles is indexed
// by 5 bits of Pb::MixerCtrl(): left, right, delta, surround and surround delta for one bus. kRemoteMixCycles is
// indexed by 2 bits of Pb::rmt_mixer_ctrl.
constexpr std::array<uint32_t, 32> kMixCycles = {2,    408,  408,  810,  1404, 1404, 1404, 1404, 408,  816,  816,
                                                 1218, 1812, 1812, 1812, 1812, 707,  1115, 1115, 1517, 2111, 2111,
                                                 2111, 2111, 707,  1115, 1115, 1517, 2111, 2111, 2111, 2111};
constexpr std::array<uint32_t, 4> kRemoteMixCycles = {4, 86, 151, 151};

// The same for the build of October 2006 (at 0x80530348 in Smooth Moves' main.dol). That build counts no cycles for the
// Wii Remotes.
constexpr std::array<uint32_t, 32> kMixCyclesOctober2006 = {
    0, 760, 760, 1470, 0, 1265, 1265, 2470, 760,  1520, 1520, 2230, 760,  2025, 2025, 3230,
    0, 760, 760, 1470, 0, 1265, 1265, 2470, 1265, 2025, 2025, 2735, 1265, 2530, 2530, 3735};

// The commands in the microcode's command list that every build numbers alike. BuildTraits numbers the others.
enum Command : uint16_t
{
    kCommandSetup = 0,
    kCommandAddToLr = 1,
    kCommandProcess = 4,
};

// The compressor's threshold and release time, in frames.
constexpr uint16_t kCompressorThreshold = 32768;
constexpr uint16_t kCompressorRelease = 10;

// The entries of the depop block, in order: 12 for the main mix and its aux buses, then 8 for the Wii Remotes.
enum DepopEntry
{
    kDepopL,
    kDepopR,
    kDepopS,
    kDepopAuxAL,
    kDepopAuxAR,
    kDepopAuxAS,
    kDepopAuxBL,
    kDepopAuxBR,
    kDepopAuxBS,
    kDepopAuxCL,
    kDepopAuxCR,
    kDepopAuxCS,
    kDepopMain0,
    kDepopAux0,
    kDepopMain1,
    kDepopAux1,
    kDepopMain2,
    kDepopAux2,
    kDepopMain3,
    kDepopAux3,
};

constexpr uint32_t kStudioEntrySize = 6;

// The samples in one Wii Remote ring buffer.
constexpr int32_t kRemoteBufferLength = layout::kRemoteBufferSamples;

// The addresses in the Wii Remotes' rings that a frame's command list gives the microcode: each ring at `position`, in
// samples from its start.
std::array<ntdlib::PAddr, kRemoteCount> RemoteBuffersAt(int32_t position)
{
    std::array<ntdlib::PAddr, kRemoteCount> remote;
    for (int i = 0; i < kRemoteCount; i++)
    {
        remote[i] = layout::kRemoteBuffers + static_cast<ntdlib::PAddr>((i * kRemoteBufferLength + position) * 2);
    }

    return remote;
}

uint16_t Hi(uint32_t value)
{
    return static_cast<uint16_t>(value >> 16);
}

uint16_t Lo(uint32_t value)
{
    return static_cast<uint16_t>(value);
}

// The DSP cycles for the voice's sample rate conversion (__AXGetSrcCycles, inlined in __AXSyncPBs).
uint32_t SrcCycles(const Pb& pb)
{
    const uint32_t ratio = Join(pb.src.ratio_hi, pb.src.ratio_lo);
    if (pb.src_select == static_cast<uint16_t>(SrcMode::kFourTap))
    {
        return (ratio * 512 + 32768) / 65536 + 1561;
    }
    if (pb.src_select == static_cast<uint16_t>(SrcMode::kLinear))
    {
        return (ratio * 512 + 32768) / 65536 + 1466;
    }

    return 605;
}

// The DSP cycles that a running voice takes in the build of December 2006 (__AXSyncPBs, 0x80106d64).
uint32_t RunningCyclesDecember2006(const Pb& pb)
{
    uint32_t cycles = 387;
    if (pb.lpf.on != 0)
    {
        cycles += 309;
    }

    if (pb.biquad.on != 0)
    {
        cycles += 1024;
    }

    if (pb.itd.flag == 1)
    {
        cycles += 27;
    }

    const uint32_t ctrl = pb.MixerCtrl();
    cycles += SrcCycles(pb);
    cycles +=
        kMixCycles[ctrl & 31] + kMixCycles[ctrl >> 16 & 31] + kMixCycles[ctrl >> 21 & 31] + kMixCycles[ctrl >> 26 & 31];

    if (pb.remote == 1)
    {
        cycles += 613;
        if (pb.rmt_iir.on == kLpfOn)
        {
            cycles += 118;
        }
        else if (pb.rmt_iir.on == kBiquadOn)
        {
            cycles += 834;
        }

        for (int shift = 0; shift < 16; shift += 2)
        {
            cycles += kRemoteMixCycles[pb.rmt_mixer_ctrl >> shift & 3];
        }
    }

    return cycles;
}

// The same in the build of October 2006 (0x80030af0). Both sample rate converters take the same cycles, and no
// conversion takes none. The low-pass filter and the second filter take 555 each. The model never turns the second
// filter on.
uint32_t RunningCyclesOctober2006(const Pb& pb)
{
    const uint32_t ctrl = pb.MixerCtrl();
    uint32_t cycles = kMixCyclesOctober2006[ctrl & 31] + kMixCyclesOctober2006[ctrl >> 16 & 31] +
                      kMixCyclesOctober2006[ctrl >> 21 & 31] + kMixCyclesOctober2006[ctrl >> 26 & 31] + 140;
    if (pb.src_select != static_cast<uint16_t>(SrcMode::kNone))
    {
        const uint32_t ratio = Join(pb.src.ratio_hi, pb.src.ratio_lo);
        cycles += ((ratio * 132 + 0x8000) / 65536 + 728) * 6;
    }

    if (pb.lpf.on != 0)
    {
        cycles += 555;
    }

    return cycles;
}

// What the builds of AX's CPU side differ in, apart from the PB's layout (see LoadPbOctober2006).
struct BuildTraits
{
    uint32_t voice_count;

    // The PB: its size, how it's read and written, and whether it has a buffer for the changes that the microcode
    // makes within a frame. The model never queues such changes.
    std::size_t pb_size;
    Pb (*load_pb)(const uint8_t* bytes);
    void (*store_pb)(const Pb& pb, uint8_t* bytes);
    bool update_buffers;

    // The DSP cycles: the headroom that __AXOutNewFrame passes to __AXSyncPBs, what __AXSyncPBs counts besides the
    // command list and the voices, a voice's ITD, and a running voice.
    uint32_t headroom;
    uint32_t sync_cycles;
    uint32_t itd_cycles;
    uint32_t (*running_cycles)(const Pb& pb);

    // Whether an aux bus whose callback goes marks the three frames of its ring to be cleared (__clearAuxA and so on).
    bool clears_aux_rings;

    // The command list (__AXNextFrame): the numbers of the commands after PROCESS, and AX's estimates of the cycles of
    // SETUP, of each aux bus and of OUTPUT.
    std::optional<uint16_t> after_process; // a command without arguments
    uint16_t mix_aux_a;                    // B and C follow
    uint16_t compressor;
    uint16_t wm_output;
    uint16_t output;
    bool output_volume; // whether OUTPUT takes the master volume
    uint16_t end;
    uint32_t setup_cycles;
    uint32_t aux_cycles;
    uint32_t output_cycles;
};

// The build that Wii Sports links, and the later ones. In its default mode, AX passes 24,400 cycles of headroom
// (__AXOutNewFrame, 0x80105c00).
constexpr BuildTraits kDecember2006Traits = {
    .voice_count = kVoiceCount,
    .pb_size = kPbSize,
    .load_pb = LoadPb,
    .store_pb = StorePb,
    .update_buffers = false,
    .headroom = 24400,
    .sync_cycles = 32 + kVoiceCount * 600,
    .itd_cycles = 129,
    .running_cycles = RunningCyclesDecember2006,
    .clears_aux_rings = true,
    .after_process = std::nullopt,
    .mix_aux_a = 5,
    .compressor = 10,
    .wm_output = 13,
    .output = 11,
    .output_volume = true,
    .end = 14,
    .setup_cycles = 4126,
    .aux_cycles = 2235,
    .output_cycles = 1172,
};

// The build of October 2006. Its __AXOutNewFrame (0x8002fb70) passes 0 cycles of headroom when the DSP has finished the
// last frame by the audio interface's interrupt. Otherwise it passes the time it waited for the DSP. Here the DSP is
// always done in time. Its __AXSyncPBs counts no cycles for the ITD. Command 5 always follows PROCESS. The commands
// after it are numbered one higher than in later builds.
constexpr BuildTraits kOctober2006Traits = {
    .voice_count = kVoiceCountOctober2006,
    .pb_size = kPbSizeOctober2006,
    .load_pb = LoadPbOctober2006,
    .store_pb = StorePbOctober2006,
    .update_buffers = true,
    .headroom = 0,
    .sync_cycles = 43536,
    .itd_cycles = 0,
    .running_cycles = RunningCyclesOctober2006,
    .clears_aux_rings = false,
    .after_process = 5,
    .mix_aux_a = 6,
    .compressor = 11,
    .wm_output = 14,
    .output = 12,
    .output_volume = false,
    .end = 15,
    .setup_cycles = 4246,
    .aux_cycles = 1249,
    .output_cycles = 665,
};

const BuildTraits& TraitsOf(Build build)
{
    return build == Build::kOctober2006 ? kOctober2006Traits : kDecember2006Traits;
}

// Fills one entry of the depop block from its accumulator (__AXDepopFadeMain 0x801063f8, __AXDepopFadeRmt 0x80106464).
// The accumulator falls by at most 20 per sample. The entry gives the microcode the accumulator value and the fall per
// sample.
void DepopFade(int32_t& sum, int32_t& value, int16_t& delta, int32_t samples)
{
    if (sum / samples == 0)
    {
        sum = 0;
        value = 0;
        delta = 0;

        return;
    }

    const int32_t step = std::clamp(sum / samples, -20, 20);
    value = sum;
    sum -= step * samples;
    delta = static_cast<int16_t>(-step);
}

// __AXServiceVPB's step for the voice's converter and mixer settings, state, type and mix. A state that the CPU hasn't
// changed comes back from the microcode.
void ServiceSettings(Pb& dst, Pb& src, uint32_t sync)
{
    if (sync & sync::kSelect)
    {
        dst.src_select = src.src_select;
        dst.coef_select = src.coef_select;
    }

    if (sync & sync::kMixerCtrl)
    {
        dst.mixer_ctrl_hi = src.mixer_ctrl_hi;
        dst.mixer_ctrl_lo = src.mixer_ctrl_lo;
    }

    if (sync & sync::kState)
    {
        dst.state = src.state;
    }
    else
    {
        src.state = dst.state;
    }

    if (sync & sync::kType)
    {
        dst.type = src.type;
    }

    if (sync & sync::kMix)
    {
        dst.mix = src.mix;
    }
}

// __AXServiceVPB's step for the decoder and the sample rate converter: the ADPCM block, the converter's ratio or the
// whole converter, and the ADPCM loop block.
void ServiceDecoder(Pb& dst, const Pb& src, uint32_t sync)
{
    if (sync & sync::kAdpcm)
    {
        dst.adpcm = src.adpcm;
    }

    if (sync & sync::kSrcRatio)
    {
        dst.src.ratio_hi = src.src.ratio_hi;
        dst.src.ratio_lo = src.src.ratio_lo;
    }
    else if (sync & sync::kSrc)
    {
        dst.src = src.src;
    }

    if (sync & sync::kAdpcmLoop)
    {
        dst.adpcm_loop = src.adpcm_loop;
    }
}

// __AXServiceVPB's step for the addresses: the CPU's loop flag and addresses, or the whole block. A current address
// that the CPU hasn't changed comes back from the microcode.
void ServiceAddresses(Pb& dst, Pb& src, uint32_t sync)
{
    if (sync & (sync::kLoopFlag | sync::kLoopAddr | sync::kEndAddr | sync::kCurrAddr))
    {
        if (sync & sync::kLoopFlag)
        {
            dst.addr.loop_flag = src.addr.loop_flag;
        }

        if (sync & sync::kLoopAddr)
        {
            dst.addr.loop_address_hi = src.addr.loop_address_hi;
            dst.addr.loop_address_lo = src.addr.loop_address_lo;
        }

        if (sync & sync::kEndAddr)
        {
            dst.addr.end_address_hi = src.addr.end_address_hi;
            dst.addr.end_address_lo = src.addr.end_address_lo;
        }

        if (sync & sync::kCurrAddr)
        {
            dst.addr.current_address_hi = src.addr.current_address_hi;
            dst.addr.current_address_lo = src.addr.current_address_lo;
        }
        else
        {
            src.addr.current_address_hi = dst.addr.current_address_hi;
            src.addr.current_address_lo = dst.addr.current_address_lo;
        }
    }
    else if (sync & sync::kAddr)
    {
        dst.addr = src.addr;
    }
    else
    {
        src.addr.current_address_hi = dst.addr.current_address_hi;
        src.addr.current_address_lo = dst.addr.current_address_lo;
    }
}

// __AXServiceVPB's step for the low-pass and the biquad: the CPU's coefficients, or the whole filter.
void ServiceFilters(Pb& dst, const Pb& src, uint32_t sync)
{
    if (sync & sync::kLpfCoefs)
    {
        dst.lpf.a0 = src.lpf.a0;
        dst.lpf.b0 = src.lpf.b0;
    }
    else if (sync & sync::kLpf)
    {
        dst.lpf = src.lpf;
    }

    if (sync & sync::kBiquadCoefs)
    {
        dst.biquad.b0 = src.biquad.b0;
        dst.biquad.b1 = src.biquad.b1;
        dst.biquad.b2 = src.biquad.b2;
        dst.biquad.a1 = src.biquad.a1;
        dst.biquad.a2 = src.biquad.a2;
    }
    else if (sync & sync::kBiquad)
    {
        dst.biquad = src.biquad;
    }
}

// __AXServiceVPB's step for the Wii Remotes' fields: their switch, mixer control, mixes, depop values, converter and
// filter.
void ServiceRemote(Pb& dst, const Pb& src, uint32_t sync)
{
    if (sync & sync::kRemote)
    {
        dst.remote = src.remote;
    }

    if (sync & sync::kRmtMixerCtrl)
    {
        dst.rmt_mixer_ctrl = src.rmt_mixer_ctrl;
    }

    if (sync & sync::kRmtMix)
    {
        dst.rmt_mix = src.rmt_mix;
    }

    if (sync & sync::kRmtDpop)
    {
        dst.rmt_dpop = src.rmt_dpop;
    }

    if (sync & sync::kRmtSrc)
    {
        dst.rmt_src = src.rmt_src;
    }

    // When the remote filter is a low-pass filter, its third and fourth words are a0 and b0.
    if (sync & sync::kRmtIirLpfCoefs)
    {
        dst.rmt_iir.xn2 = src.rmt_iir.xn2;
        dst.rmt_iir.yn1 = src.rmt_iir.yn1;
    }
    else if (sync & sync::kRmtIirBiquadCoefs)
    {
        dst.rmt_iir.b0 = src.rmt_iir.b0;
        dst.rmt_iir.b1 = src.rmt_iir.b1;
        dst.rmt_iir.b2 = src.rmt_iir.b2;
        dst.rmt_iir.a1 = src.rmt_iir.a1;
        dst.rmt_iir.a2 = src.rmt_iir.a2;
    }
    else if (sync & sync::kRmtIir)
    {
        dst.rmt_iir = src.rmt_iir;
    }
}

} // namespace
} // namespace ntdlib::rvl::ax
