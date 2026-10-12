// SPDX-License-Identifier: MIT

#include "ntdlib/flipper/dsp_interface.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#include "ntdlib/dsp/dsp.h"
#include "ntdlib/dsp/memory.h"
#include "ntdlib/ppc/cpu.h"

namespace ntdlib::flipper
{
namespace
{

// DSPCR's bits.
namespace dspcr
{

constexpr uint16_t kReset = 1u << 0;
constexpr uint16_t kAiInterrupt = 1u << 3;
constexpr uint16_t kAiMask = 1u << 4;
constexpr uint16_t kAramInterrupt = 1u << 5;
constexpr uint16_t kAramMask = 1u << 6;
constexpr uint16_t kDspInterrupt = 1u << 7;
constexpr uint16_t kDspMask = 1u << 8;
constexpr uint16_t kAramBusy = 1u << 9;

// The bits that the CPU side keeps, and among them the interrupt flags. Writing 1 to an interrupt flag clears it.
constexpr uint16_t kCpuSide = 0xf3f8;
constexpr uint16_t kFlags = kAiInterrupt | kAramInterrupt | kDspInterrupt;

} // namespace dspcr

// The DSP's timing: a slice every 12,600 CPU cycles, at 6 CPU cycles per DSP cycle. A mailbox poll takes 12 DSP cycles.
constexpr int64_t kSliceCycles = 12'600;
constexpr int64_t kCpuCyclesPerDspCycle = 6;
constexpr int64_t kPollDspCycles = 12;

// The AI DMA's first interrupt comes 200 cycles after the DMA starts. The blocks' timing is an option.
constexpr uint64_t kFirstAiInterrupt = 200;
constexpr uint16_t kAiEnable = 0x8000;

// The ARAM DMA takes 246 CPU cycles for each 32 bytes. On the Wii, "ARAM" is MEM2.
constexpr uint64_t kAramCyclesPerBlock = 246;

} // namespace

template <typename Memory>
DspInterface<Memory>::DspInterface(dsp::Dsp<Memory>& dsp, Memory& memory, ppc::Cpu& cpu,
                                   const DspInterfaceOptions& options)
    : dsp_(dsp), memory_(memory), cpu_(cpu), options_(options)
{
    ai_next_block_ = options_.block_cycles_32k;
    next_slice_ = kSliceCycles;
}

template <typename Memory>
uint16_t DspInterface<Memory>::Read(uint32_t offset)
{
    switch (offset)
    {
    case 0x000:
        RunForPoll();
        return dsp_.CpuReadMailboxHigh(true);

    case 0x002:
        return dsp_.CpuReadMailboxLow(true);

    case 0x004:
        RunForPoll();
        return dsp_.CpuReadMailboxHigh(false);

    case 0x006:
        return dsp_.CpuReadMailboxLow(false);

    case 0x00a:
        return (control_ & dspcr::kCpuSide) | dsp_.Control(cpu_.TimeBase());

    case 0x012:
        return aram_size_;

    case 0x016:
        return 1;

    case 0x01a:
        return aram_refresh_;

    case 0x020:
        return static_cast<uint16_t>(aram_dma_main_ >> 16);

    case 0x022:
        return static_cast<uint16_t>(aram_dma_main_);

    case 0x024:
        return static_cast<uint16_t>(aram_dma_aram_ >> 16);

    case 0x026:
        return static_cast<uint16_t>(aram_dma_aram_);

    case 0x028:
        return static_cast<uint16_t>(aram_dma_count_ >> 16);

    case 0x02a:
        return static_cast<uint16_t>(aram_dma_count_);

    case 0x030:
        return static_cast<uint16_t>(ai_start_ >> 16);

    case 0x032:
        return static_cast<uint16_t>(ai_start_);

    case 0x036:
        return ai_control_;

    case 0x03a:
        return static_cast<uint16_t>(ai_remaining_ == 0 ? 0 : ai_remaining_ - 1);

    default:
        return 0;
    }
}

template <typename Memory>
void DspInterface<Memory>::Write(uint32_t offset, uint16_t value)
{
    switch (offset)
    {
    case 0x000:
        dsp_.CpuWriteMailboxHigh(value);
        break;

    case 0x002:
        dsp_.CpuWriteMailboxLow(value);
        break;

    case 0x00a:
        SetControl(value);
        break;

    case 0x012:
        aram_size_ = value & 0x7f;
        break;

    case 0x01a:
        aram_refresh_ = value & 0x7ff;
        break;

    case 0x020:
        aram_dma_main_ = (aram_dma_main_ & 0xffff) | uint32_t{value & 0x03ffu} << 16;
        break;

    case 0x022:
        aram_dma_main_ = (aram_dma_main_ & 0xffff0000) | (value & 0xffe0u);
        break;

    case 0x024:
        aram_dma_aram_ = (aram_dma_aram_ & 0xffff) | uint32_t{value & 0x03ffu} << 16;
        break;

    case 0x026:
        aram_dma_aram_ = (aram_dma_aram_ & 0xffff0000) | (value & 0xffe0u);
        break;

    case 0x028:
        aram_dma_count_ = (aram_dma_count_ & 0xffff) | uint32_t{value & 0x83ffu} << 16;
        break;

    case 0x02a:
        aram_dma_count_ = (aram_dma_count_ & 0xffff0000) | (value & 0xffe0u);
        AramDma();
        break;

    case 0x030:
        ai_start_ = (ai_start_ & 0xffff) | uint32_t{static_cast<uint16_t>(value & options_.ai_start_high_mask)} << 16;
        break;

    case 0x032:
        ai_start_ = (ai_start_ & 0xffff0000) | (value & 0xffe0u);
        break;

    case 0x036:
        {
            // Starting the DMA begins a pass at once. A change while the DMA runs waits for the pass to end.
            const bool was_enabled = (ai_control_ & kAiEnable) != 0;
            ai_control_ = value;
            if (!was_enabled && (value & kAiEnable))
            {
                ai_current_ = ai_start_;
                ai_remaining_ = value & 0x7fff;
                ai_first_interrupt_ = cpu_.Cycles() + kFirstAiInterrupt;
            }
            break;
        }

    default:
        break;
    }
}

template <typename Memory>
void DspInterface<Memory>::SetControl(uint16_t value)
{
    // The DSP side first, then the CPU side. A reset stops the AI DMA. The masks and bits 12-15 come from the value. A
    // 1 clears an interrupt flag.
    dsp_.WriteControl(value, cpu_.TimeBase());
    if (value & dspcr::kReset)
    {
        ai_control_ = 0;
    }

    constexpr uint16_t kFromValue = dspcr::kAiMask | dspcr::kAramMask | dspcr::kDspMask | 0xf000;
    control_ = static_cast<uint16_t>((control_ & ~kFromValue & ~(value & dspcr::kFlags)) | (value & kFromValue));
}

template <typename Memory>
bool DspInterface<Memory>::InterruptPending() const
{
    return ((control_ & dspcr::kAiInterrupt) && (control_ & dspcr::kAiMask)) ||
           ((control_ & dspcr::kAramInterrupt) && (control_ & dspcr::kAramMask)) ||
           ((control_ & dspcr::kDspInterrupt) && (control_ & dspcr::kDspMask));
}

template <typename Memory>
uint64_t DspInterface<Memory>::NextEvent() const
{
    return std::min({next_slice_, aram_dma_done_, ai_first_interrupt_, ai_next_block_});
}

template <typename Memory>
void DspInterface<Memory>::Advance(uint64_t now)
{
    // The DSP's slice. The DSP runs for as many cycles as the budget allows. The budget then gets the next slice's
    // share. The next slice is scheduled earlier by however late this one ran.
    if (now >= next_slice_)
    {
        const uint64_t late = std::min<uint64_t>(now - next_slice_, kSliceCycles);
        const int64_t cycles = budget_ / kCpuCyclesPerDspCycle;
        budget_ %= kCpuCyclesPerDspCycle;
        RunBatch(cycles);
        budget_ += kSliceCycles;
        next_slice_ = now + kSliceCycles - late;
    }

    if (now >= aram_dma_done_)
    {
        control_ = static_cast<uint16_t>((control_ & ~dspcr::kAramBusy) | dspcr::kAramInterrupt);
        aram_dma_done_ = UINT64_MAX;
    }

    if (now >= ai_first_interrupt_)
    {
        control_ |= dspcr::kAiInterrupt;
        ai_first_interrupt_ = UINT64_MAX;
    }

    // The AI DMA's blocks: 32 bytes from the current address while the DMA is on, or silence when it's off.
    while (now >= ai_next_block_)
    {
        static constexpr std::array<uint8_t, 32> kSilence{};
        if (ai_control_ & kAiEnable)
        {
            const uint8_t* block = memory_.At(ai_current_, 32);
            if (on_audio)
            {
                on_audio(block != nullptr ? std::span<const uint8_t, 32>(block, 32) : std::span(kSilence));
            }

            if (ai_remaining_ != 0)
            {
                ai_remaining_--;
                ai_current_ += 32;
            }
            if (ai_remaining_ == 0)
            {
                ai_current_ = ai_start_;
                ai_remaining_ = ai_control_ & 0x7fff;
                control_ |= dspcr::kAiInterrupt;
            }
        }
        else if (on_audio)
        {
            on_audio(kSilence);
        }

        ai_next_block_ += rate_32k_ ? options_.block_cycles_32k : options_.block_cycles_48k;
    }
}

template <typename Memory>
void DspInterface<Memory>::RunBatch(int64_t cycles)
{
    // First eight steps that stop at a halt. Then groups of eight steps that also stop at a polling loop. Each group is
    // followed by up to 200 steps with no checks. Stopping early forfeits the rest of the batch.
    int64_t ran = 0;
    for (int i = 0; i < 8 && ran < cycles && !dsp_.Halted(); i++, ran++)
    {
        dsp_.Step();
    }
    while (ran < cycles && !dsp_.Halted())
    {
        bool stopped = false;
        for (int i = 0; i < 8 && ran < cycles; i++, ran++)
        {
            if (dsp_.Halted() || dsp_.AtIdleLoop())
            {
                stopped = true;
                break;
            }

            dsp_.Step();
        }
        if (stopped)
        {
            break;
        }

        for (int i = 0; i < 200 && ran < cycles; i++, ran++)
        {
            dsp_.Step();
        }
    }

    // The DSP's interrupt request reaches DSPCR at the end of the batch.
    if (dsp_.TakeInterruptRequest())
    {
        control_ |= dspcr::kDspInterrupt;
    }
}

template <typename Memory>
void DspInterface<Memory>::RunForPoll()
{
    if (budget_ > kPollDspCycles * kCpuCyclesPerDspCycle)
    {
        RunBatch(kPollDspCycles);
        budget_ -= kPollDspCycles * kCpuCyclesPerDspCycle;
    }
}

template <typename Memory>
void DspInterface<Memory>::AramDma()
{
    // The data moves at once, 8 bytes at a time. The registers end with the end addresses and a count of 0. The
    // interrupt comes later. ARAM has no expansion: a transfer that starts past its end moves nothing. The SDK's ARInit
    // probes for an expansion that way, and it then finds none.
    const bool to_main = (aram_dma_count_ & 0x80000000) != 0;
    uint32_t count = aram_dma_count_ & 0x7fffffff;
    control_ |= dspcr::kAramBusy;
    aram_dma_done_ = cpu_.Cycles() + count / 32 * kAramCyclesPerBlock;
    cpu_.LimitRun(aram_dma_done_);

    aram_dma_main_ &= 0x03ffffff;
    aram_dma_aram_ &= 0x03ffffff;
    if (memory_.Aram(aram_dma_aram_) == nullptr)
    {
        return;
    }

    for (; count > 0; count -= 8, aram_dma_main_ += 8, aram_dma_aram_ += 8)
    {
        uint8_t* main = memory_.At(aram_dma_main_, 8);
        uint8_t* aram = memory_.Aram(aram_dma_aram_, 8);
        if (main == nullptr || aram == nullptr)
        {
            continue;
        }

        if (to_main)
        {
            std::memcpy(main, aram, 8);
        }
        else
        {
            std::memcpy(aram, main, 8);
            uint8_t* mirror = memory_.Aram(aram_dma_aram_ + 0x400000, 8);
            if (options_.aram_mirror && (aram_size_ & 0xf) == 4 && aram_dma_aram_ < 0x400000 && mirror != nullptr)
            {
                std::memcpy(mirror, main, 8);
            }
        }
    }
    aram_dma_count_ &= 0x80000000;
}

template class DspInterface<dsp::GameCubeMemory>;
template class DspInterface<dsp::WiiMemory>;

} // namespace ntdlib::flipper
