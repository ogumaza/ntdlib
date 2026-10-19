// SPDX-License-Identifier: MIT

#include "ntdlib/flipper/dsp_interface.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#include "ntdlib/dsp/dsp.h"
#include "ntdlib/dsp/memory.h"
#include "ntdlib/flipper/audio_interface.h"
#include "ntdlib/ppc/cpu.h"

namespace ntdlib::flipper
{
namespace
{

// Whether each of the DSP interface's blocks lasts 8 periods of the audio interface's sample counter, at both rates.
constexpr bool BlocksMatch(const DspInterfaceOptions& dsp, const AudioInterfaceOptions& ai)
{
    return static_cast<double>(dsp.block_cycles_32k) == 8 * ai.clock * ai.multiplier_32k / ai.divisor_32k &&
           static_cast<double>(dsp.block_cycles_48k) == 8 * ai.clock * ai.multiplier_48k / ai.divisor_48k;
}

static_assert(BlocksMatch(kGameCubeDspInterface, kGameCubeAudioInterface));
static_assert(BlocksMatch(kWiiDspInterface, kWiiAudioInterface));

// DSPCR's bits on the CPU side. dsp::control has the DSP's side, its reset among them.
namespace dspcr
{

constexpr uint16_t kAiInterrupt = 1u << 3;
constexpr uint16_t kAiMask = 1u << 4;
constexpr uint16_t kAramInterrupt = 1u << 5;
constexpr uint16_t kAramMask = 1u << 6;
constexpr uint16_t kDspInterrupt = 1u << 7;
constexpr uint16_t kDspMask = 1u << 8;
constexpr uint16_t kAramBusy = 1u << 9;

// The bits that the CPU side keeps, and among them the interrupt flags. Writing 1 to an interrupt flag clears it.
constexpr uint16_t kCpuSide = static_cast<uint16_t>(~dsp::control::kDspSide);
constexpr uint16_t kFlags = kAiInterrupt | kAramInterrupt | kDspInterrupt;

} // namespace dspcr

// The registers' offsets from 0x0C005000.
namespace reg
{

constexpr uint32_t kMailboxToDspHigh = 0x000;
constexpr uint32_t kMailboxToDspLow = 0x002;
constexpr uint32_t kMailboxToCpuHigh = 0x004;
constexpr uint32_t kMailboxToCpuLow = 0x006;
constexpr uint32_t kControl = 0x00a; // DSPCR
constexpr uint32_t kAramSize = 0x012;
constexpr uint32_t kAramMode = 0x016;
constexpr uint32_t kAramRefresh = 0x01a;
constexpr uint32_t kAramDmaMainHigh = 0x020; // the ARAM DMA's address in main memory
constexpr uint32_t kAramDmaMainLow = 0x022;
constexpr uint32_t kAramDmaAramHigh = 0x024; // the ARAM DMA's address in ARAM
constexpr uint32_t kAramDmaAramLow = 0x026;
constexpr uint32_t kAramDmaCountHigh = 0x028;
constexpr uint32_t kAramDmaCountLow = 0x02a;
constexpr uint32_t kAiDmaStartHigh = 0x030;
constexpr uint32_t kAiDmaStartLow = 0x032;
constexpr uint32_t kAiDmaControl = 0x036;
constexpr uint32_t kAiDmaBlocksLeft = 0x03a;

} // namespace reg

// The DSP's timing: a slice every 12,600 CPU cycles, at 6 CPU cycles per DSP cycle. A mailbox poll takes 12 DSP cycles.
constexpr int64_t kSliceCycles = 12'600;
constexpr int64_t kCpuCyclesPerDspCycle = 6;
constexpr int64_t kPollDspCycles = 12;

// The AI DMA's first interrupt comes 200 cycles after the DMA starts. The blocks' timing is an option. In the DMA's
// control register, bit 15 enables the DMA, and bits 14-0 give a pass's length in 32-byte blocks.
constexpr uint64_t kFirstAiInterrupt = 200;
constexpr uint16_t kAiEnable = 0x8000;
constexpr uint16_t kAiLength = 0x7fff;

// The ARAM DMA takes 246 CPU cycles for each 32 bytes. On the Wii, "ARAM" is MEM2. Bit 31 of the DMA's count gives the
// direction: set for ARAM to main memory.
constexpr uint64_t kAramCyclesPerBlock = 246;
constexpr uint32_t kAramToMain = 0x80000000;

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
    case reg::kMailboxToDspHigh:
        RunForPoll();
        return dsp_.CpuReadMailboxHigh(true);

    case reg::kMailboxToDspLow:
        return dsp_.CpuReadMailboxLow(true);

    case reg::kMailboxToCpuHigh:
        RunForPoll();
        return dsp_.CpuReadMailboxHigh(false);

    case reg::kMailboxToCpuLow:
        return dsp_.CpuReadMailboxLow(false);

    case reg::kControl:
        return (control_ & dspcr::kCpuSide) | dsp_.Control(cpu_.TimeBase());

    case reg::kAramSize:
        return aram_size_;

    case reg::kAramMode: // reads 1 and ignores writes
        return 1;

    case reg::kAramRefresh:
        return aram_refresh_;

    case reg::kAramDmaMainHigh:
        return static_cast<uint16_t>(aram_dma_main_ >> 16);

    case reg::kAramDmaMainLow:
        return static_cast<uint16_t>(aram_dma_main_);

    case reg::kAramDmaAramHigh:
        return static_cast<uint16_t>(aram_dma_aram_ >> 16);

    case reg::kAramDmaAramLow:
        return static_cast<uint16_t>(aram_dma_aram_);

    case reg::kAramDmaCountHigh:
        return static_cast<uint16_t>(aram_dma_count_ >> 16);

    case reg::kAramDmaCountLow:
        return static_cast<uint16_t>(aram_dma_count_);

    case reg::kAiDmaStartHigh:
        return static_cast<uint16_t>(ai_start_ >> 16);

    case reg::kAiDmaStartLow:
        return static_cast<uint16_t>(ai_start_);

    case reg::kAiDmaControl:
        return ai_control_;

    case reg::kAiDmaBlocksLeft:
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
    case reg::kMailboxToDspHigh:
        dsp_.CpuWriteMailboxHigh(value);
        break;

    case reg::kMailboxToDspLow:
        dsp_.CpuWriteMailboxLow(value);
        break;

    case reg::kControl:
        SetControl(value);
        break;

    case reg::kAramSize:
        aram_size_ = value & 0x7f;
        break;

    case reg::kAramRefresh:
        aram_refresh_ = value & 0x7ff;
        break;

    case reg::kAramDmaMainHigh:
        aram_dma_main_ = (aram_dma_main_ & 0xffff) | uint32_t{value & 0x03ffu} << 16;
        break;

    case reg::kAramDmaMainLow:
        aram_dma_main_ = (aram_dma_main_ & 0xffff0000) | (value & 0xffe0u);
        break;

    case reg::kAramDmaAramHigh:
        aram_dma_aram_ = (aram_dma_aram_ & 0xffff) | uint32_t{value & 0x03ffu} << 16;
        break;

    case reg::kAramDmaAramLow:
        aram_dma_aram_ = (aram_dma_aram_ & 0xffff0000) | (value & 0xffe0u);
        break;

    case reg::kAramDmaCountHigh:
        aram_dma_count_ = (aram_dma_count_ & 0xffff) | uint32_t{value & 0x83ffu} << 16;
        break;

    case reg::kAramDmaCountLow:
        aram_dma_count_ = (aram_dma_count_ & 0xffff0000) | (value & 0xffe0u);
        AramDma();
        break;

    case reg::kAiDmaStartHigh:
        ai_start_ = (ai_start_ & 0xffff) | uint32_t{static_cast<uint16_t>(value & options_.ai_start_high_mask)} << 16;
        break;

    case reg::kAiDmaStartLow:
        ai_start_ = (ai_start_ & 0xffff0000) | (value & 0xffe0u);
        break;

    case reg::kAiDmaControl:
        {
            // Starting the DMA begins a pass at once. While the DMA runs, a new length waits for the pass to end.
            // Clearing the enable bit stops the DMA at the next block.
            const bool was_enabled = (ai_control_ & kAiEnable) != 0;
            ai_control_ = value;
            if (!was_enabled && (value & kAiEnable))
            {
                ai_current_ = ai_start_;
                ai_remaining_ = value & kAiLength;
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
    if (value & dsp::control::kReset)
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

    // The AI DMA's blocks that are due, one every block's time at the DMA's rate.
    while (now >= ai_next_block_)
    {
        SendAiBlock();
        ai_next_block_ += rate_32k_ ? options_.block_cycles_32k : options_.block_cycles_48k;
    }
}

template <typename Memory>
void DspInterface<Memory>::SendAiBlock()
{
    // 32 bytes from the current address while the DMA is on, or silence while it's off or the bytes lie outside memory.
    static constexpr std::array<uint8_t, 32> kSilence{};
    const bool enabled = (ai_control_ & kAiEnable) != 0;
    const uint8_t* block = enabled ? memory_.At(ai_current_, 32) : nullptr;
    if (on_audio)
    {
        on_audio(block != nullptr ? std::span<const uint8_t, 32>(block, 32) : std::span(kSilence));
    }

    if (!enabled)
    {
        return;
    }

    // The pass moves on by a block. At its end, it starts again at the start address with the length that the control
    // register now gives, and the AI interrupt is raised.
    if (ai_remaining_ != 0)
    {
        ai_remaining_--;
        ai_current_ += 32;
    }
    if (ai_remaining_ == 0)
    {
        ai_current_ = ai_start_;
        ai_remaining_ = ai_control_ & kAiLength;
        control_ |= dspcr::kAiInterrupt;
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
    // probes for an expansion that way, and it then finds none. Such a transfer leaves the addresses and the count as
    // they were written.
    const bool to_main = (aram_dma_count_ & kAramToMain) != 0;
    uint32_t count = aram_dma_count_ & ~kAramToMain;
    control_ |= dspcr::kAramBusy;
    aram_dma_done_ = cpu_.Cycles() + count / 32 * kAramCyclesPerBlock;

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
    aram_dma_count_ &= kAramToMain;
}

template class DspInterface<dsp::GameCubeMemory>;
template class DspInterface<dsp::WiiMemory>;

} // namespace ntdlib::flipper
