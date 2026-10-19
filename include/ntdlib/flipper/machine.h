// SPDX-License-Identifier: MIT

// The parts of a GameCube or Wii machine for game mode that don't depend on its other devices: the run loop, main
// memory's pages for the CPU, the blocks of the hardware page that both consoles have and their registers, the CPU's
// interrupt line, the snapshot of the devices that both consoles have, the part of the boot that both share, and the
// report of registers that the machine doesn't model.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/disc/header.h"
#include "ntdlib/dsp/dsp.h"
#include "ntdlib/dsp/memory.h"
#include "ntdlib/flipper/audio_interface.h"
#include "ntdlib/flipper/dsp_interface.h"
#include "ntdlib/flipper/exi.h"
#include "ntdlib/flipper/processor_interface.h"
#include "ntdlib/flipper/registers.h"
#include "ntdlib/ppc/cpu.h"
#include "ntdlib/types.h"
#include "xsflib/bytes.h"

namespace ntdlib::flipper
{

// The registers of the blocks that ReadShared and WriteShared reach, as a value. A machine's registers derive from
// SharedRegisters and give it the console's rates for the audio interface. A snapshot copies them with the machine's
// other registers.
class SharedRegisters
{
protected:
    explicit SharedRegisters(const AudioInterfaceOptions& audio) : ai_(audio)
    {
    }

    ProcessorInterface pi_;
    AudioInterface ai_;
    RegisterFile<0x100> cp_{};
    RegisterFile<0x100> pe_{};
    RegisterFile<0x100> vi_{};
    RegisterFile<0x100> mi_{};
    RegisterFile<0x400> si_{};
};

// A snapshot of the devices that both consoles' machines have: main memory (sharing its unchanged pages with an earlier
// snapshot), the CPU, the DSP, the DSP interface and EXI.
template <typename Memory>
struct DeviceSnapshot
{
    typename Memory::Snapshot memory;
    ppc::Cpu::Snapshot cpu;
    dsp::Dsp<Memory> dsp;
    typename DspInterface<Memory>::Snapshot dsp_interface;
    typename Exi<Memory>::Snapshot exi;
};

// The OS globals in low memory that the IPL and the apploader leave on both consoles (the SDK's OSBootInfo). The
// addresses are the cached ones.
namespace low
{

constexpr uint32_t kBootMagic = 0x80000020;
constexpr uint32_t kBootVersion = 0x80000024;
constexpr uint32_t kMemorySize = 0x80000028;
constexpr uint32_t kConsoleType = 0x8000002c;
constexpr uint32_t kArenaLo = 0x80000030;
constexpr uint32_t kArenaHi = 0x80000034;
constexpr uint32_t kFst = 0x80000038;
constexpr uint32_t kFstMaxSize = 0x8000003c;
constexpr uint32_t kSimulatedMemorySize = 0x800000f0;
constexpr uint32_t kBusClock = 0x800000f8;
constexpr uint32_t kCpuClock = 0x800000fc;

} // namespace low

// The console's values for the boot's low memory (see MachineBase::LoadBootInfo).
struct BootInfo
{
    uint32_t offset_unit = 1;  // the header gives the file system table's offset and size in units of this many bytes
    uint32_t arena_top = 0;    // the top of the OS's arena. The file system table lies at least 64 KiB above it
    uint32_t console_type = 0; // what the SDK's OSGetConsoleType returns
    uint32_t bus_clock = 0;
    uint32_t cpu_clock = 0;
};

// A machine `Machine` derives from MachineBase<Machine> and declares it a friend. MachineBase uses the machine's
// members cpu_ (a ppc::Cpu), memory_ (main memory), pi_ (a ProcessorInterface), ai_ (an AudioInterface) and
// dsp_interface_. The machine provides NextEvent() and Advance() for its devices' events, and InterruptCauses() for the
// processor interface. WriteAi passes AIDFR's DMA rate to dsp_interface_.SetDmaRate32k. A machine that calls ReadShared
// and WriteShared also has the blocks they reach: cp_, pe_, vi_ and mi_ (each a RegisterFile<0x100>), si_ (a
// RegisterFile<0x400>), exi_, and the DSP interface's 16-bit Read and Write. SharedRegisters holds pi_, ai_ and the
// registers of those blocks. A machine that calls SaveDevices and RestoreDevices also has dsp_ and exi_. A machine that
// calls LoadBootInfo has disc_, whose Read(offset, out, size) reads the disc.
template <typename Machine>
class MachineBase : public ppc::Bus
{
public:
    // Called with a line for the first access to each register that the machine doesn't model.
    std::function<void(const std::string&)> on_unknown_io;

    // Runs the machine for up to `cycles` CPU cycles. The CPU runs up to the next device event. Then the event happens
    // and the interrupt line is updated.
    ppc::StopReason Run(uint64_t cycles)
    {
        ppc::Cpu& cpu = Self().cpu_;
        const uint64_t end = cpu.Cycles() + cycles;
        while (cpu.Cycles() < end)
        {
            const uint64_t until = std::min(end, std::max(Self().NextEvent(), cpu.Cycles() + 1));
            const ppc::StopReason reason = cpu.Run(until - cpu.Cycles());
            Self().Advance();
            if (reason != ppc::StopReason::kBudget)
            {
                return reason;
            }
        }

        return ppc::StopReason::kBudget;
    }

    uint8_t* RamPage(PAddr page) override
    {
        return Self().memory_.At(page, kRamPageSize);
    }

protected:
    // The offsets of the blocks in the hardware page (physical 0x0C000000) that the GameCube and the Wii share. The
    // command processor's block starts at 0. The audio interface's block is the last. It ends at kAudioInterfaceEnd.
    static constexpr uint32_t kPixelEngine = 0x1000;
    static constexpr uint32_t kVideoInterface = 0x2000;
    static constexpr uint32_t kProcessorInterface = 0x3000;
    static constexpr uint32_t kMemoryInterface = 0x4000;
    static constexpr uint32_t kDspInterface = 0x5000;
    static constexpr uint32_t kDiscInterface = 0x6000;
    static constexpr uint32_t kSerialInterface = 0x6400;
    static constexpr uint32_t kExternalInterface = 0x6800;
    static constexpr uint32_t kAudioInterface = 0x6c00;
    static constexpr uint32_t kAudioInterfaceEnd = 0x7000;

    // Raises or lowers the CPU's external interrupt for the pending causes.
    void UpdateInterrupt()
    {
        Self().cpu_.SetExternalInterrupt(Self().pi_.Asserts(Self().InterruptCauses()));
    }

    // The register at `offset` in the hardware page (physical 0x0C000000), in the blocks that the GameCube and the Wii
    // share: the command processor (CP), the pixel engine (PE), the video interface (VI), the processor interface (PI),
    // the memory interface (MI), the DSP interface, and in the 0x6000 block the serial interface (SI), the external
    // interface (EXI) and the audio interface (AI). The machine handles the disc interface (DI, 0x6000-0x63FF) and
    // every other block. Returns nothing for an offset outside the shared blocks, or whether the machine models the
    // register: one that only stores what's written to it doesn't count. On the Wii, the page at 0x0D000000 mirrors
    // only the 0x6000 block. Hollywood's registers take the rest of that page. A Wii machine passes ReadShared and
    // WriteShared only offsets 0x6000-0x6FFF from it. Offset 0 there would reach CP.
    std::optional<bool> ReadShared(uint32_t offset, int size, uint32_t& value)
    {
        Machine& m = Self();
        if (offset < kPixelEngine)
        {
            value = m.cp_.Read(offset & 0xff, size);
            return false;
        }
        if (offset < kVideoInterface)
        {
            value = m.pe_.Read(offset & 0xff, size);
            return false;
        }
        if (offset < kProcessorInterface)
        {
            value = m.vi_.Read(offset & 0xff, size);
            return false;
        }
        if (offset < kMemoryInterface)
        {
            value = ReadPi(offset & 0xff);
            return true;
        }
        if (offset < kDspInterface)
        {
            value = m.mi_.Read(offset & 0xff, size);
            return false;
        }
        if (offset < kDiscInterface)
        {
            // 16-bit registers. A 32-bit read takes the high half first.
            const uint32_t reg = offset & 0x1fe;
            value = size == 4 ? uint32_t{m.dsp_interface_.Read(reg)} << 16 | m.dsp_interface_.Read(reg + 2)
                              : m.dsp_interface_.Read(reg);
            return true;
        }
        if (offset < kSerialInterface)
        {
            return std::nullopt;
        }
        if (offset < kExternalInterface)
        {
            value = m.si_.Read(offset & 0x3ff, size);
            return false;
        }
        if (offset < kAudioInterface)
        {
            value = m.exi_.Read(offset & 0x3f);
            return true;
        }
        if (offset < kAudioInterfaceEnd)
        {
            value = ReadAi(offset & 0x1f);
            return true;
        }

        return std::nullopt;
    }

    std::optional<bool> WriteShared(uint32_t offset, int size, uint32_t value)
    {
        Machine& m = Self();
        if (offset < kPixelEngine)
        {
            m.cp_.Write(offset & 0xff, size, value);
            return false;
        }
        if (offset < kVideoInterface)
        {
            m.pe_.Write(offset & 0xff, size, value);
            return false;
        }
        if (offset < kProcessorInterface)
        {
            m.vi_.Write(offset & 0xff, size, value);
            return false;
        }
        if (offset < kMemoryInterface)
        {
            WritePi(offset & 0xff, value);
            return true;
        }
        if (offset < kDspInterface)
        {
            m.mi_.Write(offset & 0xff, size, value);
            return false;
        }
        if (offset < kDiscInterface)
        {
            const uint32_t reg = offset & 0x1fe;
            if (size == 4)
            {
                m.dsp_interface_.Write(reg, static_cast<uint16_t>(value >> 16));
                m.dsp_interface_.Write(reg + 2, static_cast<uint16_t>(value));
            }
            else
            {
                m.dsp_interface_.Write(reg, static_cast<uint16_t>(value));
            }
            return true;
        }
        if (offset < kSerialInterface)
        {
            return std::nullopt;
        }
        if (offset < kExternalInterface)
        {
            // Nothing is connected to SI. A write that sets SICOMCSR's TSTART (0x34) finishes the transfer at once.
            // Then TSTART reads as clear and TCINT (the completion flag) as set.
            const uint32_t reg = offset & 0x3ff;
            m.si_.Write(reg, size, reg == 0x34 && (value & 1) ? (value & ~1u) | 0x80000000 : value);
            return false;
        }
        if (offset < kAudioInterface)
        {
            m.exi_.Write(offset & 0x3f, value);
            return true;
        }
        if (offset < kAudioInterfaceEnd)
        {
            WriteAi(offset & 0x1f, value);
            return true;
        }

        return std::nullopt;
    }

    // The devices of DeviceSnapshot, saved and restored. A saved snapshot shares main memory's unchanged pages with
    // `previous`. The machine restores its other devices and its registers, then updates the interrupt line.
    template <typename Memory>
    DeviceSnapshot<Memory> SaveDevices(const DeviceSnapshot<Memory>* previous) const
    {
        const Machine& m = Self();
        return {m.memory_.Save(previous ? &previous->memory : nullptr), m.cpu_.Save(), m.dsp_, m.dsp_interface_.Save(),
                m.exi_.Save()};
    }

    template <typename Memory>
    void RestoreDevices(const DeviceSnapshot<Memory>& snapshot)
    {
        Machine& m = Self();
        m.memory_.Restore(snapshot.memory);
        m.cpu_.Restore(snapshot.cpu);
        m.dsp_ = snapshot.dsp;
        m.dsp_interface_.Restore(snapshot.dsp_interface);
        m.exi_.Restore(snapshot.exi);
    }

    // The part of the boot that both consoles share: the disc header's first 32 bytes at the start of memory, the
    // disc's file system table at the top of MEM1, and the OS globals in `low`. Returns the disc's header. Throws
    // std::runtime_error if the disc can't give its header or table, or if the table doesn't fit above the arena.
    std::array<uint8_t, disc::kHeaderSize> LoadBootInfo(const BootInfo& info)
    {
        Machine& m = Self();
        const auto put32 = [&m](uint32_t address, uint32_t value)
        {
            xsflib::StoreBe32(m.memory_.At(dsp::ToPhysical(address), 4), value);
        };

        std::array<uint8_t, disc::kHeaderSize> header{};
        if (!m.disc_.Read(0, header.data(), header.size()))
        {
            throw std::runtime_error("the disc's header can't be read");
        }

        std::copy(header.begin(), header.begin() + 0x20, m.memory_.At(0, 0x20));

        // The file system table, at the top of MEM1. The DVD library looks files up there.
        const uint32_t fst_offset = xsflib::LoadBe32(&header[disc::kHeaderFstOffset]) * info.offset_unit;
        const uint32_t fst_size = xsflib::LoadBe32(&header[disc::kHeaderFstSize]) * info.offset_unit;
        const uint32_t fst_address = (dsp::ToCached(dsp::kMem1Size) - fst_size) & ~0x1fu;
        std::vector<uint8_t> fst(fst_size);
        if (fst_size > 0x100000 || !m.disc_.Read(fst_offset, fst.data(), fst.size()))
        {
            throw std::runtime_error("the disc's file system table can't be read");
        }

        if (fst_address < info.arena_top + 0x10000)
        {
            throw std::runtime_error("the disc's file system table is too large");
        }

        std::copy(fst.begin(), fst.end(), m.memory_.At(dsp::ToPhysical(fst_address), fst.size()));

        put32(low::kBootMagic, 0x0d15ea5e);
        put32(low::kBootVersion, 1);
        put32(low::kMemorySize, dsp::kMem1Size);
        put32(low::kConsoleType, info.console_type);
        put32(low::kArenaLo, 0);
        put32(low::kArenaHi, info.arena_top);
        put32(low::kFst, fst_address);
        put32(low::kFstMaxSize, fst_size);
        put32(low::kSimulatedMemorySize, dsp::kMem1Size);
        put32(low::kBusClock, info.bus_clock);
        put32(low::kCpuClock, info.cpu_clock);

        return header;
    }

    // Starts the CPU at `entry`: translation on through the BATs that map MEM1 cached at 0x80000000 and uncached at
    // 0xC0000000, and floating point on. The uncached mapping also reaches the hardware registers. A Wii machine sets
    // MEM2's BATs with SetBat first.
    void StartCpu(uint32_t entry)
    {
        ppc::State& s = Self().cpu_.state_;
        s.msr = ppc::msr::kIr | ppc::msr::kDr | ppc::msr::kRi | ppc::msr::kFp;
        SetBat(s, ppc::spr::kIbat0u, 0x80001fff, 0x00000002);
        SetBat(s, ppc::spr::kIbat0u + 8, 0x80001fff, 0x00000002);
        SetBat(s, ppc::spr::kIbat0u + 10, 0xc0001fff, 0x0000002a);
        Self().cpu_.InvalidateTranslation();
        s.pc = entry;
    }

    // Sets the pair of BAT registers at SPR `base` to `upper` and `lower`.
    static void SetBat(ppc::State& s, int base, uint32_t upper, uint32_t lower)
    {
        s.spr[base] = upper;
        s.spr[base + 1] = lower;
    }

    // Ends a write that the machine handled: reports the register if the machine doesn't model it, then lets an event
    // that the write scheduled, or an interrupt that it raised or cleared, take effect.
    void FinishWrite(PAddr address, int size, uint32_t value, bool known)
    {
        if (!known)
        {
            ReportUnknown(address, size, true, value);
        }

        Self().cpu_.LimitRun(Self().NextEvent());
        UpdateInterrupt();
    }

    // Reports the first access to `address`, a register that the machine doesn't model, through on_unknown_io.
    void ReportUnknown(PAddr address, int size, bool write, uint32_t value)
    {
        if (!on_unknown_io || !reported_.insert(address).second)
        {
            return;
        }

        char line[80];
        const uint32_t pc = Self().cpu_.stop_pc_;
        if (write)
        {
            std::snprintf(line, sizeof(line), "write%d %08x = %08x at pc %08x", size * 8, address, value, pc);
        }
        else
        {
            std::snprintf(line, sizeof(line), "read%d %08x at pc %08x", size * 8, address, pc);
        }
        on_unknown_io(line);
    }

private:
    // The size of the pages through which the CPU reaches main memory, and the hardware page's physical address.
    static constexpr uint32_t kRamPageSize = 0x1000;
    static constexpr PAddr kHardwarePage = 0x0c000000;

    // The processor interface's 32-bit register at `offset`.
    uint32_t ReadPi(uint32_t offset)
    {
        return Self().pi_.Read(offset, Self().InterruptCauses());
    }

    void WritePi(uint32_t offset, uint32_t value)
    {
        if (Self().pi_.Write(offset, value))
        {
            ReportUnknown(kHardwarePage + kProcessorInterface + offset, 4, true, value);
        }
    }

    // The audio interface's 32-bit register at `offset`.
    uint32_t ReadAi(uint32_t offset)
    {
        return Self().ai_.Read(offset, Self().cpu_.Cycles());
    }

    void WriteAi(uint32_t offset, uint32_t value)
    {
        if (const auto dma_rate_32k = Self().ai_.Write(offset, value, Self().cpu_.Cycles()))
        {
            Self().dsp_interface_.SetDmaRate32k(*dma_rate_32k);
        }
    }

    Machine& Self()
    {
        return static_cast<Machine&>(*this);
    }

    const Machine& Self() const
    {
        return static_cast<const Machine&>(*this);
    }

    std::set<PAddr> reported_;
};

} // namespace ntdlib::flipper
