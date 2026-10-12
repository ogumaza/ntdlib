// SPDX-License-Identifier: MIT

// The processor interface's registers (physical 0x0C003000): the interrupt causes and their mask. Its other registers
// only store what's written to them.

#pragma once

#include <cstdint>

#include "ntdlib/flipper/registers.h"

namespace ntdlib::flipper
{

// A value type. A machine keeps it with its other registers and copies it in its snapshots.
struct ProcessorInterface
{
    // The 32-bit register at `offset`. `causes` are the machine's pending interrupt causes.
    uint32_t Read(uint32_t offset, uint32_t causes) const
    {
        switch (offset)
        {
        case 0x00:
            return causes;
        case 0x04:
            return mask;
        default:
            return registers.Read(offset, 4);
        }
    }

    // Writes the 32-bit register at `offset`. Returns true for a write to a register that only stores it. The machine
    // reports such a write.
    bool Write(uint32_t offset, uint32_t value)
    {
        switch (offset)
        {
        case 0x00: // the causes clear at their sources
            return false;

        case 0x04:
            mask = value;
            return false;

        default:
            registers.Write(offset, 4, value);
            return true;
        }
    }

    // Whether `causes` raise the CPU's external interrupt: whether any of them is enabled in the mask's low 16 bits.
    bool Asserts(uint32_t causes) const
    {
        return (causes & mask & 0xffff) != 0;
    }

    uint32_t mask = 0;
    RegisterFile<0x100> registers{};
};

} // namespace ntdlib::flipper
