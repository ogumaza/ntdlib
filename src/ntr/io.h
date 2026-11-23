// SPDX-License-Identifier: MIT

// What the ARM7's side of a DS (Arm7System) and the ARM9's side (Machine) share: the memory and the registers that both
// CPUs have at the same addresses and with the same bits, the reads and writes of those registers, and the data
// accesses that a system makes on behalf of its CPU.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "ntdlib/arm/cpu.h"
#include "ntdlib/ntr/main_memory.h"

namespace ntdlib::ntr
{

constexpr uint32_t kIo = 0x04000000;
constexpr uint32_t kIpcFifoReceive = 0x04100000;
constexpr uint32_t kCardData = 0x04100010;
constexpr std::size_t kFifoWords = 16;

// The registers' offsets from kIo that both CPUs have: the DMA channels' source, destination and control, 12 bytes for
// each channel, and the interrupt registers IME, IE and IF.
constexpr uint32_t kDmaStart = 0x0B0;
constexpr uint32_t kDmaEnd = 0x0E0;
constexpr uint32_t kIme = 0x208;
constexpr uint32_t kIe = 0x210;
constexpr uint32_t kIf = 0x214;

// KEYINPUT with every key released.
constexpr uint16_t kKeysReleased = 0x03FF;

// DISPSTAT's bits.
constexpr uint16_t kVblankFlag = 1u << 0;
constexpr uint16_t kHblankFlag = 1u << 1;
constexpr uint16_t kVcountFlag = 1u << 2;
constexpr uint16_t kVblankIrq = 1u << 3;
constexpr uint16_t kHblankIrq = 1u << 4;
constexpr uint16_t kVcountIrq = 1u << 5;
constexpr uint16_t kDispstatWritable = 0xFFB8;

// The horizontal blank starts at dot 256.
constexpr uint64_t kHblankCycle = 256 * 6;

// The line of DISPSTAT's V-count match: bits 8 to 15, with bit 8 of the line in bit 7.
inline uint64_t VcountSetting(uint16_t dispstat)
{
    return static_cast<uint64_t>(dispstat >> 8) | static_cast<uint64_t>(dispstat & 0x80) << 1;
}

// IPCFIFOCNT's bits.
constexpr uint16_t kSendEmpty = 1u << 0;
constexpr uint16_t kSendFull = 1u << 1;
constexpr uint16_t kSendEmptyIrq = 1u << 2;
constexpr uint16_t kSendClear = 1u << 3;
constexpr uint16_t kReceiveEmpty = 1u << 8;
constexpr uint16_t kReceiveFull = 1u << 9;
constexpr uint16_t kReceiveIrq = 1u << 10;
constexpr uint16_t kFifoError = 1u << 14;
constexpr uint16_t kFifoEnable = 1u << 15;
constexpr uint16_t kFifoWritable = kSendEmptyIrq | kReceiveIrq | kFifoEnable;

// IPCFIFOCNT as a CPU reads it: the bits of `control` that the CPU wrote, the states of its send queue of `sending`
// words and its receive queue of `receiving` words, and the error flag.
inline uint16_t FifoStatus(uint16_t control, std::size_t sending, std::size_t receiving, bool error)
{
    uint16_t value = control;
    value |= sending == 0 ? kSendEmpty : 0;
    value |= sending == kFifoWords ? kSendFull : 0;
    value |= receiving == 0 ? kReceiveEmpty : 0;
    value |= receiving == kFifoWords ? kReceiveFull : 0;
    value |= error ? kFifoError : 0;

    return value;
}

// IPCSYNC's bits.
constexpr uint16_t kSyncOutput = 0x0F00;
constexpr uint16_t kSyncRequest = 1u << 13;
constexpr uint16_t kSyncIrqEnable = 1u << 14;

// The DMA control bits that both CPUs have.
constexpr uint32_t kDmaEnable = 1u << 31;
constexpr uint32_t kDmaIrq = 1u << 30;
constexpr uint32_t kDmaWords = 1u << 26;

// `old` with the bits of `value` in `mask` written over it.
inline uint16_t Merge16(uint16_t old, uint16_t value, uint16_t mask)
{
    return static_cast<uint16_t>((old & ~mask) | (value & mask));
}

// `word` with the bits of `value` in `mask` written over its halfword at bit `shift`, 0 or 16.
inline uint32_t MergeHalf(uint32_t word, int shift, uint16_t value, uint16_t mask)
{
    return (word & ~(0xFFFFu << shift)) |
           static_cast<uint32_t>(Merge16(static_cast<uint16_t>(word >> shift), value, mask)) << shift;
}

// A read of 1, 2 or 4 bytes at `offset` from kIo, from the reads `read16(offset)` of the halfwords that it covers.
template <typename Read16>
uint32_t ReadIoBySize(uint32_t offset, int size, const Read16& read16)
{
    if (size == 4)
    {
        return read16(offset) | static_cast<uint32_t>(read16(offset + 2)) << 16;
    }

    const uint16_t half = read16(offset & ~1u);
    return size == 2 ? half : (half >> (8 * (offset & 1))) & 0xFF;
}

// A write of 1, 2 or 4 bytes at `offset` from kIo, as the writes `write16(offset, value, mask)` of the halfwords that
// it covers. The mask of a byte's write selects the byte's half of the halfword.
template <typename Write16>
void WriteIoBySize(uint32_t offset, int size, uint32_t value, const Write16& write16)
{
    if (size == 4)
    {
        write16(offset, static_cast<uint16_t>(value), 0xFFFF);
        write16(offset + 2, static_cast<uint16_t>(value >> 16), 0xFFFF);
    }
    else if (size == 2)
    {
        write16(offset, static_cast<uint16_t>(value), 0xFFFF);
    }
    else
    {
        const int shift = 8 * static_cast<int>(offset & 1);
        write16(offset & ~1u, static_cast<uint16_t>((value & 0xFF) << shift), static_cast<uint16_t>(0xFF << shift));
    }
}

// The channel of the DMA register at `offset`, from kDmaStart to kDmaEnd.
inline int DmaChannelOf(uint32_t offset)
{
    return static_cast<int>((offset - kDmaStart) / 12);
}

// A read of the halfword at `offset` in the DMA channels' registers. Only a control's high half reads back.
template <typename Dma>
uint16_t ReadDmaRegister(const std::array<Dma, 4>& dmas, uint32_t offset)
{
    const Dma& dma = dmas[static_cast<std::size_t>(DmaChannelOf(offset))];
    return (offset - kDmaStart) % 12 == 10 ? static_cast<uint16_t>(dma.control >> 16) : 0;
}

// A write of the bits of `value` in `mask` to the halfword at `offset` in the DMA channels' registers: a source's, a
// destination's or a control's low half. A write to a control's high half can start the transfer. The function then
// changes nothing and returns the new control, for the system to set.
template <typename Dma>
std::optional<uint32_t> WriteDmaRegister(std::array<Dma, 4>& dmas, uint32_t offset, uint16_t value, uint16_t mask)
{
    Dma& dma = dmas[static_cast<std::size_t>(DmaChannelOf(offset))];
    const uint32_t field = (offset - kDmaStart) % 12;
    if (field < 4)
    {
        dma.source = MergeHalf(dma.source, field == 2 ? 16 : 0, value, mask);
    }
    else if (field < 8)
    {
        dma.destination = MergeHalf(dma.destination, field == 6 ? 16 : 0, value, mask);
    }
    else if (field == 8)
    {
        dma.control = MergeHalf(dma.control, 0, value, mask);
    }
    else
    {
        return MergeHalf(dma.control, 16, value, mask);
    }

    return std::nullopt;
}

// The bytes of a DMA unit under `control`: a word or a halfword.
inline int DmaUnitSize(uint32_t control)
{
    return (control & kDmaWords) != 0 ? 4 : 2;
}

// The step of a DMA address under `control`, for the mode at bit `shift`: 21 for the destination's, 23 for the
// source's. The modes 0 and 3 go up, 1 goes down and 2 stays.
inline int DmaStep(uint32_t control, int shift)
{
    const int size = DmaUnitSize(control);
    const uint32_t mode = (control >> shift) & 3;
    return mode == 1 ? -size : mode == 2 ? 0 : size;
}

// A read of the halfword of IME (`ime`), IE (`enable`) or IF (`flags`) at `offset`: kIme, kIme + 2, kIe, kIe + 2, kIf
// or kIf + 2. IME's high half reads as 0.
inline uint16_t ReadInterruptRegister(uint32_t offset, uint16_t ime, uint32_t enable, uint32_t flags)
{
    switch (offset)
    {
    case kIme:
        return ime;
    case kIe:
        return static_cast<uint16_t>(enable);
    case kIe + 2:
        return static_cast<uint16_t>(enable >> 16);
    case kIf:
        return static_cast<uint16_t>(flags);
    case kIf + 2:
        return static_cast<uint16_t>(flags >> 16);
    default:
        return 0;
    }
}

// A write of the bits of `value` in `mask` to the halfword of IME (`ime`), IE (`enable`) or IF (`flags`) at `offset`:
// kIme, kIe, kIe + 2, kIf or kIf + 2. IME keeps only bit 0. A write to IF clears the requests of its set bits. Returns
// the requests that the write cleared.
inline uint32_t WriteInterruptRegister(uint32_t offset, uint16_t value, uint16_t mask, uint16_t& ime, uint32_t& enable,
                                       uint32_t& flags)
{
    switch (offset)
    {
    case kIme:
        ime = static_cast<uint16_t>(Merge16(ime, value, mask) & 1);
        return 0;
    case kIe:
    case kIe + 2:
        enable = MergeHalf(enable, offset == kIe ? 0 : 16, value, mask);
        return 0;
    case kIf:
    case kIf + 2:
        {
            const uint32_t acks = static_cast<uint32_t>(value & mask) << (offset == kIf ? 0 : 16);
            const uint32_t cleared = acks & flags;
            flags &= ~acks;
            return cleared;
        }
    default:
        return 0;
    }
}

// A read of 1, 2 or 4 bytes from `bus` at `address`, aligned to the size: in place where the bus has host memory for
// the page, and through Read otherwise. A system reads this way for its BIOS and its DMA.
inline uint32_t LoadFromBus(arm::Bus& bus, uint32_t address, int size)
{
    address &= ~static_cast<uint32_t>(size - 1);
    const uint8_t* page = bus.Page(address & ~0xFFFu);
    if (page == nullptr)
    {
        return bus.Read(address, size);
    }

    uint32_t value = 0;
    for (int i = 0; i < size; i++)
    {
        value |= static_cast<uint32_t>(page[(address & 0xFFF) + static_cast<uint32_t>(i)]) << (8 * i);
    }

    return value;
}

// A write of 1, 2 or 4 bytes to `bus`: the counterpart of LoadFromBus.
inline void StoreToBus(arm::Bus& bus, uint32_t address, int size, uint32_t value)
{
    address &= ~static_cast<uint32_t>(size - 1);
    uint8_t* page = bus.Page(address & ~0xFFFu);
    if (page == nullptr)
    {
        bus.Write(address, size, value);
        return;
    }

    for (int i = 0; i < size; i++)
    {
        page[(address & 0xFFF) + static_cast<uint32_t>(i)] = static_cast<uint8_t>(value >> (8 * i));
    }
}

// Copies `count` units of DMA under `control` on `bus`, from `source` to `destination`, each aligned to the unit. Moves
// both by their steps after each unit.
inline void CopyDmaUnits(arm::Bus& bus, uint32_t control, uint32_t count, uint32_t& source, uint32_t& destination)
{
    const int size = DmaUnitSize(control);
    const int destination_step = DmaStep(control, 21);
    const int source_step = DmaStep(control, 23);
    source &= ~static_cast<uint32_t>(size - 1);
    destination &= ~static_cast<uint32_t>(size - 1);
    for (uint32_t i = 0; i < count; i++)
    {
        StoreToBus(bus, destination, size, LoadFromBus(bus, source, size));
        source += static_cast<uint32_t>(source_step);
        destination += static_cast<uint32_t>(destination_step);
    }
}

} // namespace ntdlib::ntr
