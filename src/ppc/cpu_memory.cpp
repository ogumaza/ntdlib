// SPDX-License-Identifier: MIT

// The CPU's exceptions, special registers, address translation and memory accesses (see cpu.h): the BATs and the maps
// of pages, the fast and the slow paths of loads and stores, and the fetch.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/ppc/cpu.h"
#include "ntdlib/types.h"
#include "ppc/cpu_internal.h"

namespace ntdlib::ppc
{

void Cpu::TakeException(Exception exception, uint32_t return_pc, uint32_t srr1_bits)
{
    // The handler runs in real mode, with interrupts, floating point and the rest off. IP selects the base of the
    // vectors.
    State& s = state_;
    s.spr[spr::kSrr0] = return_pc;
    s.spr[spr::kSrr1] = (s.msr & kSrr1Msr) | srr1_bits;
    const uint32_t base = (s.msr & msr::kIp) ? 0xfff00000 : 0;
    uint32_t new_msr = s.msr & (msr::kIp | msr::kMe | msr::kIle);
    if (s.msr & msr::kIle)
    {
        new_msr |= msr::kLe;
    }

    SetMsr(new_msr);
    s.pc = base | static_cast<uint32_t>(exception);
}

void Cpu::CheckInterrupts()
{
    if (!(state_.msr & msr::kEe))
    {
        return;
    }

    if (external_)
    {
        TakeException(Exception::kExternal, state_.pc);
    }
    else if (dec_pending_)
    {
        dec_pending_ = false;
        TakeException(Exception::kDecrementer, state_.pc);
    }
}

uint32_t Cpu::MfSpr(int number)
{
    const State& s = state_;
    switch (number)
    {
    case spr::kXer:
        return s.xer;
    case spr::kLr:
        return s.lr;
    case spr::kCtr:
        return s.ctr;
    case spr::kDec:
        return Decrementer();
    case spr::kTblRead:
        return static_cast<uint32_t>(TimeBase());
    case spr::kTbuRead:
        return static_cast<uint32_t>(TimeBase() >> 32);
    default:
        return s.spr[number & 0x3ff];
    }
}

void Cpu::MtSpr(int number, uint32_t value)
{
    State& s = state_;
    switch (number)
    {
    case spr::kXer:
        s.xer = value;
        return;

    case spr::kLr:
        s.lr = value;
        return;

    case spr::kCtr:
        s.ctr = value;
        return;

    case spr::kDec:
        SetDecrementer(value);
        return;

    case spr::kTblWrite:
        tb_base_ = (TimeBase() & 0xffffffff00000000) | value;
        tb_cycle_ = cycles_;
        return;

    case spr::kTbuWrite:
        tb_base_ = (TimeBase() & 0xffffffff) | (uint64_t{value} << 32);
        tb_cycle_ = cycles_;
        return;

    case spr::kPvr:
        return;

    default:
        break;
    }

    s.spr[number & 0x3ff] = value;
    if ((number >= spr::kIbat0u && number < spr::kIbat0u + 16) ||
        (number >= spr::kIbat4u && number < spr::kIbat4u + 16) || number == spr::kHid4)
    {
        InvalidateTranslation();
    }
}

void Cpu::SetMsr(uint32_t value)
{
    state_.msr = value;
    fetch_page_ = kNoFetchPage;

    // A change may enable an interrupt that's waiting.
    next_check_ = 0;
}

uint32_t Cpu::Decrementer() const
{
    return dec_base_ - static_cast<uint32_t>((cycles_ - dec_cycle_) / kCyclesPerTick);
}

void Cpu::SetDecrementer(uint32_t value)
{
    // The exception comes when the count goes from 0 to -1. For a positive value that's value + 1 ticks from now. The
    // hardware also raises it at once when a write turns a count that isn't negative into a negative one. The model
    // waits for the count to wrap and reach -1 instead. The SDK never writes a negative count.
    dec_base_ = value;
    dec_cycle_ = cycles_;
    const uint64_t ticks = uint64_t{value} + 1;
    dec_event_ = cycles_ + ticks * kCyclesPerTick;
    next_check_ = 0;
}

void Cpu::RebuildMap(bool instruction)
{
    // The BATs that are valid in supervisor mode. The lowest-numbered BAT comes last so that it wins where two overlap.
    std::vector<uint8_t*>& map = instruction ? fetch_map_ : data_map_;
    std::fill(map.begin(), map.end(), nullptr);
    if (instruction)
    {
        fetch_page_ = kNoFetchPage;
    }

    for (int i = BatCount(state_) - 1; i >= 0; i--)
    {
        const Bat bat = BatAt(state_, i, instruction);
        if (!bat.valid)
        {
            continue;
        }

        for (uint32_t offset = 0; offset < bat.length; offset += 0x1000)
        {
            map[(bat.effective + offset) >> 12] = bus_.RamPage(bat.physical + offset);
        }
    }
}

bool Cpu::Translate(uint32_t address, bool instruction, PAddr& physical) const
{
    if (!(state_.msr & (instruction ? msr::kIr : msr::kDr)))
    {
        physical = address;
        return true;
    }

    for (int i = 0; i < BatCount(state_); i++)
    {
        const Bat bat = BatAt(state_, i, instruction);
        const uint32_t mask = ~(bat.length - 1);
        if (bat.valid && (address & mask) == bat.effective)
        {
            physical = bat.physical | (address & ~mask);
            return true;
        }
    }

    return false;
}

uint8_t Cpu::Read8(uint32_t address)
{
    if (const uint8_t* page = DataPage(address))
    {
        return page[address & kPageMask];
    }

    return static_cast<uint8_t>(ReadSlow(address, 1));
}

uint16_t Cpu::Read16(uint32_t address)
{
    const uint8_t* page = DataPage(address);
    if (page && (address & kPageMask) <= 0xffe)
    {
        return xsflib::LoadBe16(page + (address & kPageMask));
    }

    return static_cast<uint16_t>(ReadSlow(address, 2));
}

uint32_t Cpu::Read32(uint32_t address)
{
    const uint8_t* page = DataPage(address);
    if (page && (address & kPageMask) <= 0xffc)
    {
        return xsflib::LoadBe32(page + (address & kPageMask));
    }

    return ReadSlow(address, 4);
}

uint64_t Cpu::Read64(uint32_t address)
{
    const uint8_t* page = DataPage(address);
    if (page && (address & kPageMask) <= 0xff8)
    {
        return xsflib::LoadBe64(page + (address & kPageMask));
    }

    const uint64_t high = Read32(address);

    return high << 32 | Read32(address + 4);
}

void Cpu::Write8(uint32_t address, uint8_t value)
{
    if (uint8_t* page = DataPage(address))
    {
        page[address & kPageMask] = value;
        return;
    }

    WriteSlow(address, 1, value);
}

void Cpu::Write16(uint32_t address, uint16_t value)
{
    uint8_t* page = DataPage(address);
    if (page && (address & kPageMask) <= 0xffe)
    {
        xsflib::StoreBe16(page + (address & kPageMask), value);
        return;
    }

    WriteSlow(address, 2, value);
}

void Cpu::Write32(uint32_t address, uint32_t value)
{
    uint8_t* page = DataPage(address);
    if (page && (address & kPageMask) <= 0xffc)
    {
        xsflib::StoreBe32(page + (address & kPageMask), value);
        return;
    }

    WriteSlow(address, 4, value);
}

void Cpu::Write64(uint32_t address, uint64_t value)
{
    uint8_t* page = DataPage(address);
    if (page && (address & kPageMask) <= 0xff8)
    {
        xsflib::StoreBe64(page + (address & kPageMask), value);
        return;
    }

    Write32(address, static_cast<uint32_t>(value >> 32));
    Write32(address + 4, static_cast<uint32_t>(value));
}

uint32_t Cpu::ReadSlow(uint32_t address, int size)
{
    // An access that crosses a page goes a byte at a time.
    if ((address & kPageMask) + static_cast<uint32_t>(size) > 0x1000)
    {
        uint32_t value = 0;
        for (int i = 0; i < size; i++)
        {
            value = value << 8 | ReadSlow(address + static_cast<uint32_t>(i), 1);
        }

        return value;
    }

    PAddr physical = 0;
    if (!Translate(address, false, physical))
    {
        Fault(address, false);
        return 0;
    }

    if (const uint8_t* page = bus_.RamPage(physical & ~kPageMask))
    {
        const uint8_t* p = page + (physical & kPageMask);
        return size == 1 ? *p : size == 2 ? xsflib::LoadBe16(p) : xsflib::LoadBe32(p);
    }

    uint32_t value = 0;
    if (!bus_.ReadIo(physical, size, value))
    {
        Fault(address, false);
        return 0;
    }

    return value;
}

void Cpu::WriteSlow(uint32_t address, int size, uint32_t value)
{
    if ((address & kPageMask) + static_cast<uint32_t>(size) > 0x1000)
    {
        for (int i = 0; i < size; i++)
        {
            WriteSlow(address + static_cast<uint32_t>(i), 1, value >> ((size - 1 - i) * 8) & 0xff);
        }

        return;
    }

    PAddr physical = 0;
    if (!Translate(address, false, physical))
    {
        Fault(address, true);
        return;
    }

    if (uint8_t* page = bus_.RamPage(physical & ~kPageMask))
    {
        uint8_t* p = page + (physical & kPageMask);
        if (size == 1)
        {
            *p = static_cast<uint8_t>(value);
        }
        else if (size == 2)
        {
            xsflib::StoreBe16(p, static_cast<uint16_t>(value));
        }
        else
        {
            xsflib::StoreBe32(p, value);
        }

        return;
    }

    if (!bus_.WriteIo(physical, size, value))
    {
        Fault(address, true);
    }
}

bool Cpu::Fetch(uint32_t address, uint32_t& op)
{
    // The page of the last fetch through the map, then the map.
    if (address >> 12 == fetch_page_)
    {
        op = xsflib::LoadBe32(fetch_host_ + (address & kPageMask));
        return true;
    }

    if (state_.msr & msr::kIr)
    {
        if (const uint8_t* page = fetch_map_[address >> 12])
        {
            fetch_page_ = address >> 12;
            fetch_host_ = page;
            op = xsflib::LoadBe32(page + (address & kPageMask));
            return true;
        }
    }

    PAddr physical = 0;
    const uint8_t* page = Translate(address, true, physical) ? bus_.RamPage(physical & ~kPageMask) : nullptr;
    if (page == nullptr)
    {
        stop_pc_ = address;
        fault_address_ = address;
        fault_write_ = false;
        stop_reason_ = StopReason::kFault;
        stopped_ = true;
        return false;
    }

    op = xsflib::LoadBe32(page + (physical & kPageMask));

    return true;
}

void Cpu::Fault(uint32_t address, bool write)
{
    if (!stopped_)
    {
        fault_address_ = address;
        fault_write_ = write;
        stop_reason_ = StopReason::kFault;
        stopped_ = true;
        state_.pc = stop_pc_;
    }
}

void Cpu::SetCr0(uint32_t result)
{
    SetCrField(0, CompareField(static_cast<int32_t>(result), int32_t{0}, state_.xer));
}

void Cpu::SetCrField(int field, uint32_t value)
{
    const int shift = 28 - field * 4;
    state_.cr = (state_.cr & ~(0xfu << shift)) | ((value & 0xf) << shift);
}

} // namespace ntdlib::ppc
