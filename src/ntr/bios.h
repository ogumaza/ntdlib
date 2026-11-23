// SPDX-License-Identifier: MIT

// The BIOS calls that the DS's two CPUs share. Arm7System and Machine use these models of them. `load(address, size)`
// and `store(address, size, value)` reach memory as the CPU does.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

#include "xsflib/text.h"

#include "dsi_bios.h"
#include "ntdlib/arm/cpu.h"
#include "ntdlib/ntr/arm7.h"
#include "ntdlib/rounding.h"

namespace ntdlib::ntr::bios
{

// The SWIs that both CPUs' BIOSes have, by number.
enum Swi : uint32_t
{
    kWaitByLoop = 0x03,
    kIntrWait = 0x04,
    kVBlankIntrWait = 0x05,
    kHalt = 0x06,
    kDiv = 0x09,
    kCpuSet = 0x0B,
    kCpuFastSet = 0x0C,
    kSqrt = 0x0D,
    kGetCrc16 = 0x0E,
    kIsDebugger = 0x0F,
};

// Div: r0 / r1. Sets the quotient in r0, the remainder in r1 and the quotient's magnitude in r3. Returns false, and
// changes nothing, for a division by 0 and for the most negative number by -1.
inline bool Div(arm::Cpu& cpu)
{
    const auto numerator = static_cast<int32_t>(cpu.Reg(0));
    const auto denominator = static_cast<int32_t>(cpu.Reg(1));
    if (denominator == 0 || (numerator == std::numeric_limits<int32_t>::min() && denominator == -1))
    {
        return false;
    }

    const int32_t quotient = numerator / denominator;
    cpu.SetReg(0, static_cast<uint32_t>(quotient));
    cpu.SetReg(1, static_cast<uint32_t>(numerator % denominator));
    cpu.SetReg(3, static_cast<uint32_t>(quotient < 0 ? -static_cast<int64_t>(quotient) : quotient));

    return true;
}

// CpuSet, or CpuFastSet with `fast`: copies the count of units in r2's low 21 bits from r0 to r1, or fills r1 with
// the first unit when r2's bit 24 is set. A unit is a word with `fast` or r2's bit 26, and a halfword otherwise.
// CpuFastSet rounds the count up to a multiple of 8.
template <typename Load, typename Store>
void CpuSet(arm::Cpu& cpu, bool fast, const Load& load, const Store& store)
{
    const uint32_t control = cpu.Reg(2);
    const bool fill = (control & (1u << 24)) != 0;
    const int size = fast || (control & (1u << 26)) != 0 ? 4 : 2;
    uint32_t count = control & 0x1FFFFF;
    if (fast)
    {
        count = AlignUp(count, 8);
    }

    uint32_t source = cpu.Reg(0) & ~static_cast<uint32_t>(size - 1);
    uint32_t destination = cpu.Reg(1) & ~static_cast<uint32_t>(size - 1);
    const uint32_t first = load(source, size);
    for (uint32_t i = 0; i < count; i++)
    {
        store(destination, size, fill ? first : load(source, size));
        destination += static_cast<uint32_t>(size);
        source += fill ? 0 : static_cast<uint32_t>(size);
    }
}

// Sqrt: sets r0 to the integer square root of r0.
inline void Sqrt(arm::Cpu& cpu)
{
    const uint32_t value = cpu.Reg(0);
    uint32_t root = 0;
    for (uint32_t bit = 1u << 15; bit != 0; bit >>= 1)
    {
        const uint32_t trial = root | bit;
        if (static_cast<uint64_t>(trial) * trial <= value)
        {
            root = trial;
        }
    }
    cpu.SetReg(0, root);
}

// GetCrc16: GBATEK's CRC-16, a byte at a time, of the r2 bytes from r1, starting from r0's low halfword. Sets the CRC
// in r0, and the last halfword read in r3.
template <typename Load>
void GetCrc16(arm::Cpu& cpu, const Load& load)
{
    constexpr std::array<uint16_t, 8> kValues = {0xC0C1, 0xC181, 0xC301, 0xC601, 0xCC01, 0xD801, 0xF001, 0xA001};
    uint32_t crc = cpu.Reg(0) & 0xFFFF;
    const uint32_t start = cpu.Reg(1) & ~1u;
    const uint32_t length = cpu.Reg(2) & ~1u;
    uint32_t last = 0;
    for (uint32_t at = start; at < start + length; at += 2)
    {
        last = load(at, 2);
        for (const uint32_t byte : {last & 0xFF, last >> 8})
        {
            crc ^= byte;
            for (std::size_t j = 0; j < kValues.size(); j++)
            {
                const bool carry = (crc & 1) != 0;
                crc >>= 1;
                if (carry)
                {
                    crc ^= static_cast<uint32_t>(kValues[j]) << (7 - j);
                }
            }
        }
    }
    cpu.SetReg(0, crc & 0xFFFF);
    cpu.SetReg(3, last);
}

// SWI `number`, where both CPUs make it the same way: IntrWait, VBlankIntrWait, Div, CpuSet, CpuFastSet, Sqrt, GetCrc16
// and IsDebugger, and in DSi mode (`dsi`) the SHA-1 calls. `fail(message)` reports a call that the model can't make.
// Returns nothing for another SWI. Otherwise it returns what a handler of SWIs returns: false for IntrWait and
// VBlankIntrWait, which the BIOS's code runs, and true for the rest.
template <typename Load, typename Store, typename Fail>
std::optional<bool> SharedCall(arm::Cpu& cpu, uint32_t number, bool dsi, const Load& load, const Store& store,
                               const Fail& fail)
{
    switch (number)
    {
    case kIntrWait:
        return false;
    case kVBlankIntrWait:
        cpu.SetReg(0, 1);
        cpu.SetReg(1, irq::kVblank);
        return false;
    case kDiv:
        if (!Div(cpu))
        {
            fail("Div with " + xsflib::Hex(cpu.Reg(0), 8, true) + " / " + xsflib::Hex(cpu.Reg(1), 8, true));
        }
        return true;
    case kCpuSet:
    case kCpuFastSet:
        CpuSet(cpu, number == kCpuFastSet, load, store);
        return true;
    case kSqrt:
        Sqrt(cpu);
        return true;
    case kGetCrc16:
        GetCrc16(cpu, load);
        return true;
    case kIsDebugger:
        cpu.SetReg(0, 0);
        return true;
    default:
        if (dsi && number >= kSha1First && number <= kSha1Last)
        {
            const std::string error = Sha1Call(cpu, number, load, store);
            if (!error.empty())
            {
                fail(error);
            }
            return true;
        }

        return std::nullopt;
    }
}

} // namespace ntdlib::ntr::bios
