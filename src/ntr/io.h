// SPDX-License-Identifier: MIT

// What the ARM7's side of a DS (Arm7System) and the ARM9's side (Machine) share: the memory and the registers that both
// CPUs have at the same addresses and with the same bits, and the data accesses that a system makes on behalf of its
// CPU.

#pragma once

#include <cstddef>
#include <cstdint>

#include "ntdlib/arm/cpu.h"

namespace ntdlib::ntr
{

constexpr uint32_t kMainSize = 0x400000;

constexpr uint32_t kIo = 0x04000000;
constexpr uint32_t kIpcFifoReceive = 0x04100000;
constexpr uint32_t kCardData = 0x04100010;
constexpr std::size_t kFifoWords = 16;

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

// IPCSYNC's bits.
constexpr uint16_t kSyncOutput = 0x0F00;
constexpr uint16_t kSyncRequest = 1u << 13;
constexpr uint16_t kSyncIrqEnable = 1u << 14;

// The DMA control bits that both CPUs have.
constexpr uint32_t kDmaEnable = 1u << 31;
constexpr uint32_t kDmaIrq = 1u << 30;
constexpr uint32_t kDmaWords = 1u << 26;

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

} // namespace ntdlib::ntr
