// SPDX-License-Identifier: MIT

#include "ntdlib/rvl/ax/render.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "xsflib/bytes.h"

#include "ntdlib/dsp/memory.h"
#include "ntdlib/rvl/ax/pb.h"
#include "rvl/ax/render_internal.h"

namespace ntdlib::rvl::ax
{

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
    // The build of October 2006 keeps its PBs in another layout. Its microcode copies a PB into DRAM as it is.
    FromMain(address, kPb, build_ == Build::kOctober2006 ? uint32_t{kPbSizeOctober2006} : kPbBytes);
}

void Renderer::StorePb(uint32_t address)
{
    ToMain(kPb, address, build_ == Build::kOctober2006 ? uint32_t{kPbSizeOctober2006} : kPbBytes);
}

uint16_t Renderer::Field(uint16_t word) const
{
    if (build_ != Build::kOctober2006)
    {
        return word;
    }
    if (word >= kPbRemote)
    {
        return static_cast<uint16_t>(word - 1);
    }
    if (word >= kPbDpop)
    {
        return static_cast<uint16_t>(word + 5);
    }

    return word;
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
                const uint32_t surround_address = Arg32(at);
                Output(volume, surround_address, Arg32(at));
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
            if (meters_ != nullptr)
            {
                StorePeaks(meter_sums_, *meters_);
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
        if (!(build_ == Build::kOctober2006 ? ProcessVoiceOctober2006() : ProcessVoice()))
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
        if (!Convert(dram_[kPbSrcSelect], dram_[kPbCoefSelect], kFrame, kSamples))
        {
            return false;
        }

        Envelope(kFrame, kSamples);
        if (dram_[kPbLpf] != 0)
        {
            LowPass(kPbLpf + 1, kFrame, kFrame);
        }
        if (dram_[kPbBiquad] != 0)
        {
            Biquad(kPbBiquad + 1, kFrame, kFrame);
        }

        ClearDepop();
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

bool Renderer::ProcessVoiceOctober2006()
{
    // The build of October 2006 (0x0278 and 0x0554 load a PB, 0x0349 mixes it). The PB's updates come into DRAM with
    // the PB. The microcode takes the sample rate converter, the filter table and the mixes that the PB selects then,
    // and sets up the ITD's history. It then works through the voice in three parts of 32 samples. Each part takes its
    // updates first. A part of a voice that doesn't play does nothing more. In a part of a voice that plays, the
    // converter adds the part's samples after the last part's. The envelope, the filters, the mixes and the Wii
    // Remotes' path then work on those samples.
    FromMain(uint32_t{dram_[kPbUpdateBuffer]} << 16 | dram_[kPbUpdateBuffer + 1u], kUpdates, kUpdatesBytes);
    const uint16_t src = dram_[kPbSrcSelect];
    const uint16_t filter = dram_[kPbCoefSelect];
    const uint16_t high = dram_[kPbMixerCtrlHi];
    const std::array<uint32_t, 4> bits = {dram_[kPbMixerCtrlLo] & 0x1fu, high & 0x1fu, high >> 5 & 0x1fu,
                                          uint32_t{high} >> 10};

    // Where the converter writes (0x0cda), the part's samples (0x0cdd), and the left and right channels' samples
    // (0x0cde and 0x0cdf). With ITD, the left and right channels start in the 32 samples of history.
    uint16_t write = kFrame;
    uint16_t source = kFrame;
    uint16_t left = kFrame;
    uint16_t right = kFrame;
    if (dram_[kPbItdFlag] != 0)
    {
        left = static_cast<uint16_t>(kItdHistory + dram_[kPbItdShift]);
        right = static_cast<uint16_t>(kItdHistory + dram_[kPbItdShift + 1u]);
        FromMain(uint32_t{dram_[kPbItdBuffer]} << 16 | dram_[kPbItdBuffer + 1u], kItdHistory, 0x40);
    }

    uint16_t update = kUpdates;
    for (int part = 0; part < kParts; part++)
    {
        ApplyUpdates(part, update);
        if (dram_[kPbState] != 1)
        {
            continue;
        }

        if (!Convert(src, filter, write, kPartSamples))
        {
            return false;
        }
        write = static_cast<uint16_t>(write + kPartSamples);

        Envelope(source, kPartSamples);
        if (dram_[Field(kPbLpf)] != 0)
        {
            LowPass(static_cast<uint16_t>(Field(kPbLpf) + 1), source, source, kPartSamples);
        }
        if (dram_[kPbSecondFilter] != 0)
        {
            SecondFilter(source);
        }

        ClearDepop();
        for (int bus = 0; bus < 4; bus++)
        {
            MixBusOctober2006(bus, bits[static_cast<std::size_t>(bus)], part, source, left, right);
        }
        if (dram_[Field(kPbRemote)] != 0)
        {
            RemoteOctober2006(part, source);
        }

        source = write;
        left = write;
        right = write;
        if (dram_[kPbItdFlag] != 0)
        {
            MoveItdOctober2006(write, left, right);
        }
    }

    // The last part's samples go back as the ITD's history, for a voice that has stopped too.
    if (dram_[kPbItdFlag] != 0)
    {
        ToMain(kItdOut, uint32_t{dram_[kPbItdBuffer]} << 16 | dram_[kPbItdBuffer + 1u], 0x40);
    }

    return true;
}

void Renderer::ApplyUpdates(int part, uint16_t& update)
{
    // Each of the part's updates writes a value to a word of the PB. AX's CPU side fills at most the 32 pairs that the
    // buffer holds.
    for (uint16_t n = dram_[kPbUpdateCounts + part]; n > 0; n--)
    {
        const auto at = static_cast<uint16_t>(kPb + dram_[update & 0x0fffu]);
        if (at < dram_.size())
        {
            dram_[at] = dram_[(update + 1u) & 0x0fffu];
        }
        update = static_cast<uint16_t>(update + 2);
    }
}

void Renderer::MoveItdOctober2006(uint16_t write, uint16_t& left, uint16_t& right)
{
    // Each ITD shift moves a sample towards its target after each part. The next part's left and right channels start
    // that many samples into the 32 before the part. In 40-bit mode, the shifts and the starts saturate.
    for (int side = 0; side < 2; side++)
    {
        uint16_t& shift = dram_[kPbItdShift + side];
        const auto target = static_cast<int16_t>(dram_[kPbItdTarget + side]);
        int64_t moved = static_cast<int16_t>(shift);
        if (moved > target)
        {
            moved--;
        }
        else if (moved < target)
        {
            moved++;
        }
        shift = Saturated(moved);
        (side == 0 ? left : right) = Saturated(int64_t{write} - kPartSamples + moved);
    }
}

} // namespace ntdlib::rvl::ax
