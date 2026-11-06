// SPDX-License-Identifier: MIT

// The ARM instruction set of ARMv4T and ARMv5TE, and its decoding table. The handlers follow ARM's architecture
// reference manual, with GBATEK's notes on what the DS's two CPUs do where the manual leaves the result open.

#include <array>
#include <bit>
#include <cstdint>
#include <utility>

#include "arm/cpu_impl.h"

namespace ntdlib::arm::detail
{
namespace
{

int Rn(uint32_t op)
{
    return static_cast<int>((op >> 16) & 15);
}

int Rd(uint32_t op)
{
    return static_cast<int>((op >> 12) & 15);
}

int Rs(uint32_t op)
{
    return static_cast<int>((op >> 8) & 15);
}

int Rm(uint32_t op)
{
    return static_cast<int>(op & 15);
}

bool Bit(uint32_t op, int bit)
{
    return ((op >> bit) & 1) != 0;
}

// An 8-bit immediate rotated right by twice the 4-bit rotation above it.
uint32_t RotatedImmediate(uint32_t op)
{
    return std::rotr(op & 0xFF, static_cast<int>((op >> 7) & 0x1E));
}

void UndefinedInstruction(Core& core, uint32_t /*op*/)
{
    core.Undefined();
}

// The second operand of a data-processing instruction.
enum class Operand2
{
    kImmediate,      // an 8-bit immediate, rotated
    kShiftImmediate, // a register shifted by an immediate
    kShiftRegister,  // a register shifted by the low byte of a register
};

// AND, EOR, SUB, RSB, ADD, ADC, SBC, RSC, TST, TEQ, CMP, CMN, ORR, MOV, BIC and MVN.
template <uint32_t kOpcode, bool kSetFlags, Operand2 kForm>
void DataProcessing(Core& core, uint32_t op)
{
    constexpr bool kLogical = kOpcode <= 1 || kOpcode == 8 || kOpcode == 9 || kOpcode >= 12;
    constexpr bool kTest = kOpcode >= 8 && kOpcode <= 11;

    // The shifter. With a shift by a register, an operand of r15 reads as the instruction's address plus 12.
    uint32_t carry = core.Carry();
    uint32_t first;
    uint32_t second;
    if constexpr (kForm == Operand2::kImmediate)
    {
        first = core.regs_[Rn(op)];
        second = RotatedImmediate(op);
        if ((op & 0xF00) != 0)
        {
            carry = second >> 31;
        }
    }
    else if constexpr (kForm == Operand2::kShiftImmediate)
    {
        first = core.regs_[Rn(op)];
        second = ShiftByImmediate(core.regs_[Rm(op)], (op >> 5) & 3, (op >> 7) & 31, carry);
    }
    else
    {
        const int rn = Rn(op);
        const int rm = Rm(op);
        first = core.regs_[rn] + (rn == 15 ? 4 : 0);
        const uint32_t value = core.regs_[rm] + (rm == 15 ? 4 : 0);
        second = ShiftByRegister(value, (op >> 5) & 3, core.regs_[Rs(op)] & 0xFF, carry);
    }

    uint32_t result = 0;
    uint32_t overflow = 0;
    if constexpr (kOpcode == 0 || kOpcode == 8)
    {
        result = first & second;
    }
    else if constexpr (kOpcode == 1 || kOpcode == 9)
    {
        result = first ^ second;
    }
    else if constexpr (kOpcode == 2 || kOpcode == 10)
    {
        result = AddWithCarry(first, ~second, 1, carry, overflow);
    }
    else if constexpr (kOpcode == 3)
    {
        result = AddWithCarry(second, ~first, 1, carry, overflow);
    }
    else if constexpr (kOpcode == 4 || kOpcode == 11)
    {
        result = AddWithCarry(first, second, 0, carry, overflow);
    }
    else if constexpr (kOpcode == 5)
    {
        result = AddWithCarry(first, second, core.Carry(), carry, overflow);
    }
    else if constexpr (kOpcode == 6)
    {
        result = AddWithCarry(first, ~second, core.Carry(), carry, overflow);
    }
    else if constexpr (kOpcode == 7)
    {
        result = AddWithCarry(second, ~first, core.Carry(), carry, overflow);
    }
    else if constexpr (kOpcode == 12)
    {
        result = first | second;
    }
    else if constexpr (kOpcode == 13)
    {
        result = second;
    }
    else if constexpr (kOpcode == 14)
    {
        result = first & ~second;
    }
    else
    {
        result = ~second;
    }

    // A write to r15 branches. With the S bit, the CPSR also takes the SPSR's value: an exception's return.
    if constexpr (!kTest)
    {
        const int rd = Rd(op);
        if (rd == 15)
        {
            if constexpr (kSetFlags)
            {
                core.RestoreCpsr();
            }

            core.BranchInState(result);
            return;
        }

        core.regs_[rd] = result;
    }

    if constexpr (kSetFlags && kLogical)
    {
        core.SetNzc(result, carry);
    }
    else if constexpr (kSetFlags)
    {
        core.SetNzcv(result, carry, overflow);
    }
}

template <uint32_t kOpcode>
constexpr std::array<Handler, 6> DataProcessingForms()
{
    return {&DataProcessing<kOpcode, false, Operand2::kImmediate>,
            &DataProcessing<kOpcode, false, Operand2::kShiftImmediate>,
            &DataProcessing<kOpcode, false, Operand2::kShiftRegister>,
            &DataProcessing<kOpcode, true, Operand2::kImmediate>,
            &DataProcessing<kOpcode, true, Operand2::kShiftImmediate>,
            &DataProcessing<kOpcode, true, Operand2::kShiftRegister>};
}

template <size_t... kOpcodes>
constexpr std::array<std::array<Handler, 6>, 16> DataProcessingTable(std::index_sequence<kOpcodes...> /*opcodes*/)
{
    return {DataProcessingForms<kOpcodes>()...};
}

constexpr std::array<std::array<Handler, 6>, 16> kDataProcessing = DataProcessingTable(std::make_index_sequence<16>());

// MRS: the CPSR or the SPSR to a register.
template <bool kSpsr>
void MoveFromPsr(Core& core, uint32_t op)
{
    core.regs_[Rd(op)] = kSpsr ? core.Spsr() : core.cpsr_;
}

// MSR: a register or an immediate to the fields of the CPSR or the SPSR that bits 19-16 select. User mode writes only
// the flags of the CPSR, and MSR never changes the T bit of the CPSR.
template <bool kSpsr, bool kImmediate>
void MoveToPsr(Core& core, uint32_t op)
{
    const uint32_t value = kImmediate ? RotatedImmediate(op) : core.regs_[Rm(op)];
    uint32_t mask = 0;
    for (int field = 0; field < 4; ++field)
    {
        if (Bit(op, 16 + field))
        {
            mask |= 0xFFu << (8 * field);
        }
    }

    mask &= core.psr_bits_;
    if constexpr (kSpsr)
    {
        core.WriteSpsr(value, mask);
    }
    else
    {
        if (!core.Privileged())
        {
            mask &= 0xFF000000;
        }

        mask &= ~psr::kT;
        core.WriteCpsr((core.cpsr_ & ~mask) | (value & mask));
    }
}

void BranchAndExchange(Core& core, uint32_t op)
{
    core.BranchExchange(core.regs_[Rm(op)]);
}

// ARMv5TE: BLX with a register.
void BranchLinkAndExchange(Core& core, uint32_t op)
{
    if (!core.v5_)
    {
        core.Undefined();
        return;
    }

    const uint32_t target = core.regs_[Rm(op)];
    core.regs_[14] = core.regs_[15] - 4;
    core.BranchExchange(target);
}

// ARMv5TE: CLZ.
void CountLeadingZeros(Core& core, uint32_t op)
{
    if (!core.v5_)
    {
        core.Undefined();
        return;
    }

    core.regs_[Rd(op)] = static_cast<uint32_t>(std::countl_zero(core.regs_[Rm(op)]));
}

// Limits a sum to the signed 32-bit range, and sets the Q flag when it had to.
uint32_t Saturate(Core& core, int64_t value)
{
    if (value > INT32_MAX || value < INT32_MIN)
    {
        core.cpsr_ |= psr::kQ;
        return value > INT32_MAX ? 0x7FFFFFFF : 0x80000000;
    }

    return static_cast<uint32_t>(value);
}

// ARMv5TE: QADD, QSUB, QDADD and QDSUB. The doubling saturates by itself before the sum.
void SaturatingArithmetic(Core& core, uint32_t op)
{
    if (!core.v5_)
    {
        core.Undefined();
        return;
    }

    const uint32_t kind = (op >> 21) & 3;
    const int64_t first = static_cast<int32_t>(core.regs_[Rm(op)]);
    int64_t second = static_cast<int32_t>(core.regs_[Rn(op)]);
    if ((kind & 2) != 0)
    {
        second = static_cast<int32_t>(Saturate(core, second * 2));
    }

    core.regs_[Rd(op)] = Saturate(core, (kind & 1) != 0 ? first - second : first + second);
}

// ARMv5TE: BKPT.
void BreakpointInstruction(Core& core, uint32_t /*op*/)
{
    if (!core.v5_)
    {
        core.Undefined();
        return;
    }

    core.Breakpoint();
}

int32_t Half(uint32_t value, bool top)
{
    return static_cast<int16_t>(top ? value >> 16 : value);
}

// ARMv5TE: SMLAxy, SMLAWy, SMULWy, SMLALxy and SMULxy. Only the 32-bit accumulations set the Q flag, and none of
// them stops at the limits.
void SignedHalfwordMultiply(Core& core, uint32_t op)
{
    if (!core.v5_)
    {
        core.Undefined();
        return;
    }

    const int rd = Rn(op);
    const int rn = Rd(op);
    const uint32_t m = core.regs_[Rm(op)];
    const int32_t s = Half(core.regs_[Rs(op)], Bit(op, 6));
    const bool x = Bit(op, 5);
    switch ((op >> 21) & 3)
    {
    case 0:
        {
            const int64_t sum = static_cast<int64_t>(Half(m, x) * s) + static_cast<int32_t>(core.regs_[rn]);
            Saturate(core, sum);
            core.regs_[rd] = static_cast<uint32_t>(sum);
            break;
        }

    case 1:
        {
            const int64_t product = (static_cast<int64_t>(static_cast<int32_t>(m)) * s) >> 16;
            if (x)
            {
                core.regs_[rd] = static_cast<uint32_t>(product);
                break;
            }

            const int64_t sum = product + static_cast<int32_t>(core.regs_[rn]);
            Saturate(core, sum);
            core.regs_[rd] = static_cast<uint32_t>(sum);
            break;
        }

    case 2:
        {
            const uint64_t accumulator = (static_cast<uint64_t>(core.regs_[rd]) << 32) | core.regs_[rn];
            const uint64_t sum = accumulator + static_cast<uint64_t>(static_cast<int64_t>(Half(m, x) * s));
            core.regs_[rn] = static_cast<uint32_t>(sum);
            core.regs_[rd] = static_cast<uint32_t>(sum >> 32);
            break;
        }

    default:
        core.regs_[rd] = static_cast<uint32_t>(Half(m, x) * s);
        break;
    }
}

// MUL and MLA. The C flag stays as it was, on both architectures.
template <bool kAccumulate, bool kSetFlags>
void Multiply(Core& core, uint32_t op)
{
    uint32_t result = core.regs_[Rm(op)] * core.regs_[Rs(op)];
    if constexpr (kAccumulate)
    {
        result += core.regs_[Rd(op)];
    }

    core.regs_[Rn(op)] = result;
    if constexpr (kSetFlags)
    {
        core.SetNz(result);
    }
}

// UMULL, UMLAL, SMULL and SMLAL. The C and V flags stay as they were.
template <bool kSigned, bool kAccumulate, bool kSetFlags>
void MultiplyLong(Core& core, uint32_t op)
{
    const int high = Rn(op);
    const int low = Rd(op);
    const uint32_t m = core.regs_[Rm(op)];
    const uint32_t s = core.regs_[Rs(op)];
    uint64_t result =
        kSigned ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(m)) * static_cast<int32_t>(s))
                : static_cast<uint64_t>(m) * s;
    if constexpr (kAccumulate)
    {
        result += (static_cast<uint64_t>(core.regs_[high]) << 32) | core.regs_[low];
    }

    core.regs_[low] = static_cast<uint32_t>(result);
    core.regs_[high] = static_cast<uint32_t>(result >> 32);
    if constexpr (kSetFlags)
    {
        const uint32_t n = static_cast<uint32_t>(result >> 32) & psr::kN;
        core.cpsr_ = (core.cpsr_ & 0x3FFFFFFF) | n | (result == 0 ? psr::kZ : 0);
    }
}

// SWP and SWPB: a load and a store at one address. The word load rotates as LDR does, and the store doesn't.
template <bool kByte>
void Swap(Core& core, uint32_t op)
{
    const uint32_t address = core.regs_[Rn(op)];
    const uint32_t source = core.regs_[Rm(op)];
    if constexpr (kByte)
    {
        const uint32_t value = core.Read8(address);
        core.Write8(address, source);
        core.regs_[Rd(op)] = value;
    }
    else
    {
        const uint32_t value = RotateLoaded(core.Read32(address & ~3u), address);
        core.Write32(address & ~3u, source);
        core.regs_[Rd(op)] = value;
    }
}

// The address of a load or a store, and the base register's new value. Post-indexing always writes the base back.
struct Addressing
{
    uint32_t address;
    uint32_t updated_base;
    bool writeback;
};

Addressing Address(const Core& core, uint32_t op, uint32_t offset)
{
    const uint32_t base = core.regs_[Rn(op)];
    const uint32_t updated = Bit(op, 23) ? base + offset : base - offset;
    const bool pre = Bit(op, 24);

    return {pre ? updated : base, updated, !pre || Bit(op, 21)};
}

// LDR, STR, LDRB and STRB, with an immediate or a shifted register as the offset. A store of r15 stores the
// instruction's address plus 12. The T forms run as the plain ones: nothing here checks permissions.
template <bool kLoad, bool kByte, bool kRegisterOffset>
void SingleTransfer(Core& core, uint32_t op)
{
    uint32_t offset = op & 0xFFF;
    if constexpr (kRegisterOffset)
    {
        uint32_t carry = core.Carry();
        offset = ShiftByImmediate(core.regs_[Rm(op)], (op >> 5) & 3, (op >> 7) & 31, carry);
    }

    const Addressing at = Address(core, op, offset);
    const int rd = Rd(op);
    if constexpr (kLoad)
    {
        const uint32_t value = kByte ? core.Read8(at.address) : RotateLoaded(core.Read32(at.address & ~3u), at.address);
        if (at.writeback)
        {
            core.regs_[Rn(op)] = at.updated_base;
        }

        if (rd == 15)
        {
            core.LoadPc(value);
        }
        else
        {
            core.regs_[rd] = value;
        }
    }
    else
    {
        const uint32_t value = rd == 15 ? core.regs_[15] + 4 : core.regs_[rd];
        if constexpr (kByte)
        {
            core.Write8(at.address, value);
        }
        else
        {
            core.Write32(at.address & ~3u, value);
        }

        if (at.writeback)
        {
            core.regs_[Rn(op)] = at.updated_base;
        }
    }
}

// The halfword, signed and doubleword transfers: STRH, LDRH, LDRSB, LDRSH, and on ARMv5TE LDRD and STRD. `kKind` is
// bits 6-5 of the instruction.
template <bool kLoad, uint32_t kKind>
void HalfwordTransfer(Core& core, uint32_t op)
{
    constexpr bool kDoubleword = !kLoad && kKind != 1;
    const int rd = Rd(op);
    if constexpr (kDoubleword)
    {
        if (!core.v5_ || (rd & 1) != 0)
        {
            core.Undefined();
            return;
        }
    }

    const uint32_t offset = Bit(op, 22) ? ((op >> 4) & 0xF0) | (op & 0xF) : core.regs_[Rm(op)];
    const Addressing at = Address(core, op, offset);
    if constexpr (kLoad)
    {
        uint32_t value;
        if constexpr (kKind == 1)
        {
            value = core.ReadHalfword(at.address);
        }
        else if constexpr (kKind == 2)
        {
            value = static_cast<uint32_t>(static_cast<int8_t>(core.Read8(at.address)));
        }
        else
        {
            value = core.ReadSignedHalfword(at.address);
        }

        if (at.writeback)
        {
            core.regs_[Rn(op)] = at.updated_base;
        }

        if (rd == 15)
        {
            core.LoadPc(value);
        }
        else
        {
            core.regs_[rd] = value;
        }
    }
    else if constexpr (kKind == 1)
    {
        const uint32_t value = rd == 15 ? core.regs_[15] + 4 : core.regs_[rd];
        core.Write16(at.address & ~1u, value & 0xFFFF);
        if (at.writeback)
        {
            core.regs_[Rn(op)] = at.updated_base;
        }
    }
    else if constexpr (kKind == 2)
    {
        // LDRD. The DS takes a word-aligned address.
        const uint32_t address = at.address & ~3u;
        const uint32_t low = core.Read32(address);
        const uint32_t high = core.Read32(address + 4);
        if (at.writeback)
        {
            core.regs_[Rn(op)] = at.updated_base;
        }

        core.regs_[rd] = low;
        if (rd + 1 == 15)
        {
            core.LoadPc(high);
        }
        else
        {
            core.regs_[rd + 1] = high;
        }
    }
    else
    {
        // STRD.
        const uint32_t address = at.address & ~3u;
        core.Write32(address, core.regs_[rd]);
        core.Write32(address + 4, rd + 1 == 15 ? core.regs_[15] + 4 : core.regs_[rd + 1]);
        if (at.writeback)
        {
            core.regs_[Rn(op)] = at.updated_base;
        }
    }
}

// LDM and STM. GBATEK gives what the DS's CPUs do with an empty list and with the base in the list.
template <bool kLoad>
void BlockTransfer(Core& core, uint32_t op)
{
    const int rn = Rn(op);
    const bool pre = Bit(op, 24);
    const bool up = Bit(op, 23);
    const bool user_bank = Bit(op, 22) && (!kLoad || !Bit(op, 15));
    const bool writeback = Bit(op, 21);
    uint32_t list = op & 0xFFFF;

    // An empty list moves the base by 0x40. ARMv4T then transfers r15 alone, at the place where a list of 16 registers
    // would start.
    const uint32_t base = core.regs_[rn];
    const uint32_t bytes = list == 0 ? 0x40 : static_cast<uint32_t>(std::popcount(list)) * 4;
    if (list == 0 && !core.v5_)
    {
        list = 1u << 15;
    }

    const uint32_t updated_base = up ? base + bytes : base - bytes;
    uint32_t address = (up ? base : updated_base) + (pre == up ? 4 : 0);
    const bool base_in_list = ((list >> rn) & 1) != 0;

    if constexpr (!kLoad)
    {
        // ARMv4T stores the old base when it's the first register of the list, and the new one otherwise. ARMv5TE
        // always stores the old base.
        const bool first = (list & ((1u << rn) - 1)) == 0;
        for (int i = 0; i < 16; ++i)
        {
            if (((list >> i) & 1) == 0)
            {
                continue;
            }

            uint32_t value = user_bank ? core.UserRegister(i) : core.regs_[i];
            if (i == 15)
            {
                value = core.regs_[15] + 4;
            }
            else if (i == rn && writeback && !core.v5_ && !first)
            {
                value = updated_base;
            }

            core.Write32(address & ~3u, value);
            address += 4;
        }

        if (writeback)
        {
            core.regs_[rn] = updated_base;
        }

        return;
    }

    uint32_t pc = 0;
    for (int i = 0; i < 16; ++i)
    {
        if (((list >> i) & 1) == 0)
        {
            continue;
        }

        const uint32_t value = core.Read32(address & ~3u);
        address += 4;
        if (i == 15)
        {
            pc = value;
        }
        else if (user_bank)
        {
            core.SetUserRegister(i, value);
        }
        else
        {
            core.regs_[i] = value;
        }
    }

    // With the base in the list, ARMv4T keeps the loaded value. ARMv5TE writes the new base back when the base is the
    // only register or not the last one.
    if (writeback)
    {
        const bool last = (list >> rn) == 1;
        if (!base_in_list || (core.v5_ && (list == (1u << rn) || !last)))
        {
            core.regs_[rn] = updated_base;
        }
    }

    if ((list & 0x8000) != 0)
    {
        if (Bit(op, 22))
        {
            core.RestoreCpsr();
            core.BranchInState(pc);
        }
        else
        {
            core.LoadPc(pc);
        }
    }
}

// B and BL.
template <bool kLink>
void Branch(Core& core, uint32_t op)
{
    const uint32_t offset = static_cast<uint32_t>(static_cast<int32_t>(op << 8) >> 6);
    if constexpr (kLink)
    {
        core.regs_[14] = core.regs_[15] - 4;
    }

    core.next_pc_ = core.regs_[15] + offset;
}

void SoftwareInterruptInstruction(Core& core, uint32_t op)
{
    core.SoftwareInterrupt(op & 0xFFFFFF);
}

// MCR and MRC. Only the ARM946E-S has a coprocessor: CP15. An MRC to r15 sets the flags from the value's top bits.
// An MCR of r15 passes the instruction's address plus 12.
void CoprocessorTransfer(Core& core, uint32_t op)
{
    if (!core.v5_ || ((op >> 8) & 15) != 15)
    {
        core.Undefined();
        return;
    }

    const uint32_t opcode1 = (op >> 21) & 7;
    const uint32_t crn = (op >> 16) & 15;
    const uint32_t crm = op & 15;
    const uint32_t opcode2 = (op >> 5) & 7;
    const int rd = Rd(op);
    if (Bit(op, 20))
    {
        uint32_t value = 0;
        if (!core.ReadCp15(opcode1, crn, crm, opcode2, value))
        {
            core.Undefined();
        }
        else if (rd == 15)
        {
            core.cpsr_ = (core.cpsr_ & 0x0FFFFFFF) | (value & 0xF0000000);
        }
        else
        {
            core.regs_[rd] = value;
        }

        return;
    }

    if (!core.WriteCp15(opcode1, crn, crm, opcode2, rd == 15 ? core.regs_[15] + 4 : core.regs_[rd]))
    {
        core.Undefined();
    }
}

// The instructions in the space of TST, TEQ, CMP and CMN without the S bit: bits 27-23 are 00010 and bit 20 is 0.
Handler MiscellaneousHandler(uint32_t high, uint32_t low)
{
    const uint32_t opcode = (high >> 1) & 3;
    const bool spsr = (high & 4) != 0;
    switch (low)
    {
    case 0x0:
        if ((high & 2) != 0)
        {
            return spsr ? &MoveToPsr<true, false> : &MoveToPsr<false, false>;
        }

        return spsr ? &MoveFromPsr<true> : &MoveFromPsr<false>;

    case 0x1:
        if (opcode == 1)
        {
            return &BranchAndExchange;
        }

        return opcode == 3 ? &CountLeadingZeros : &UndefinedInstruction;

    case 0x3:
        return opcode == 1 ? &BranchLinkAndExchange : &UndefinedInstruction;

    case 0x5:
        return &SaturatingArithmetic;

    case 0x7:
        return opcode == 1 ? &BreakpointInstruction : &UndefinedInstruction;

    case 0x8:
    case 0xA:
    case 0xC:
    case 0xE:
        return &SignedHalfwordMultiply;

    default:
        return &UndefinedInstruction;
    }
}

Handler MultiplyHandler(uint32_t high)
{
    constexpr std::array<Handler, 4> kShort = {&Multiply<false, false>, &Multiply<false, true>, &Multiply<true, false>,
                                               &Multiply<true, true>};
    constexpr std::array<Handler, 8> kLong = {&MultiplyLong<false, false, false>, &MultiplyLong<false, false, true>,
                                              &MultiplyLong<false, true, false>,  &MultiplyLong<false, true, true>,
                                              &MultiplyLong<true, false, false>,  &MultiplyLong<true, false, true>,
                                              &MultiplyLong<true, true, false>,   &MultiplyLong<true, true, true>};

    // Bits 27-22 are 000000 for MUL and MLA, bits 27-23 are 00001 for the long forms, and SWP has bits 27-23 of 00010
    // with bits 21-20 of 00.
    if ((high & 0xFC) == 0x00)
    {
        return kShort[high & 3];
    }

    if ((high & 0xF8) == 0x08)
    {
        return kLong[high & 7];
    }

    if ((high & 0xFB) == 0x10)
    {
        return (high & 4) != 0 ? &Swap<true> : &Swap<false>;
    }

    return &UndefinedInstruction;
}

Handler HalfwordHandler(uint32_t high, uint32_t low)
{
    constexpr std::array<Handler, 4> kStores = {&UndefinedInstruction, &HalfwordTransfer<false, 1>,
                                                &HalfwordTransfer<false, 2>, &HalfwordTransfer<false, 3>};
    constexpr std::array<Handler, 4> kLoads = {&UndefinedInstruction, &HalfwordTransfer<true, 1>,
                                               &HalfwordTransfer<true, 2>, &HalfwordTransfer<true, 3>};
    const uint32_t kind = (low >> 1) & 3;

    return (high & 1) != 0 ? kLoads[kind] : kStores[kind];
}

Handler SingleTransferHandler(uint32_t high, bool register_offset)
{
    constexpr std::array<Handler, 8> kForms = {
        &SingleTransfer<false, false, false>, &SingleTransfer<false, false, true>, &SingleTransfer<false, true, false>,
        &SingleTransfer<false, true, true>,   &SingleTransfer<true, false, false>, &SingleTransfer<true, false, true>,
        &SingleTransfer<true, true, false>,   &SingleTransfer<true, true, true>};
    const uint32_t load = high & 1;
    const uint32_t byte = (high >> 2) & 1;

    return kForms[(load << 2) | (byte << 1) | (register_offset ? 1 : 0)];
}

// The handler of the instructions with bits 27-20 of `high` and bits 7-4 of `low`.
Handler ArmHandler(uint32_t high, uint32_t low)
{
    const uint32_t opcode = (high >> 1) & 15;
    const uint32_t set_flags = high & 1;
    const bool miscellaneous = (high & 0x19) == 0x10;
    switch (high >> 5)
    {
    case 0:
        if ((low & 9) == 9)
        {
            return low == 9 ? MultiplyHandler(high) : HalfwordHandler(high, low);
        }

        if (miscellaneous)
        {
            return MiscellaneousHandler(high, low);
        }

        return kDataProcessing[opcode][set_flags * 3 + ((low & 1) != 0 ? 2 : 1)];

    case 1:
        if (miscellaneous)
        {
            // MSR with an immediate. The same space without bit 21 is undefined.
            if ((high & 2) == 0)
            {
                return &UndefinedInstruction;
            }

            return (high & 4) != 0 ? &MoveToPsr<true, true> : &MoveToPsr<false, true>;
        }

        return kDataProcessing[opcode][set_flags * 3];

    case 2:
        return SingleTransferHandler(high, false);

    case 3:
        return (low & 1) != 0 ? &UndefinedInstruction : SingleTransferHandler(high, true);

    case 4:
        return (high & 1) != 0 ? &BlockTransfer<true> : &BlockTransfer<false>;

    case 5:
        return (high & 0x10) != 0 ? &Branch<true> : &Branch<false>;

    case 6:
        // LDC and STC, and ARMv5TE's MCRR and MRRC: no coprocessor takes them.
        return &UndefinedInstruction;

    default:
        if ((high & 0x10) != 0)
        {
            return &SoftwareInterruptInstruction;
        }

        return (low & 1) != 0 ? &CoprocessorTransfer : &UndefinedInstruction;
    }
}

std::array<Handler, 4096> BuildArmHandlers()
{
    std::array<Handler, 4096> handlers{};
    for (uint32_t index = 0; index < handlers.size(); ++index)
    {
        handlers[index] = ArmHandler(index >> 4, index & 15);
    }

    return handlers;
}

} // namespace

const std::array<Handler, 4096>& ArmHandlers()
{
    static const std::array<Handler, 4096> kHandlers = BuildArmHandlers();

    return kHandlers;
}

void RunUnconditional(Core& core, uint32_t op)
{
    if (!core.v5_)
    {
        core.Undefined();
        return;
    }

    // BLX with an immediate: bit 24 adds a halfword to the target, and the target runs in Thumb state.
    if ((op & 0x0E000000) == 0x0A000000)
    {
        const uint32_t offset = static_cast<uint32_t>(static_cast<int32_t>(op << 8) >> 6) | ((op >> 23) & 2);
        core.regs_[14] = core.regs_[15] - 4;
        core.BranchExchange((core.regs_[15] + offset) | 1);
        return;
    }

    // PLD does nothing here. Every other instruction of this space is a coprocessor's, and no coprocessor takes them.
    if ((op & 0x0D70F000) != 0x0550F000)
    {
        core.Undefined();
    }
}

} // namespace ntdlib::arm::detail
