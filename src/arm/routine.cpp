// SPDX-License-Identifier: MIT

#include "ntdlib/arm/routine.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

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

uint32_t RotatedImmediate(uint32_t instruction)
{
    const uint32_t value = instruction & 0xFF;
    const uint32_t rotation = 2 * Field(instruction, 8);

    return rotation == 0 ? value : (value >> rotation) | (value << (32 - rotation));
}

// A branch's offset: 24 bits of words, signed.
int32_t BranchOffset(uint32_t instruction)
{
    return static_cast<int32_t>(instruction << 8) >> 6;
}

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
    std::optional<uint32_t> Word(uint32_t address) const
    {
        if (address < base_ || address % 4 != 0 || code_.size() < 4 || address - base_ > code_.size() - 4)
        {
            return std::nullopt;
        }

        return xsflib::LoadLe32(&code_[address - base_]);
    }

    // Reads one instruction. Returns false where the routine's path ends.
    bool Instruction(uint32_t address, uint32_t instruction)
    {
        const uint32_t condition = instruction >> 28;
        current_ = {.address = address, .condition = condition == kExtension ? kAlways : condition};
        runs_ = condition == kAlways || condition == kExtension;
        if (condition == kExtension)
        {
            if ((instruction & 0xFE000000) == 0xFA000000)
            {
                const uint32_t half = (instruction >> 23) & 2;
                Call((address + 8 + static_cast<uint32_t>(BranchOffset(instruction)) + half) | 1);
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
                const uint32_t target = address + 8 + static_cast<uint32_t>(BranchOffset(instruction));
                if ((instruction & 0x01000000) != 0)
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
                current_.immediate = immediate ? std::optional<uint32_t>(RotatedImmediate(instruction)) : std::nullopt;
                routine_.steps.push_back(current_);
            }
            return true;
        }
        if (rd == kPc)
        {
            return Branch(std::nullopt);
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
            const uint32_t value = RotatedImmediate(instruction);
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
        if (load && rn != kPc)
        {
            const bool immediate = (instruction & 0x00400000) != 0;
            const int32_t magnitude = static_cast<int32_t>(((instruction >> 4) & 0xF0) | (instruction & 0xF));
            const int32_t offset = (instruction & 0x00800000) != 0 ? magnitude : -magnitude;
            current_.kind = StepKind::kLoad;
            current_.base = registers_[rn];
            current_.offset = !immediate ? std::nullopt : std::optional<int32_t>(pre ? offset : 0);
            current_.size = kind == 2 ? 1 : 2;
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
        if (!load)
        {
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

    bool BlockTransfer(uint32_t instruction)
    {
        const bool load = (instruction & 0x00100000) != 0;
        const uint32_t rn = Field(instruction, 16);
        if (load && (instruction & 0x8000) != 0)
        {
            return Branch(std::nullopt);
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

    // A load or a store that moves its base: after the access, or before it with the W bit. A move of sp leaves the
    // values alone: the reader doesn't follow the stack.
    void WriteBack(uint32_t instruction, uint32_t rn, bool pre)
    {
        if ((!pre || (instruction & 0x00200000) != 0) && rn != kSp)
        {
            Write(rn, Value());
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

    std::span<const uint8_t> code_;
    uint32_t base_ = 0;
    Routine routine_;
    std::array<Value, 16> registers_ = {};
    Step current_;
    bool runs_ = true;
    uint32_t calls_ = 0;
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
