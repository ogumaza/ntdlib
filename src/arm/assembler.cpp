// SPDX-License-Identifier: MIT

#include "ntdlib/arm/assembler.h"

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <vector>

#include "xsflib/bytes.h"

namespace ntdlib::arm
{
namespace
{

// The data-processing opcodes.
constexpr uint32_t kOpAnd = 0x0;
constexpr uint32_t kOpSub = 0x2;
constexpr uint32_t kOpAdd = 0x4;
constexpr uint32_t kOpTst = 0x8;
constexpr uint32_t kOpCmp = 0xA;
constexpr uint32_t kOpOrr = 0xC;
constexpr uint32_t kOpMov = 0xD;
constexpr uint32_t kOpMvn = 0xF;

constexpr uint32_t kAlways = uint32_t(Condition::kAl) << 28;

// The Thumb instructions of a call's veneer: bx pc and mov r8, r8.
constexpr uint16_t kThumbBxPc = 0x4778;
constexpr uint16_t kThumbNop = 0x46C0;

uint32_t RotateRight(uint32_t value, uint32_t bits)
{
    bits &= 31;
    return bits == 0 ? value : (value >> bits) | (value << (32 - bits));
}

void CheckRegister(int r)
{
    if (r < 0 || r > 15)
    {
        throw std::logic_error("no such register");
    }
}

uint32_t Immediate(uint32_t value)
{
    const std::optional<uint32_t> operand = RotatedImmediate(value);
    if (!operand)
    {
        throw std::logic_error("the constant doesn't fit in an instruction");
    }

    return *operand;
}

// A literal's key in a pool: its label, or for a constant -1 and its value.
std::pair<int, uint32_t> PoolKey(int label, uint32_t value)
{
    return label >= 0 ? std::pair<int, uint32_t>{label, 0} : std::pair<int, uint32_t>{-1, value};
}

} // namespace

std::optional<uint32_t> RotatedImmediate(uint32_t value)
{
    for (uint32_t rotate = 0; rotate < 16; rotate++)
    {
        // value = imm8 rotated right by 2 * rotate, so imm8 = value rotated left by as much.
        const uint32_t imm8 = RotateRight(value, 32 - 2 * rotate);
        if (imm8 <= 0xFF)
        {
            return rotate << 8 | imm8;
        }
    }

    return std::nullopt;
}

ArmAssembler::ArmAssembler(uint32_t base) : base_(base)
{
    if (base % 4 != 0)
    {
        throw std::logic_error("ARM code starts at a multiple of 4");
    }
}

uint32_t ArmAssembler::Here() const
{
    return base_ + 4 * uint32_t(words_.size());
}

int ArmAssembler::NewLabel()
{
    labels_.emplace_back();
    return int(labels_.size()) - 1;
}

void ArmAssembler::Bind(int label)
{
    labels_.at(size_t(label)) = Here();
}

uint32_t ArmAssembler::Address(int label) const
{
    const std::optional<uint32_t>& address = labels_.at(size_t(label));
    if (!address)
    {
        throw std::logic_error("a label isn't bound");
    }

    return *address;
}

void ArmAssembler::LoadConstant(int rd, uint32_t value)
{
    if (RotatedImmediate(value))
    {
        DataImmediate(kOpMov, false, 0, rd, value);
    }
    else if (RotatedImmediate(~value))
    {
        DataImmediate(kOpMvn, false, 0, rd, ~value);
    }
    else
    {
        CheckRegister(rd);
        AddLiteral({uint32_t(words_.size()), rd, value, -1});
        Emit(0);
    }
}

void ArmAssembler::LoadAddress(int rd, int label)
{
    CheckRegister(rd);
    AddLiteral({uint32_t(words_.size()), rd, 0, label});
    Emit(0);
}

void ArmAssembler::Add(int rd, int rn, uint32_t immediate)
{
    DataImmediate(kOpAdd, false, rn, rd, immediate);
}

void ArmAssembler::Sub(int rd, int rn, uint32_t immediate, bool set_flags)
{
    DataImmediate(kOpSub, set_flags, rn, rd, immediate);
}

void ArmAssembler::And(int rd, int rn, uint32_t immediate)
{
    DataImmediate(kOpAnd, false, rn, rd, immediate);
}

void ArmAssembler::Orr(int rd, int rn, uint32_t immediate)
{
    DataImmediate(kOpOrr, false, rn, rd, immediate);
}

void ArmAssembler::Tst(int rn, uint32_t immediate)
{
    DataImmediate(kOpTst, true, rn, 0, immediate);
}

void ArmAssembler::Cmp(int rn, uint32_t immediate)
{
    DataImmediate(kOpCmp, true, rn, 0, immediate);
}

void ArmAssembler::Mov(int rd, uint32_t immediate)
{
    DataImmediate(kOpMov, false, 0, rd, immediate);
}

void ArmAssembler::AndRegister(int rd, int rn, int rm)
{
    CheckRegister(rm);
    DataRegister(kOpAnd, false, rn, rd, uint32_t(rm));
}

void ArmAssembler::AndShifted(int rd, int rn, int rm, int shift)
{
    CheckRegister(rm);
    if (shift < 1 || shift > 31)
    {
        throw std::logic_error("a logical shift right goes from 1 to 31 bits here");
    }

    // The shift's amount in bits 7 to 11, and LSR (01) in bits 5 and 6.
    DataRegister(kOpAnd, false, rn, rd, uint32_t(shift) << 7 | 0x20 | uint32_t(rm));
}

void ArmAssembler::OrrRegister(int rd, int rn, int rm)
{
    CheckRegister(rm);
    DataRegister(kOpOrr, false, rn, rd, uint32_t(rm));
}

void ArmAssembler::SubRegister(int rd, int rn, int rm)
{
    CheckRegister(rm);
    DataRegister(kOpSub, false, rn, rd, uint32_t(rm));
}

void ArmAssembler::CmpRegister(int rn, int rm)
{
    CheckRegister(rm);
    DataRegister(kOpCmp, true, rn, 0, uint32_t(rm));
}

void ArmAssembler::MovRegister(int rd, int rm)
{
    CheckRegister(rm);
    DataRegister(kOpMov, false, 0, rd, uint32_t(rm));
}

void ArmAssembler::Ldr(int rd, int rn, int offset)
{
    Transfer(true, false, rd, rn, offset, false);
}

void ArmAssembler::Str(int rd, int rn, int offset)
{
    Transfer(false, false, rd, rn, offset, false);
}

void ArmAssembler::Ldrb(int rd, int rn, int offset)
{
    Transfer(true, true, rd, rn, offset, false);
}

void ArmAssembler::Strb(int rd, int rn, int offset)
{
    Transfer(false, true, rd, rn, offset, false);
}

void ArmAssembler::Ldrh(int rd, int rn, int offset)
{
    TransferHalf(true, rd, rn, offset);
}

void ArmAssembler::Strh(int rd, int rn, int offset)
{
    TransferHalf(false, rd, rn, offset);
}

void ArmAssembler::LdrIndexed(int rd, int rn, int rm, int shift)
{
    CheckRegister(rd);
    CheckRegister(rn);
    CheckRegister(rm);
    if (shift < 0 || shift > 31)
    {
        throw std::logic_error("a register offset's shift goes up to 31");
    }

    // The register form (bit 25) with P = 1, U = 1 and L = 1, and a logical shift left of rm.
    Emit(kAlways | 0x07900000 | uint32_t(rn) << 16 | uint32_t(rd) << 12 | uint32_t(shift) << 7 | uint32_t(rm));
}

void ArmAssembler::LdrPost(int rd, int rn, int step)
{
    Transfer(true, false, rd, rn, step, true);
}

void ArmAssembler::StrPost(int rd, int rn, int step)
{
    Transfer(false, false, rd, rn, step, true);
}

void ArmAssembler::LdrbPost(int rd, int rn, int step)
{
    Transfer(true, true, rd, rn, step, true);
}

void ArmAssembler::StrbPost(int rd, int rn, int step)
{
    Transfer(false, true, rd, rn, step, true);
}

void ArmAssembler::Push(uint16_t registers)
{
    // stmdb sp!: P = 1, U = 0, W = 1, L = 0.
    Emit(kAlways | 0x092D0000 | registers);
}

void ArmAssembler::Pop(uint16_t registers)
{
    // ldmia sp!: P = 0, U = 1, W = 1, L = 1.
    Emit(kAlways | 0x08BD0000 | registers);
}

void ArmAssembler::SetControl(uint32_t control)
{
    // msr cpsr_c, #control: the field mask 0001 writes the control byte alone.
    Emit(kAlways | 0x0321F000 | Immediate(control));
}

void ArmAssembler::B(int label, Condition condition)
{
    branches_.push_back({uint32_t(words_.size()), label, condition});
    Emit(0);
}

void ArmAssembler::Bl(int label)
{
    branches_.push_back({uint32_t(words_.size()), label, Condition::kAl, true});
    Emit(0);
}

void ArmAssembler::Bx(int rm)
{
    CheckRegister(rm);
    Emit(kAlways | 0x012FFF10 | uint32_t(rm));
}

void ArmAssembler::CallRegister(int rm)
{
    // The pc reads 8 bytes ahead: lr then holds the address after the bx.
    MovRegister(kLr, kPc);
    Bx(rm);
}

void ArmAssembler::CallThumb(int rm)
{
    // lr = the veneer's address, 8 bytes on, with bit 0 set. The veneer's bx pc, at a multiple of 4, goes on in ARM
    // state 4 bytes after it.
    Add(kLr, kPc, 1);
    Bx(rm);
    Emit(uint32_t(kThumbNop) << 16 | kThumbBxPc);
}

void ArmAssembler::Swi(uint32_t number)
{
    if (number > 0xFF)
    {
        throw std::logic_error("the BIOS's calls go up to 0xFF");
    }

    Emit(kAlways | 0x0F000000 | number << 16);
}

void ArmAssembler::Word(uint32_t value)
{
    Emit(value);
}

void ArmAssembler::AddLiteral(Literal literal)
{
    if (pool_)
    {
        throw std::logic_error("a load from the literal pool after the pool");
    }

    literals_.push_back(literal);
}

void ArmAssembler::Pool()
{
    if (pool_)
    {
        throw std::logic_error("a second literal pool");
    }

    pool_.emplace();
    for (const Literal& literal : literals_)
    {
        if (pool_->try_emplace(PoolKey(literal.label, literal.value), uint32_t(words_.size())).second)
        {
            Emit(0);
        }
    }
}

std::vector<uint8_t> ArmAssembler::Finish()
{
    std::vector<uint32_t> words = words_;

    // The pool holds each value once, where Pool() put it or after the code.
    std::map<std::pair<int, uint32_t>, uint32_t> pool = pool_ ? *pool_ : std::map<std::pair<int, uint32_t>, uint32_t>{};
    for (const Literal& literal : literals_)
    {
        const uint32_t value = literal.label >= 0 ? Address(literal.label) : literal.value;
        auto [it, added] = pool.try_emplace(PoolKey(literal.label, literal.value), uint32_t(words.size()));
        if (added)
        {
            words.push_back(value);
        }
        words[it->second] = value;

        // ldr rd, [pc, #offset]: the pc reads 8 bytes past the load.
        const int64_t offset = int64_t(it->second) * 4 - (int64_t(literal.at) * 4 + 8);
        if (offset < 0 || offset > 0xFFF)
        {
            throw std::logic_error("a literal lies out of a load's reach");
        }

        words[literal.at] = kAlways | 0x059F0000 | uint32_t(literal.rd) << 12 | uint32_t(offset);
    }

    for (const Branch& branch : branches_)
    {
        const int64_t target = int64_t(Address(branch.label)) - base_;
        const int64_t offset = (target - (int64_t(branch.at) * 4 + 8)) / 4;
        if (offset < -0x800000 || offset > 0x7FFFFF)
        {
            throw std::logic_error("a branch lies out of reach");
        }

        words[branch.at] = uint32_t(branch.condition) << 28 | (branch.link ? 0x0B000000u : 0x0A000000u) |
                           (uint32_t(offset) & 0xFFFFFF);
    }

    std::vector<uint8_t> bytes;
    bytes.reserve(4 * words.size());
    for (const uint32_t word : words)
    {
        xsflib::AppendLe32(bytes, word);
    }

    return bytes;
}

void ArmAssembler::Emit(uint32_t word)
{
    words_.push_back(word);
}

void ArmAssembler::DataImmediate(uint32_t opcode, bool set_flags, int rn, int rd, uint32_t immediate)
{
    CheckRegister(rn);
    CheckRegister(rd);
    Emit(kAlways | 0x02000000 | opcode << 21 | uint32_t(set_flags) << 20 | uint32_t(rn) << 16 | uint32_t(rd) << 12 |
         Immediate(immediate));
}

void ArmAssembler::DataRegister(uint32_t opcode, bool set_flags, int rn, int rd, uint32_t operand)
{
    CheckRegister(rn);
    CheckRegister(rd);
    Emit(kAlways | opcode << 21 | uint32_t(set_flags) << 20 | uint32_t(rn) << 16 | uint32_t(rd) << 12 | operand);
}

void ArmAssembler::Transfer(bool load, bool byte, int rd, int rn, int offset, bool post)
{
    CheckRegister(rd);
    CheckRegister(rn);
    if (offset < -0xFFF || offset > 0xFFF)
    {
        throw std::logic_error("a load's or a store's offset goes up to 4095");
    }

    // P = 1 for an offset, 0 for a step after the access. U = 1 adds the offset.
    const uint32_t up = offset >= 0 ? 1 : 0;
    const uint32_t magnitude = uint32_t(offset >= 0 ? offset : -offset);
    Emit(kAlways | 0x04000000 | uint32_t(!post) << 24 | up << 23 | uint32_t(byte) << 22 | uint32_t(load) << 20 |
         uint32_t(rn) << 16 | uint32_t(rd) << 12 | magnitude);
}

void ArmAssembler::TransferHalf(bool load, int rd, int rn, int offset)
{
    CheckRegister(rd);
    CheckRegister(rn);
    if (offset < -0xFF || offset > 0xFF)
    {
        throw std::logic_error("a halfword's offset goes up to 255");
    }

    // P = 1, the immediate form (bit 22), and the halfword's code 1011 in bits 4 to 7. The offset's high 4 bits go in
    // bits 8 to 11 and its low 4 bits in bits 0 to 3.
    const uint32_t up = offset >= 0 ? 1 : 0;
    const uint32_t magnitude = uint32_t(offset >= 0 ? offset : -offset);
    Emit(kAlways | 0x01400000 | up << 23 | uint32_t(load) << 20 | uint32_t(rn) << 16 | uint32_t(rd) << 12 |
         (magnitude >> 4) << 8 | 0xB0 | (magnitude & 0xF));
}

} // namespace ntdlib::arm
