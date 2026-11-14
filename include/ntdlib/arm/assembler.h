// SPDX-License-Identifier: MIT

// An assembler of the few ARM instructions that a stub uses: a short program, written into a game's memory, that calls
// the game's routines. The instructions are the ARM7TDMI's (ARMv4T), which the ARM946E-S runs too. The encodings follow
// ARM's architecture reference manual. Constants that an instruction can't hold go in a literal pool after the code.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace ntdlib::arm
{

// The registers that have other names.
constexpr int kSp = 13;
constexpr int kLr = 14;
constexpr int kPc = 15;

// The conditions that the assembler supports.
enum class Condition : uint32_t
{
    kEq = 0x0,
    kNe = 0x1,
    kCs = 0x2, // unsigned higher or same (hs)
    kCc = 0x3, // unsigned lower (lo)
    kAl = 0xE,
};

// The processor modes that SetControl can enter, with the CPSR's I and F bits.
constexpr uint32_t kModeIrq = 0x12;
constexpr uint32_t kModeSupervisor = 0x13;
constexpr uint32_t kModeSystem = 0x1F;
constexpr uint32_t kIrqsOff = 0x80;
constexpr uint32_t kFiqsOff = 0x40;

// Returns the 12 bits of a data-processing instruction that give `value`: an 8-bit constant rotated right by an even
// number of bits. Returns nothing for a value that no rotation gives.
std::optional<uint32_t> RotatedImmediate(uint32_t value);

class ArmAssembler
{
public:
    // Code that starts at `base`, a multiple of 4.
    explicit ArmAssembler(uint32_t base);

    // The address of the next instruction.
    uint32_t Here() const;

    // A new label, and binds it to the next instruction's address.
    int NewLabel();
    void Bind(int label);

    // The address that `label` is bound to. Throws std::logic_error for a label that isn't bound.
    uint32_t Address(int label) const;

    // rd = value: a mov or a mvn where one holds the value, or else a load from the literal pool.
    void LoadConstant(int rd, uint32_t value);

    // rd = the address of `label`, from the literal pool.
    void LoadAddress(int rd, int label);

    // Data processing with an immediate: add, sub, and, orr, tst, cmp and mov. The immediate must fit
    // (RotatedImmediate).
    void Add(int rd, int rn, uint32_t immediate);
    void Sub(int rd, int rn, uint32_t immediate, bool set_flags = false);
    void And(int rd, int rn, uint32_t immediate);
    void Orr(int rd, int rn, uint32_t immediate);
    void Tst(int rn, uint32_t immediate);
    void Cmp(int rn, uint32_t immediate);
    void Mov(int rd, uint32_t immediate);

    // Data processing with registers: rd = rn op rm. AndShifted gives rd = rn & (rm >> shift), a logical shift.
    // CmpRegister sets the flags from rn - rm.
    void AndRegister(int rd, int rn, int rm);
    void AndShifted(int rd, int rn, int rm, int shift);
    void OrrRegister(int rd, int rn, int rm);
    void SubRegister(int rd, int rn, int rm);
    void CmpRegister(int rn, int rm);
    void MovRegister(int rd, int rm);

    // Loads and stores with an offset from rn: words and bytes up to ±4095, halfwords up to ±255.
    void Ldr(int rd, int rn, int offset = 0);
    void Str(int rd, int rn, int offset = 0);
    void Ldrb(int rd, int rn, int offset = 0);
    void Strb(int rd, int rn, int offset = 0);
    void Ldrh(int rd, int rn, int offset = 0);
    void Strh(int rd, int rn, int offset = 0);

    // ldr rd, [rn, rm, lsl #shift]: the word at rn plus rm shifted left.
    void LdrIndexed(int rd, int rn, int rm, int shift);

    // Loads and stores that move rn on by `step` after the access: ldr rd, [rn], #step.
    void LdrPost(int rd, int rn, int step);
    void StrPost(int rd, int rn, int step);
    void LdrbPost(int rd, int rn, int step);
    void StrbPost(int rd, int rn, int step);

    // stmfd sp!, {registers} and ldmfd sp!, {registers}: bit n of `registers` stands for rn.
    void Push(uint16_t registers);
    void Pop(uint16_t registers);

    // msr cpsr_c, #control: the mode and the I and F bits.
    void SetControl(uint32_t control);

    // A branch to `label`, under `condition`.
    void B(int label, Condition condition = Condition::kAl);

    // A call of the routine at `label`: bl.
    void Bl(int label);

    // bx rm.
    void Bx(int rm);

    // A call of the ARM routine whose address rm holds: mov lr, pc, then bx rm.
    void CallRegister(int rm);

    // A call of the Thumb routine whose address, with bit 0 set, rm holds. On the ARM7TDMI, a Thumb routine that
    // returns with pop {pc} stays in Thumb state. The call therefore returns to a Thumb veneer, bx pc and a nop, that
    // goes back to ARM state: add lr, pc, #1, then bx rm, then the veneer.
    void CallThumb(int rm);

    // An SWI in ARM state: the BIOS call's number goes in bits 16 to 23 of the comment.
    void Swi(uint32_t number);

    // A word of data.
    void Word(uint32_t value);

    // Puts the literal pool here, in place of after the code, so that data after it stays out of the loads' way. The
    // pool holds the literals of the loads so far, and no load can follow. Throws std::logic_error for a second pool.
    void Pool();

    // The code, with the literal pool after it unless Pool() placed it. Throws std::logic_error for a label that isn't
    // bound, or for a literal or a branch that lies out of reach.
    std::vector<uint8_t> Finish();

private:
    // A load from the literal pool that Finish() fills in: of a constant, or of a label's address.
    struct Literal
    {
        uint32_t at = 0; // the load's index among the words
        int rd = 0;
        uint32_t value = 0;
        int label = -1;
    };

    // A branch that Finish() fills in.
    struct Branch
    {
        uint32_t at = 0;
        int label = 0;
        Condition condition = Condition::kAl;
        bool link = false;
    };

    void Emit(uint32_t word);
    void AddLiteral(Literal literal);
    void DataImmediate(uint32_t opcode, bool set_flags, int rn, int rd, uint32_t immediate);
    void DataRegister(uint32_t opcode, bool set_flags, int rn, int rd, uint32_t operand);
    void Transfer(bool load, bool byte, int rd, int rn, int offset, bool post);
    void TransferHalf(bool load, int rd, int rn, int offset);

    uint32_t base_;
    std::vector<uint32_t> words_;
    std::vector<std::optional<uint32_t>> labels_;
    std::vector<Literal> literals_;
    std::optional<std::map<std::pair<int, uint32_t>, uint32_t>> pool_; // a literal's label or value, then its word
    std::vector<Branch> branches_;
};

} // namespace ntdlib::arm
