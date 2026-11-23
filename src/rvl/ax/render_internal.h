// SPDX-License-Identifier: MIT

// Definitions that the parts of the microcode's model share (render.cpp and render_stages.cpp): the layout of DRAM and
// of the PB's fields, the buses' samples, the ramps, the saturation and the scaling of samples by gains, and the
// meters' peaks. The two files build as one translation unit. The definitions therefore keep internal linkage. Only
// those files include this header.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "ntdlib/dsp/accumulator.h"
#include "ntdlib/rvl/ax/pb.h"
#include "ntdlib/rvl/ax/render.h"

namespace ntdlib::rvl::ax
{
namespace
{

using ntdlib::dsp::MiddleWord;
using ntdlib::dsp::Round;
using ntdlib::dsp::Wrap40;

// The frame's samples, and the Wii Remotes' samples in a frame.
constexpr int kSamples = 96;
constexpr int kRemoteSamples = 18;

// The buses in DRAM. Each sample takes two words, high first. The main bus and the aux buses A, B and C have 96 samples
// for each of their channels: left, right and surround.
constexpr std::array<std::array<uint16_t, 3>, 4> kBuses = {{
    {0x0000, 0x00c0, 0x0180},
    {0x0400, 0x04c0, 0x0580},
    {0x0640, 0x0700, 0x07c0},
    {0x0880, 0x0940, 0x0a00},
}};

// The Wii Remotes' buses: 18 samples each, in the order of the PB's mixes: main 0, aux 0, main 1, aux 1 and so on.
constexpr std::array<uint16_t, 8> kRemoteBuses = {0x0240, 0x0ac0, 0x0264, 0x0ae4, 0x0288, 0x0b08, 0x02ac, 0x0b2c};

// The rest of DRAM that the microcode uses: the command list, the voice's frame with 32 samples of the ITD's history
// before it, the Wii Remotes' frame, the pointers to the samples that the mixes take, the state between frames, a zero
// and a scratch area.
constexpr uint16_t kCommandList = 0x0c00;
constexpr uint16_t kCommandListEnd = 0x0c40;
constexpr uint16_t kItdHistory = 0x0c40;
constexpr uint16_t kFrame = 0x0c60;
constexpr uint16_t kItdOut = 0x0ca0;
constexpr uint16_t kRemoteFrame = 0x0cc0;
constexpr uint16_t kSource = 0x0ce1;
constexpr uint16_t kSourceL = 0x0ce2;
constexpr uint16_t kSourceR = 0x0ce3;
constexpr uint16_t kCompressorCount = 0x0ce4;
constexpr uint16_t kMasterVolume = 0x0ce5;
constexpr uint16_t kAuxVolumes = 0x0ce6;
constexpr uint16_t kZero = 0x0ce9;
constexpr uint16_t kScratch = 0x0d08;
constexpr uint16_t kRemoteInput = 0x0d0c;

// The copy of the PB in DRAM (0x140 bytes) and its words (see pb.h).
constexpr uint16_t kPb = 0x02d0;
constexpr uint32_t kPbBytes = 0x140;
constexpr uint16_t kPbNext = 0x02d0;
constexpr uint16_t kPbCurr = 0x02d2;
constexpr uint16_t kPbSrcSelect = 0x02d4;
constexpr uint16_t kPbCoefSelect = 0x02d5;
constexpr uint16_t kPbMixerCtrlHi = 0x02d6;
constexpr uint16_t kPbMixerCtrlLo = 0x02d7;
constexpr uint16_t kPbState = 0x02d8;
constexpr uint16_t kPbType = 0x02d9;
constexpr uint16_t kPbMix = 0x02da;
constexpr uint16_t kPbItdFlag = 0x02f2;
constexpr uint16_t kPbItdBuffer = 0x02f3;
constexpr uint16_t kPbItdShift = 0x02f5;
constexpr uint16_t kPbItdTarget = 0x02f7;
constexpr uint16_t kPbDpop = 0x02f9;
constexpr uint16_t kPbVolume = 0x0305;
constexpr uint16_t kPbDelta = 0x0306;
constexpr uint16_t kPbLoopFlag = 0x0307;
constexpr uint16_t kPbFormat = 0x0308;
constexpr uint16_t kPbLoopAddress = 0x0309;
constexpr uint16_t kPbEndAddress = 0x030b;
constexpr uint16_t kPbCurrentAddress = 0x030d;
constexpr uint16_t kPbCoefs = 0x030f;
constexpr uint16_t kPbGain = 0x031f;
constexpr uint16_t kPbPredScale = 0x0320;
constexpr uint16_t kPbYn1 = 0x0321;
constexpr uint16_t kPbYn2 = 0x0322;
constexpr uint16_t kPbRatio = 0x0323;
constexpr uint16_t kPbFrac = 0x0325;
constexpr uint16_t kPbLast = 0x0326;
constexpr uint16_t kPbLoopPredScale = 0x032a;
constexpr uint16_t kPbLoopYn1 = 0x032b;
constexpr uint16_t kPbLoopYn2 = 0x032c;
constexpr uint16_t kPbLpf = 0x032d;
constexpr uint16_t kPbBiquad = 0x0331;
constexpr uint16_t kPbRemote = 0x033b;
constexpr uint16_t kPbRmtMixerCtrl = 0x033c;
constexpr uint16_t kPbRmtMix = 0x033d;
constexpr uint16_t kPbRmtDpop = 0x034d;
constexpr uint16_t kPbRmtFrac = 0x0355;
constexpr uint16_t kPbRmtLast = 0x0356;
constexpr uint16_t kPbRmtIir = 0x035a;

// The dpop words of the remote mixes, in the order of kRemoteBuses.
constexpr std::array<uint16_t, 8> kRemoteDpop = {0x034d, 0x0351, 0x034e, 0x0352, 0x034f, 0x0353, 0x0350, 0x0354};

// The build of October 2006 keeps the PB in DRAM in that build's layout (see LoadPbOctober2006). The fields up to the
// ITD keep the words above. An update block follows at 0x02f9: a count of updates for each third of the frame, then the
// address of the buffer that holds the updates. The fields from the depop values to the low-pass filter therefore come
// five words later than above. A second filter follows at 0x0336: its on word, its state and two coefficients. The Wii
// Remotes' fields then come a word earlier than above (Renderer::Field).
constexpr uint16_t kPbUpdateCounts = 0x02f9;
constexpr uint16_t kPbUpdateBuffer = 0x02fc;
constexpr uint16_t kPbSecondFilter = 0x0336;

// The updates (0x0360): pairs of a word's offset in the PB and its new value. The buffer holds 32 pairs.
constexpr uint16_t kUpdates = 0x0360;
constexpr uint32_t kUpdatesBytes = 0x80;

// The build of October 2006 works through each voice in three parts of 32 samples, and gives the Wii Remotes 6 samples
// of each part.
constexpr int kParts = 3;
constexpr int kPartSamples = 32;
constexpr int kPartRemoteSamples = 6;

// The commands of the build's command list.
enum Command : uint16_t
{
    kSetup = 0,
    kAddToLr = 1,
    kSubtractFromL = 2,
    kProcess = 4,
    kMixAuxA = 5,
    kMixAuxC = 7,
    kCompressor = 10,
    kOutput = 11,
    kWmOutput = 13,
    kEnd = 14,
};

// The sample rate converters of PB::src_select (0x0db7) and the filter tables of PB::coef_select (0x0dba), in the
// coefficient ROM at 0x1000.
constexpr uint16_t kSrcFourTap = 0;
constexpr uint16_t kSrcLinear = 1;
constexpr uint16_t kSrcNone = 2;
constexpr uint16_t kCoefficientRom = 0x1000;
constexpr uint16_t kFilterWords = 0x200;

// The ratio of the Wii Remotes' converter: 32 kHz to 6 kHz, in 16.16.
constexpr int64_t kRemoteRatio = 0x55555;

// A PB list longer than AX's voices is a loop.
constexpr int kMostPbs = 128;

// A 32-bit sample of a bus, as the microcode loads it in 40-bit mode.
int64_t Load32(const Renderer::Dram& dram, uint16_t at)
{
    return static_cast<int32_t>(uint32_t{dram[at]} << 16 | dram[at + 1u]);
}

// An accumulator stored as a 32-bit sample: the middle word as the microcode reads it in 40-bit mode (saturated), then
// the low word.
void Store32(Renderer::Dram& dram, uint16_t at, int64_t accumulator)
{
    dram[at] = static_cast<uint16_t>(MiddleWord(accumulator));
    dram[at + 1u] = static_cast<uint16_t>(accumulator);
}

// The volume that goes from `from` to `to` across a frame: one gain for each sample (0x04e6 and 0x0618). Each step adds
// 683 65536ths of the change, or 682 for every third step. The first sample already has one step. The last sample
// gets `to`.
std::array<uint16_t, kSamples> Ramp(uint16_t from, uint16_t to)
{
    std::array<uint16_t, kSamples> gains{};
    const auto change = static_cast<int16_t>(static_cast<uint16_t>(to - from));
    int64_t sum = int64_t{from} * 0x10000;
    for (int i = 0; i + 1 < kSamples; i++)
    {
        sum += int64_t{change} * (i % 3 == 2 ? 682 : 683);
        gains[static_cast<std::size_t>(i)] = static_cast<uint16_t>(sum >> 16);
    }
    gains[kSamples - 1] = to;

    return gains;
}

// A value held within the range of a signed 16-bit word. A read of an accumulator's middle word in 40-bit mode holds a
// value there too.
uint16_t Saturated(int64_t value)
{
    return static_cast<uint16_t>(MiddleWord(value * 0x10000));
}

// The volume after `steps` steps of a ramp of the build of October 2006. That build ramps a mix's volume as a signed
// 16-bit value in 40-bit mode. A ramp stops at either end of the range.
uint16_t SaturatedRamp(uint16_t from, uint16_t delta, int steps)
{
    return Saturated(static_cast<int16_t>(from) + int64_t{steps} * static_cast<int16_t>(delta));
}

// The magnitude of `value`, held within 32 bits, for a meter's peak.
int32_t Magnitude(int64_t value)
{
    return static_cast<int32_t>(std::min<int64_t>(value < 0 ? -value : value, std::numeric_limits<int32_t>::max()));
}

// A 32-bit sample times a 1.15 gain, as the aux return, the compressor and the output take it: the low word times the
// gain, shifted down 16, plus the high word times the gain. The products are doubled. Both gain and low word are
// unsigned.
int64_t Scaled(int64_t sample, uint16_t gain)
{
    const int64_t high = sample >> 16;
    const int64_t low = sample & 0xffff;
    return ((2 * low * gain) >> 16) + 2 * high * gain;
}

// The same without the doubling, as the build of October 2006 takes an aux bus's return volume. A volume of 0x8000
// returns half the bus.
int64_t ScaledOnce(int64_t sample, uint16_t gain)
{
    const int64_t high = sample >> 16;
    const int64_t low = sample & 0xffff;
    return ((low * gain) >> 16) + high * gain;
}

// A frame's meter peaks from the meters' sums: the largest magnitude on each side. The voices' meters come first, then
// the aux buses'.
void StorePeaks(std::span<const std::array<std::array<int64_t, kSamplesPerFrame>, 2>> sums, Meters& meters)
{
    const std::size_t tracks = meters.peaks.size();
    for (std::size_t meter = 0; meter < sums.size(); meter++)
    {
        std::array<int32_t, 2>& peaks = meter < tracks ? meters.peaks[meter] : meters.aux_peaks[meter - tracks];
        for (std::size_t side = 0; side < 2; side++)
        {
            int32_t peak = 0;
            for (const int64_t sum : sums[meter][side])
            {
                peak = std::max(peak, Magnitude(sum));
            }
            peaks[side] = peak;
        }
    }
}

} // namespace
} // namespace ntdlib::rvl::ax
