// SPDX-License-Identifier: MIT

// Definitions that the parts of the interpreter share (cpu.cpp and cpu_memory.cpp): the XER, HID4 and SRR1 bits, the
// processor version, the fields of an instruction, the rotate masks, the compare and carry helpers, the watches' bits,
// and the BATs. The two files build as one translation unit. The definitions therefore keep internal linkage. Only
// those files include this header.

#pragma once

#include <bit>
#include <cfenv>
#include <cstdint>

#include "ntdlib/ppc/cpu.h"

namespace ntdlib::ppc
{
namespace
{

constexpr uint32_t kPageMask = 0xfff;

// XER bits.
constexpr uint32_t kXerSo = 1u << 31;
constexpr uint32_t kXerOv = 1u << 30;
constexpr uint32_t kXerCa = 1u << 29;

// Broadway's processor version.
constexpr uint32_t kBroadwayPvr = 0x00087102;

// HID4's bit that enables BATs 4 to 7.
constexpr uint32_t kHid4Sbe = 1u << 25;

// The MSR bits that SRR1 saves and rfi restores.
constexpr uint32_t kSrr1Msr = 0x0000ff73;

// The causes of a program exception, in SRR1.
constexpr uint32_t kProgramTrap = 1u << 17;

int Rd(uint32_t op)
{
    return op >> 21 & 31;
}

int Ra(uint32_t op)
{
    return op >> 16 & 31;
}

int Rb(uint32_t op)
{
    return op >> 11 & 31;
}

int32_t Simm(uint32_t op)
{
    return static_cast<int16_t>(op & 0xffff);
}

uint32_t Uimm(uint32_t op)
{
    return op & 0xffff;
}

bool Rc(uint32_t op)
{
    return (op & 1) != 0;
}

// The mask of bits MB to ME (IBM numbering, with 0 the most significant bit). The mask wraps around if MB > ME.
uint32_t Mask(int mb, int me)
{
    const uint32_t begin = 0xffffffffu >> mb;
    const uint32_t end = 0xffffffffu << (31 - me);

    return mb <= me ? begin & end : begin | end;
}

uint32_t Rotate(uint32_t value, int amount)
{
    return std::rotl(value, amount & 31);
}

// The CR field that a comparison of x with y sets: LT, GT or EQ, with SO a copy of XER's summary overflow.
template <typename T>
uint32_t CompareField(T x, T y, uint32_t xer)
{
    return (x < y ? 8u : x > y ? 4u : 2u) | ((xer & kXerSo) ? 1u : 0u);
}

void SetCarry(uint32_t& xer, bool carry)
{
    xer = carry ? xer | kXerCa : xer & ~kXerCa;
}

// An overflow also sets the summary overflow. The summary stays set until an mcrxr or a write of XER clears it.
void SetOverflow(uint32_t& xer, bool overflow)
{
    xer = overflow ? xer | kXerOv | kXerSo : xer & ~kXerOv;
}

// Whether a trap instruction traps: its TO field `to` selects the relations of x to y that trap. They are less than,
// greater than and equal as signed values (bits 16, 8 and 4), then less than and greater than as unsigned values (bits
// 2 and 1).
bool Traps(uint32_t to, int32_t x, int32_t y)
{
    return ((to & 16) && x < y) || ((to & 8) && x > y) || ((to & 4) && x == y) ||
           ((to & 2) && static_cast<uint32_t>(x) < static_cast<uint32_t>(y)) ||
           ((to & 1) && static_cast<uint32_t>(x) > static_cast<uint32_t>(y));
}

// The bit of the watch filter (Cpu::watch_filter_) for the instruction at `pc`: bit (pc / 4) % 64.
uint64_t WatchBit(uint32_t pc)
{
    return uint64_t{1} << (pc >> 2 & 63);
}

// A BAT pair: an upper and a lower register that map a block of effective addresses to physical ones.
struct Bat
{
    bool valid;         // Vs, the pair's valid bit for supervisor mode
    uint32_t effective; // the block's first effective address (BEPI)
    uint32_t physical;  // the block's first physical address (BRPN)
    uint32_t length;    // the block's length in bytes, from BL: 128 KiB to 256 MiB
};

// The number of BAT pairs of each kind that translate: 8 while HID4 enables BATs 4 to 7, otherwise 4.
int BatCount(const State& state)
{
    return (state.spr[spr::kHid4] & kHid4Sbe) ? 8 : 4;
}

// BAT pair `index` (0 to 7) of the instruction BATs or of the data BATs.
Bat BatAt(const State& state, int index, bool instruction)
{
    const int number = (index < 4 ? spr::kIbat0u + index * 2 : spr::kIbat4u + (index - 4) * 2) + (instruction ? 0 : 8);
    const uint32_t upper = state.spr[number];
    const uint32_t lower = state.spr[number + 1];
    const uint32_t length = ((upper >> 2 & 0x7ff) + 1) << 17;

    return {(upper & 2) != 0, upper & 0xfffe0000 & ~(length - 1), lower & 0xfffe0000 & ~(length - 1), length};
}

} // namespace
} // namespace ntdlib::ppc
