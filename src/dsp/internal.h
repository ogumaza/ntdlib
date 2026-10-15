// SPDX-License-Identifier: MIT

// Definitions shared by the DSP's source files: the bits of the status register, the numbers of the hardware registers
// that more than one file uses, the full flag of the mailboxes, 40-bit arithmetic, and register access by number. Most
// instructions access registers by number. That access is defined here so that the instruction handlers can inline it.
// Only the files in src/dsp include this header.

#pragma once

#include <cstdint>

#include "ntdlib/dsp/accumulator.h"
#include "ntdlib/dsp/dsp.h"

// Forces the compiler to inline a function. The instruction handlers rely on this to fold their decoding away.
#if defined(_MSC_VER) && !defined(__clang__)
#define NTDLIB_DSP_FORCE_INLINE __forceinline
#else
#define NTDLIB_DSP_FORCE_INLINE inline __attribute__((always_inline))
#endif

namespace ntdlib::dsp
{

// $sr bits.
inline constexpr uint16_t kSrCarry = 1u << 0;
inline constexpr uint16_t kSrOverflow = 1u << 1;
inline constexpr uint16_t kSrZero = 1u << 2;
inline constexpr uint16_t kSrSign = 1u << 3;
inline constexpr uint16_t kSrAboveS32 = 1u << 4;
inline constexpr uint16_t kSrTopBits = 1u << 5;
inline constexpr uint16_t kSrLogicZero = 1u << 6;
inline constexpr uint16_t kSrOverflowSticky = 1u << 7;
inline constexpr uint16_t kSrInterruptEnable = 1u << 9;
inline constexpr uint16_t kSrExternalEnable = 1u << 11;
inline constexpr uint16_t kSrAm = 1u << 13;
inline constexpr uint16_t kSrSxm = 1u << 14;
inline constexpr uint16_t kSrSu = 1u << 15;

// Hardware register numbers (the low 8 bits of their addresses): DMA and the mailboxes.
inline constexpr uint8_t kDscr = 0xc9;
inline constexpr uint8_t kDsbl = 0xcb;
inline constexpr uint8_t kDspa = 0xcd;
inline constexpr uint8_t kDsmah = 0xce;
inline constexpr uint8_t kDsmal = 0xcf;
inline constexpr uint8_t kDmbh = 0xfc;
inline constexpr uint8_t kDmbl = 0xfd;
inline constexpr uint8_t kCmbh = 0xfe;
inline constexpr uint8_t kCmbl = 0xff;

// A mailbox's full flag, in its 32 bits.
inline constexpr uint32_t kMailboxFull = 0x80000000;

inline int64_t Sext16(uint16_t value)
{
    return static_cast<int16_t>(value);
}

// A 40-bit value's bits, as an unsigned number.
inline uint64_t Unsigned40(int64_t value)
{
    return static_cast<uint64_t>(value) & 0xffffffffff;
}

// Raises the CPU's interrupt request (DSPCR bit 1) as exception 7 if `$sr.EIE` is set. A request that the CPU makes
// while EIE is clear stays in DSPCR. The DSP takes it once EIE is set again: by an RTI, an SBSET or a move to $sr, or
// at the CPU's next write of DSPCR. Super Mario Sunshine's microcode clears EIE while it decrements a count that its
// handler of the CPU's interrupt increments. A request lost in that window would leave the game waiting for the DSP.
template <typename Memory>
NTDLIB_DSP_FORCE_INLINE void Dsp<Memory>::RaiseCpuInterrupt()
{
    if ((control_ & control::kInterrupt) && (regs_.sr & kSrExternalEnable))
    {
        RaiseException(7);
        control_ &= ~control::kInterrupt;
    }
}

template <typename Memory>
NTDLIB_DSP_FORCE_INLINE uint16_t Dsp<Memory>::ReadReg(int reg)
{
    // Reading by number: a stack pops, h is sign-extended, and m saturates in 40-bit mode.
    switch (reg)
    {
    case 0x0c:
    case 0x0d:
    case 0x0e:
    case 0x0f:
        return Pop(reg - 0x0c);

    case 0x1e:
    case 0x1f:
        {
            const int i = reg - 0x1e;
            return (regs_.sr & kSrSxm) ? static_cast<uint16_t>(MiddleWord(regs_.ac[i])) : AcM(i);
        }

    default:
        return RawReg(reg);
    }
}

template <typename Memory>
NTDLIB_DSP_FORCE_INLINE void Dsp<Memory>::WriteReg(int reg, uint16_t value)
{
    Registers& r = regs_;
    switch (reg)
    {
    case 0x00:
    case 0x01:
    case 0x02:
    case 0x03:
        r.ar[reg] = value;
        break;
    case 0x04:
    case 0x05:
    case 0x06:
    case 0x07:
        r.ix[reg - 4] = value;
        break;
    case 0x08:
    case 0x09:
    case 0x0a:
    case 0x0b:
        r.wr[reg - 8] = value;
        break;
    case 0x0c:
    case 0x0d:
    case 0x0e:
    case 0x0f:
        Push(reg - 0x0c, value);
        break;
    case 0x10:
    case 0x11:
        SetAcH(reg - 0x10, value);
        break;
    case 0x12:
        r.cr = static_cast<uint8_t>(value);
        break;
    case 0x13:
        r.sr = value & ~0x0100;
        RaiseCpuInterrupt();
        break;
    case 0x14:
        r.prod_l = value;
        break;
    case 0x15:
        r.prod_m1 = value;
        break;
    case 0x16:
        r.prod_h = value & 0xff;
        break;
    case 0x17:
        r.prod_m2 = value;
        break;
    case 0x18:
    case 0x19:
        r.ax_l[reg - 0x18] = value;
        break;
    case 0x1a:
    case 0x1b:
        r.ax_h[reg - 0x1a] = value;
        break;
    case 0x1c:
    case 0x1d:
        SetAcL(reg - 0x1c, value);
        break;
    default:
        SetAcM(reg - 0x1e, value);
        break;
    }
}

template <typename Memory>
NTDLIB_DSP_FORCE_INLINE uint16_t Dsp<Memory>::RawReg(int reg) const
{
    const Registers& r = regs_;
    switch (reg)
    {
    case 0x00:
    case 0x01:
    case 0x02:
    case 0x03:
        return r.ar[reg];
    case 0x04:
    case 0x05:
    case 0x06:
    case 0x07:
        return r.ix[reg - 4];
    case 0x08:
    case 0x09:
    case 0x0a:
    case 0x0b:
        return r.wr[reg - 8];
    case 0x0c:
    case 0x0d:
    case 0x0e:
    case 0x0f:
        return r.stacks[reg - 0x0c].top;
    case 0x10:
    case 0x11:
        return AcH(reg - 0x10);
    case 0x12:
        return r.cr;
    case 0x13:
        return r.sr;
    case 0x14:
        return r.prod_l;
    case 0x15:
        return r.prod_m1;
    case 0x16:
        return r.prod_h;
    case 0x17:
        return r.prod_m2;
    case 0x18:
    case 0x19:
        return r.ax_l[reg - 0x18];
    case 0x1a:
    case 0x1b:
        return r.ax_h[reg - 0x1a];
    case 0x1c:
    case 0x1d:
        return AcL(reg - 0x1c);
    default:
        return AcM(reg - 0x1e);
    }
}

} // namespace ntdlib::dsp
