// SPDX-License-Identifier: MIT

// The Thumb instruction set of ARMv4T and ARMv5TE, and its decoding table. The formats are numbered as in GBATEK's
// "THUMB Instruction Summary". While a Thumb instruction runs, r15 holds its address plus 4.

#include <array>
#include <bit>
#include <cstdint>

#include "arm/cpu_impl.h"

namespace ntdlib::arm::detail
{
namespace
{

int Low3(uint32_t op, int bit)
{
    return static_cast<int>((op >> bit) & 7);
}

uint32_t Offset5(uint32_t op)
{
    return (op >> 6) & 31;
}

void UndefinedThumb(Core& core, uint32_t /*op*/)
{
    core.Undefined();
}

// Format 1: LSL, LSR and ASR by an immediate.
template <uint32_t kType>
void ShiftImmediate(Core& core, uint32_t op)
{
    uint32_t carry = core.Carry();
    const uint32_t result = ShiftByImmediate(core.regs_[Low3(op, 3)], kType, Offset5(op), carry);
    core.regs_[Low3(op, 0)] = result;
    core.SetNzc(result, carry);
}

// Format 2: ADD and SUB of a register or a 3-bit immediate.
template <uint32_t kOp>
void AddSubtract(Core& core, uint32_t op)
{
    const uint32_t first = core.regs_[Low3(op, 3)];
    const uint32_t second = (kOp & 2) != 0 ? Low3(op, 6) : core.regs_[Low3(op, 6)];
    uint32_t carry = 0;
    uint32_t overflow = 0;
    const uint32_t result = (kOp & 1) != 0 ? AddWithCarry(first, ~second, 1, carry, overflow)
                                           : AddWithCarry(first, second, 0, carry, overflow);
    core.regs_[Low3(op, 0)] = result;
    core.SetNzcv(result, carry, overflow);
}

// Format 3: MOV, CMP, ADD and SUB of an 8-bit immediate.
template <uint32_t kOp>
void Immediate(Core& core, uint32_t op)
{
    const int rd = Low3(op, 8);
    const uint32_t value = op & 0xFF;
    if constexpr (kOp == 0)
    {
        core.regs_[rd] = value;
        core.SetNz(value);
        return;
    }

    uint32_t carry = 0;
    uint32_t overflow = 0;
    const uint32_t result = kOp == 2 ? AddWithCarry(core.regs_[rd], value, 0, carry, overflow)
                                     : AddWithCarry(core.regs_[rd], ~value, 1, carry, overflow);
    if constexpr (kOp != 1)
    {
        core.regs_[rd] = result;
    }

    core.SetNzcv(result, carry, overflow);
}

// Format 4: the ALU operations on two low registers.
template <uint32_t kOp>
void Alu(Core& core, uint32_t op)
{
    const int rd = Low3(op, 0);
    const uint32_t a = core.regs_[rd];
    const uint32_t b = core.regs_[Low3(op, 3)];
    uint32_t carry = core.Carry();
    uint32_t overflow = 0;
    uint32_t result = 0;
    if constexpr (kOp == 2 || kOp == 3 || kOp == 4 || kOp == 7)
    {
        constexpr uint32_t kType = kOp == 2 ? 0 : kOp == 3 ? 1 : kOp == 4 ? 2 : 3;
        result = ShiftByRegister(a, kType, b & 0xFF, carry);
        core.regs_[rd] = result;
        core.SetNzc(result, carry);
        return;
    }
    else if constexpr (kOp == 5 || kOp == 6 || kOp == 9 || kOp == 10 || kOp == 11)
    {
        if constexpr (kOp == 5)
        {
            result = AddWithCarry(a, b, core.Carry(), carry, overflow);
        }
        else if constexpr (kOp == 6)
        {
            result = AddWithCarry(a, ~b, core.Carry(), carry, overflow);
        }
        else if constexpr (kOp == 9)
        {
            result = AddWithCarry(0, ~b, 1, carry, overflow);
        }
        else if constexpr (kOp == 10)
        {
            result = AddWithCarry(a, ~b, 1, carry, overflow);
        }
        else
        {
            result = AddWithCarry(a, b, 0, carry, overflow);
        }

        if constexpr (kOp != 10 && kOp != 11)
        {
            core.regs_[rd] = result;
        }

        core.SetNzcv(result, carry, overflow);
        return;
    }
    else
    {
        // The logical operations and MUL set N and Z, and leave C and V.
        if constexpr (kOp == 0 || kOp == 8)
        {
            result = a & b;
        }
        else if constexpr (kOp == 1)
        {
            result = a ^ b;
        }
        else if constexpr (kOp == 12)
        {
            result = a | b;
        }
        else if constexpr (kOp == 13)
        {
            result = a * b;
        }
        else if constexpr (kOp == 14)
        {
            result = a & ~b;
        }
        else
        {
            result = ~b;
        }

        if constexpr (kOp != 8)
        {
            core.regs_[rd] = result;
        }

        core.SetNz(result);
    }
}

// Format 5: ADD, CMP and MOV on any registers, BX, and on ARMv5TE BLX with a register. Only CMP sets the flags. ADD
// and MOV to r15 branch in Thumb state.
template <uint32_t kOp>
void HighRegister(Core& core, uint32_t op)
{
    const int rd = Low3(op, 0) | static_cast<int>((op >> 4) & 8);
    const int rs = static_cast<int>((op >> 3) & 15);
    if constexpr (kOp == 1)
    {
        uint32_t carry = 0;
        uint32_t overflow = 0;
        const uint32_t result = AddWithCarry(core.regs_[rd], ~core.regs_[rs], 1, carry, overflow);
        core.SetNzcv(result, carry, overflow);
    }
    else if constexpr (kOp == 3)
    {
        const uint32_t target = core.regs_[rs];
        if ((op & 0x80) != 0)
        {
            if (!core.v5_)
            {
                core.Undefined();
                return;
            }

            core.regs_[14] = (core.regs_[15] - 2) | 1;
        }

        core.BranchExchange(target);
    }
    else
    {
        const uint32_t result = kOp == 0 ? core.regs_[rd] + core.regs_[rs] : core.regs_[rs];
        if (rd == 15)
        {
            core.BranchInState(result);
        }
        else
        {
            core.regs_[rd] = result;
        }
    }
}

// Format 6: LDR from the PC with its bit 1 cleared, plus a word offset.
void LoadPcRelative(Core& core, uint32_t op)
{
    core.regs_[Low3(op, 8)] = core.Read32((core.regs_[15] & ~2u) + (op & 0xFF) * 4);
}

uint32_t LoadWord(Core& core, uint32_t address)
{
    return RotateLoaded(core.Read32(address & ~3u), address);
}

// Format 7: STR, STRB, LDR and LDRB with a register offset.
template <uint32_t kOp>
void TransferRegisterOffset(Core& core, uint32_t op)
{
    const uint32_t address = core.regs_[Low3(op, 3)] + core.regs_[Low3(op, 6)];
    const int rd = Low3(op, 0);
    if constexpr (kOp == 0)
    {
        core.Write32(address & ~3u, core.regs_[rd]);
    }
    else if constexpr (kOp == 1)
    {
        core.Write8(address, core.regs_[rd]);
    }
    else if constexpr (kOp == 2)
    {
        core.regs_[rd] = LoadWord(core, address);
    }
    else
    {
        core.regs_[rd] = core.Read8(address);
    }
}

// Format 8: STRH, LDSB, LDRH and LDSH with a register offset.
template <uint32_t kOp>
void TransferSigned(Core& core, uint32_t op)
{
    const uint32_t address = core.regs_[Low3(op, 3)] + core.regs_[Low3(op, 6)];
    const int rd = Low3(op, 0);
    if constexpr (kOp == 0)
    {
        core.Write16(address & ~1u, core.regs_[rd] & 0xFFFF);
    }
    else if constexpr (kOp == 1)
    {
        core.regs_[rd] = static_cast<uint32_t>(static_cast<int8_t>(core.Read8(address)));
    }
    else if constexpr (kOp == 2)
    {
        core.regs_[rd] = core.ReadHalfword(address);
    }
    else
    {
        core.regs_[rd] = core.ReadSignedHalfword(address);
    }
}

// Format 9: STR, LDR, STRB and LDRB with an immediate offset, in words for STR and LDR.
template <uint32_t kOp>
void TransferImmediate(Core& core, uint32_t op)
{
    const uint32_t base = core.regs_[Low3(op, 3)];
    const int rd = Low3(op, 0);
    if constexpr (kOp == 0)
    {
        core.Write32((base + Offset5(op) * 4) & ~3u, core.regs_[rd]);
    }
    else if constexpr (kOp == 1)
    {
        core.regs_[rd] = LoadWord(core, base + Offset5(op) * 4);
    }
    else if constexpr (kOp == 2)
    {
        core.Write8(base + Offset5(op), core.regs_[rd]);
    }
    else
    {
        core.regs_[rd] = core.Read8(base + Offset5(op));
    }
}

// Format 10: STRH and LDRH with an immediate offset in halfwords.
template <bool kLoad>
void TransferHalfword(Core& core, uint32_t op)
{
    const uint32_t address = core.regs_[Low3(op, 3)] + Offset5(op) * 2;
    const int rd = Low3(op, 0);
    if constexpr (kLoad)
    {
        core.regs_[rd] = core.ReadHalfword(address);
    }
    else
    {
        core.Write16(address & ~1u, core.regs_[rd] & 0xFFFF);
    }
}

// Format 11: STR and LDR relative to SP.
template <bool kLoad>
void TransferStack(Core& core, uint32_t op)
{
    const uint32_t address = core.regs_[13] + (op & 0xFF) * 4;
    const int rd = Low3(op, 8);
    if constexpr (kLoad)
    {
        core.regs_[rd] = LoadWord(core, address);
    }
    else
    {
        core.Write32(address & ~3u, core.regs_[rd]);
    }
}

// Format 12: ADD of a word offset to the PC (with bit 1 cleared) or to SP.
template <bool kStack>
void AddressOf(Core& core, uint32_t op)
{
    const uint32_t base = kStack ? core.regs_[13] : core.regs_[15] & ~2u;
    core.regs_[Low3(op, 8)] = base + (op & 0xFF) * 4;
}

// Format 13: ADD or subtract a word offset to SP.
void AdjustStack(Core& core, uint32_t op)
{
    const uint32_t offset = (op & 0x7F) * 4;
    core.regs_[13] = (op & 0x80) != 0 ? core.regs_[13] - offset : core.regs_[13] + offset;
}

// Format 14: PUSH and POP, with LR and PC. An empty list moves SP by 0x40 and, on ARMv4T, transfers r15 alone. ARM's
// LDM and STM do the same. POP {PC} switches to ARM state by bit 0 on ARMv5TE only.
template <bool kPop>
void PushPop(Core& core, uint32_t op)
{
    const bool extra = (op & 0x100) != 0;
    uint32_t list = op & 0xFF;
    const bool empty = list == 0 && !extra;
    const uint32_t count = static_cast<uint32_t>(std::popcount(list)) + (extra ? 1 : 0);
    const uint32_t bytes = empty ? 0x40 : count * 4;
    const bool transfer_pc = (kPop && extra) || (empty && !core.v5_);
    if constexpr (kPop)
    {
        uint32_t address = core.regs_[13];
        for (int i = 0; i < 8; ++i)
        {
            if (((list >> i) & 1) != 0)
            {
                core.regs_[i] = core.Read32(address & ~3u);
                address += 4;
            }
        }

        const uint32_t pc = transfer_pc ? core.Read32(address & ~3u) : 0;
        core.regs_[13] += bytes;
        if (transfer_pc)
        {
            core.LoadPc(pc);
        }

        return;
    }

    uint32_t address = core.regs_[13] - bytes;
    core.regs_[13] = address;
    for (int i = 0; i < 8; ++i)
    {
        if (((list >> i) & 1) != 0)
        {
            core.Write32(address & ~3u, core.regs_[i]);
            address += 4;
        }
    }

    if (extra)
    {
        core.Write32(address & ~3u, core.regs_[14]);
    }
    else if (transfer_pc)
    {
        core.Write32(address & ~3u, core.regs_[15] + 2);
    }
}

// Format 15: STMIA and LDMIA with writeback. An empty list moves the base by 0x40 and, on ARMv4T, transfers r15
// alone. With the base in the list, LDMIA keeps the loaded value on both architectures. STMIA stores the old base
// when it's the first register of the list or on ARMv5TE, and the new base otherwise.
template <bool kLoad>
void BlockTransfer(Core& core, uint32_t op)
{
    const int rb = Low3(op, 8);
    const uint32_t list = op & 0xFF;
    const uint32_t base = core.regs_[rb];
    const uint32_t updated_base = base + (list == 0 ? 0x40 : static_cast<uint32_t>(std::popcount(list)) * 4);
    uint32_t address = base;
    if (list == 0)
    {
        if (!core.v5_)
        {
            if constexpr (kLoad)
            {
                const uint32_t pc = core.Read32(address & ~3u);
                core.regs_[rb] = updated_base;
                core.LoadPc(pc);
                return;
            }

            core.Write32(address & ~3u, core.regs_[15] + 2);
        }

        core.regs_[rb] = updated_base;
        return;
    }

    const bool first = (list & ((1u << rb) - 1)) == 0;
    for (int i = 0; i < 8; ++i)
    {
        if (((list >> i) & 1) == 0)
        {
            continue;
        }

        if constexpr (kLoad)
        {
            core.regs_[i] = core.Read32(address & ~3u);
        }
        else
        {
            const bool new_base = i == rb && !core.v5_ && !first;
            core.Write32(address & ~3u, new_base ? updated_base : core.regs_[i]);
        }

        address += 4;
    }

    if (!kLoad || ((list >> rb) & 1) == 0)
    {
        core.regs_[rb] = updated_base;
    }
}

// Format 16: a conditional branch. Condition 1110b is undefined, and 1111b is SWI (format 17).
void ConditionalBranch(Core& core, uint32_t op)
{
    if (core.ConditionPassed((op >> 8) & 15))
    {
        core.next_pc_ = core.regs_[15] + static_cast<uint32_t>(static_cast<int8_t>(op & 0xFF) * 2);
    }
}

// Format 17: SWI.
void SoftwareInterruptThumb(Core& core, uint32_t op)
{
    core.SoftwareInterrupt(op & 0xFF);
}

// ARMv5TE: BKPT.
void BreakpointThumb(Core& core, uint32_t /*op*/)
{
    if (!core.v5_)
    {
        core.Undefined();
        return;
    }

    core.Breakpoint();
}

// Format 18: an unconditional branch.
void Branch(Core& core, uint32_t op)
{
    const int32_t offset = static_cast<int32_t>(op << 21) >> 20;
    core.next_pc_ = core.regs_[15] + static_cast<uint32_t>(offset);
}

// Format 19, first half: LR = PC plus the high part of the offset.
void BranchLinkPrefix(Core& core, uint32_t op)
{
    const int32_t offset = static_cast<int32_t>(op << 21) >> 9;
    core.regs_[14] = core.regs_[15] + static_cast<uint32_t>(offset);
}

// Format 19, second half: BL, or on ARMv5TE BLX to ARM state. LR gets the next instruction's address with bit 0 set.
template <bool kExchange>
void BranchLinkSuffix(Core& core, uint32_t op)
{
    if constexpr (kExchange)
    {
        if (!core.v5_ || (op & 1) != 0)
        {
            core.Undefined();
            return;
        }
    }

    const uint32_t target = core.regs_[14] + (op & 0x7FF) * 2;
    core.regs_[14] = (core.regs_[15] - 2) | 1;
    if constexpr (kExchange)
    {
        core.BranchExchange(target & ~3u);
    }
    else
    {
        core.next_pc_ = target & ~1u;
    }
}

// The instructions with bits 15-8 of 1011xxxx: format 13, format 14 and BKPT.
Handler MiscellaneousHandler(uint32_t bits11_8)
{
    switch (bits11_8)
    {
    case 0x0:
        return &AdjustStack;
    case 0x4:
    case 0x5:
        return &PushPop<false>;
    case 0xC:
    case 0xD:
        return &PushPop<true>;
    case 0xE:
        return &BreakpointThumb;
    default:
        return &UndefinedThumb;
    }
}

// The handler of the instructions with bits 15-6 of `index`.
Handler ThumbHandler(uint32_t index)
{
    constexpr std::array<Handler, 4> kAddSubtract = {&AddSubtract<0>, &AddSubtract<1>, &AddSubtract<2>,
                                                     &AddSubtract<3>};
    constexpr std::array<Handler, 4> kImmediate = {&Immediate<0>, &Immediate<1>, &Immediate<2>, &Immediate<3>};
    constexpr std::array<Handler, 16> kAlu = {&Alu<0>,  &Alu<1>,  &Alu<2>,  &Alu<3>, &Alu<4>,  &Alu<5>,
                                              &Alu<6>,  &Alu<7>,  &Alu<8>,  &Alu<9>, &Alu<10>, &Alu<11>,
                                              &Alu<12>, &Alu<13>, &Alu<14>, &Alu<15>};
    constexpr std::array<Handler, 4> kHighRegister = {&HighRegister<0>, &HighRegister<1>, &HighRegister<2>,
                                                      &HighRegister<3>};
    constexpr std::array<Handler, 4> kRegisterOffset = {&TransferRegisterOffset<0>, &TransferRegisterOffset<1>,
                                                        &TransferRegisterOffset<2>, &TransferRegisterOffset<3>};
    constexpr std::array<Handler, 4> kSigned = {&TransferSigned<0>, &TransferSigned<1>, &TransferSigned<2>,
                                                &TransferSigned<3>};
    constexpr std::array<Handler, 4> kImmediateOffset = {&TransferImmediate<0>, &TransferImmediate<1>,
                                                         &TransferImmediate<2>, &TransferImmediate<3>};

    const uint32_t top = index >> 5;
    switch (top)
    {
    case 0x00:
        return &ShiftImmediate<0>;

    case 0x01:
        return &ShiftImmediate<1>;

    case 0x02:
        return &ShiftImmediate<2>;

    case 0x03:
        return kAddSubtract[(index >> 3) & 3];

    case 0x04:
    case 0x05:
    case 0x06:
    case 0x07:
        return kImmediate[top & 3];

    case 0x08:
        return (index & 0x10) != 0 ? kHighRegister[(index >> 2) & 3] : kAlu[index & 15];

    case 0x09:
        return &LoadPcRelative;

    case 0x0A:
    case 0x0B:
        return (index & 8) != 0 ? kSigned[(index >> 4) & 3] : kRegisterOffset[(index >> 4) & 3];

    case 0x0C:
    case 0x0D:
    case 0x0E:
    case 0x0F:
        return kImmediateOffset[top & 3];

    case 0x10:
    case 0x11:
        return (top & 1) != 0 ? &TransferHalfword<true> : &TransferHalfword<false>;

    case 0x12:
    case 0x13:
        return (top & 1) != 0 ? &TransferStack<true> : &TransferStack<false>;

    case 0x14:
    case 0x15:
        return (top & 1) != 0 ? &AddressOf<true> : &AddressOf<false>;

    case 0x16:
    case 0x17:
        return MiscellaneousHandler((index >> 2) & 15);

    case 0x18:
    case 0x19:
        return (top & 1) != 0 ? &BlockTransfer<true> : &BlockTransfer<false>;

    case 0x1A:
    case 0x1B:
        {
            const uint32_t cond = (index >> 2) & 15;
            if (cond == 0xE)
            {
                return &UndefinedThumb;
            }

            return cond == 0xF ? &SoftwareInterruptThumb : &ConditionalBranch;
        }

    case 0x1C:
        return &Branch;

    case 0x1D:
        return &BranchLinkSuffix<true>;

    case 0x1E:
        return &BranchLinkPrefix;

    default:
        return &BranchLinkSuffix<false>;
    }
}

std::array<Handler, 1024> BuildThumbHandlers()
{
    std::array<Handler, 1024> handlers{};
    for (uint32_t index = 0; index < handlers.size(); ++index)
    {
        handlers[index] = ThumbHandler(index);
    }

    return handlers;
}

} // namespace

const std::array<Handler, 1024>& ThumbHandlers()
{
    static const std::array<Handler, 1024> kHandlers = BuildThumbHandlers();

    return kHandlers;
}

} // namespace ntdlib::arm::detail
