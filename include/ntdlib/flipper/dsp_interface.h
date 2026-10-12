// SPDX-License-Identifier: MIT

// The CPU side of the DSP, at physical 0x0C005000: the mailboxes, the DSP control register (DSPCR) and its three
// interrupts, the ARAM DMA (which reaches MEM2 on the Wii), and the AI DMA. The AI DMA feeds the audio interface from
// main memory and provides game mode's output. The DSP runs in batches, one slice behind the CPU. Dolphin runs it the
// same way.

#pragma once

#include <cstdint>
#include <functional>
#include <span>

#include "ntdlib/dsp/dsp.h"
#include "ntdlib/ppc/cpu.h"

namespace ntdlib::flipper
{

// The interface's registers and the DSP's timing. A snapshot of the system copies them (see DspInterface::Save).
class DspInterfaceState
{
protected:
    // DSPCR's CPU-side bits (3-9 and 12-15).
    uint16_t control_ = 0;

    uint16_t aram_size_ = 0;
    uint16_t aram_refresh_ = 156;
    uint32_t aram_dma_main_ = 0;
    uint32_t aram_dma_aram_ = 0;
    uint32_t aram_dma_count_ = 0;
    uint64_t aram_dma_done_ = UINT64_MAX;

    // The AI DMA: the registers, and the pass in progress.
    uint32_t ai_start_ = 0;
    uint16_t ai_control_ = 0;
    uint32_t ai_current_ = 0;
    uint32_t ai_remaining_ = 0;
    uint64_t ai_first_interrupt_ = UINT64_MAX;
    uint64_t ai_next_block_ = 0;
    bool rate_32k_ = true;

    // The DSP's time budget in CPU cycles, and the next slice.
    int64_t budget_ = 0;
    uint64_t next_slice_ = 0;
};

// What differs between the GameCube's and the Wii's interfaces.
struct DspInterfaceOptions
{
    // The AI DMA's 32-byte blocks of 8 samples: one every `block_cycles_32k` CPU cycles at 32 kHz, or every
    // `block_cycles_48k` at 48 kHz.
    uint64_t block_cycles_32k;
    uint64_t block_cycles_48k;

    // The bits of the AI DMA's start address (its high half) that the register keeps.
    uint16_t ai_start_high_mask;

    // Whether a write by the ARAM DMA below 4 MiB also lands 4 MiB higher while the ARAM size register's low bits
    // are 4.
    bool aram_mirror;
};

// The options for each console. The GameCube's audio interface runs at 108 MHz / 3,372 (32,028.5 Hz) or
// 108 MHz / 2,248.
inline constexpr DspInterfaceOptions kGameCubeDspInterface{121'392, 80'928, 0x03ff, false};
inline constexpr DspInterfaceOptions kWiiDspInterface{182'250, 121'500, 0x1fff, true};

// The interface with main memory and ARAM of type `Memory` (dsp::GameCubeMemory or dsp::WiiMemory).
template <typename Memory>
class DspInterface : private DspInterfaceState
{
public:
    // The interface's state as Save captures it.
    using Snapshot = DspInterfaceState;

    DspInterface(dsp::Dsp<Memory>& dsp, Memory& memory, ppc::Cpu& cpu, const DspInterfaceOptions& options);

    // A 16-bit register at `offset` from 0x0C005000.
    uint16_t Read(uint32_t offset);
    void Write(uint32_t offset, uint16_t value);

    // The audio interface's DMA rate: 32 kHz (AICR's AIDFR set) or 48 kHz.
    void SetDmaRate32k(bool rate_32k)
    {
        rate_32k_ = rate_32k;
    }

    // Whether any of DSPCR's interrupts is pending and enabled: the DSP line to the processor interface.
    bool InterruptPending() const;

    // The CPU cycle of the next timed event, and the events due by `now`.
    uint64_t NextEvent() const;
    void Advance(uint64_t now);

    // The interface's state between runs of the CPU, and its restoration. The DSP is saved separately.
    Snapshot Save() const
    {
        return *this;
    }

    void Restore(const Snapshot& snapshot)
    {
        DspInterfaceState::operator=(snapshot);
    }

    // Called with each 32-byte block of the AI DMA (8 stereo samples, big-endian, right first), or zeros while the DMA
    // is off.
    std::function<void(std::span<const uint8_t, 32>)> on_audio;

private:
    // Runs the DSP for `cycles` cycles, as one of Dolphin's batches.
    void RunBatch(int64_t cycles);

    // The mailbox polls' share of the DSP's time.
    void RunForPoll();

    void AramDma();
    void SetControl(uint16_t value);

    dsp::Dsp<Memory>& dsp_;
    Memory& memory_;
    ppc::Cpu& cpu_;
    DspInterfaceOptions options_;
};

} // namespace ntdlib::flipper
