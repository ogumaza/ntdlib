// SPDX-License-Identifier: MIT

// The DSP's registers, stacks, flags, address arithmetic, exceptions and step.

#include "ntdlib/dsp/dsp.h"

#include <bit>
#include <cstddef>
#include <cstdint>

#include "dsp/internal.h"

namespace ntdlib::dsp
{

template <typename Memory>
Dsp<Memory>::Dsp(Memory& memory)
    : memory_(&memory),
      iram_(Memory::kIramSize, 0x0021),
      dram_(Memory::kDramSize, 0),
      loop_end_(0x10000, 0),
      idle_(Memory::kIramSize, 0)
{
    BuildLoopMap();
}

template <typename Memory>
void Dsp<Memory>::Step()
{
    steps_++;
    taken_exception_ = -1;
    if (pending_exceptions_ != 0)
    {
        CheckExceptions();
    }

    // The boot ROM isn't present. Its routines run in boot_rom.cpp instead. Without them, a program that leaves IRAM
    // halts the DSP.
    if (Memory::kBootRom && regs_.pc >= kRomEntry && regs_.pc < kRomEnd)
    {
        RomStep();
    }
    else if (!Memory::kBootRom && regs_.pc >= Memory::kIramSize)
    {
        control_ |= control::kHalt;
        return;
    }
    else
    {
        const uint16_t op = ReadImem(regs_.pc);
        regs_.pc++;
        handlers_[op](*this, op);
    }

    if (loop_end_[static_cast<uint16_t>(regs_.pc - 1)])
    {
        CheckLoop();
    }
}

template <typename Memory>
uint64_t Dsp<Memory>::Run(uint64_t steps)
{
    uint64_t run = 0;
    for (; run < steps && !Halted(); run++)
    {
        Step();
    }

    return run;
}

template <typename Memory>
void Dsp<Memory>::LoadIram(uint16_t address, std::span<const uint16_t> words)
{
    for (std::size_t i = 0; i < words.size() && address + i < Memory::kIramSize; i++)
    {
        iram_[address + i] = words[i];
    }

    BuildLoopMap();
}

template <typename Memory>
void Dsp<Memory>::LoadDram(uint16_t address, std::span<const uint16_t> words)
{
    for (std::size_t i = 0; i < words.size() && address + i < Memory::kDramSize; i++)
    {
        dram_[address + i] = words[i];
    }
}

template <typename Memory>
void Dsp<Memory>::WriteRegLoad(int reg, uint16_t value)
{
    // The 40-bit-mode load extension: a load into $acX.m in 40-bit mode sets h from its sign and clears l.
    if ((reg == 0x1e || reg == 0x1f) && (regs_.sr & kSrSxm))
    {
        regs_.ac[reg - 0x1e] = static_cast<int64_t>(static_cast<int16_t>(value)) * 0x10000;
        return;
    }

    WriteReg(reg, value);
}

template <typename Memory>
void Dsp<Memory>::Push(int stack, uint16_t value)
{
    // The stacks wrap silently at 32 entries.
    Stack& s = regs_.stacks[stack];
    s.pointer = (s.pointer + 1) & 31;
    s.hidden[s.pointer] = s.top;
    s.top = value;
}

template <typename Memory>
uint16_t Dsp<Memory>::Pop(int stack)
{
    Stack& s = regs_.stacks[stack];
    const uint16_t value = s.top;
    s.top = s.hidden[s.pointer];
    s.pointer = (s.pointer - 1) & 31;

    return value;
}

template <typename Memory>
int64_t Dsp<Memory>::Product() const
{
    // P = sext8(h) * 2^32 + (m1 + m2) * 2^16 + l, exactly.
    const int64_t high = static_cast<int8_t>(regs_.prod_h & 0xff);

    return high * (int64_t{1} << 32) + (int64_t{regs_.prod_m1} + regs_.prod_m2) * 0x10000 + regs_.prod_l;
}

template <typename Memory>
void Dsp<Memory>::SetProduct(int64_t value)
{
    const auto bits = static_cast<uint64_t>(value);
    regs_.prod_l = static_cast<uint16_t>(bits);
    regs_.prod_m1 = static_cast<uint16_t>(bits >> 16);
    regs_.prod_h = static_cast<uint16_t>(bits >> 32 & 0xff);
    regs_.prod_m2 = 0;
}

template <typename Memory>
uint16_t Dsp<Memory>::AcH(int i) const
{
    return static_cast<uint16_t>(static_cast<int16_t>(static_cast<int8_t>(regs_.ac[i] >> 32)));
}

template <typename Memory>
uint16_t Dsp<Memory>::AcM(int i) const
{
    return static_cast<uint16_t>(regs_.ac[i] >> 16);
}

template <typename Memory>
uint16_t Dsp<Memory>::AcL(int i) const
{
    return static_cast<uint16_t>(regs_.ac[i]);
}

template <typename Memory>
void Dsp<Memory>::SetAcH(int i, uint16_t value)
{
    const uint64_t rest = static_cast<uint64_t>(regs_.ac[i]) & 0xffffffff;
    regs_.ac[i] = Wrap40(static_cast<int64_t>(uint64_t{value & 0xffu} << 32 | rest));
}

template <typename Memory>
void Dsp<Memory>::SetAcM(int i, uint16_t value)
{
    const uint64_t bits = static_cast<uint64_t>(regs_.ac[i]);
    regs_.ac[i] = Wrap40(static_cast<int64_t>((bits & 0xff0000ffff) | uint64_t{value} << 16));
}

template <typename Memory>
void Dsp<Memory>::SetAcL(int i, uint16_t value)
{
    const uint64_t bits = static_cast<uint64_t>(regs_.ac[i]);
    regs_.ac[i] = Wrap40(static_cast<int64_t>((bits & 0xffffff0000) | value));
}

template <typename Memory>
void Dsp<Memory>::SetAc(int i, int64_t value)
{
    regs_.ac[i] = Wrap40(value);
}

template <typename Memory>
int32_t Dsp<Memory>::Ax(int i) const
{
    return static_cast<int32_t>(uint32_t{regs_.ax_h[i]} << 16 | regs_.ax_l[i]);
}

template <typename Memory>
bool Dsp<Memory>::Condition(int cc) const
{
    // The 16 conditions.
    const uint16_t sr = regs_.sr;
    const bool o = (sr & kSrOverflow) != 0;
    const bool s = (sr & kSrSign) != 0;
    const bool z = (sr & kSrZero) != 0;
    const bool as = (sr & kSrAboveS32) != 0;
    const bool tb = (sr & kSrTopBits) != 0;
    switch (cc)
    {
    case 0x0:
        return o == s;
    case 0x1:
        return o != s;
    case 0x2:
        return o == s && !z;
    case 0x3:
        return o != s || z;
    case 0x4:
        return !z;
    case 0x5:
        return z;
    case 0x6:
        return (sr & kSrCarry) == 0;
    case 0x7:
        return (sr & kSrCarry) != 0;
    case 0x8:
        return !as;
    case 0x9:
        return as;
    case 0xa:
        return (as || tb) && !z;
    case 0xb:
        return (!as && !tb) || z;
    case 0xc:
        return (sr & kSrLogicZero) == 0;
    case 0xd:
        return (sr & kSrLogicZero) != 0;
    case 0xe:
        return o;
    default:
        return true;
    }
}

template <typename Memory>
void Dsp<Memory>::SetFlags40(int64_t value, bool carry, bool overflow)
{
    // The flags of a 40-bit result. Bits 0-5 are replaced together. OS is only ever set.
    const auto bits = static_cast<uint64_t>(value);
    uint16_t flags = 0;
    if (carry)
    {
        flags |= kSrCarry;
    }

    if (overflow)
    {
        flags |= kSrOverflow | kSrOverflowSticky;
    }

    if (value == 0)
    {
        flags |= kSrZero;
    }

    if (value < 0)
    {
        flags |= kSrSign;
    }

    if (value < kS32Min || value > kS32Max)
    {
        flags |= kSrAboveS32;
    }

    if ((bits >> 31 & 1) == (bits >> 30 & 1))
    {
        flags |= kSrTopBits;
    }

    regs_.sr = (regs_.sr & ~0x3f) | flags;
}

template <typename Memory>
void Dsp<Memory>::SetFlagsAdd(int64_t a, int64_t b, int64_t result)
{
    const bool carry = Unsigned40(result) < Unsigned40(a);
    const bool overflow = ((a < 0) != (result < 0)) && ((b < 0) != (result < 0));
    SetFlags40(result, carry, overflow);
}

template <typename Memory>
void Dsp<Memory>::SetFlagsSub(int64_t a, int64_t b, int64_t result)
{
    const bool carry = Unsigned40(a) >= Unsigned40(result);
    const bool overflow = ((a < 0) != (result < 0)) && ((-b < 0) != (result < 0));
    SetFlags40(result, carry, overflow);
}

template <typename Memory>
void Dsp<Memory>::SetFlags16(uint16_t value, bool above_s32)
{
    // The flags of a 16-bit result.
    uint16_t flags = 0;
    if (value == 0)
    {
        flags |= kSrZero;
    }

    if (value & 0x8000)
    {
        flags |= kSrSign;
    }

    if (above_s32)
    {
        flags |= kSrAboveS32;
    }

    if ((value >> 15 & 1) == (value >> 14 & 1))
    {
        flags |= kSrTopBits;
    }

    regs_.sr = (regs_.sr & ~0x3f) | flags;
}

template <typename Memory>
uint16_t Dsp<Memory>::Increment(uint16_t address, uint16_t wrap) const
{
    // The address wraps within the aligned block of 2^n words that holds it. n is the bit width of (W | 1).
    const uint32_t mask = (uint32_t{1} << std::bit_width(uint32_t{wrap} | 1u)) - 1;
    if ((address & mask) == mask)
    {
        return static_cast<uint16_t>(address - wrap);
    }

    return static_cast<uint16_t>(address + 1);
}

template <typename Memory>
uint16_t Dsp<Memory>::Decrement(uint16_t address, uint16_t wrap) const
{
    const uint32_t mask = (uint32_t{1} << std::bit_width(uint32_t{wrap} | 1u)) - 1;
    if ((address & mask) + (wrap & mask) > mask)
    {
        return static_cast<uint16_t>(address - 1);
    }

    return static_cast<uint16_t>(address + wrap);
}

template <typename Memory>
uint16_t Dsp<Memory>::AddIndex(uint16_t address, uint16_t index, uint16_t wrap) const
{
    const int n = std::bit_width(uint32_t{wrap} | 1u);
    const uint32_t mask = (uint32_t{1} << n) - 1;
    const int32_t i = static_cast<int16_t>(index);
    const int32_t sum = int32_t{address} + i;

    const auto carries = [mask](uint32_t x, uint32_t y)
    {
        return (x & mask) + (y & mask) > mask;
    };

    if (i >= 0)
    {
        return static_cast<uint16_t>(carries(address, static_cast<uint32_t>(i)) ? sum - (wrap + 1) : sum);
    }

    const bool c = carries(address, static_cast<uint32_t>(i));
    const bool f = uint32_t{wrap} + 1 == mask + 1 || carries(static_cast<uint32_t>(sum), uint32_t{wrap} + 1);

    return static_cast<uint16_t>(c && f ? sum : sum + (wrap + 1));
}

template <typename Memory>
uint16_t Dsp<Memory>::SteppedAddress(int i, int step) const
{
    switch (step)
    {
    case kDecrementStep:
        return Decrement(regs_.ar[i], regs_.wr[i]);
    case kIncrementStep:
        return Increment(regs_.ar[i], regs_.wr[i]);
    case kAddIndexStep:
        return AddIndex(regs_.ar[i], regs_.ix[i], regs_.wr[i]);
    default:
        return regs_.ar[i];
    }
}

template <typename Memory>
uint16_t Dsp<Memory>::SubtractIndex(uint16_t address, uint16_t index, uint16_t wrap) const
{
    const int n = std::bit_width(uint32_t{wrap} | 1u);
    const uint32_t mask = (uint32_t{1} << n) - 1;
    const int32_t i = static_cast<int16_t>(index);
    const int32_t difference = int32_t{address} - i;
    const uint32_t not_index = ~static_cast<uint32_t>(i);

    // Whether adding A, bitwise-not I and a carry-in of 1 carries out of the low n bits.
    const bool c = (address & mask) + (not_index & mask) + 1 > mask;
    if (i >= -32767 && i <= -1)
    {
        return static_cast<uint16_t>(c ? difference - (wrap + 1) : difference);
    }

    const bool f = uint32_t{wrap} + 1 == mask + 1 ||
                   (static_cast<uint32_t>(difference) & mask) + ((uint32_t{wrap} + 1) & mask) > mask;

    return static_cast<uint16_t>(c && f ? difference : difference + (wrap + 1));
}

template <typename Memory>
void Dsp<Memory>::RaiseException(int number)
{
    pending_exceptions_ |= static_cast<uint8_t>(1u << number);
}

template <typename Memory>
void Dsp<Memory>::CheckExceptions()
{
    // Takes the highest-numbered pending exception that's enabled, at most one per step. Exceptions 1-6 require IE.
    // Exception 7 was checked against EIE when it was raised.
    if (pending_exceptions_ == 0)
    {
        return;
    }

    for (int k = 7; k >= 1; k--)
    {
        if (!(pending_exceptions_ & (1u << k)))
        {
            continue;
        }
        if (k != 7 && !(regs_.sr & kSrInterruptEnable))
        {
            continue;
        }

        Push(0, regs_.pc);
        Push(1, regs_.sr);
        regs_.pc = static_cast<uint16_t>(2 * k);
        taken_exception_ = k;
        pending_exceptions_ &= static_cast<uint8_t>(~(1u << k));
        regs_.sr &= k == 7 ? ~kSrExternalEnable : ~kSrInterruptEnable;
        return;
    }
}

template <typename Memory>
void Dsp<Memory>::CheckLoop()
{
    // The loop check applies only to the innermost loop. It runs only at the loop ends that the static analysis found.
    const uint16_t last = static_cast<uint16_t>(regs_.pc - 1);
    Registers& r = regs_;
    if (r.stacks[2].top == 0 || r.stacks[3].top == 0 || last != r.stacks[2].top)
    {
        return;
    }

    r.stacks[3].top--;
    if (r.stacks[3].top != 0)
    {
        r.pc = r.stacks[0].top;
        return;
    }

    Pop(0);
    Pop(2);
    Pop(3);
}

} // namespace ntdlib::dsp
