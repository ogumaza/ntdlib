// SPDX-License-Identifier: MIT

#include "ntdlib/arm/routine.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/arm/alu.h"
#include "ntdlib/arm/branch.h"

namespace ntdlib::arm
{

namespace
{

constexpr uint32_t kSp = 13;
constexpr uint32_t kLr = 14;
constexpr uint32_t kPc = 15;
constexpr uint32_t kExtension = 0xF; // the condition field of the instructions that always run, such as blx

// The data-processing opcodes that the reader follows.
constexpr uint32_t kSub = 0x2;
constexpr uint32_t kAdd = 0x4;
constexpr uint32_t kCmp = 0xA;
constexpr uint32_t kMov = 0xD;
constexpr uint32_t kMvn = 0xF;

// The instructions that save lr at a routine's start: stmfd sp!, {..., lr} and str lr, [sp, #-4]!.
bool SavesLr(uint32_t instruction)
{
    return (instruction & 0xFFFF4000) == 0xE92D4000 || instruction == 0xE52DE004;
}

// The returns: bx lr, mov pc, lr, ldmfd sp!, {..., pc} and ldr pc, [sp], #4, under any condition.
bool IsReturn(uint32_t instruction)
{
    const uint32_t operation = instruction & 0x0FFFFFFF;
    return operation == 0x012FFF1E || operation == 0x01A0F00E || (operation & 0x0FFF8000) == 0x08BD8000 ||
           operation == 0x049DF004;
}

uint32_t Field(uint32_t instruction, int shift)
{
    return (instruction >> shift) & 0xF;
}

// The Thumb instruction that starts a routine by saving lr: push {..., lr}.
bool ThumbSavesLr(uint32_t halfword)
{
    return (halfword & 0xFF00) == 0xB500;
}

// The count of registers in the list of a block transfer.
int32_t Registers(uint32_t list)
{
    return static_cast<int32_t>(std::popcount(list & 0xFFFF));
}

// The first argument that the stack holds: r0 to r3 come first.
constexpr uint32_t kFirstStackArgument = 4;

class Reader
{
public:
    Reader(std::span<const uint8_t> code, uint32_t base, uint32_t address) : code_(code), base_(base)
    {
        routine_.address = address;
        for (uint32_t reg = 0; reg < 4; reg++)
        {
            registers_[reg] = Value::Argument(reg);
        }
    }

    Routine Read(std::size_t limit)
    {
        if ((routine_.address & 1) != 0)
        {
            return ReadThumb(limit);
        }

        for (std::size_t i = 0; i < limit; i++)
        {
            const uint32_t address = routine_.address + static_cast<uint32_t>(4 * i);
            const std::optional<uint32_t> instruction = Word(address);
            if (!instruction || (i > 0 && SavesLr(*instruction)) || !Instruction(address, *instruction))
            {
                break;
            }
        }

        return std::move(routine_);
    }

private:
    Routine ReadThumb(std::size_t limit)
    {
        uint32_t address = routine_.address & ~1u;
        for (std::size_t i = 0; i < limit; i++)
        {
            const std::optional<uint32_t> instruction = Half(address);
            if (!instruction || (i > 0 && ThumbSavesLr(*instruction)))
            {
                break;
            }

            // bl and blx take two halfwords: the high part of the offset, then the low part.
            if ((*instruction & 0xF800) == 0xF000)
            {
                const std::optional<uint32_t> low = Half(address + 2);
                if (low && ((*low & 0xF800) == 0xF800 || (*low & 0xF801) == 0xE800))
                {
                    StartThumb(address);
                    const int32_t high = static_cast<int32_t>(*instruction << 21) >> 9;
                    const uint32_t target = address + 4 + static_cast<uint32_t>(high) + ((*low & 0x7FF) << 1);
                    Call((*low & 0xF800) == 0xF800 ? target | 1 : target & ~3u);
                    address += 4;
                    continue;
                }
            }

            if (!ThumbInstruction(address, *instruction))
            {
                break;
            }
            address += 2;
        }

        return std::move(routine_);
    }

    std::optional<uint32_t> Word(uint32_t address) const
    {
        if (address < base_ || address % 4 != 0 || code_.size() < 4 || address - base_ > code_.size() - 4)
        {
            return std::nullopt;
        }

        return xsflib::LoadLe32(&code_[address - base_]);
    }

    std::optional<uint32_t> Half(uint32_t address) const
    {
        if (address < base_ || address % 2 != 0 || code_.size() < 2 || address - base_ > code_.size() - 2)
        {
            return std::nullopt;
        }

        return xsflib::LoadLe16(&code_[address - base_]);
    }

    // Reads one Thumb instruction. Returns false where the routine's path ends. The formats are those of the
    // architecture reference manual's Thumb chapter. While a Thumb instruction runs, pc holds its address plus 4.
    bool ThumbInstruction(uint32_t address, uint32_t op)
    {
        StartThumb(address);
        const uint32_t rd = op & 7;
        const uint32_t rs = (op >> 3) & 7;
        switch (op >> 13)
        {
        case 0:
            if ((op & 0x1800) != 0x1800)
            {
                // lsl, lsr and asr by an immediate. lsls rd, rs, #0 copies rs.
                Write(rd, (op & 0x1FC0) == 0 ? registers_[rs] : Value());
                return true;
            }
            {
                // adds and subs with a register or a 3-bit immediate. adds rd, rs, #0 copies rs.
                const bool immediate = (op & 0x0400) != 0;
                const bool subtract = (op & 0x0200) != 0;
                const uint32_t value = (op >> 6) & 7;
                const Value& source = registers_[rs];
                if (immediate && source.kind == Value::Kind::kConstant)
                {
                    Write(rd, Value::Constant(subtract ? source.number - value : source.number + value));
                }
                else
                {
                    Write(rd, immediate && value == 0 ? source : Value());
                }
                return true;
            }

        case 1:
            {
                // mov, cmp, add and sub with an 8-bit immediate.
                const uint32_t reg = (op >> 8) & 7;
                const uint32_t value = op & 0xFF;
                const Value& source = registers_[reg];
                switch ((op >> 11) & 3)
                {
                case 0:
                    Write(reg, Value::Constant(value));
                    break;
                case 1:
                    Compare(source, value);
                    break;
                case 2:
                    Write(reg,
                          source.kind == Value::Kind::kConstant ? Value::Constant(source.number + value) : Value());
                    break;
                default:
                    Write(reg,
                          source.kind == Value::Kind::kConstant ? Value::Constant(source.number - value) : Value());
                    break;
                }
                return true;
            }

        case 2:
            return ThumbRegisterOrLiteral(address, op);

        case 3:
            {
                // ldr, str, ldrb and strb with a 5-bit immediate offset: words scale it by 4.
                const bool byte = (op & 0x1000) != 0;
                const auto offset = static_cast<int32_t>(((op >> 6) & 0x1F) * (byte ? 1 : 4));
                Transfer((op & 0x0800) != 0, rs, rd, offset, byte ? 1 : 4);
                return true;
            }

        case 4:
            if ((op & 0x1000) == 0)
            {
                // ldrh and strh with a 5-bit immediate offset of halfwords.
                Transfer((op & 0x0800) != 0, rs, rd, static_cast<int32_t>(((op >> 6) & 0x1F) * 2), 2);
            }
            else
            {
                // ldr and str relative to sp.
                Transfer((op & 0x0800) != 0, kSp, (op >> 8) & 7, static_cast<int32_t>((op & 0xFF) * 4), 4);
            }
            return true;

        case 5:
            return ThumbMiscellaneous(address, op);

        case 6:
            if ((op & 0x1000) == 0)
            {
                // ldmia and stmia: the base moves, and a load writes its registers.
                const uint32_t rb = (op >> 8) & 7;
                if ((op & 0x0800) != 0)
                {
                    Forget(op & 0xFF);
                }
                Write(rb, Value());
                return true;
            }
            if ((op & 0x0F00) == 0x0F00)
            {
                // An SWI: the BIOS returns its results in r0 to r3.
                Forget(0, 4);
                return true;
            }
            if ((op & 0x0F00) == 0x0E00)
            {
                return true;
            }
            {
                // A conditional branch: a step, and the path goes on. A forward branch over code that ends in a
                // return or a branch skips an early exit: that code runs only under the opposite condition, as an ARM
                // routine's conditional instructions do. Code that falls through to the target is on the path.
                const uint32_t condition = (op >> 8) & 0xF;
                const int32_t offset = static_cast<int32_t>(op << 24) >> 23;
                const uint32_t target = address + 4 + static_cast<uint32_t>(offset);
                if (runs_ && target > address && EndsPath(target - 2))
                {
                    skip_end_ = target;
                    skip_condition_ = condition ^ 1;
                }
                current_.condition = condition;
                runs_ = false;
                return Branch(target | 1);
            }

        default:
            if ((op & 0x1800) == 0)
            {
                const int32_t offset = static_cast<int32_t>(op << 21) >> 20;
                return Branch((address + 4 + static_cast<uint32_t>(offset)) | 1);
            }
            // The halves of bl and blx that the reader didn't pair.
            return true;
        }
    }

    // The ALU operations, the operations on high registers with bx and blx, the loads of literals and the loads and
    // stores with a register offset.
    bool ThumbRegisterOrLiteral(uint32_t address, uint32_t op)
    {
        const uint32_t rd = op & 7;
        const uint32_t rs = (op >> 3) & 7;
        if ((op & 0x1C00) == 0x0000)
        {
            // The ALU operations: cmp gives a step, tst and cmn write nothing, mvn of a constant is a constant.
            const uint32_t operation = (op >> 6) & 0xF;
            if (operation == kCmp)
            {
                Compare(registers_[rd], std::nullopt);
            }
            else if (operation == kMvn && registers_[rs].kind == Value::Kind::kConstant)
            {
                Write(rd, Value::Constant(~registers_[rs].number));
            }
            else if (operation != 0x8 && operation != 0xB)
            {
                Write(rd, Value());
            }
            return true;
        }
        if ((op & 0x1C00) == 0x0400)
        {
            const uint32_t hd = rd | ((op >> 4) & 8);
            const uint32_t hs = (op >> 3) & 0xF;
            switch ((op >> 8) & 3)
            {
            case 0:
                if (hd == kPc)
                {
                    return Branch(std::nullopt);
                }
                if (hd == kSp)
                {
                    stack_.reset();
                }
                Write(hd, Value());
                return true;
            case 1:
                Compare(registers_[hd], std::nullopt);
                return true;
            case 2:
                if (hd == kPc)
                {
                    return Branch(std::nullopt);
                }
                if (hd == kSp)
                {
                    stack_.reset();
                }
                Write(hd, hs == kPc ? Value::Constant(address + 4) : registers_[hs]);
                return true;
            default:
                if ((op & 0x80) != 0)
                {
                    Call(std::nullopt);
                    return true;
                }
                if (hs == kLr)
                {
                    return Return();
                }
                return Branch(std::nullopt);
            }
        }
        if ((op & 0x1800) == 0x0800)
        {
            // A literal, from the word-aligned pc.
            const uint32_t at = ((address + 4) & ~3u) + (op & 0xFF) * 4;
            const std::optional<uint32_t> literal = Word(at);
            Write((op >> 8) & 7, literal ? Value::Constant(*literal) : Value());
            return true;
        }

        // Loads and stores with a register offset: str, strh, strb, ldrsb, ldr, ldrh, ldrb and ldrsh.
        constexpr std::array<uint32_t, 8> kSizes = {4, 2, 1, 1, 4, 2, 1, 2};
        const uint32_t operation = (op >> 9) & 7;
        const bool load = operation >= 3;
        current_.kind = load ? StepKind::kLoad : StepKind::kStore;
        current_.base = registers_[rs];
        current_.size = kSizes[operation];
        if (!load)
        {
            current_.value = registers_[rd];
        }
        routine_.steps.push_back(current_);
        if (load)
        {
            Write(rd, Value());
        }

        return true;
    }

    // add rd, pc or sp, the moves of sp, push and pop, and the instructions that the reader skips.
    bool ThumbMiscellaneous(uint32_t address, uint32_t op)
    {
        if ((op & 0x1000) == 0)
        {
            // add rd, pc, #imm gives a constant. add rd, sp, #imm gives the address of a local.
            const uint32_t at = ((address + 4) & ~3u) + (op & 0xFF) * 4;
            Write((op >> 8) & 7, (op & 0x0800) == 0 ? Value::Constant(at) : Value());
            return true;
        }
        if ((op & 0x0F00) == 0x0000)
        {
            // add sp, #imm and sub sp, #imm.
            const auto bytes = static_cast<int32_t>((op & 0x7F) * 4);
            MoveStack((op & 0x80) != 0 ? -bytes : bytes);
            return true;
        }
        if ((op & 0x0600) == 0x0400)
        {
            const bool with_link = (op & 0x0100) != 0;
            const int32_t count = Registers(op & 0xFF) + (with_link ? 1 : 0);
            if ((op & 0x0800) == 0)
            {
                // push.
                MoveStack(-4 * count);
                return true;
            }

            // pop: with pc, a return.
            if (with_link)
            {
                return Return();
            }
            Forget(op & 0xFF);
            MoveStack(4 * count);
            return true;
        }

        return true;
    }

    // A load or a store of `size` bytes through `rn` at `offset`. Through sp, a load of an argument on the stack gives
    // that argument's value, and neither gives a step.
    void Transfer(bool load, uint32_t rn, uint32_t rd, int32_t offset, uint32_t size)
    {
        if (rn == kSp)
        {
            if (load)
            {
                Write(rd, size == 4 ? StackValue(offset) : Value());
            }
            return;
        }

        current_.kind = load ? StepKind::kLoad : StepKind::kStore;
        current_.base = registers_[rn];
        current_.offset = offset;
        current_.size = size;
        if (!load)
        {
            current_.value = registers_[rd];
        }
        routine_.steps.push_back(current_);
        if (load)
        {
            Write(rd, Value());
        }
    }

    // Whether the Thumb instruction at `address` always leaves the code after it: pop {..., pc}, bx and b.
    bool EndsPath(uint32_t address) const
    {
        const std::optional<uint32_t> op = Half(address);
        return op && ((*op & 0xFF00) == 0xBD00 || (*op & 0xFF87) == 0x4700 || (*op & 0xF800) == 0xE000);
    }

    // Starts a Thumb instruction at `address`. Inside code that a forward conditional branch skips, the instruction
    // runs only under the opposite condition: it gives its step with that condition, and leaves the values alone.
    void StartThumb(uint32_t address)
    {
        if (skip_end_ && address >= *skip_end_)
        {
            skip_end_.reset();
        }

        current_ = {.address = address, .condition = skip_end_ ? skip_condition_ : kAlways};
        runs_ = !skip_end_;
    }

    // A return: a step. A return that always runs ends the path.
    bool Return()
    {
        current_.kind = StepKind::kReturn;
        routine_.steps.push_back(current_);
        routine_.returns = runs_;

        return !runs_;
    }

    // The value of the word at sp + `offset`: an argument passed on the stack, where the reader follows sp.
    Value StackValue(int32_t offset) const
    {
        if (!stack_ || *stack_ + offset < 0 || (*stack_ + offset) % 4 != 0)
        {
            return Value();
        }

        return Value::Argument(kFirstStackArgument + static_cast<uint32_t>((*stack_ + offset) / 4));
    }

    // sp moves by `bytes`, on the path that the reader follows.
    void MoveStack(int32_t bytes)
    {
        if (runs_ && stack_)
        {
            *stack_ += bytes;
        }
    }

    void Compare(const Value& value, std::optional<uint32_t> immediate)
    {
        current_.kind = StepKind::kCompare;
        current_.base = value;
        current_.immediate = immediate;
        routine_.steps.push_back(current_);
    }

    // Reads one instruction. Returns false where the routine's path ends.
    bool Instruction(uint32_t address, uint32_t instruction)
    {
        const uint32_t condition = instruction >> 28;
        current_ = {.address = address, .condition = condition == kExtension ? kAlways : condition};
        runs_ = condition == kAlways || condition == kExtension;
        if (condition == kExtension)
        {
            if (IsBlxImmediate(instruction))
            {
                Call(BlxTarget(address, instruction) | 1);
            }
            return true;
        }
        if (IsReturn(instruction))
        {
            current_.kind = StepKind::kReturn;
            routine_.steps.push_back(current_);
            routine_.returns = runs_;
            return !runs_;
        }

        switch ((instruction >> 25) & 7)
        {
        case 0:
        case 1:
            return DataProcessing(address, instruction);
        case 2:
        case 3:
            return SingleTransfer(address, instruction);
        case 4:
            return BlockTransfer(instruction);
        case 5:
            {
                const uint32_t target = BranchTarget(address, instruction);
                if (IsLink(instruction))
                {
                    Call(target);
                    return true;
                }
                return Branch(target);
            }
        case 7:
            if ((instruction & 0x0F000000) == 0x0F000000)
            {
                // An SWI: the BIOS returns its results in r0 to r3.
                Forget(0, 4);
            }
            else if ((instruction & 0x00100010) == 0x00100010)
            {
                // mrc.
                Write(Field(instruction, 12), Value());
            }
            return true;
        default:
            return true;
        }
    }

    bool DataProcessing(uint32_t address, uint32_t instruction)
    {
        const bool immediate = (instruction & 0x02000000) != 0;
        if (!immediate && (instruction & 0x90) == 0x90)
        {
            return MultiplyOrExtraTransfer(instruction);
        }

        const uint32_t opcode = (instruction >> 21) & 0xF;
        const bool sets_flags = (instruction & 0x00100000) != 0;
        const uint32_t rn = Field(instruction, 16);
        const uint32_t rd = Field(instruction, 12);
        if (!sets_flags && (opcode >> 2) == 2)
        {
            // The miscellaneous instructions: msr writes no register, and bx and blx take a register.
            return Miscellaneous(instruction);
        }
        if ((opcode >> 2) == 2)
        {
            // tst, teq, cmp and cmn write no register. Only cmp gives a step.
            if (opcode == kCmp)
            {
                current_.kind = StepKind::kCompare;
                current_.base = registers_[rn];
                current_.immediate = immediate ? std::optional<uint32_t>(ExpandImmediate(instruction)) : std::nullopt;
                routine_.steps.push_back(current_);
            }
            return true;
        }
        if (rd == kPc)
        {
            return Branch(std::nullopt);
        }
        if (rd == kSp)
        {
            // add sp, sp, #imm and sub sp, sp, #imm move sp by a constant. Any other write loses it.
            if (immediate && rn == kSp && (opcode == kAdd || opcode == kSub))
            {
                const auto bytes = static_cast<int32_t>(ExpandImmediate(instruction));
                MoveStack(opcode == kAdd ? bytes : -bytes);
            }
            else if (runs_)
            {
                stack_.reset();
            }
            return true;
        }

        Write(rd, DataValue(address, instruction, opcode, immediate, rn));

        return true;
    }

    // The value that a data-processing instruction writes, where the reader follows it.
    Value DataValue(uint32_t address, uint32_t instruction, uint32_t opcode, bool immediate, uint32_t rn) const
    {
        const Value source = rn == kPc ? Value::Constant(address + 8) : registers_[rn];
        if (immediate)
        {
            const uint32_t value = ExpandImmediate(instruction);
            switch (opcode)
            {
            case kMov:
                return Value::Constant(value);
            case kMvn:
                return Value::Constant(~value);
            case kAdd:
                return source.kind == Value::Kind::kConstant ? Value::Constant(source.number + value) : Value();
            case kSub:
                return source.kind == Value::Kind::kConstant ? Value::Constant(source.number - value) : Value();
            default:
                return Value();
            }
        }

        // mov rd, rm without a shift copies rm.
        const uint32_t rm = instruction & 0xF;
        if (opcode == kMov && (instruction & 0xFF0) == 0)
        {
            return rm == kPc ? Value::Constant(address + 8) : registers_[rm];
        }

        return Value();
    }

    bool Miscellaneous(uint32_t instruction)
    {
        const uint32_t operation = instruction & 0x0FFFFFF0;
        if (operation == 0x012FFF30)
        {
            Call(std::nullopt);
            return true;
        }
        if (operation == 0x012FFF10)
        {
            return Branch(std::nullopt);
        }

        // The DSP's multiplies write the register at bits 16, and smlal also the one at 12.
        if ((instruction & 0x90) == 0x80)
        {
            Write(Field(instruction, 16), Value());
            Write(Field(instruction, 12), Value());
            return true;
        }

        // mrs, clz and the saturating additions write rd. msr's field holds 0xF and writes no register.
        const uint32_t rd = Field(instruction, 12);
        const bool msr = (instruction & 0x00200000) != 0 && rd == kPc;
        if (!msr && rd != kPc)
        {
            Write(rd, Value());
        }

        return true;
    }

    bool MultiplyOrExtraTransfer(uint32_t instruction)
    {
        const uint32_t kind = (instruction >> 5) & 3;
        if (kind == 0)
        {
            // mul and mla write the register at bits 16, the long multiplies both at 16 and 12, and swp at 12.
            if ((instruction & 0x0F800000) == 0x00800000)
            {
                Write(Field(instruction, 16), Value());
                Write(Field(instruction, 12), Value());
            }
            else if ((instruction & 0x0F000000) == 0x01000000)
            {
                Write(Field(instruction, 12), Value());
            }
            else
            {
                Write(Field(instruction, 16), Value());
            }
            return true;
        }

        const bool load = (instruction & 0x00100000) != 0;
        const uint32_t rn = Field(instruction, 16);
        const uint32_t rd = Field(instruction, 12);
        const bool pre = (instruction & 0x01000000) != 0;
        const bool halfword_store = !load && kind == 1;
        if ((load || halfword_store) && rn != kPc && rn != kSp)
        {
            const bool immediate = (instruction & 0x00400000) != 0;
            const int32_t magnitude = static_cast<int32_t>(((instruction >> 4) & 0xF0) | (instruction & 0xF));
            const int32_t offset = (instruction & 0x00800000) != 0 ? magnitude : -magnitude;
            current_.kind = load ? StepKind::kLoad : StepKind::kStore;
            current_.base = registers_[rn];
            current_.offset = !immediate ? std::nullopt : std::optional<int32_t>(pre ? offset : 0);
            current_.size = kind == 2 ? 1 : 2;
            if (!load)
            {
                current_.value = registers_[rd];
            }
            routine_.steps.push_back(current_);
        }
        if (load)
        {
            Write(rd, Value());
        }
        else if (kind == 2)
        {
            // ldrd loads rd and the register after it.
            Write(rd, Value());
            Write((rd + 1) & 0xF, Value());
        }
        WriteBack(instruction, rn, pre);

        return true;
    }

    bool SingleTransfer(uint32_t address, uint32_t instruction)
    {
        const bool registered = (instruction & 0x02000000) != 0;
        if (registered && (instruction & 0x10) != 0)
        {
            // The media instructions and the undefined ones.
            return true;
        }

        const bool load = (instruction & 0x00100000) != 0;
        const bool pre = (instruction & 0x01000000) != 0;
        const bool byte = (instruction & 0x00400000) != 0;
        const uint32_t rn = Field(instruction, 16);
        const uint32_t rd = Field(instruction, 12);
        const int32_t magnitude = static_cast<int32_t>(instruction & 0xFFF);
        const int32_t offset = (instruction & 0x00800000) != 0 ? magnitude : -magnitude;
        if (rn == kSp && !registered)
        {
            return StackTransfer(instruction, load, rd, offset, byte);
        }
        if (!load)
        {
            current_.kind = StepKind::kStore;
            current_.base = registers_[rn];
            current_.offset = registered ? std::nullopt : std::optional<int32_t>(pre ? offset : 0);
            current_.size = byte ? 1 : 4;
            current_.value = registers_[rd];
            routine_.steps.push_back(current_);
            WriteBack(instruction, rn, pre);
            return true;
        }
        if (rd == kPc)
        {
            return Branch(std::nullopt);
        }

        if (rn == kPc && !registered && pre)
        {
            // A literal.
            const std::optional<uint32_t> literal = Word(address + 8 + static_cast<uint32_t>(offset));
            Write(rd, literal && !byte ? Value::Constant(*literal) : Value());
            return true;
        }

        current_.kind = StepKind::kLoad;
        current_.base = registers_[rn];
        current_.offset = registered ? std::nullopt : std::optional<int32_t>(pre ? offset : 0);
        current_.size = byte ? 1 : 4;
        routine_.steps.push_back(current_);
        WriteBack(instruction, rn, pre);
        Write(rd, Value());

        return true;
    }

    // A load or a store through sp with an immediate offset: the pushes and pops of one register move sp, and a load
    // of an argument on the stack gives its value. Neither gives a step.
    bool StackTransfer(uint32_t instruction, bool load, uint32_t rd, int32_t offset, bool byte)
    {
        const bool pre = (instruction & 0x01000000) != 0;
        const bool write_back = !pre || (instruction & 0x00200000) != 0;
        if (load && rd == kPc)
        {
            return Branch(std::nullopt);
        }
        if (load)
        {
            Write(rd, !byte ? StackValue(pre ? offset : 0) : Value());
        }
        if (write_back)
        {
            MoveStack(offset);
        }

        return true;
    }

    bool BlockTransfer(uint32_t instruction)
    {
        const bool load = (instruction & 0x00100000) != 0;
        const uint32_t rn = Field(instruction, 16);
        if (load && (instruction & 0x8000) != 0)
        {
            return Branch(std::nullopt);
        }
        if (rn == kSp && (instruction & 0x00200000) != 0)
        {
            // stmfd sp! and ldmfd sp!.
            MoveStack((load ? 4 : -4) * Registers(instruction));
        }
        if (load)
        {
            for (uint32_t reg = 0; reg < 16; reg++)
            {
                if ((instruction >> reg & 1) != 0)
                {
                    Write(reg, Value());
                }
            }
        }
        if ((instruction & 0x00200000) != 0 && rn != kSp)
        {
            Write(rn, Value());
        }

        return true;
    }

    void Call(std::optional<uint32_t> target)
    {
        current_.kind = StepKind::kCall;
        current_.target = target;
        for (uint32_t reg = 0; reg < 4; reg++)
        {
            current_.arguments[reg] = registers_[reg];
        }
        routine_.steps.push_back(current_);

        // The callee may change r0 to r3, r12 and lr. r0 holds its result.
        if (runs_)
        {
            Forget(1, 4);
            registers_[12] = Value();
            registers_[kLr] = Value();
            registers_[0] = Value::Result(calls_);
        }
        calls_++;
    }

    // A branch to `target`, or to where a register gives. The path ends at a branch that always runs.
    bool Branch(std::optional<uint32_t> target)
    {
        current_.kind = StepKind::kBranch;
        current_.target = target;
        routine_.steps.push_back(current_);

        return !runs_;
    }

    // A load or a store that moves its base: after the access, or before it with the W bit. A move of sp by a
    // transfer that StackTransfer doesn't read loses sp.
    void WriteBack(uint32_t instruction, uint32_t rn, bool pre)
    {
        if (pre && (instruction & 0x00200000) == 0)
        {
            return;
        }

        if (rn != kSp)
        {
            Write(rn, Value());
        }
        else if (runs_)
        {
            stack_.reset();
        }
    }

    void Write(uint32_t reg, Value value)
    {
        if (runs_)
        {
            registers_[reg] = value;
        }
    }

    void Forget(uint32_t first, uint32_t end)
    {
        for (uint32_t reg = first; reg < end; reg++)
        {
            Write(reg, Value());
        }
    }

    // Forgets the low registers of the list `list`.
    void Forget(uint32_t list)
    {
        for (uint32_t reg = 0; reg < 8; reg++)
        {
            if ((list >> reg & 1) != 0)
            {
                Write(reg, Value());
            }
        }
    }

    std::span<const uint8_t> code_;
    uint32_t base_ = 0;
    Routine routine_;
    std::array<Value, 16> registers_ = {};
    Step current_;
    bool runs_ = true;
    uint32_t calls_ = 0;

    // How far sp is below its value at the routine's start, in bytes, while the reader follows it.
    std::optional<int32_t> stack_ = 0;

    // In Thumb code, the end of the code that a forward conditional branch skips, and the condition under which it
    // runs.
    std::optional<uint32_t> skip_end_;
    uint32_t skip_condition_ = kAlways;
};

} // namespace

Value Value::Constant(uint32_t value)
{
    return {.kind = Kind::kConstant, .number = value};
}

Value Value::Argument(uint32_t reg)
{
    return {.kind = Kind::kArgument, .number = reg};
}

Value Value::Result(uint32_t call)
{
    return {.kind = Kind::kResult, .number = call};
}

std::vector<const Step*> Routine::Calls() const
{
    std::vector<const Step*> calls;
    for (const Step& step : steps)
    {
        if (step.kind == StepKind::kCall)
        {
            calls.push_back(&step);
        }
    }

    return calls;
}

Routine ReadRoutine(std::span<const uint8_t> code, uint32_t base, uint32_t address, std::size_t limit)
{
    return Reader(code, base, address).Read(limit);
}

std::vector<uint32_t> ThumbRoutineStarts(std::span<const uint8_t> code, uint32_t base)
{
    std::vector<uint32_t> starts;
    for (std::size_t at = 0; at + 2 <= code.size(); at += 2)
    {
        if (ThumbSavesLr(xsflib::LoadLe16(&code[at])))
        {
            starts.push_back((base + static_cast<uint32_t>(at)) | 1);
        }
    }

    return starts;
}

std::vector<uint32_t> RoutineStarts(std::span<const uint8_t> code, uint32_t base)
{
    std::vector<uint32_t> starts;
    for (std::size_t at = 0; at + 4 <= code.size(); at += 4)
    {
        if (SavesLr(xsflib::LoadLe32(&code[at])))
        {
            starts.push_back(base + static_cast<uint32_t>(at));
        }
    }

    return starts;
}

} // namespace ntdlib::arm
