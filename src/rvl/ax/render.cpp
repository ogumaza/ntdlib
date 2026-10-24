// SPDX-License-Identifier: MIT

#include "ntdlib/rvl/ax/render.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "ntdlib/dsp/accelerator.h"
#include "ntdlib/dsp/accumulator.h"
#include "ntdlib/dsp/coef_rom.h"
#include "ntdlib/dsp/memory.h"
#include "xsflib/bytes.h"

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

} // namespace

Renderer::Renderer(Build build) : build_(build)
{
    // The microcode's start (0x001a) sets the old master and return volumes to 1.0 and the zero to 0.
    dram_[kMasterVolume] = 0x8000;
    for (int bus = 0; bus < 3; bus++)
    {
        dram_[kAuxVolumes + bus] = 0x8000;
    }
    dram_[kZero] = 0;
}

void Renderer::FromMain(uint32_t address, uint16_t to, uint32_t bytes)
{
    // The DSP's DMA masks the address to 30 bits. A transfer that leaves main memory doesn't happen.
    const uint8_t* p = memory_->At(address & 0x3fffffff, bytes == 0 ? 1 : bytes);
    if (p == nullptr)
    {
        return;
    }
    for (uint32_t i = 0; i < bytes / 2 && to + i < dram_.size(); i++)
    {
        dram_[to + i] = xsflib::LoadBe16(p + i * 2);
    }
}

void Renderer::ToMain(uint16_t from, uint32_t address, uint32_t bytes)
{
    uint8_t* p = memory_->At(address & 0x3fffffff, bytes == 0 ? 1 : bytes);
    if (p == nullptr)
    {
        return;
    }
    for (uint32_t i = 0; i < bytes / 2 && from + i < dram_.size(); i++)
    {
        xsflib::StoreBe16(p + i * 2, dram_[from + i]);
    }
}

void Renderer::LoadPb(uint32_t address)
{
    // The build of October 2006 keeps its PBs in another layout. LoadPbOctober2006 puts the layout's fields where the
    // later builds keep them in DRAM. The fields that the layout lacks are 0: the biquad filter and the Wii Remotes'
    // filter stay off.
    if (build_ != Build::kOctober2006)
    {
        FromMain(address, kPb, kPbBytes);
        return;
    }

    const uint8_t* p = memory_->At(address & 0x3fffffff, kPbSizeOctober2006);
    std::array<uint8_t, kPbBytes> bytes{};
    if (p != nullptr)
    {
        ntdlib::rvl::ax::StorePb(LoadPbOctober2006(p), bytes.data());
    }
    for (std::size_t i = 0; i < kPbBytes / 2; i++)
    {
        dram_[kPb + i] = xsflib::LoadBe16(&bytes[2 * i]);
    }
}

void Renderer::StorePb(uint32_t address)
{
    // A PB of the build of October 2006 takes back the fields that its layout has. The update block, the second filter
    // and the padding keep their bytes.
    if (build_ != Build::kOctober2006)
    {
        ToMain(kPb, address, kPbBytes);
        return;
    }

    uint8_t* p = memory_->At(address & 0x3fffffff, kPbSizeOctober2006);
    if (p == nullptr)
    {
        return;
    }
    std::array<uint8_t, kPbBytes> bytes{};
    for (std::size_t i = 0; i < kPbBytes / 2; i++)
    {
        xsflib::StoreBe16(&bytes[2 * i], dram_[kPb + i]);
    }
    StorePbOctober2006(ntdlib::rvl::ax::LoadPb(bytes.data()), p);
}

uint32_t Renderer::Arg32(uint16_t& at) const
{
    const uint32_t value = uint32_t{dram_[at]} << 16 | dram_[at + 1u];
    at = static_cast<uint16_t>(at + 2);
    return value;
}

bool Renderer::RunFrame(ntdlib::dsp::WiiMemory& memory, uint32_t command_list, uint32_t size, Meters* meters)
{
    // The command loop (0x004c): the list goes into DRAM, then each command runs with its arguments. The meters' sums
    // start the frame at 0. The aux buses' returns into the surround channel in the last frame go to ADD_TO_LR.
    memory_ = &memory;
    meters_ = meters;
    Meters::Surround surround{};
    if (meters_ != nullptr)
    {
        meter_sums_.resize(meters_->peaks.size() + kAuxBusCount);
        for (MeterSums& sums : meter_sums_)
        {
            for (std::array<int64_t, kSamplesPerFrame>& side : sums)
            {
                side.fill(0);
            }
        }
        surround = meters_->aux_surround;
        meters_->aux_surround = {};
    }
    FromMain(command_list & 0x7fffffff, kCommandList, size);
    uint16_t at = kCommandList;
    while (at < kCommandListEnd)
    {
        // In the build of October 2006, PROCESS loads the first PB, and command 5 mixes the voices. The model's PROCESS
        // does both. The later commands are numbered one higher.
        uint16_t command = dram_[at++];
        if (build_ == Build::kOctober2006 && command == kProcess + 1)
        {
            continue;
        }
        if (build_ == Build::kOctober2006 && command > kProcess)
        {
            command--;
        }
        switch (command)
        {
        case kSetup:
            Setup(Arg32(at));
            break;
        case kAddToLr:
        case kSubtractFromL:
            AddToLr(Arg32(at), command == kSubtractFromL, surround);
            break;
        case kProcess:
            if (!Process(Arg32(at)))
            {
                meters_ = nullptr;
                return false;
            }
            break;
        case kMixAuxA:
        case kMixAuxA + 1:
        case kMixAuxC:
            {
                const uint16_t volume = dram_[at++];
                const uint32_t write = Arg32(at);
                const uint32_t read = Arg32(at);
                MixAux(command - kMixAuxA, volume, write, read);
                break;
            }
        case kCompressor:
            {
                const uint16_t threshold = dram_[at++];
                const uint16_t release = dram_[at++];
                Compressor(threshold, release, Arg32(at));
                break;
            }
        case kOutput:
            {
                // The build of October 2006 has no master volume. Its OUTPUT takes the buses as they are.
                const uint16_t volume = build_ == Build::kOctober2006 ? 0x8000 : dram_[at++];
                const uint32_t surround = Arg32(at);
                Output(volume, surround, Arg32(at));
                break;
            }
        case kWmOutput:
            {
                std::array<uint32_t, 4> remotes{};
                for (uint32_t& remote : remotes)
                {
                    remote = Arg32(at);
                }
                WmOutput(remotes);
                break;
            }
        case kEnd:
            for (std::size_t meter = 0; meters_ != nullptr && meter < meter_sums_.size(); meter++)
            {
                const std::size_t tracks = meters_->peaks.size();
                std::array<int32_t, 2>& peaks =
                    meter < tracks ? meters_->peaks[meter] : meters_->aux_peaks[meter - tracks];
                for (std::size_t side = 0; side < 2; side++)
                {
                    int32_t peak = 0;
                    for (const int64_t sum : meter_sums_[meter][side])
                    {
                        peak = std::max(peak, Magnitude(sum));
                    }
                    peaks[side] = peak;
                }
            }
            meters_ = nullptr;
            return true;
        default:
            meters_ = nullptr;
            return false;
        }
    }

    meters_ = nullptr;
    return false;
}

void Renderer::Setup(uint32_t studio)
{
    // SETUP (0x008a): the depop block holds a start and a step for each bus and channel. A bus with a start of 0 starts
    // the frame silent. Otherwise its samples start at the start and move by the step.
    FromMain(studio, kScratch, 0x78);
    for (int entry = 0; entry < 20; entry++)
    {
        const auto at = static_cast<uint16_t>(kScratch + entry * 3);
        int64_t value = Load32(dram_, at);
        const int64_t delta = static_cast<int16_t>(dram_[at + 2u]);
        const bool remote = entry >= 12;
        const uint16_t bus = remote ? kRemoteBuses[static_cast<std::size_t>(entry - 12)]
                                    : kBuses[static_cast<std::size_t>(entry / 3)][static_cast<std::size_t>(entry % 3)];
        const int samples = remote ? kRemoteSamples : kSamples;
        for (int i = 0; i < samples; i++)
        {
            const auto sample_at = static_cast<uint16_t>(bus + 2 * i);
            if (value == 0 && i == 0)
            {
                for (int j = 0; j < 2 * samples; j++)
                {
                    dram_[static_cast<std::size_t>(bus + j)] = 0;
                }
                break;
            }
            if (i > 0)
            {
                value = Wrap40(value + delta);
            }
            Store32(dram_, sample_at, value);
        }
    }
}

void Renderer::AddToLr(uint32_t surround, bool subtract_from_l, const Meters::Surround& returns)
{
    // ADD_TO_LR (0x0222): the surround channel that OUTPUT saved goes into the left and right channels. The other
    // command (0x024f) takes it from the left channel instead. Each aux bus's meter takes the bus's returns in it.
    FromMain(surround, kScratch, 0x180);
    for (int i = 0; i < kSamples; i++)
    {
        const int64_t s = Load32(dram_, static_cast<uint16_t>(kScratch + 2 * i));
        const auto left = static_cast<uint16_t>(kBuses[0][0] + 2 * i);
        const auto right = static_cast<uint16_t>(kBuses[0][1] + 2 * i);
        Store32(dram_, right, Wrap40(s + Load32(dram_, right)));
        Store32(dram_, left, Wrap40((subtract_from_l ? -s : s) + Load32(dram_, left)));
    }

    for (std::size_t bus = 0; meters_ != nullptr && bus < returns.size(); bus++)
    {
        MeterSums& sums = meter_sums_[meters_->peaks.size() + bus];
        for (std::size_t i = 0; i < returns[bus].size(); i++)
        {
            const int64_t s = returns[bus][i];
            sums[0][i] += subtract_from_l ? -s : s;
            sums[1][i] += s;
        }
    }
}

bool Renderer::Process(uint32_t first)
{
    // PROCESS (0x027e): each PB of the list in turn. The microcode writes the PB back to the address in the PB's curr
    // field, then takes the next.
    uint32_t address = first;
    for (int count = 0; count < kMostPbs; count++)
    {
        // The list gives the PBs' cached addresses. The host gives their physical addresses.
        meter_ = -1;
        if (meters_ != nullptr)
        {
            for (const auto& [pb, meter] : meters_->pbs)
            {
                if (pb == (address & 0x3fffffff) && meter >= 0 && static_cast<std::size_t>(meter) < meter_sums_.size())
                {
                    meter_ = meter;
                }
            }
        }
        LoadPb(address);
        if (!ProcessVoice())
        {
            return false;
        }
        StorePb(uint32_t{dram_[kPbCurr]} << 16 | dram_[kPbCurr + 1u]);
        address = uint32_t{dram_[kPbNext]} << 16 | dram_[kPbNext + 1u];
        if (address == 0)
        {
            return true;
        }
    }

    return false;
}

bool Renderer::ProcessVoice()
{
    // The ITD delays the left and the right channel by the samples that the shifts leave out of the 32 before the
    // frame. The surround channel, and every channel without ITD, takes the frame as it is.
    const bool itd = dram_[kPbItdFlag] != 0;
    const uint32_t itd_buffer = uint32_t{dram_[kPbItdBuffer]} << 16 | dram_[kPbItdBuffer + 1u];
    dram_[kSource] = kFrame;
    if (itd)
    {
        dram_[kSourceL] = static_cast<uint16_t>(kItdHistory + dram_[kPbItdShift]);
        dram_[kSourceR] = static_cast<uint16_t>(kItdHistory + dram_[kPbItdShift + 1u]);
        FromMain(itd_buffer, kItdHistory, 0x40);
    }
    else
    {
        dram_[kSourceL] = kFrame;
        dram_[kSourceR] = kFrame;
    }

    if (dram_[kPbState] == 1)
    {
        const uint16_t src = dram_[kPbSrcSelect];
        const uint16_t filter = dram_[kPbCoefSelect];
        if (src > kSrcNone || (src == kSrcFourTap && filter > 2))
        {
            return false;
        }

        LoadAccelerator();
        if (src == kSrcFourTap)
        {
            FourTap(filter);
        }
        else if (src == kSrcLinear)
        {
            Linear();
        }
        else
        {
            Copy();
        }
        StoreAccelerator();

        Envelope();
        if (dram_[kPbLpf] != 0)
        {
            LowPass(kPbLpf + 1, kFrame, kFrame);
        }
        if (dram_[kPbBiquad] != 0)
        {
            Biquad(kPbBiquad + 1, kFrame, kFrame);
        }

        // The mixes write the depop values afresh.
        for (int i = 0; i < 12; i++)
        {
            dram_[kPbDpop + i] = 0;
        }
        for (int i = 0; i < 8; i++)
        {
            dram_[kPbRmtDpop + i] = 0;
        }
        const uint16_t high = dram_[kPbMixerCtrlHi];
        MixBus(0, dram_[kPbMixerCtrlLo] & 0x1fu);
        MixBus(1, high & 0x1fu);
        MixBus(2, high >> 5 & 0x1fu);
        MixBus(3, high >> 10);
        if (dram_[kPbRemote] != 0)
        {
            Remote();
        }

        // Each ITD shift moves a sample towards its target each frame.
        if (itd)
        {
            for (int side = 0; side < 2; side++)
            {
                uint16_t& shift = dram_[kPbItdShift + side];
                const auto target = static_cast<int16_t>(dram_[kPbItdTarget + side]);
                if (static_cast<int16_t>(shift) > target)
                {
                    shift--;
                }
                else if (static_cast<int16_t>(shift) < target)
                {
                    shift++;
                }
            }
        }
    }

    // The frame's last 32 samples go back as the ITD's history, for a voice that has stopped too.
    if (itd)
    {
        ToMain(kItdOut, itd_buffer, 0x40);
    }

    return true;
}

void Renderer::LoadAccelerator()
{
    // 0x07e6: the format, the addresses, the coefficients, the gain and the decoder's state go into the accelerator's
    // registers. Each register keeps its bits.
    accelerator_.format = dram_[kPbFormat];
    accelerator_.start =
        (uint32_t{dram_[kPbLoopAddress]} << 16 | dram_[kPbLoopAddress + 1u]) & ntdlib::dsp::kAcceleratorAddressMask;
    accelerator_.end =
        (uint32_t{dram_[kPbEndAddress]} << 16 | dram_[kPbEndAddress + 1u]) & ntdlib::dsp::kAcceleratorAddressMask;
    accelerator_.current = (uint32_t{dram_[kPbCurrentAddress]} << 16 | dram_[kPbCurrentAddress + 1u]) &
                           ntdlib::dsp::kAcceleratorCurrentMask;
    for (std::size_t i = 0; i < coefs_.size(); i++)
    {
        coefs_[i] = dram_[kPbCoefs + i];
    }
    accelerator_.gain = dram_[kPbGain];
    accelerator_.pred_scale = dram_[kPbPredScale] & ntdlib::dsp::kAcceleratorPredScaleMask;
    accelerator_.yn1 = dram_[kPbYn1];
    accelerator_.yn2 = dram_[kPbYn2];
    accelerator_.reads_stopped = false;
    reads_zero_ = false;
}

int16_t Renderer::ReadSample()
{
    if (reads_zero_)
    {
        return static_cast<int16_t>(dram_[kZero]);
    }

    // On the Wii, the accelerator reads MEM1 for an address with bit 28 clear, and MEM2 otherwise.
    const auto byte = [this](uint32_t address) -> uint8_t
    {
        const uint8_t* p =
            (address & 0x10000000) == 0 ? memory_->At(address & 0x01ffffff) : memory_->Aram(address & 0x03ffffff);
        return p != nullptr ? *p : 0;
    };

    return static_cast<int16_t>(ntdlib::dsp::AcceleratorNext(accelerator_, coefs_, byte, [this] { AtEnd(); }));
}

void Renderer::AtEnd()
{
    // At the end address the accelerator stops its reads and raises exception 5. The handler (0x0f61) stops a voice
    // that doesn't loop, and points the frame's later reads at a zero. For a voice that loops, it loads the decoder's
    // state at the loop start, and the reads go on. A stream keeps the decoder's history.
    accelerator_.reads_stopped = true;
    if (dram_[kPbLoopFlag] != 1)
    {
        dram_[kPbState] = 0;
        reads_zero_ = true;
        return;
    }

    accelerator_.pred_scale = dram_[kPbLoopPredScale] & ntdlib::dsp::kAcceleratorPredScaleMask;
    if (dram_[kPbType] != 1)
    {
        accelerator_.yn1 = dram_[kPbLoopYn1];
        accelerator_.yn2 = dram_[kPbLoopYn2];
    }
    accelerator_.reads_stopped = false;
}

void Renderer::StoreAccelerator()
{
    // The converters end by storing the decoder's state and the current address back in the PB.
    dram_[kPbYn2] = accelerator_.yn2;
    dram_[kPbYn1] = accelerator_.yn1;
    dram_[kPbPredScale] = accelerator_.pred_scale;
    dram_[kPbCurrentAddress + 1u] = static_cast<uint16_t>(accelerator_.current);
    dram_[kPbCurrentAddress] = static_cast<uint16_t>(accelerator_.current >> 16);
}

void Renderer::FourTap(uint16_t filter)
{
    // The 4-tap converter (0x071a). The ratio advances a 16.16 position. Each whole sample is a read into a ring of the
    // last four. The position's top 7 bits select four coefficients of the filter, which multiply the four samples,
    // oldest first. Each product is doubled.
    const auto ratio = static_cast<int32_t>(uint32_t{dram_[kPbRatio]} << 16 | dram_[kPbRatio + 1u]);
    std::array<int16_t, 4> ring{};
    for (std::size_t i = 0; i < ring.size(); i++)
    {
        ring[i] = static_cast<int16_t>(dram_[kPbLast + i]);
    }
    std::size_t write = 0;
    const auto table = static_cast<uint16_t>(kCoefficientRom + filter * kFilterWords);
    int64_t position = int64_t{dram_[kPbFrac]} + ratio;
    for (int i = 0; i < kSamples; i++)
    {
        const auto reads = static_cast<uint16_t>(MiddleWord(position));
        const auto frac = static_cast<uint32_t>(position & 0xffff);
        for (uint16_t n = 0; n < reads; n++)
        {
            ring[write] = ReadSample();
            write = (write + 1) & 3;
        }
        position = Wrap40(position - int64_t{static_cast<int16_t>(reads)} * 0x10000);
        position = Wrap40(position + ratio);

        const std::size_t first = std::size_t{table} - kCoefficientRom + (frac >> 9) * 4;
        int64_t sum = 0;
        for (std::size_t k = 0; k < 4; k++)
        {
            const auto coefficient = static_cast<int16_t>(ntdlib::dsp::kCoefficientRom[first + k]);
            sum += 2 * int64_t{coefficient} * ring[(write + k) & 3];
        }
        dram_[kFrame + i] = static_cast<uint16_t>(MiddleWord(Wrap40(sum)));
    }
    position = Wrap40(position - ratio);

    for (std::size_t i = 0; i < ring.size(); i++)
    {
        dram_[kPbLast + i] = static_cast<uint16_t>(ring[(write + i) & 3]);
    }
    dram_[kPbFrac] = static_cast<uint16_t>(position);
}

void Renderer::Linear()
{
    // The linear converter (0x076b): the same reads, then the oldest of the four samples and the next one, weighted by
    // the position. At a whole position, the oldest sample as it is.
    const auto ratio = static_cast<int32_t>(uint32_t{dram_[kPbRatio]} << 16 | dram_[kPbRatio + 1u]);
    std::array<int16_t, 4> ring{};
    for (std::size_t i = 0; i < ring.size(); i++)
    {
        ring[i] = static_cast<int16_t>(dram_[kPbLast + i]);
    }
    std::size_t write = 0;
    int64_t position = int64_t{dram_[kPbFrac]} + ratio;
    for (int i = 0; i < kSamples; i++)
    {
        const auto reads = static_cast<uint16_t>(MiddleWord(position));
        for (uint16_t n = 0; n < reads; n++)
        {
            ring[write] = ReadSample();
            write = (write + 1) & 3;
        }
        position = Wrap40(position - int64_t{static_cast<int16_t>(reads)} * 0x10000);

        const int64_t frac = position;
        int16_t sample = ring[write];
        if (frac != 0)
        {
            const auto weight = static_cast<uint16_t>(-frac);
            sample = MiddleWord(int64_t{ring[write]} * weight + int64_t{ring[(write + 1) & 3]} * (frac & 0xffff));
        }
        dram_[kFrame + i] = static_cast<uint16_t>(sample);
        position = Wrap40(position + ratio);
    }
    position = Wrap40(position - ratio);

    for (std::size_t i = 0; i < ring.size(); i++)
    {
        dram_[kPbLast + i] = static_cast<uint16_t>(ring[(write + i) & 3]);
    }
    dram_[kPbFrac] = static_cast<uint16_t>(position);
}

void Renderer::Copy()
{
    // Without a converter (0x07c0), the frame takes 96 samples as they come. The PB keeps the last four.
    for (int i = 0; i < kSamples; i++)
    {
        dram_[kFrame + i] = static_cast<uint16_t>(ReadSample());
    }
    for (int i = 0; i < 4; i++)
    {
        dram_[kPbLast + i] = dram_[kFrame + kSamples - 4 + i];
    }
}

void Renderer::Envelope()
{
    // The volume envelope (0x034c): a gain that starts at the PB's volume and moves by the delta each sample. Each pair
    // of samples steps by twice the delta, a 16-bit signed value. The gain is unsigned, the product doubled and
    // saturated to 16 bits.
    const uint16_t volume = dram_[kPbVolume];
    const uint16_t delta = dram_[kPbDelta];
    const auto step = static_cast<int16_t>(static_cast<uint16_t>(2 * delta));
    for (int i = 0; i < kSamples; i++)
    {
        const uint32_t start = i % 2 == 0 ? volume : volume + uint32_t{delta};
        const auto gain = static_cast<uint16_t>(start + static_cast<uint32_t>((i / 2) * step));
        const auto x = static_cast<int16_t>(dram_[kFrame + i]);
        dram_[kFrame + i] = static_cast<uint16_t>(MiddleWord(2 * int64_t{x} * gain));
    }
    dram_[kPbVolume] = static_cast<uint16_t>(volume + static_cast<uint32_t>((kSamples / 2) * step));
}

void Renderer::LowPass(uint16_t state, uint16_t from, uint16_t to)
{
    // The one-pole low-pass filter (0x06ab): y = a0 x + b0 y1, with the products doubled. a0 multiplies as unsigned
    // (mulx), b0 as signed (maddx). Each output saturates to 16 bits and feeds back that way. The state holds y1, a0
    // and b0.
    auto y = static_cast<int16_t>(dram_[state]);
    const uint16_t a0 = dram_[state + 1u];
    const auto b0 = static_cast<int16_t>(dram_[state + 2u]);
    for (int i = 0; i < kSamples; i++)
    {
        const auto x = static_cast<int16_t>(dram_[from + static_cast<uint16_t>(i)]);
        y = MiddleWord(2 * int64_t{x} * a0 + 2 * int64_t{y} * b0);
        dram_[to + static_cast<uint16_t>(i)] = static_cast<uint16_t>(y);
    }
    dram_[state] = static_cast<uint16_t>(y);
}

void Renderer::Biquad(uint16_t state, uint16_t from, uint16_t to)
{
    // The biquad filter (0x06c3), with coefficients in 2.14. The sum is shifted left two places, rounded to a whole
    // sample (ties to even) and saturated to 16 bits. Each output feeds back that way. The state holds x1, x2, y1, y2,
    // b0, b1, b2, a1 and a2.
    auto x1 = static_cast<int16_t>(dram_[state]);
    auto x2 = static_cast<int16_t>(dram_[state + 1u]);
    auto y1 = static_cast<int16_t>(dram_[state + 2u]);
    auto y2 = static_cast<int16_t>(dram_[state + 3u]);
    const auto b0 = static_cast<int16_t>(dram_[state + 4u]);
    const auto b1 = static_cast<int16_t>(dram_[state + 5u]);
    const auto b2 = static_cast<int16_t>(dram_[state + 6u]);
    const auto a1 = static_cast<int16_t>(dram_[state + 7u]);
    const auto a2 = static_cast<int16_t>(dram_[state + 8u]);
    for (int i = 0; i < kSamples; i++)
    {
        const auto x = static_cast<int16_t>(dram_[from + static_cast<uint16_t>(i)]);
        const int64_t sum = int64_t{b0} * x + int64_t{b1} * x1 + int64_t{b2} * x2 + int64_t{a1} * y1 + int64_t{a2} * y2;
        const int16_t y = MiddleWord(Wrap40(Round(Wrap40(sum * 4))));
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        dram_[to + static_cast<uint16_t>(i)] = static_cast<uint16_t>(y);
    }
    dram_[state] = static_cast<uint16_t>(x1);
    dram_[state + 1u] = static_cast<uint16_t>(x2);
    dram_[state + 2u] = static_cast<uint16_t>(y1);
    dram_[state + 3u] = static_cast<uint16_t>(y2);
}

void Renderer::MixBus(int bus, uint32_t bits)
{
    // The handlers that the mixer control selects for each bus (the tables at 0x0d13, 0x0d33, 0x0d53 and 0x0d73). The
    // surround channel mixes with a volume that moves when its ramp bit is set, and otherwise when its bit is set. The
    // ramp bit of the left and right channels mixes both with volumes that move. Otherwise each mixes when its bit is
    // set. Bus C's sixth bit makes its left and right channels take the frame without the ITD.
    const auto b = static_cast<std::size_t>(bus);
    const bool unshifted = bus == 3 && (bits & 0x20) != 0;
    const uint16_t left = unshifted ? kFrame : dram_[kSourceL];
    const uint16_t right = unshifted ? kFrame : dram_[kSourceR];
    const auto volume_l = static_cast<uint16_t>(kPbMix + 4 * bus);
    const auto volume_s = static_cast<uint16_t>(kPbMix + 16 + 2 * bus);
    if ((bits & 0x18) != 0)
    {
        dram_[kPbDpop + 8 + b] = MixChannel(kFrame, volume_s, (bits & 0x10) != 0, kBuses[b][2], kSamples);
    }
    // The voice's meter adds up its mix into the main bus's left and right.
    const bool ramp = (bits & 4) != 0;
    MeterSums* sums = bus == 0 && meter_ >= 0 ? &meter_sums_[static_cast<std::size_t>(meter_)] : nullptr;
    if (ramp || (bits & 1) != 0)
    {
        dram_[kPbDpop + b] =
            MixChannel(left, volume_l, ramp, kBuses[b][0], kSamples, sums != nullptr ? (*sums)[0].data() : nullptr);
    }
    if (ramp || (bits & 2) != 0)
    {
        dram_[kPbDpop + 4 + b] = MixChannel(right, static_cast<uint16_t>(volume_l + 2), ramp, kBuses[b][1], kSamples,
                                            sums != nullptr ? (*sums)[1].data() : nullptr);
    }
}

uint16_t Renderer::MixChannel(uint16_t source, uint16_t volume, bool ramp, uint16_t bus, int samples, int64_t* meter)
{
    // Adds the samples into one bus channel (0x0bd1, 0x0beb, 0x0c21 and 0x0c51): each product, doubled, is added below
    // the bus sample's 16 fractional bits, in 40 bits. The volume is constant, or it starts at the PB's volume and
    // moves by the delta each sample. The PB keeps the volume after the frame. Returns the depop value: the middle word
    // of the last product. A meter adds up what each product adds to the bus sample.
    const uint16_t start = dram_[volume];
    const uint16_t delta = dram_[volume + 1u];
    int64_t product = 0;
    for (int i = 0; i < samples; i++)
    {
        const auto gain = ramp ? static_cast<uint16_t>(start + static_cast<uint32_t>(i) * delta) : start;
        const auto x = static_cast<int16_t>(dram_[static_cast<uint16_t>(source + i) & 0x0fffu]);
        product = 2 * int64_t{x} * gain;
        const auto at = static_cast<uint16_t>(bus + 2 * i);
        Store32(dram_, at, Wrap40(Wrap40(Load32(dram_, at) * 0x10000) + product) >> 16);
        if (meter != nullptr)
        {
            meter[i] += product >> 16;
        }
    }
    if (ramp)
    {
        dram_[volume] = static_cast<uint16_t>(start + static_cast<uint32_t>(samples) * delta);
    }

    return static_cast<uint16_t>(MiddleWord(product));
}

void Renderer::Remote()
{
    // The Wii Remotes' path (0x03a2): the remote filter, a 4-tap converter from 32 kHz to 6 kHz on the 8 kHz filter,
    // then the eight mixes. Each mix takes two bits of the remote mixer control: none, a constant volume, or a volume
    // that moves.
    const uint16_t iir = dram_[kPbRmtIir];
    if (iir == 0)
    {
        for (int i = 0; i < kSamples; i++)
        {
            dram_[kRemoteInput + i] = dram_[kFrame + i];
        }
    }
    else if (iir == 2)
    {
        Biquad(kPbRmtIir + 1, kFrame, kRemoteInput);
    }
    else
    {
        LowPass(kPbRmtIir + 1, kFrame, kRemoteInput);
    }

    for (int i = 0; i < 4; i++)
    {
        dram_[kScratch + i] = dram_[kPbRmtLast + i];
    }
    int64_t position = dram_[kPbRmtFrac];
    uint16_t index = 0;
    for (int j = 0; j < kRemoteSamples; j++)
    {
        position = Wrap40(position + kRemoteRatio);
        index = static_cast<uint16_t>(MiddleWord(position));
        const auto frac = static_cast<uint32_t>(position & 0xffff);
        const std::size_t first = (frac >> 9) * 4;
        int64_t sum = 0;
        for (std::size_t k = 0; k < 4; k++)
        {
            const auto coefficient = static_cast<int16_t>(ntdlib::dsp::kCoefficientRom[first + k]);
            const auto sample = static_cast<int16_t>(dram_[(kScratch + index + k) & 0x0fffu]);
            sum += 2 * int64_t{coefficient} * sample;
        }
        dram_[kRemoteFrame + j] = static_cast<uint16_t>(MiddleWord(Wrap40(sum)));
    }
    for (int i = 0; i < 4; i++)
    {
        dram_[kPbRmtLast + i] = dram_[(kScratch + index + i) & 0x0fff];
    }
    dram_[kPbRmtFrac] = static_cast<uint16_t>(position);

    const uint16_t control = dram_[kPbRmtMixerCtrl];
    for (std::size_t k = 0; k < kRemoteBuses.size(); k++)
    {
        const unsigned bits = control >> (2 * k) & 3u;
        uint16_t dpop = 0;
        if (bits != 0)
        {
            dpop = MixChannel(kRemoteFrame, static_cast<uint16_t>(kPbRmtMix + 2 * k), bits >= 2, kRemoteBuses[k],
                              kRemoteSamples);
        }
        dram_[kRemoteDpop[k]] = dpop;
    }
}

void Renderer::MixAux(int bus, uint16_t volume, uint32_t write, uint32_t read)
{
    // MIX_AUX_A, B and C (0x049b, 0x04b4 and 0x04cd): the bus's three channels go out to the frame that the callback
    // processes later, and the frame that it has processed comes back in their place. That frame goes into the main
    // bus at the return volume, which moves from the last frame's. The build of October 2006 doesn't double the
    // return's products. The bus's meter takes the left and right, and keeps the surround channel for the next frame's
    // ADD_TO_LR.
    const auto b = static_cast<std::size_t>(bus);
    const uint16_t base = kBuses[b + 1][0];
    const uint16_t old = dram_[kAuxVolumes + b];
    dram_[kAuxVolumes + b] = volume;
    ToMain(base, write, 0x480);
    const std::array<uint16_t, kSamples> gains = Ramp(old, volume);
    FromMain(read, base, 0x480);
    for (std::size_t channel = 0; channel < 3; channel++)
    {
        for (int i = 0; i < kSamples; i++)
        {
            const auto main_at = static_cast<uint16_t>(kBuses[0][channel] + 2 * i);
            const auto aux_at = static_cast<uint16_t>(kBuses[b + 1][channel] + 2 * i);
            const int64_t sample = Load32(dram_, aux_at);
            const uint16_t gain = gains[static_cast<std::size_t>(i)];
            const int64_t added = build_ == Build::kOctober2006 ? ScaledOnce(sample, gain) : Scaled(sample, gain);
            Store32(dram_, main_at, Wrap40(Load32(dram_, main_at) + added));
            if (meters_ != nullptr && channel < 2)
            {
                meter_sums_[meters_->peaks.size() + b][channel][static_cast<std::size_t>(i)] += added;
            }
            else if (meters_ != nullptr)
            {
                meters_->aux_surround[b][static_cast<std::size_t>(i)] = added;
            }
        }
    }
}

void Renderer::Compressor(uint16_t threshold, uint16_t release, uint32_t table)
{
    // COMPRESSOR (0x057b). While any sample of the left and right channels reaches the threshold, the frame takes a row
    // of the table's gains: the attack's row for the frames counted down so far, and the release count starts again.
    // In a quiet frame, the count goes down, and the frame takes the release's row for the count. Once the count is 0,
    // a quiet frame stays as it is.
    bool loud = false;
    for (int i = 0; i < 2 * kSamples && !loud; i++)
    {
        const int64_t sample = Load32(dram_, static_cast<uint16_t>(2 * i));
        loud = (sample < 0 ? -sample : sample) >= threshold;
    }

    uint32_t row = 0;
    if (loud)
    {
        row = table + uint32_t{dram_[kCompressorCount]} * 0xc0;
        dram_[kCompressorCount] = release;
    }
    else
    {
        if (dram_[kCompressorCount] == 0)
        {
            return;
        }
        dram_[kCompressorCount]--;
        row = table + uint32_t{dram_[kCompressorCount]} * 0xc0 + 0x840;
    }

    FromMain(row, kScratch, 0xc0);
    for (const uint16_t channel : {kBuses[0][0], kBuses[0][1]})
    {
        for (int i = 0; i < kSamples; i++)
        {
            const auto at = static_cast<uint16_t>(channel + 2 * i);
            const int64_t sample = Load32(dram_, at);
            const uint16_t gain = dram_[kScratch + i];
            const int64_t high = Wrap40(2 * (sample >> 16) * gain * 0x10000);
            Store32(dram_, at, Wrap40(high + 2 * (sample & 0xffff) * gain) >> 16);
        }
    }
}

void Renderer::WmOutput(const std::array<uint32_t, 4>& remotes)
{
    // WM_OUTPUT (0x067c): each Wii Remote's main bus, saturated to 16 bits, goes to its buffer.
    for (std::size_t remote = 0; remote < remotes.size(); remote++)
    {
        const auto staging = static_cast<uint16_t>(kRemoteBuses[1] + kRemoteSamples * remote);
        for (int i = 0; i < kRemoteSamples; i++)
        {
            const int64_t sample = Load32(dram_, static_cast<uint16_t>(kRemoteBuses[2 * remote] + 2 * i));
            dram_[staging + static_cast<uint16_t>(i)] = static_cast<uint16_t>(MiddleWord(Wrap40(sample * 0x10000)));
        }
        ToMain(staging, remotes[remote], kRemoteSamples * 2);
    }
}

void Renderer::Output(uint16_t volume, uint32_t surround, uint32_t output)
{
    // OUTPUT (0x060b): the surround channel goes out for the next frame's ADD_TO_LR. The left and right channels go out
    // at the master volume, which moves from the last frame's, saturated to 16 bits: right, then left, for each
    // sample.
    ToMain(kBuses[0][2], surround, 0x180);
    const std::array<uint16_t, kSamples> gains = Ramp(dram_[kMasterVolume], volume);
    dram_[kMasterVolume] = volume;
    const uint16_t staging = kBuses[1][0];
    for (int i = 0; i < kSamples; i++)
    {
        const uint16_t gain = gains[static_cast<std::size_t>(i)];
        for (int side = 0; side < 2; side++)
        {
            const uint16_t channel = side == 0 ? kBuses[0][1] : kBuses[0][0];
            const int64_t sample = Load32(dram_, static_cast<uint16_t>(channel + 2 * i));
            const int64_t scaled = Wrap40(Scaled(sample, gain) * 0x10000);
            dram_[staging + static_cast<uint16_t>(2 * i + side)] = static_cast<uint16_t>(MiddleWord(scaled));
        }
    }
    ToMain(staging, output, 0x180);
}

} // namespace ntdlib::rvl::ax
