// SPDX-License-Identifier: MIT

// Tests of the ARM interpreter (ntdlib/arm/cpu.h) on the ARM7TDMI (ARMv4T) and the ARM946E-S (ARMv5TE), with programs
// that LLVM's assembler built. Each instruction's word comes with its assembly. The expected values follow ARM's
// architecture reference manual and GBATEK's ARM CPU reference, worked out by hand.

#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/arm/cpu.h"

using xsflib::testing::ErrorOf;

namespace
{

using ntdlib::arm::Architecture;
using ntdlib::arm::Bus;
using ntdlib::arm::Cpu;
using ntdlib::arm::Mode;
using ntdlib::arm::StopReason;
namespace psr = ntdlib::arm::psr;

constexpr Architecture kV4 = Architecture::kArmV4T;
constexpr Architecture kV5 = Architecture::kArmV5TE;

// The test machine's memory: RAM from 0, 64 KiB at the high vectors, and I/O registers at kIo.
constexpr uint32_t kRamSize = 0x40000;
constexpr uint32_t kHigh = 0xFFFF0000;
constexpr uint32_t kIo = 0x04000000;
constexpr uint32_t kIoSize = 0x1000;

// Where the tests put their code, their data and the stack.
constexpr uint32_t kCode = 0x1000;
constexpr uint32_t kData = 0x8000;
constexpr uint32_t kStack = 0x20000;

// A page that the bus can show from a second buffer.
constexpr uint32_t kSwappedPage = 0x5000;

// The CPSR that a Machine starts with: system mode with IRQs and FIQs disabled.
constexpr uint32_t kSystemCpsr = 0xDF;

// A bus with RAM, memory at the high vectors and I/O registers that log their accesses. Other addresses read as 0
// and log their accesses too.
class TestBus : public Bus
{
public:
    struct Access
    {
        uint32_t address = 0;
        int size = 0;
        uint32_t value = 0;
        bool write = false;

        bool operator==(const Access& other) const = default;
    };

    uint8_t* Page(uint32_t page) override;
    uint32_t Read(uint32_t address, int size) override;
    void Write(uint32_t address, int size, uint32_t value) override;

    void Put32(uint32_t address, std::initializer_list<uint32_t> words);
    void Put16(uint32_t address, std::initializer_list<uint16_t> halfwords);
    uint32_t Get32(uint32_t address);
    void SetIo32(uint32_t address, uint32_t value);
    const std::vector<Access>& Accesses() const;
    void ClearAccesses();
    void SwapPage();

private:
    std::vector<uint8_t> ram_ = std::vector<uint8_t>(kRamSize);
    std::vector<uint8_t> high_ = std::vector<uint8_t>(0x10000);
    std::vector<uint8_t> io_ = std::vector<uint8_t>(kIoSize);
    std::vector<uint8_t> swapped_ = std::vector<uint8_t>(0x1000);
    bool swap_ = false;
    std::vector<Access> accesses_;
};

uint8_t* TestBus::Page(uint32_t page)
{
    if (page == kSwappedPage && swap_)
    {
        return swapped_.data();
    }

    if (page < kRamSize)
    {
        return ram_.data() + page;
    }

    return page >= kHigh ? high_.data() + (page - kHigh) : nullptr;
}

uint32_t TestBus::Read(uint32_t address, int size)
{
    uint32_t value = 0;
    if (address - kIo < kIoSize)
    {
        for (int i = 0; i < size; ++i)
        {
            value |= static_cast<uint32_t>(io_[address - kIo + i]) << (8 * i);
        }
    }

    accesses_.push_back({address, size, value, false});

    return value;
}

void TestBus::Write(uint32_t address, int size, uint32_t value)
{
    if (address - kIo < kIoSize)
    {
        for (int i = 0; i < size; ++i)
        {
            io_[address - kIo + i] = static_cast<uint8_t>(value >> (8 * i));
        }
    }

    accesses_.push_back({address, size, value, true});
}

void TestBus::Put32(uint32_t address, std::initializer_list<uint32_t> words)
{
    for (const uint32_t word : words)
    {
        xsflib::StoreLe32(Page(address & ~0xFFFu) + (address & 0xFFF), word);
        address += 4;
    }
}

void TestBus::Put16(uint32_t address, std::initializer_list<uint16_t> halfwords)
{
    for (const uint16_t halfword : halfwords)
    {
        xsflib::StoreLe16(Page(address & ~0xFFFu) + (address & 0xFFF), halfword);
        address += 2;
    }
}

uint32_t TestBus::Get32(uint32_t address)
{
    return xsflib::LoadLe32(Page(address & ~0xFFFu) + (address & 0xFFF));
}

void TestBus::SetIo32(uint32_t address, uint32_t value)
{
    xsflib::StoreLe32(&io_[address - kIo], value);
}

const std::vector<TestBus::Access>& TestBus::Accesses() const
{
    return accesses_;
}

void TestBus::ClearAccesses()
{
    accesses_.clear();
}

void TestBus::SwapPage()
{
    swap_ = true;
}

// A CPU on a TestBus, in system mode at kCode with SP at kStack. kData holds the bytes 11 22 33 ... FF 00.
struct Machine
{
    explicit Machine(Architecture architecture);

    TestBus bus;
    Cpu cpu;
};

Machine::Machine(Architecture architecture) : cpu(bus, architecture)
{
    cpu.Jump(kCode, Mode::kSystem);
    cpu.SetReg(13, kStack);
    bus.Put32(kData, {0x44332211, 0x88776655, 0xCCBBAA99, 0x00FFEEDD});
}

Mode CurrentMode(const Cpu& cpu)
{
    return static_cast<Mode>(cpu.Cpsr() & psr::kModeMask);
}

// The flags as a 4-bit number: N, Z, C and V from the top bit down.
uint32_t Nzcv(const Cpu& cpu)
{
    return cpu.Cpsr() >> 28;
}

void SetNzcv(Cpu& cpu, uint32_t flags)
{
    cpu.SetCpsr((cpu.Cpsr() & 0x0FFFFFFF) | (flags << 28));
}

// Puts ARM code at `address` and runs it from there in the current mode until the CPU reaches `stop`.
StopReason RunArmAt(Machine& m, uint32_t address, std::initializer_list<uint32_t> program, uint32_t stop)
{
    m.bus.Put32(address, program);
    m.cpu.Jump(address, CurrentMode(m.cpu));
    m.cpu.SetStopAddress(stop);

    return m.cpu.Run(1000);
}

// Runs ARM code at kCode until it reaches `stop`.
StopReason RunArmTo(Machine& m, std::initializer_list<uint32_t> program, uint32_t stop)
{
    return RunArmAt(m, kCode, program, stop);
}

// Runs ARM code at kCode until it passes its last instruction.
void RunArm(Machine& m, std::initializer_list<uint32_t> program)
{
    const uint32_t end = kCode + static_cast<uint32_t>(program.size()) * 4;

    const StopReason reason = RunArmTo(m, program, end);

    XSFLIB_CHECK(reason == StopReason::kStopAddress && m.cpu.Pc() == end);
}

// Runs Thumb code at `address` until the CPU reaches `stop`.
StopReason RunThumbAt(Machine& m, uint32_t address, std::initializer_list<uint16_t> program, uint32_t stop)
{
    m.bus.Put16(address, program);
    m.cpu.Jump(address | 1, CurrentMode(m.cpu));
    m.cpu.SetStopAddress(stop);

    return m.cpu.Run(1000);
}

StopReason RunThumbTo(Machine& m, std::initializer_list<uint16_t> program, uint32_t stop)
{
    return RunThumbAt(m, kCode, program, stop);
}

// Runs Thumb code at kCode until it passes its last instruction.
void RunThumb(Machine& m, std::initializer_list<uint16_t> program)
{
    const uint32_t end = kCode + static_cast<uint32_t>(program.size()) * 2;

    const StopReason reason = RunThumbTo(m, program, end);

    XSFLIB_CHECK(reason == StopReason::kStopAddress && m.cpu.Pc() == (end | 1));
}

// Whether `op` at kCode, in system mode, enters the undefined instruction exception with the address after it in LR
// and the old CPSR in the SPSR.
bool RaisesUndefined(Architecture architecture, uint32_t op, bool thumb)
{
    Machine m(architecture);
    const uint32_t vector = (architecture == kV5 ? kHigh : 0) + 0x04;
    const uint32_t old_cpsr = kSystemCpsr | (thumb ? psr::kT : 0);

    const StopReason reason = thumb ? RunThumbTo(m, {static_cast<uint16_t>(op)}, vector) : RunArmTo(m, {op}, vector);

    return reason == StopReason::kStopAddress && m.cpu.Pc() == vector && m.cpu.Cpsr() == 0xDB &&
           m.cpu.Reg(14) == kCode + (thumb ? 2 : 4) && m.cpu.Spsr() == old_cpsr;
}

void TestArithmetic()
{
    Machine m(kV4);
    m.cpu.SetReg(0, 0x80000000);
    m.cpu.SetReg(3, 1);
    m.cpu.SetReg(12, 0x80000000);
    SetNzcv(m.cpu, 0);

    // Two negatives that add to 0 carry out and overflow.
    RunArm(m, {0xE0901000}); // adds r1, r0, r0
    XSFLIB_CHECK(m.cpu.Reg(1) == 0 && Nzcv(m.cpu) == 0b0111);

    XSFLIB_CHECK(m.cpu.Cpsr() == (0x70000000 | kSystemCpsr));

    // A borrow clears C.
    RunArm(m, {0xE2534002}); // subs r4, r3, #2
    XSFLIB_CHECK(m.cpu.Reg(4) == 0xFFFFFFFF && Nzcv(m.cpu) == 0b1000);

    // ADC adds C, without setting the flags.
    RunArm(m, {0xE2A36010}); // adc r6, r3, #0x10
    XSFLIB_CHECK(m.cpu.Reg(6) == 0x11 && Nzcv(m.cpu) == 0b1000);

    SetNzcv(m.cpu, 0b0010);

    RunArm(m, {0xE2A36010}); // adc r6, r3, #0x10
    XSFLIB_CHECK(m.cpu.Reg(6) == 0x12);

    // No borrow sets C.
    RunArm(m, {0xE2537001}); // subs r7, r3, #1
    XSFLIB_CHECK(m.cpu.Reg(7) == 0 && Nzcv(m.cpu) == 0b0110);

    // SBC subtracts NOT C.
    RunArm(m, {0xE2C38000}); // sbc r8, r3, #0
    XSFLIB_CHECK(m.cpu.Reg(8) == 1);

    SetNzcv(m.cpu, 0);

    RunArm(m, {0xE2C38000}); // sbc r8, r3, #0
    XSFLIB_CHECK(m.cpu.Reg(8) == 0);

    RunArm(m, {0xE2739000}); // rsbs r9, r3, #0
    XSFLIB_CHECK(m.cpu.Reg(9) == 0xFFFFFFFF && Nzcv(m.cpu) == 0b1000);

    // RSC: 5 - 1 - NOT C, with C clear.
    RunArm(m, {0xE2E3A005}); // rsc r10, r3, #5
    XSFLIB_CHECK(m.cpu.Reg(10) == 3);

    RunArm(m, {0xE3730001}); // cmn r3, #1
    XSFLIB_CHECK(Nzcv(m.cpu) == 0b0000 && m.cpu.Reg(3) == 1);

    // The smallest negative minus 1 overflows to the largest positive, with no borrow.
    RunArm(m, {0xE25CC001}); // subs r12, r12, #1
    XSFLIB_CHECK(m.cpu.Reg(12) == 0x7FFFFFFF && Nzcv(m.cpu) == 0b0011);
}

void TestLogical()
{
    Machine m(kV4);
    m.cpu.SetReg(1, 0x80000000);
    m.cpu.SetReg(2, 0x10);

    // An immediate with a rotation gives C its top bit. V stays.
    SetNzcv(m.cpu, 0b0001);

    RunArm(m, {0xE3110102}); // tst r1, #0x80000000
    XSFLIB_CHECK(Nzcv(m.cpu) == 0b1011);

    // A register without a shift leaves C.
    SetNzcv(m.cpu, 0b0010);

    RunArm(m, {0xE1310001}); // teq r1, r1
    XSFLIB_CHECK(Nzcv(m.cpu) == 0b0110);

    RunArm(m, {0xE1810002}); // orr r0, r1, r2
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x80000010);

    m.cpu.SetReg(1, 0x800000FF);

    RunArm(m, {0xE3C100FF}); // bic r0, r1, #0xFF
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x80000000);

    SetNzcv(m.cpu, 0);

    RunArm(m, {0xE1F00001}); // mvns r0, r1
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x7FFFFF00 && Nzcv(m.cpu) == 0b0000);

    // 0x180 ROR 8 is 0x80000001.
    m.cpu.SetReg(1, 0x80000001);
    m.cpu.SetReg(2, 0x180);

    RunArm(m, {0xE0210462}); // eor r0, r1, r2, ror #8
    XSFLIB_CHECK(m.cpu.Reg(0) == 0);

    // An immediate without a rotation leaves C.
    SetNzcv(m.cpu, 0b0010);

    RunArm(m, {0xE3B000FF}); // movs r0, #0xFF
    XSFLIB_CHECK(m.cpu.Reg(0) == 0xFF && Nzcv(m.cpu) == 0b0010);

    SetNzcv(m.cpu, 0);

    RunArm(m, {0xE3B00102}); // movs r0, #0x80000000
    XSFLIB_CHECK(Nzcv(m.cpu) == 0b1010);

    // 0x3FC is 0xFF ROR 30: a rotation, with bit 31 clear.
    SetNzcv(m.cpu, 0b0010);

    RunArm(m, {0xE2110FFF}); // ands r0, r1, #0x3FC
    XSFLIB_CHECK(m.cpu.Reg(0) == 0 && Nzcv(m.cpu) == 0b0100);
}

void TestShifts()
{
    Machine m(kV4);
    m.cpu.SetReg(1, 0x80000001);

    // Shifts by an immediate.
    SetNzcv(m.cpu, 0b0001);

    RunArm(m, {0xE1B00081}); // movs r0, r1, lsl #1
    XSFLIB_CHECK(m.cpu.Reg(0) == 2 && Nzcv(m.cpu) == 0b0011);

    SetNzcv(m.cpu, 0b0010);

    RunArm(m, {0xE1B00001}); // movs r0, r1
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x80000001 && Nzcv(m.cpu) == 0b1010);

    SetNzcv(m.cpu, 0);

    RunArm(m, {0xE1B00001}); // movs r0, r1
    XSFLIB_CHECK(Nzcv(m.cpu) == 0b1000);

    RunArm(m, {0xE1B00021}); // movs r0, r1, lsr #32
    XSFLIB_CHECK(m.cpu.Reg(0) == 0 && Nzcv(m.cpu) == 0b0110);

    RunArm(m, {0xE1B000A1}); // movs r0, r1, lsr #1
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x40000000 && Nzcv(m.cpu) == 0b0010);

    RunArm(m, {0xE1B00041}); // movs r0, r1, asr #32
    XSFLIB_CHECK(m.cpu.Reg(0) == 0xFFFFFFFF && Nzcv(m.cpu) == 0b1010);

    RunArm(m, {0xE1B00241}); // movs r0, r1, asr #4
    XSFLIB_CHECK(m.cpu.Reg(0) == 0xF8000000 && Nzcv(m.cpu) == 0b1000);

    RunArm(m, {0xE1B00061}); // movs r0, r1, rrx
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x40000000 && Nzcv(m.cpu) == 0b0010);

    RunArm(m, {0xE1B00061}); // movs r0, r1, rrx
    XSFLIB_CHECK(m.cpu.Reg(0) == 0xC0000000 && Nzcv(m.cpu) == 0b1010);

    RunArm(m, {0xE1B00261}); // movs r0, r1, ror #4
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x18000000 && Nzcv(m.cpu) == 0b0000);

    // Shifts by a register's low byte. An amount of 0 leaves the value and C.
    m.cpu.SetReg(2, 0x100);
    SetNzcv(m.cpu, 0b0010);

    RunArm(m, {0xE1B00211}); // movs r0, r1, lsl r2
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x80000001 && Nzcv(m.cpu) == 0b1010);

    m.cpu.SetReg(2, 32);
    SetNzcv(m.cpu, 0);

    RunArm(m, {0xE1B00211}); // movs r0, r1, lsl r2
    XSFLIB_CHECK(m.cpu.Reg(0) == 0 && Nzcv(m.cpu) == 0b0110);

    m.cpu.SetReg(2, 33);

    RunArm(m, {0xE1B00211}); // movs r0, r1, lsl r2
    XSFLIB_CHECK(m.cpu.Reg(0) == 0 && Nzcv(m.cpu) == 0b0100);

    m.cpu.SetReg(2, 32);

    RunArm(m, {0xE1B00231}); // movs r0, r1, lsr r2
    XSFLIB_CHECK(m.cpu.Reg(0) == 0 && Nzcv(m.cpu) == 0b0110);

    m.cpu.SetReg(2, 33);

    RunArm(m, {0xE1B00231}); // movs r0, r1, lsr r2
    XSFLIB_CHECK(m.cpu.Reg(0) == 0 && Nzcv(m.cpu) == 0b0100);

    m.cpu.SetReg(2, 31);

    RunArm(m, {0xE1B00231}); // movs r0, r1, lsr r2
    XSFLIB_CHECK(m.cpu.Reg(0) == 1 && Nzcv(m.cpu) == 0b0000);

    m.cpu.SetReg(2, 200);

    RunArm(m, {0xE1B00251}); // movs r0, r1, asr r2
    XSFLIB_CHECK(m.cpu.Reg(0) == 0xFFFFFFFF && Nzcv(m.cpu) == 0b1010);

    // A rotation by a multiple of 32 leaves the value and gives C its top bit.
    m.cpu.SetReg(2, 32);
    SetNzcv(m.cpu, 0);

    RunArm(m, {0xE1B00271}); // movs r0, r1, ror r2
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x80000001 && Nzcv(m.cpu) == 0b1010);

    m.cpu.SetReg(2, 0x104);

    RunArm(m, {0xE1B00271}); // movs r0, r1, ror r2
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x18000000 && Nzcv(m.cpu) == 0b0000);
}

void TestPcOperands()
{
    Machine m(kV4);
    m.cpu.SetReg(1, 0x10);
    m.cpu.SetReg(2, 0);

    // r15 reads as the instruction's address plus 8, or plus 12 with a shift by a register.
    RunArm(m, {0xE28F0000}); // add r0, pc, #0
    XSFLIB_CHECK(m.cpu.Reg(0) == kCode + 8);

    RunArm(m, {0xE1A0000F}); // mov r0, pc
    XSFLIB_CHECK(m.cpu.Reg(0) == kCode + 8);

    RunArm(m, {0xE08F0211}); // add r0, pc, r1, lsl r2
    XSFLIB_CHECK(m.cpu.Reg(0) == kCode + 12 + 0x10);

    RunArm(m, {0xE1A0021F}); // mov r0, pc, lsl r2
    XSFLIB_CHECK(m.cpu.Reg(0) == kCode + 12);

    // A store of r15 stores the address plus 12.
    m.cpu.SetReg(1, kData);

    RunArm(m, {0xE581F000}); // str pc, [r1]
    XSFLIB_CHECK(m.bus.Get32(kData) == kCode + 12);

    m.bus.Put32(kData, {0});

    RunArm(m, {0xE8818000}); // stmia r1, {pc}
    XSFLIB_CHECK(m.bus.Get32(kData) == kCode + 12);

    // A write to r15 clears the low bits in ARM state.
    m.cpu.SetReg(1, 0x2002);
    XSFLIB_CHECK(RunArmTo(m, {0xE1A0F001}, 0x2000) == StopReason::kStopAddress); // mov pc, r1
    XSFLIB_CHECK(m.cpu.Pc() == 0x2000);
}

void TestMultiply()
{
    for (const Architecture architecture : {kV4, kV5})
    {
        Machine m(architecture);

        m.cpu.SetReg(1, 0xFFFFFFFF);
        m.cpu.SetReg(2, 5);

        RunArm(m, {0xE0000291}); // mul r0, r1, r2
        XSFLIB_CHECK(m.cpu.Reg(0) == 0xFFFFFFFB);

        m.cpu.SetReg(1, 3);
        m.cpu.SetReg(2, 4);
        m.cpu.SetReg(3, 5);

        RunArm(m, {0xE0203291}); // mla r0, r1, r2, r3
        XSFLIB_CHECK(m.cpu.Reg(0) == 17);

        // MULS sets N and Z and leaves C and V.
        m.cpu.SetReg(1, 0x10000);
        m.cpu.SetReg(2, 0x10000);
        SetNzcv(m.cpu, 0b0011);

        RunArm(m, {0xE0100291}); // muls r0, r1, r2
        XSFLIB_CHECK(m.cpu.Reg(0) == 0 && Nzcv(m.cpu) == 0b0111);

        m.cpu.SetReg(2, 0xFFFFFFFF);
        m.cpu.SetReg(3, 2);

        RunArm(m, {0xE0810392}); // umull r0, r1, r2, r3
        XSFLIB_CHECK(m.cpu.Reg(0) == 0xFFFFFFFE && m.cpu.Reg(1) == 1);

        RunArm(m, {0xE0C10392}); // smull r0, r1, r2, r3
        XSFLIB_CHECK(m.cpu.Reg(0) == 0xFFFFFFFE && m.cpu.Reg(1) == 0xFFFFFFFF);

        // 0xFFFFFFFF squared is 0xFFFFFFFE00000001.
        m.cpu.SetReg(0, 1);
        m.cpu.SetReg(1, 0);
        m.cpu.SetReg(3, 0xFFFFFFFF);

        RunArm(m, {0xE0A10392}); // umlal r0, r1, r2, r3
        XSFLIB_CHECK(m.cpu.Reg(0) == 2 && m.cpu.Reg(1) == 0xFFFFFFFE);

        // -2^31 * 2 is -2^32.
        m.cpu.SetReg(0, 0);
        m.cpu.SetReg(1, 0);
        m.cpu.SetReg(2, 0x80000000);
        m.cpu.SetReg(3, 2);
        SetNzcv(m.cpu, 0);

        RunArm(m, {0xE0F10392}); // smlals r0, r1, r2, r3
        XSFLIB_CHECK(m.cpu.Reg(0) == 0 && m.cpu.Reg(1) == 0xFFFFFFFF && Nzcv(m.cpu) == 0b1000);

        m.cpu.SetReg(2, 0);

        RunArm(m, {0xE0910392}); // umulls r0, r1, r2, r3
        XSFLIB_CHECK(m.cpu.Reg(0) == 0 && m.cpu.Reg(1) == 0 && Nzcv(m.cpu) == 0b0100);
    }
}

void TestPsrTransfer()
{
    // The flags field. Only ARMv5TE has the Q flag.
    Machine v4(kV4);
    v4.cpu.SetReg(1, 0xFFFFFFFF);

    RunArm(v4, {0xE128F001}); // msr cpsr_f, r1
    XSFLIB_CHECK(v4.cpu.Cpsr() == (0xF0000000 | kSystemCpsr));

    Machine v5(kV5);
    v5.cpu.SetReg(1, 0xFFFFFFFF);

    RunArm(v5, {0xE128F001}); // msr cpsr_f, r1
    XSFLIB_CHECK(v5.cpu.Cpsr() == (0xF8000000 | kSystemCpsr));

    // The control field changes the mode and the interrupt masks, but not the T bit.
    Machine m(kV4);
    m.cpu.SetReg(1, 0x32);

    RunArm(m, {0xE121F001}); // msr cpsr_c, r1
    XSFLIB_CHECK(m.cpu.Cpsr() == 0x12);

    m.cpu.SetReg(0, 0);

    RunArm(m, {0xE10F0000}); // mrs r0, cpsr
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x12);

    RunArm(m, {0xE328F20F}); // msr cpsr_f, #0xF0000000
    XSFLIB_CHECK(m.cpu.Cpsr() == 0xF0000012);

    // The SPSR takes every field, the T bit included.
    m.cpu.SetReg(1, 0xFFFFFFFF);

    RunArm(m, {0xE16FF001}); // msr spsr_fsxc, r1
    XSFLIB_CHECK(m.cpu.Spsr() == 0xF00000FF);

    RunArm(m, {0xE14F2000}); // mrs r2, spsr
    XSFLIB_CHECK(m.cpu.Reg(2) == 0xF00000FF);

    // User mode writes only the flags. It has no SPSR.
    m.cpu.Jump(kCode, Mode::kUser);
    m.cpu.SetReg(1, 0xA00000DF);

    RunArm(m, {0xE129F001}); // msr cpsr_fc, r1
    XSFLIB_CHECK(m.cpu.Cpsr() == 0xA0000010);

    RunArm(m, {0xE16FF001}); // msr spsr_fsxc, r1
    RunArm(m, {0xE14F2000}); // mrs r2, spsr
    XSFLIB_CHECK(m.cpu.Spsr() == 0 && m.cpu.Reg(2) == 0);
}

void TestBanks()
{
    Machine m(kV4);

    // FIQ mode banks r8 to r14, IRQ mode r13 and r14.
    RunArm(m, {
                  0xE3A08088, // mov r8, #0x88
                  0xE3A0D013, // mov r13, #0x13
                  0xE3A0E014, // mov r14, #0x14
                  0xE321F0D1, // msr cpsr_c, #0xD1
                  0xE3A08098, // mov r8, #0x98
                  0xE3A0D093, // mov r13, #0x93
                  0xE321F0D2, // msr cpsr_c, #0xD2
                  0xE3A0D0A3, // mov r13, #0xA3
                  0xE1A01008, // mov r1, r8
                  0xE321F0DF, // msr cpsr_c, #0xDF
              });

    XSFLIB_CHECK(m.cpu.Reg(8) == 0x88 && m.cpu.Reg(13) == 0x13 && m.cpu.Reg(14) == 0x14 && m.cpu.Reg(1) == 0x88);

    m.cpu.Jump(kCode, Mode::kFiq);
    XSFLIB_CHECK(m.cpu.Reg(8) == 0x98 && m.cpu.Reg(13) == 0x93 && m.cpu.Reg(14) == 0);

    m.cpu.SetCpsr(0xD2);
    XSFLIB_CHECK(m.cpu.Reg(8) == 0x88 && m.cpu.Reg(13) == 0xA3);

    m.cpu.SetCpsr(0xD0);
    XSFLIB_CHECK(m.cpu.Reg(13) == 0x13 && m.cpu.Spsr() == 0);
}

void TestSingleTransfer()
{
    Machine m(kV4);

    // Offsets: pre-indexed, with writeback, post-indexed, and a shifted register subtracted.
    m.cpu.SetReg(1, kData);

    RunArm(m, {0xE5910004}); // ldr r0, [r1, #4]
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x88776655 && m.cpu.Reg(1) == kData);

    RunArm(m, {0xE5B10004}); // ldr r0, [r1, #4]!
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x88776655 && m.cpu.Reg(1) == kData + 4);

    m.cpu.SetReg(1, kData);

    RunArm(m, {0xE4910004}); // ldr r0, [r1], #4
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x44332211 && m.cpu.Reg(1) == kData + 4);

    m.cpu.SetReg(1, kData + 8);
    m.cpu.SetReg(2, 1);

    RunArm(m, {0xE7110102}); // ldr r0, [r1, -r2, lsl #2]
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x88776655 && m.cpu.Reg(1) == kData + 8);

    RunArm(m, {0xE5310004}); // ldr r0, [r1, #-4]!
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x88776655 && m.cpu.Reg(1) == kData + 4);

    // An unaligned LDR rotates the aligned word: the addressed byte goes to the low byte.
    m.cpu.SetReg(1, kData + 1);

    RunArm(m, {0xE5910000}); // ldr r0, [r1]
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x11443322);

    m.cpu.SetReg(1, kData + 2);

    RunArm(m, {0xE5910000}); // ldr r0, [r1]
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x22114433);

    m.cpu.SetReg(1, kData + 3);

    RunArm(m, {0xE5910000}); // ldr r0, [r1]
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x33221144);

    m.cpu.SetReg(1, kData + 2);

    RunArm(m, {0xE5D10000}); // ldrb r0, [r1]
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x33);

    m.cpu.SetReg(1, kData + 5);
    m.cpu.SetReg(2, 2);

    RunArm(m, {0xE6510002}); // ldrb r0, [r1], -r2
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x66 && m.cpu.Reg(1) == kData + 3);

    // An unaligned STR stores to the aligned word, unrotated.
    m.cpu.SetReg(0, 0xDEADBEEF);
    m.cpu.SetReg(1, kData + 2);

    RunArm(m, {0xE5810000}); // str r0, [r1]
    XSFLIB_CHECK(m.bus.Get32(kData) == 0xDEADBEEF);

    m.cpu.SetReg(0, 0x1234);
    m.cpu.SetReg(1, kData);

    RunArm(m, {0xE5C10003}); // strb r0, [r1, #3]
    XSFLIB_CHECK(m.bus.Get32(kData) == 0x34ADBEEF);

    m.cpu.SetReg(2, 4);

    RunArm(m, {0xE7A10082}); // str r0, [r1, r2, lsl #1]!
    XSFLIB_CHECK(m.bus.Get32(kData + 8) == 0x1234 && m.cpu.Reg(1) == kData + 8);
}

void TestHalfwordTransfer()
{
    for (const Architecture architecture : {kV4, kV5})
    {
        Machine m(architecture);
        const bool v4 = architecture == kV4;

        m.cpu.SetReg(1, kData);

        RunArm(m, {0xE1D100B2}); // ldrh r0, [r1, #2]
        XSFLIB_CHECK(m.cpu.Reg(0) == 0x4433);

        RunArm(m, {0xE1D100D7}); // ldrsb r0, [r1, #7]
        XSFLIB_CHECK(m.cpu.Reg(0) == 0xFFFFFF88);

        RunArm(m, {0xE1D100F6}); // ldrsh r0, [r1, #6]
        XSFLIB_CHECK(m.cpu.Reg(0) == 0xFFFF8877);

        m.cpu.SetReg(2, 4);

        RunArm(m, {0xE1B100B2}); // ldrh r0, [r1, r2]!
        XSFLIB_CHECK(m.cpu.Reg(0) == 0x6655 && m.cpu.Reg(1) == kData + 4);

        m.cpu.SetReg(1, kData + 8);

        RunArm(m, {0xE05100B2}); // ldrh r0, [r1], #-2
        XSFLIB_CHECK(m.cpu.Reg(0) == 0xAA99 && m.cpu.Reg(1) == kData + 6);

        m.cpu.SetReg(1, kData + 16);

        RunArm(m, {0xE11100F2}); // ldrsh r0, [r1, -r2]
        XSFLIB_CHECK(m.cpu.Reg(0) == 0xFFFFEEDD);

        // At an odd address, the ARM7TDMI rotates LDRH's halfword and turns LDRSH into LDRSB. The ARM946E-S reads the
        // aligned halfword.
        m.cpu.SetReg(1, kData);

        RunArm(m, {0xE1D100B1}); // ldrh r0, [r1, #1]
        XSFLIB_CHECK(m.cpu.Reg(0) == (v4 ? 0x11000022 : 0x2211));

        RunArm(m, {0xE1D100F7}); // ldrsh r0, [r1, #7]
        XSFLIB_CHECK(m.cpu.Reg(0) == (v4 ? 0xFFFFFF88 : 0xFFFF8877));

        // STRH stores to the aligned halfword.
        m.cpu.SetReg(0, 0xABCD1234);

        RunArm(m, {0xE1C100B1}); // strh r0, [r1, #1]
        XSFLIB_CHECK(m.bus.Get32(kData) == 0x44331234);
    }
}

void TestDoublewordTransfer()
{
    Machine m(kV5);

    m.cpu.SetReg(1, kData);

    RunArm(m, {0xE1C120D8}); // ldrd r2, [r1, #8]
    XSFLIB_CHECK(m.cpu.Reg(2) == 0xCCBBAA99 && m.cpu.Reg(3) == 0x00FFEEDD);

    m.cpu.SetReg(4, 8);

    RunArm(m, {0xE08120D4}); // ldrd r2, r3, [r1], r4
    XSFLIB_CHECK(m.cpu.Reg(2) == 0x44332211 && m.cpu.Reg(3) == 0x88776655 && m.cpu.Reg(1) == kData + 8);

    m.cpu.SetReg(1, kData + 16);
    m.cpu.SetReg(2, 0x11111111);
    m.cpu.SetReg(3, 0x22222222);

    RunArm(m, {0xE16120F8}); // strd r2, [r1, #-8]!
    XSFLIB_CHECK(m.bus.Get32(kData + 8) == 0x11111111 && m.bus.Get32(kData + 12) == 0x22222222);

    XSFLIB_CHECK(m.cpu.Reg(1) == kData + 8);

    XSFLIB_CHECK(RaisesUndefined(kV4, 0xE1C120D8, false)); // ldrd r2, [r1, #8]
    XSFLIB_CHECK(RaisesUndefined(kV4, 0xE16120F8, false)); // strd r2, [r1, #-8]!
}

void TestSwap()
{
    Machine m(kV4);

    m.cpu.SetReg(1, kData);
    m.cpu.SetReg(2, 0xCAFEBABE);

    RunArm(m, {0xE1010092}); // swp r0, r2, [r1]
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x44332211 && m.bus.Get32(kData) == 0xCAFEBABE);

    m.cpu.SetReg(1, kData + 5);
    m.cpu.SetReg(2, 0x1FF);

    RunArm(m, {0xE1410092}); // swpb r0, r2, [r1]
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x66 && m.bus.Get32(kData + 4) == 0x8877FF55);

    // The load rotates as LDR does. The store goes to the aligned word, unrotated.
    m.cpu.SetReg(1, kData + 9);
    m.cpu.SetReg(2, 0x01020304);

    RunArm(m, {0xE1010092}); // swp r0, r2, [r1]
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x99CCBBAA && m.bus.Get32(kData + 8) == 0x01020304);
}

void TestBlockTransfer()
{
    Machine m(kV4);
    m.cpu.SetReg(1, 1);
    m.cpu.SetReg(2, 2);
    m.cpu.SetReg(3, 3);

    m.cpu.SetReg(0, kData + 0x10);

    RunArm(m, {0xE920000E}); // stmdb r0!, {r1-r3}
    XSFLIB_CHECK(m.bus.Get32(kData + 4) == 1 && m.bus.Get32(kData + 8) == 2 && m.bus.Get32(kData + 12) == 3);

    XSFLIB_CHECK(m.cpu.Reg(0) == kData + 4);

    RunArm(m, {0xE8B00070}); // ldmia r0!, {r4-r6}
    XSFLIB_CHECK(m.cpu.Reg(4) == 1 && m.cpu.Reg(5) == 2 && m.cpu.Reg(6) == 3 && m.cpu.Reg(0) == kData + 0x10);

    // The four modes, from the lowest address up.
    m.cpu.SetReg(1, 0xA1);
    m.cpu.SetReg(2, 0xA2);
    m.cpu.SetReg(0, kData);

    RunArm(m, {0xE9800006}); // stmib r0, {r1, r2}
    XSFLIB_CHECK(m.bus.Get32(kData + 4) == 0xA1 && m.bus.Get32(kData + 8) == 0xA2 && m.cpu.Reg(0) == kData);

    m.cpu.SetReg(1, 0xB1);
    m.cpu.SetReg(2, 0xB2);
    m.cpu.SetReg(0, kData + 8);

    RunArm(m, {0xE8000006}); // stmda r0, {r1, r2}
    XSFLIB_CHECK(m.bus.Get32(kData + 4) == 0xB1 && m.bus.Get32(kData + 8) == 0xB2);

    m.bus.Put32(kData + 4, {0xC1, 0xC2});

    RunArm(m, {0xE8100006}); // ldmda r0, {r1, r2}
    XSFLIB_CHECK(m.cpu.Reg(1) == 0xC1 && m.cpu.Reg(2) == 0xC2);

    m.bus.Put32(kData + 4, {0xD1, 0xD2});
    m.cpu.SetReg(0, kData);

    RunArm(m, {0xE9900006}); // ldmib r0, {r1, r2}
    XSFLIB_CHECK(m.cpu.Reg(1) == 0xD1 && m.cpu.Reg(2) == 0xD2);

    m.bus.Put32(kData + 4, {0xE1, 0xE2});
    m.cpu.SetReg(0, kData + 12);

    RunArm(m, {0xE9300006}); // ldmdb r0!, {r1, r2}
    XSFLIB_CHECK(m.cpu.Reg(1) == 0xE1 && m.cpu.Reg(2) == 0xE2 && m.cpu.Reg(0) == kData + 4);
}

void TestBaseInList()
{
    for (const Architecture architecture : {kV4, kV5})
    {
        Machine m(architecture);
        const bool v4 = architecture == kV4;

        // STM stores the old base when it's the first register. Otherwise the ARM7TDMI stores the new base.
        m.cpu.SetReg(0, kData);
        m.cpu.SetReg(1, 0x11);

        RunArm(m, {0xE8A00003}); // stmia r0!, {r0, r1}
        XSFLIB_CHECK(m.bus.Get32(kData) == kData && m.bus.Get32(kData + 4) == 0x11 && m.cpu.Reg(0) == kData + 8);

        m.cpu.SetReg(0, 0x22);
        m.cpu.SetReg(1, kData);

        RunArm(m, {0xE8A10003}); // stmia r1!, {r0, r1}
        XSFLIB_CHECK(m.bus.Get32(kData) == 0x22 && m.cpu.Reg(1) == kData + 8);

        XSFLIB_CHECK(m.bus.Get32(kData + 4) == (v4 ? kData + 8 : kData));

        m.cpu.SetReg(0, kData + 8);

        RunArm(m, {0xE9200003}); // stmdb r0!, {r0, r1}
        XSFLIB_CHECK(m.bus.Get32(kData) == kData + 8 && m.cpu.Reg(0) == kData);

        // LDM keeps the loaded base on the ARM7TDMI. The ARM946E-S writes the new base back unless the base is the
        // last of two or more registers.
        m.bus.Put32(kData, {0xA0, 0xA1});
        m.cpu.SetReg(0, kData);

        RunArm(m, {0xE8B00003}); // ldmia r0!, {r0, r1}
        XSFLIB_CHECK(m.cpu.Reg(0) == (v4 ? 0xA0 : kData + 8) && m.cpu.Reg(1) == 0xA1);

        m.cpu.SetReg(1, kData);

        RunArm(m, {0xE8B10003}); // ldmia r1!, {r0, r1}
        XSFLIB_CHECK(m.cpu.Reg(0) == 0xA0 && m.cpu.Reg(1) == 0xA1);

        m.cpu.SetReg(0, kData);

        RunArm(m, {0xE8B00001}); // ldmia r0!, {r0}
        XSFLIB_CHECK(m.cpu.Reg(0) == (v4 ? 0xA0 : kData + 4));

        m.cpu.SetReg(0, kData + 8);

        RunArm(m, {0xE9300003}); // ldmdb r0!, {r0, r1}
        XSFLIB_CHECK(m.cpu.Reg(0) == (v4 ? 0xA0 : kData));
    }
}

void TestEmptyList()
{
    for (const Architecture architecture : {kV4, kV5})
    {
        Machine m(architecture);
        const bool v4 = architecture == kV4;

        // The base moves by 0x40. The ARM7TDMI transfers r15 alone, at the place where a list of 16 registers would
        // start.
        m.cpu.SetReg(0, kData);

        RunArm(m, {0xE8A00000}); // stmia r0!, {}
        XSFLIB_CHECK(m.cpu.Reg(0) == kData + 0x40);

        XSFLIB_CHECK(m.bus.Get32(kData) == (v4 ? kCode + 12 : 0x44332211));

        m.bus.Put32(kData, {0x44332211});

        RunArm(m, {0xE9200000}); // stmdb r0!, {}
        XSFLIB_CHECK(m.cpu.Reg(0) == kData);

        XSFLIB_CHECK(m.bus.Get32(kData) == (v4 ? kCode + 12 : 0x44332211));

        m.bus.Put32(kData, {0x3000});

        const StopReason reason = RunArmTo(m, {0xE8B00000}, v4 ? 0x3000 : kCode + 4); // ldmia r0!, {}
        XSFLIB_CHECK(reason == StopReason::kStopAddress && m.cpu.Reg(0) == kData + 0x40);

        XSFLIB_CHECK(m.cpu.Pc() == (v4 ? 0x3000 : kCode + 4));
    }
}

void TestUserBank()
{
    Machine m(kV4);
    m.cpu.SetReg(8, 0x800);
    m.cpu.SetReg(13, 0x1300);
    m.cpu.SetReg(14, 0x1400);
    m.cpu.Jump(kCode, Mode::kIrq);
    m.cpu.SetReg(13, 0x2300);
    m.cpu.SetReg(14, 0x2400);

    // With the S bit and no r15 to load, LDM and STM transfer user mode's registers.
    m.cpu.SetReg(0, kData);

    RunArm(m, {0xE8C06000}); // stmia r0, {r13, r14}^
    XSFLIB_CHECK(m.bus.Get32(kData) == 0x1300 && m.bus.Get32(kData + 4) == 0x1400);

    m.bus.Put32(kData, {0x5555, 0x6666});

    RunArm(m, {0xE8D06000}); // ldmia r0, {r13, r14}^
    XSFLIB_CHECK(m.cpu.Reg(13) == 0x2300 && m.cpu.Reg(14) == 0x2400);

    m.cpu.Jump(kCode, Mode::kFiq);
    m.cpu.SetReg(8, 0xF800);

    RunArm(m, {0xE8C00100}); // stmia r0, {r8}^
    XSFLIB_CHECK(m.bus.Get32(kData) == 0x800);

    m.bus.Put32(kData, {0x777});

    RunArm(m, {0xE8D00100}); // ldmia r0, {r8}^
    XSFLIB_CHECK(m.cpu.Reg(8) == 0xF800);

    m.cpu.Jump(kCode, Mode::kSystem);
    XSFLIB_CHECK(m.cpu.Reg(8) == 0x777 && m.cpu.Reg(13) == 0x5555 && m.cpu.Reg(14) == 0x6666);
}

void TestExceptionReturns()
{
    Machine m(kV4);

    // LDM with r15 and the S bit restores the CPSR, and the PC takes the restored state.
    m.cpu.Jump(kCode, Mode::kIrq);
    m.cpu.SetReg(13, kData);
    m.bus.Put32(kData, {0x55, 0x2000});
    m.cpu.SetCpsr(0xD2);
    m.cpu.SetReg(0, 0);

    RunArm(m, {0xE16FF001}); // msr spsr_fsxc, r1 (r1 = 0)
    m.cpu.SetReg(1, 0x30);

    RunArm(m, {0xE16FF001}); // msr spsr_fsxc, r1

    const StopReason reason = RunArmTo(m, {0xE8FD8001}, 0x2000); // ldmia r13!, {r0, pc}^

    XSFLIB_CHECK(reason == StopReason::kStopAddress && m.cpu.Pc() == 0x2001 && m.cpu.Cpsr() == 0x30);

    XSFLIB_CHECK(m.cpu.Reg(0) == 0x55);

    m.cpu.Jump(kCode, Mode::kIrq);
    XSFLIB_CHECK(m.cpu.Reg(13) == kData + 8);

    // MOVS PC, LR and SUBS PC, LR, #4.
    m.cpu.Jump(kCode, Mode::kSupervisor);
    m.cpu.SetReg(1, 0x1F);

    RunArm(m, {0xE16FF001}); // msr spsr_fsxc, r1
    m.cpu.SetReg(14, 0x2004);

    XSFLIB_CHECK(RunArmTo(m, {0xE1B0F00E}, 0x2004) == StopReason::kStopAddress); // movs pc, lr
    XSFLIB_CHECK(m.cpu.Cpsr() == 0x1F);

    m.cpu.Jump(kCode, Mode::kIrq);
    m.cpu.SetReg(1, 0xF0000010);

    RunArm(m, {0xE16FF001}); // msr spsr_fsxc, r1
    m.cpu.SetReg(14, 0x2008);

    XSFLIB_CHECK(RunArmTo(m, {0xE25EF004}, 0x2004) == StopReason::kStopAddress); // subs pc, lr, #4
    XSFLIB_CHECK(m.cpu.Cpsr() == 0xF0000010);
}

void TestBranches()
{
    Machine m(kV4);

    const StopReason reason = RunArmTo(m,
                                       {
                                           0xE3A00000, // mov r0, #0
                                           // loop:
                                           0xE2800001, // add r0, r0, #1
                                           0xE3500005, // cmp r0, #5
                                           0x1AFFFFFC, // bne loop
                                           0xEB000000, // bl function
                                           0xEA000001, // b end
                                           // function:
                                           0xE3A01009, // mov r1, #9
                                           0xE12FFF1E, // bx lr
                                           // end:
                                       },
                                       kCode + 0x20);

    XSFLIB_CHECK(reason == StopReason::kStopAddress);
    XSFLIB_CHECK(m.cpu.Reg(0) == 5 && m.cpu.Reg(1) == 9 && m.cpu.Reg(14) == kCode + 0x14);
}

void TestConditions()
{
    // For each set of flags, the conditions EQ to AL (bits 0 to 14) that pass.
    struct Case
    {
        uint32_t nzcv;
        uint32_t passing;
    };

    constexpr Case kCases[] = {{0b0000, 0x56AA}, {0b0100, 0x66A9}, {0b0010, 0x55A6}, {0b0110, 0x66A5},
                               {0b1000, 0x6A9A}, {0b1001, 0x565A}, {0b0001, 0x6A6A}, {0b1101, 0x6659}};
    for (const Case& test : kCases)
    {
        uint32_t passing = 0;
        for (uint32_t cond = 0; cond < 15; ++cond)
        {
            Machine m(kV4);
            m.cpu.SetReg(0, 0);
            SetNzcv(m.cpu, test.nzcv);

            RunArm(m, {(cond << 28) | 0x03A00001}); // mov<cond> r0, #1

            passing |= m.cpu.Reg(0) << cond;
        }

        XSFLIB_CHECK(passing == test.passing);
    }
}

void TestStateChanges()
{
    Machine m(kV4);
    m.bus.Put32(kCode, {
                           0xE28F2001, // add r2, pc, #1
                           0xE12FFF12, // bx r2
                       });
    m.bus.Put16(kCode + 8, {
                               0x2007, // movs r0, #7
                               0x46C0, // mov r8, r8
                               0x4778, // bx pc
                               0xDE00, // (skipped)
                           });
    m.bus.Put32(kCode + 16, {0xE3A01009}); // mov r1, #9
    m.cpu.SetStopAddress(kCode + 20);

    const StopReason reason = m.cpu.Run(100);

    XSFLIB_CHECK(reason == StopReason::kStopAddress && m.cpu.Pc() == kCode + 20);
    XSFLIB_CHECK(m.cpu.Reg(0) == 7 && m.cpu.Reg(1) == 9 && (m.cpu.Cpsr() & psr::kT) == 0);
}

void TestInterworking()
{
    for (const Architecture architecture : {kV4, kV5})
    {
        const bool v4 = architecture == kV4;

        // LDR and LDM to r15 switch to Thumb state by bit 0 on ARMv5TE only.
        Machine m(architecture);
        m.bus.Put32(kData, {0x2001});
        m.cpu.SetReg(1, kData);
        XSFLIB_CHECK(RunArmTo(m, {0xE591F000}, 0x2000) == StopReason::kStopAddress); // ldr pc, [r1]
        XSFLIB_CHECK(m.cpu.Pc() == (v4 ? 0x2000 : 0x2001));

        m.cpu.Jump(kCode, Mode::kSystem);
        XSFLIB_CHECK(RunArmTo(m, {0xE8918000}, 0x2000) == StopReason::kStopAddress); // ldmia r1, {pc}
        XSFLIB_CHECK(m.cpu.Pc() == (v4 ? 0x2000 : 0x2001));

        // POP {PC} stays in Thumb state on ARMv4T.
        m.bus.Put32(kStack - 4, {0x2000});
        m.cpu.SetReg(13, kStack - 4);
        XSFLIB_CHECK(RunThumbTo(m, {0xBD00}, 0x2000) == StopReason::kStopAddress); // pop {pc}
        XSFLIB_CHECK(m.cpu.Pc() == (v4 ? 0x2001 : 0x2000) && m.cpu.Reg(13) == kStack);
    }

    // CP15's bit 15 gives the ARM946E-S the ARMv4T behaviour.
    Machine m(kV5);
    m.bus.Put32(kData, {0x2001});
    m.cpu.SetReg(0, 0x2078 | 0x8000);
    m.cpu.SetReg(1, kData);

    const StopReason reason = RunArmTo(m,
                                       {
                                           0xEE010F10, // mcr p15, 0, r0, c1, c0, 0
                                           0xE591F000, // ldr pc, [r1]
                                       },
                                       0x2000);

    XSFLIB_CHECK(reason == StopReason::kStopAddress && m.cpu.Pc() == 0x2000);
}

void TestBlx()
{
    Machine m(kV5);
    m.bus.Put32(kCode, {
                           0xFB000000, // blx thumb
                           0xEA000004, // b end
                       });
    m.bus.Put16(kCode + 8, {
                               0xDE00, // (skipped)
                               // thumb:
                               0x4674, // mov r4, lr
                               0xF000,
                               0xE802, // blx arm
                               0x4675, // mov r5, lr
                               0x4720, // bx r4
                           });
    m.bus.Put32(kCode + 0x14, {
                                  // arm:
                                  0xE3A01003, // mov r1, #3
                                  0xE12FFF1E, // bx lr
                                              // end:
                              });
    m.cpu.SetStopAddress(kCode + 0x1C);

    const StopReason reason = m.cpu.Run(100);

    XSFLIB_CHECK(reason == StopReason::kStopAddress && m.cpu.Pc() == kCode + 0x1C);
    XSFLIB_CHECK(m.cpu.Reg(1) == 3 && m.cpu.Reg(4) == kCode + 4 && m.cpu.Reg(5) == kCode + 0x11);

    // BLX with a register, in both states.
    m.cpu.SetReg(2, 0x2001);
    XSFLIB_CHECK(RunArmTo(m, {0xE12FFF32}, 0x2000) == StopReason::kStopAddress); // blx r2
    XSFLIB_CHECK(m.cpu.Pc() == 0x2001 && m.cpu.Reg(14) == kCode + 4);

    m.cpu.SetReg(2, 0x2000);
    XSFLIB_CHECK(RunThumbTo(m, {0x4790}, 0x2000) == StopReason::kStopAddress); // blx r2
    XSFLIB_CHECK(m.cpu.Pc() == 0x2000 && m.cpu.Reg(14) == kCode + 3);
}

void TestCountLeadingZeros()
{
    Machine m(kV5);

    for (const auto& [value, zeros] : {std::pair<uint32_t, uint32_t>{0, 32}, {1, 31}, {0x80000000, 0}, {0x10000, 15}})
    {
        m.cpu.SetReg(1, value);

        RunArm(m, {0xE16F0F11}); // clz r0, r1
        XSFLIB_CHECK(m.cpu.Reg(0) == zeros);
    }
}

void TestSaturatingArithmetic()
{
    Machine m(kV5);
    SetNzcv(m.cpu, 0b0101);

    // Saturation sets Q. An operation that doesn't saturate leaves Q.
    m.cpu.SetReg(1, 0x7FFFFFFF);
    m.cpu.SetReg(2, 1);

    RunArm(m, {0xE1020051}); // qadd r0, r1, r2
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x7FFFFFFF && (m.cpu.Cpsr() & psr::kQ) != 0);

    m.cpu.SetReg(1, 2);
    m.cpu.SetReg(2, 3);

    RunArm(m, {0xE1020051}); // qadd r0, r1, r2
    XSFLIB_CHECK(m.cpu.Reg(0) == 5 && (m.cpu.Cpsr() & psr::kQ) != 0);

    m.cpu.SetCpsr(m.cpu.Cpsr() & ~psr::kQ);

    RunArm(m, {0xE1020051}); // qadd r0, r1, r2
    XSFLIB_CHECK(m.cpu.Reg(0) == 5 && (m.cpu.Cpsr() & psr::kQ) == 0);

    m.cpu.SetReg(1, 0x80000000);
    m.cpu.SetReg(2, 1);

    RunArm(m, {0xE1220051}); // qsub r0, r1, r2
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x80000000 && (m.cpu.Cpsr() & psr::kQ) != 0);

    // The doubling saturates first: -2 + 0x7FFFFFFF, not -2 + 0x80000000.
    m.cpu.SetCpsr(m.cpu.Cpsr() & ~psr::kQ);
    m.cpu.SetReg(1, 0xFFFFFFFE);
    m.cpu.SetReg(2, 0x40000000);

    RunArm(m, {0xE1420051}); // qdadd r0, r1, r2
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x7FFFFFFD && (m.cpu.Cpsr() & psr::kQ) != 0);

    // 0 - 2 * -2^30 is 2^31, past the largest positive.
    m.cpu.SetCpsr(m.cpu.Cpsr() & ~psr::kQ);
    m.cpu.SetReg(1, 0);
    m.cpu.SetReg(2, 0xC0000000);

    RunArm(m, {0xE1620051}); // qdsub r0, r1, r2
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x7FFFFFFF && (m.cpu.Cpsr() & psr::kQ) != 0);

    XSFLIB_CHECK(Nzcv(m.cpu) == 0b0101);
}

void TestHalfwordMultiplies()
{
    Machine m(kV5);

    // Bottom halves: -2 and 5. Top halves: 3 and -1.
    m.cpu.SetReg(1, 0x0003FFFE);
    m.cpu.SetReg(2, 0xFFFF0005);

    RunArm(m, {0xE1600281}); // smulbb r0, r1, r2
    XSFLIB_CHECK(m.cpu.Reg(0) == 0xFFFFFFF6);

    RunArm(m, {0xE16002E1}); // smultt r0, r1, r2
    XSFLIB_CHECK(m.cpu.Reg(0) == 0xFFFFFFFD);

    RunArm(m, {0xE16002C1}); // smulbt r0, r1, r2
    XSFLIB_CHECK(m.cpu.Reg(0) == 2);

    // The accumulation overflows: Q is set, and the sum wraps.
    m.cpu.SetReg(1, 1);
    m.cpu.SetReg(2, 1);
    m.cpu.SetReg(3, 0x7FFFFFFF);

    RunArm(m, {0xE1003281}); // smlabb r0, r1, r2, r3
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x80000000 && (m.cpu.Cpsr() & psr::kQ) != 0);

    // 0x20000 * -0x8000 is -2^32, and its top 32 of 48 bits are -0x10000.
    m.cpu.SetCpsr(m.cpu.Cpsr() & ~psr::kQ);
    m.cpu.SetReg(1, 0x00020000);
    m.cpu.SetReg(2, 0x00008000);

    RunArm(m, {0xE12002A1}); // smulwb r0, r1, r2
    XSFLIB_CHECK(m.cpu.Reg(0) == 0xFFFF0000 && (m.cpu.Cpsr() & psr::kQ) == 0);

    // 0x7FFFFFFF * 0x7FFF is 0x3FFF7FFF8001. 0x3FFF7FFF + 0x40008001 overflows.
    m.cpu.SetReg(1, 0x7FFFFFFF);
    m.cpu.SetReg(2, 0x7FFF0000);
    m.cpu.SetReg(3, 0x40008001);

    RunArm(m, {0xE12032C1}); // smlawt r0, r1, r2, r3
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x80000000 && (m.cpu.Cpsr() & psr::kQ) != 0);

    // The 64-bit accumulations: r0 low, r1 high.
    m.cpu.SetReg(0, 0xFFFFFFFF);
    m.cpu.SetReg(1, 0);
    m.cpu.SetReg(2, 2);
    m.cpu.SetReg(3, 3);

    RunArm(m, {0xE1410382}); // smlalbb r0, r1, r2, r3
    XSFLIB_CHECK(m.cpu.Reg(0) == 5 && m.cpu.Reg(1) == 1);

    m.cpu.SetReg(0, 0);
    m.cpu.SetReg(1, 0);
    m.cpu.SetReg(2, 0xFFFF0000);
    m.cpu.SetReg(3, 1);

    RunArm(m, {0xE14103A2}); // smlaltb r0, r1, r2, r3
    XSFLIB_CHECK(m.cpu.Reg(0) == 0xFFFFFFFF && m.cpu.Reg(1) == 0xFFFFFFFF);
}

void TestUndefinedInstructions()
{
    // ARMv5TE's instructions on ARMv4T.
    XSFLIB_CHECK(RaisesUndefined(kV4, 0xE16F0F11, false)); // clz r0, r1
    XSFLIB_CHECK(RaisesUndefined(kV4, 0xE1020051, false)); // qadd r0, r1, r2
    XSFLIB_CHECK(RaisesUndefined(kV4, 0xE1600281, false)); // smulbb r0, r1, r2
    XSFLIB_CHECK(RaisesUndefined(kV4, 0xE12FFF32, false)); // blx r2
    XSFLIB_CHECK(RaisesUndefined(kV4, 0xFB000000, false)); // blx (an immediate)
    XSFLIB_CHECK(RaisesUndefined(kV4, 0xE1200172, false)); // bkpt #0x12
    XSFLIB_CHECK(RaisesUndefined(kV4, 0xF5D1F004, false)); // pld [r1, #4]
    XSFLIB_CHECK(RaisesUndefined(kV4, 0xEE100F10, false)); // mrc p15, 0, r0, c0, c0, 0
    XSFLIB_CHECK(RaisesUndefined(kV4, 0x4790, true));      // blx r2
    XSFLIB_CHECK(RaisesUndefined(kV4, 0xE800, true));      // blx (the second half)
    XSFLIB_CHECK(RaisesUndefined(kV4, 0xBE12, true));      // bkpt #0x12

    // Undefined on both.
    for (const Architecture architecture : {kV4, kV5})
    {
        XSFLIB_CHECK(RaisesUndefined(architecture, 0xE7F000F0, false)); // udf
        XSFLIB_CHECK(RaisesUndefined(architecture, 0xEE100E10, false)); // mrc p14, 0, r0, c0, c0, 0
        XSFLIB_CHECK(RaisesUndefined(architecture, 0xED900000, false)); // ldc p0, c0, [r0]
        XSFLIB_CHECK(RaisesUndefined(architecture, 0xDE00, true));      // udf
        XSFLIB_CHECK(RaisesUndefined(architecture, 0xB100, true));      // (1011 0001: reserved)
        XSFLIB_CHECK(RaisesUndefined(architecture, 0xE801, true));      // blx (the second half, bit 0 set)
    }

    XSFLIB_CHECK(RaisesUndefined(kV5, 0xEE210F10, false)); // mcr p15, 1, r0, c1, c0, 0
    XSFLIB_CHECK(RaisesUndefined(kV5, 0xF0000000, false)); // (cond 1111: no instruction)

    // PLD does nothing on ARMv5TE.
    Machine m(kV5);

    RunArm(m, {0xF5D1F004}); // pld [r1, #4]
}

void TestBreakpoint()
{
    for (const bool thumb : {false, true})
    {
        Machine m(kV5);

        // A prefetch abort, with the instruction's address plus 4 in LR in both states.
        const StopReason reason =
            thumb ? RunThumbTo(m, {0xBE12}, kHigh + 0x0C) : RunArmTo(m, {0xE1200172}, kHigh + 0x0C);

        XSFLIB_CHECK(reason == StopReason::kStopAddress);
        XSFLIB_CHECK(m.cpu.Cpsr() == 0xD7 && m.cpu.Reg(14) == kCode + 4);
        XSFLIB_CHECK(m.cpu.Spsr() == (kSystemCpsr | (thumb ? psr::kT : 0)));
    }
}

void TestSwi()
{
    // A handler that takes the SWI. The CPU goes on after it.
    Machine m(kV4);
    uint32_t comment = 0;
    uint32_t pc = 0;
    const auto handler = [&](Cpu& cpu, uint32_t value)
    {
        comment = value;
        pc = cpu.Reg(15);
        cpu.SetReg(0, 42);
        return true;
    };
    m.cpu.SetSwiHandler(handler);

    RunArm(m, {0xEF123456, 0xE1A00000}); // swi #0x123456, nop
    XSFLIB_CHECK(comment == 0x123456 && pc == kCode + 4 && m.cpu.Reg(0) == 42 && m.cpu.Cpsr() == kSystemCpsr);

    RunThumb(m, {0xDF42}); // swi #0x42
    XSFLIB_CHECK(comment == 0x42 && pc == kCode + 2);

    // A handler that leaves the SWI, and the exception.
    const auto decline = [&](Cpu& /*cpu*/, uint32_t value)
    {
        comment = value;
        return false;
    };
    m.cpu.SetSwiHandler(decline);
    XSFLIB_CHECK(RunArmTo(m, {0xEF000012}, 0x08) == StopReason::kStopAddress); // swi #0x12
    XSFLIB_CHECK(comment == 0x12 && m.cpu.Cpsr() == 0xD3 && m.cpu.Reg(14) == kCode + 4);
    XSFLIB_CHECK(m.cpu.Spsr() == kSystemCpsr);

    m.cpu.Jump(kCode, Mode::kSystem);
    XSFLIB_CHECK(RunThumbTo(m, {0xDF42}, 0x08) == StopReason::kStopAddress); // swi #0x42
    XSFLIB_CHECK(m.cpu.Cpsr() == 0xD3 && m.cpu.Reg(14) == kCode + 2 && m.cpu.Spsr() == (kSystemCpsr | psr::kT));

    // Without the vectors, an SWI that no handler takes stops the CPU at the SWI.
    Machine bare(kV4);
    bare.cpu.SetVectorsHandled(false);
    XSFLIB_CHECK(RunArmTo(bare, {0xEF000012}, 0x08) == StopReason::kUndefined); // swi #0x12
    XSFLIB_CHECK(bare.cpu.Pc() == kCode && bare.cpu.Cpsr() == kSystemCpsr);
    XSFLIB_CHECK(RunArmTo(bare, {0xE7F000F0}, 0x04) == StopReason::kUndefined); // udf
    XSFLIB_CHECK(bare.cpu.Pc() == kCode);

    // A handler that stops the CPU.
    const auto stop = [](Cpu& cpu, uint32_t /*value*/)
    {
        cpu.Stop();
        return true;
    };
    bare.cpu.SetSwiHandler(stop);
    XSFLIB_CHECK(RunArmTo(bare, {0xEF000012, 0xE1A00000}, kCode + 8) == StopReason::kStopped); // swi #0x12, nop
    XSFLIB_CHECK(bare.cpu.Pc() == kCode + 4);
}

void TestIrq()
{
    Machine m(kV4);
    m.bus.Put32(0x18, {0xE25EF004});              // subs pc, lr, #4
    m.bus.Put32(kCode, {0xE1A00000, 0xE1A00000}); // nop, nop

    // The IRQ comes before the next instruction. LR holds that instruction's address plus 4.
    m.cpu.SetCpsr(0x1F);
    m.cpu.SetIrq(true);
    m.cpu.SetStopAddress(0x18);
    XSFLIB_CHECK(m.cpu.Run(100) == StopReason::kStopAddress);
    XSFLIB_CHECK(m.cpu.Cpsr() == 0x92 && m.cpu.Reg(14) == kCode + 4 && m.cpu.Spsr() == 0x1F);

    // SUBS PC, LR, #4 returns.
    m.cpu.SetIrq(false);
    m.cpu.SetStopAddress(kCode + 8);
    XSFLIB_CHECK(m.cpu.Run(100) == StopReason::kStopAddress && m.cpu.Cpsr() == 0x1F);

    // The I bit masks it.
    m.cpu.Jump(kCode, Mode::kSystem);
    m.cpu.SetCpsr(0x9F);
    m.cpu.SetIrq(true);
    XSFLIB_CHECK(m.cpu.Run(100) == StopReason::kStopAddress && m.cpu.Cpsr() == 0x9F);

    // In Thumb state, the IRQ saves the state in the SPSR and returns to it.
    m.cpu.SetIrq(false);
    m.bus.Put16(kCode, {0x46C0, 0x46C0}); // mov r8, r8, twice
    m.cpu.Jump(kCode | 1, Mode::kSystem);
    m.cpu.SetCpsr(0x3F);
    XSFLIB_CHECK(m.cpu.Run(1) == StopReason::kCycles && m.cpu.Pc() == kCode + 3);

    m.cpu.SetIrq(true);
    m.cpu.SetStopAddress(0x18);
    XSFLIB_CHECK(m.cpu.Run(100) == StopReason::kStopAddress);
    XSFLIB_CHECK(m.cpu.Cpsr() == 0x92 && m.cpu.Reg(14) == kCode + 6 && m.cpu.Spsr() == 0x3F);

    m.cpu.SetIrq(false);
    m.cpu.SetStopAddress(kCode + 4);
    XSFLIB_CHECK(m.cpu.Run(100) == StopReason::kStopAddress && m.cpu.Pc() == kCode + 5 && m.cpu.Cpsr() == 0x3F);

    // ARMv5TE starts with the vectors at 0xFFFF0000.
    Machine v5(kV5);
    v5.cpu.SetCpsr(0x1F);
    v5.cpu.SetIrq(true);
    v5.cpu.SetStopAddress(kHigh + 0x18);
    XSFLIB_CHECK(v5.cpu.Run(100) == StopReason::kStopAddress && v5.cpu.Cpsr() == 0x92);
}

void TestHalt()
{
    // A halted CPU runs nothing until the IRQ line wakes it, masked or not.
    Machine m(kV4);
    m.bus.Put32(kCode, {0xE1A00000, 0xE1A00000}); // nop, nop
    m.cpu.SetStopAddress(kCode + 8);
    m.cpu.Halt();
    XSFLIB_CHECK(m.cpu.Run(100) == StopReason::kHalted && m.cpu.Halted() && m.cpu.Cycles() == 0);

    m.cpu.SetIrq(true);
    XSFLIB_CHECK(m.cpu.Run(100) == StopReason::kStopAddress && !m.cpu.Halted());
    XSFLIB_CHECK(m.cpu.Cpsr() == kSystemCpsr);

    // Unmasked, the IRQ that wakes the CPU is taken.
    m.cpu.SetIrq(false);
    m.cpu.Jump(kCode, Mode::kSystem);
    m.cpu.SetCpsr(0x1F);
    m.cpu.Halt();
    XSFLIB_CHECK(m.cpu.Run(100) == StopReason::kHalted);

    m.cpu.SetIrq(true);
    m.cpu.SetStopAddress(0x18);
    XSFLIB_CHECK(m.cpu.Run(100) == StopReason::kStopAddress && m.cpu.Reg(14) == kCode + 4);

    // CP15's two wait-for-interrupt operations halt the ARM946E-S after them.
    for (const uint32_t wait : {0xEE070F90u, 0xEE070F58u}) // mcr p15, 0, r0, c7, c0, 4; mcr p15, 0, r0, c7, c8, 2
    {
        Machine v5(kV5);
        XSFLIB_CHECK(RunArmTo(v5, {wait, 0xE1A00000}, kCode + 8) == StopReason::kHalted); // (wait), nop
        XSFLIB_CHECK(v5.cpu.Pc() == kCode + 4);

        v5.cpu.SetIrq(true);
        XSFLIB_CHECK(v5.cpu.Run(100) == StopReason::kStopAddress);
    }
}

void TestCall()
{
    for (const Architecture architecture : {kV4, kV5})
    {
        Machine m(architecture);
        m.bus.Put32(0x3000, {
                                0xE0800001, // add r0, r0, r1
                                0xE0800002, // add r0, r0, r2
                                0xE0800003, // add r0, r0, r3
                                0xE59DC000, // ldr r12, [sp]
                                0xE080000C, // add r0, r0, r12
                                0xE59DC004, // ldr r12, [sp, #4]
                                0xE080000C, // add r0, r0, r12
                                0xE12FFF1E, // bx lr
                            });
        m.bus.Put16(0x3100, {
                                0x1840, // adds r0, r0, r1
                                0x4770, // bx lr
                            });
        m.bus.Put16(0x3200, {
                                0xB510, // push {r4, lr}
                                0x2409, // movs r4, #9
                                0x1900, // adds r0, r0, r4
                                0xBD10, // pop {r4, pc}
                            });
        m.bus.Put32(0x3300, {0xEAFFFFFE}); // b .
        m.bus.Put32(0x3400, {0xE7F000F0}); // udf

        // Six arguments: four in registers and two on the stack, from SP down by 8.
        const uint32_t args[] = {1, 2, 3, 4, 5, 6};
        XSFLIB_CHECK(m.cpu.Call(0x3000, args, 1000) == 21);
        XSFLIB_CHECK(m.bus.Get32(kStack - 8) == 5 && m.bus.Get32(kStack - 4) == 6);
        XSFLIB_CHECK(m.cpu.Reg(13) == kStack && m.cpu.Pc() == kCode && m.cpu.Cpsr() == kSystemCpsr);

        // Thumb functions that return with BX LR or with POP {PC}.
        const uint32_t two[] = {2, 3};
        XSFLIB_CHECK(m.cpu.Call(0x3101, two, 1000) == 5);
        XSFLIB_CHECK(m.cpu.Call(0x3201, two, 1000) == 11 && m.cpu.Reg(4) == 0);

        // A call that doesn't return.
        XSFLIB_CHECK(ErrorOf<std::runtime_error>([&] { m.cpu.Call(0x3300, two, 100); }) ==
                     "the call to 0x00003300 didn't return within 100 cycles");
        XSFLIB_CHECK(m.cpu.Pc() == kCode && m.cpu.Reg(13) == kStack);

        m.cpu.SetVectorsHandled(false);
        XSFLIB_CHECK(ErrorOf<std::runtime_error>([&] { m.cpu.Call(0x3400, two, 100); }) ==
                     "the call to 0x00003400 didn't return: an undefined instruction or an unhandled SWI");
        XSFLIB_CHECK(m.cpu.Pc() == kCode);
    }

    // A call keeps the CPU's state: here Thumb state and the halt.
    Machine m(kV4);
    m.bus.Put16(0x3100, {0x1840, 0x4770}); // adds r0, r0, r1; bx lr
    m.cpu.Jump(0x2001, Mode::kSystem);
    m.cpu.Halt();
    const uint32_t two[] = {2, 3};
    XSFLIB_CHECK(m.cpu.Call(0x3101, two, 1000) == 5);
    XSFLIB_CHECK(m.cpu.Pc() == 0x2001 && (m.cpu.Cpsr() & psr::kT) != 0 && m.cpu.Halted());
}

void TestCp15Registers()
{
    Machine m(kV5);
    XSFLIB_CHECK(m.cpu.Cp15Control() == 0x2078);

    RunArm(m, {0xEE100F10}); // mrc p15, 0, r0, c0, c0, 0
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x41059461);

    RunArm(m, {0xEE100F30}); // mrc p15, 0, r0, c0, c0, 1
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x0F0D2112);

    RunArm(m, {0xEE100F50}); // mrc p15, 0, r0, c0, c0, 2
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x00140180);

    // An MRC to r15 sets the flags from the value's top 4 bits.
    SetNzcv(m.cpu, 0b1011);

    RunArm(m, {0xEE10FF10}); // mrc p15, 0, apsr_nzcv, c0, c0, 0
    XSFLIB_CHECK(Nzcv(m.cpu) == 0b0100 && (m.cpu.Cpsr() & 0x0FFFFFFF) == kSystemCpsr);

    // The registers keep the bits that exist.
    m.cpu.SetReg(0, 0xFFFFFFFF);

    RunArm(m, {0xEE060F13, 0xEE160F33}); // mcr p15, 0, r0, c6, c3, 0; mrc p15, 0, r0, c6, c3, 1
    XSFLIB_CHECK(m.cpu.Reg(0) == 0xFFFFF03F);

    m.cpu.SetReg(0, 0x1FF);

    RunArm(m, {0xEE020F30, 0xEE120F30}); // mcr p15, 0, r0, c2, c0, 1; mrc p15, 0, r0, c2, c0, 1
    XSFLIB_CHECK(m.cpu.Reg(0) == 0xFF);

    m.cpu.SetReg(0, 0xFFFFFFFF);

    RunArm(m, {0xEE050F50, 0xEE150F50}); // mcr p15, 0, r0, c5, c0, 2; mrc p15, 0, r0, c5, c0, 2
    XSFLIB_CHECK(m.cpu.Reg(0) == 0xFFFFFFFF);

    m.cpu.SetReg(0, 0x0080000B);

    RunArm(m, {0xEE090F11, 0xEE190F11}); // mcr p15, 0, r0, c9, c1, 0; mrc p15, 0, r0, c9, c1, 0
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x0080000A && m.cpu.DtcmBase() == 0x00800000);

    m.cpu.SetReg(0, 0x1234500C);

    RunArm(m, {0xEE090F31, 0xEE190F31}); // mcr p15, 0, r0, c9, c1, 1; mrc p15, 0, r0, c9, c1, 1
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x0000000C);

    m.cpu.SetReg(0, 0x55);

    RunArm(m, {0xEE0D0F30, 0xEE1D0F31}); // mcr p15, 0, r0, c13, c0, 1; mrc p15, 0, r0, c13, c1, 1
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x55);

    RunArm(m, {0xEE1D0F10}); // mrc p15, 0, r0, c13, c0, 0
    XSFLIB_CHECK(m.cpu.Reg(0) == 0);

    // The control register: bits 0, 2, 7 and 12 to 19 can change, and bits 3 to 6 stay set. The write enables the
    // ITCM's 32 KiB at 0. The code therefore runs at 0x9000.
    m.cpu.SetReg(0, 0xFFFFFFFF);

    RunArmAt(m, 0x9000, {0xEE010F10, 0xEE110F10}, 0x9008); // mcr p15, 0, r0, c1, c0, 0; mrc p15, 0, r0, c1, c0, 0
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x000FF0FD && m.cpu.Cp15Control() == 0x000FF0FD);

    m.cpu.SetReg(0, 0);

    RunArmAt(m, 0x9000, {0xEE010F10}, 0x9004); // mcr p15, 0, r0, c1, c0, 0
    XSFLIB_CHECK(m.cpu.Cp15Control() == 0x78);

    RunArm(m, {0xEE070F15}); // mcr p15, 0, r0, c7, c5, 0

    Machine v4(kV4);

    XSFLIB_CHECK(v4.cpu.Cp15Control() == 0 && v4.cpu.Itcm().empty() && v4.cpu.Dtcm().empty());
}

void TestDtcm()
{
    // 16 KiB of DTCM at 0x00800000: the stores stay in the CPU, and the region ends at 0x00804000.
    Machine m(kV5);
    m.cpu.SetReg(0, 0x0080000A);
    m.cpu.SetReg(1, 0x2078 | 0x10000);
    m.cpu.SetReg(2, 0x12345678);
    m.cpu.SetReg(3, 0x00800010);
    m.cpu.SetReg(6, 0x00804010);

    RunArm(m, {
                  0xEE090F11, // mcr p15, 0, r0, c9, c1, 0
                  0xEE011F10, // mcr p15, 0, r1, c1, c0, 0
                  0xE5832000, // str r2, [r3]
                  0xE5934000, // ldr r4, [r3]
                  0xE5965000, // ldr r5, [r6]
              });

    XSFLIB_CHECK(m.cpu.Reg(4) == 0x12345678 && m.cpu.Reg(5) == 0);
    XSFLIB_CHECK(xsflib::LoadLe32(&m.cpu.Dtcm()[0x10]) == 0x12345678);
    XSFLIB_CHECK(m.bus.Accesses() == (std::vector<TestBus::Access>{{0x00804010, 4, 0, false}}));

    // A region of 64 KiB repeats the 16 KiB.
    m.cpu.SetReg(0, 0x0080000E);

    RunArm(m, {
                  0xEE090F11, // mcr p15, 0, r0, c9, c1, 0
                  0xE5965000, // ldr r5, [r6]
              });

    XSFLIB_CHECK(m.cpu.Reg(5) == 0x12345678);

    // In load mode, the loads read the bus and the stores write the DTCM.
    Machine load(kV5);
    load.bus.Put32(0x10000, {0xAAAAAAAA});
    load.cpu.SetReg(0, 0x0001000A);
    load.cpu.SetReg(1, 0x2078 | 0x30000);
    load.cpu.SetReg(2, 0x55);
    load.cpu.SetReg(3, 0x10000);
    load.cpu.SetReg(7, 0x2078 | 0x10000);

    RunArm(load, {
                     0xEE090F11, // mcr p15, 0, r0, c9, c1, 0
                     0xEE011F10, // mcr p15, 0, r1, c1, c0, 0
                     0xE5934000, // ldr r4, [r3]
                     0xE5832000, // str r2, [r3]
                     0xEE017F10, // mcr p15, 0, r7, c1, c0, 0
                     0xE5935000, // ldr r5, [r3]
                 });

    XSFLIB_CHECK(load.cpu.Reg(4) == 0xAAAAAAAA && load.cpu.Reg(5) == 0x55);
    XSFLIB_CHECK(load.bus.Get32(0x10000) == 0xAAAAAAAA && load.cpu.Dtcm()[0] == 0x55);

    // A region of 1 KiB, smaller than a page: the rest of the page stays on the bus.
    Machine small(kV5);
    small.cpu.SetReg(0, 0x00010002);
    small.cpu.SetReg(1, 0x2078 | 0x10000);
    small.cpu.SetReg(2, 0x66);
    small.cpu.SetReg(3, 0x10000);
    small.cpu.SetReg(4, 0x10400);

    RunArm(small, {
                      0xEE090F11, // mcr p15, 0, r0, c9, c1, 0
                      0xEE011F10, // mcr p15, 0, r1, c1, c0, 0
                      0xE5832000, // str r2, [r3]
                      0xE5842000, // str r2, [r4]
                      0xE5935000, // ldr r5, [r3]
                  });

    XSFLIB_CHECK(small.cpu.Reg(5) == 0x66 && small.cpu.Dtcm()[0] == 0x66);
    XSFLIB_CHECK(small.bus.Get32(0x10000) == 0 && small.bus.Get32(0x10400) == 0x66);
}

void TestItcm()
{
    // 32 KiB of ITCM at 0. Instructions and data come from it, not from the RAM under it.
    Machine m(kV5);
    m.bus.Put32(0x2000, {0xE3A00002});                    // mov r0, #2
    xsflib::StoreLe32(&m.cpu.Itcm()[0x2000], 0xE3A00001); // mov r0, #1
    xsflib::StoreLe32(&m.cpu.Itcm()[0x2004], 0xE5943000); // ldr r3, [r4]
    m.cpu.SetReg(0, 0x0000000C);
    m.cpu.SetReg(1, 0x2078 | 0x40000);
    m.cpu.SetReg(2, 0x2000);
    m.cpu.SetReg(4, 0x2000);

    const StopReason reason = RunArmAt(m, 0x9000,
                                       {
                                           0xEE090F31, // mcr p15, 0, r0, c9, c1, 1
                                           0xEE011F10, // mcr p15, 0, r1, c1, c0, 0
                                           0xE12FFF12, // bx r2
                                       },
                                       0x2008);

    XSFLIB_CHECK(reason == StopReason::kStopAddress && m.cpu.Reg(0) == 1 && m.cpu.Reg(3) == 0xE3A00001);

    // A region of 64 KiB repeats the 32 KiB from 0x8000.
    m.cpu.SetReg(0, 0x0000000E);
    m.cpu.SetReg(6, 0xA000);

    RunArmAt(m, 0x10000,
             {
                 0xEE090F31, // mcr p15, 0, r0, c9, c1, 1
                 0xE5965000, // ldr r5, [r6]
             },
             0x10008);

    XSFLIB_CHECK(m.cpu.Reg(5) == 0xE3A00001);
}

void TestTcmOverlap()
{
    // Instruction fetches skip the DTCM. Loads see it.
    Machine m(kV5);
    m.bus.Put32(0x2000, {0xE3A00002, 0xE5943000});        // mov r0, #2; ldr r3, [r4]
    xsflib::StoreLe32(&m.cpu.Dtcm()[0x2000], 0xE3A00001); // mov r0, #1
    m.cpu.SetReg(0, 0x0000000A);
    m.cpu.SetReg(1, 0x2078 | 0x10000);
    m.cpu.SetReg(2, 0x2000);
    m.cpu.SetReg(4, 0x2000);

    const StopReason reason = RunArmAt(m, 0x9000,
                                       {
                                           0xEE090F11, // mcr p15, 0, r0, c9, c1, 0
                                           0xEE011F10, // mcr p15, 0, r1, c1, c0, 0
                                           0xE12FFF12, // bx r2
                                       },
                                       0x2008);

    XSFLIB_CHECK(reason == StopReason::kStopAddress && m.cpu.Reg(0) == 2 && m.cpu.Reg(3) == 0xE3A00001);

    // Where the two overlap, the ITCM wins.
    Machine both(kV5);
    both.cpu.SetReg(0, 0x0000000A);
    both.cpu.SetReg(1, 0x0000000C);
    both.cpu.SetReg(2, 0x2078 | 0x50000);
    both.cpu.SetReg(5, 0x77);
    both.cpu.SetReg(6, 0x100);

    RunArmAt(both, 0x9000,
             {
                 0xEE090F11, // mcr p15, 0, r0, c9, c1, 0
                 0xEE091F31, // mcr p15, 0, r1, c9, c1, 1
                 0xEE012F10, // mcr p15, 0, r2, c1, c0, 0
                 0xE5865000, // str r5, [r6]
             },
             0x9010);

    XSFLIB_CHECK(both.cpu.Itcm()[0x100] == 0x77 && both.cpu.Dtcm()[0x100] == 0 && both.bus.Get32(0x100) == 0);
}

void TestVectors()
{
    // ARMv5TE: CP15's bit 13 puts the vectors at 0xFFFF0000. Without it, they're at 0.
    Machine m(kV5);
    XSFLIB_CHECK(RaisesUndefined(kV5, 0xE7F000F0, false)); // udf
    m.cpu.SetReg(0, 0x78);

    const StopReason reason = RunArmTo(m,
                                       {
                                           0xEE010F10, // mcr p15, 0, r0, c1, c0, 0
                                           0xE7F000F0, // udf
                                       },
                                       0x04);

    XSFLIB_CHECK(reason == StopReason::kStopAddress && m.cpu.Reg(14) == kCode + 8);
}

void TestThumbArithmetic()
{
    Machine m(kV4);
    m.cpu.SetReg(1, 0x80000001);

    // Format 1: shifts by an immediate.
    SetNzcv(m.cpu, 0);
    RunThumb(m, {0x0048}); // lsls r0, r1, #1
    XSFLIB_CHECK(m.cpu.Reg(0) == 2 && Nzcv(m.cpu) == 0b0010);

    RunThumb(m, {0x0808}); // lsrs r0, r1, #32
    XSFLIB_CHECK(m.cpu.Reg(0) == 0 && Nzcv(m.cpu) == 0b0110);

    RunThumb(m, {0x1008}); // asrs r0, r1, #32
    XSFLIB_CHECK(m.cpu.Reg(0) == 0xFFFFFFFF && Nzcv(m.cpu) == 0b1010);

    SetNzcv(m.cpu, 0b0011);
    RunThumb(m, {0x0008}); // movs r0, r1 (lsls #0)
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x80000001 && Nzcv(m.cpu) == 0b1011);

    // Format 2: ADD and SUB of a register or a 3-bit immediate.
    m.cpu.SetReg(1, 0xFFFFFFFF);
    m.cpu.SetReg(2, 1);
    RunThumb(m, {0x1888}); // adds r0, r1, r2
    XSFLIB_CHECK(m.cpu.Reg(0) == 0 && Nzcv(m.cpu) == 0b0110);

    m.cpu.SetReg(1, 7);
    RunThumb(m, {0x1FC8}); // subs r0, r1, #7
    XSFLIB_CHECK(m.cpu.Reg(0) == 0 && Nzcv(m.cpu) == 0b0110);

    m.cpu.SetReg(1, 0x80000000);
    SetNzcv(m.cpu, 0b0011);
    RunThumb(m, {0x1C08}); // adds r0, r1, #0
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x80000000 && Nzcv(m.cpu) == 0b1000);

    // Format 3: MOV sets N and Z only.
    SetNzcv(m.cpu, 0b0011);
    RunThumb(m, {0x2000}); // movs r0, #0
    XSFLIB_CHECK(m.cpu.Reg(0) == 0 && Nzcv(m.cpu) == 0b0111);

    RunThumb(m, {0x2801}); // cmp r0, #1
    XSFLIB_CHECK(m.cpu.Reg(0) == 0 && Nzcv(m.cpu) == 0b1000);

    m.cpu.SetReg(0, 0xFFFFFF01);
    RunThumb(m, {0x30FF}); // adds r0, #255
    XSFLIB_CHECK(m.cpu.Reg(0) == 0 && Nzcv(m.cpu) == 0b0110);

    m.cpu.SetReg(0, 0x80000000);
    RunThumb(m, {0x3801}); // subs r0, #1
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x7FFFFFFF && Nzcv(m.cpu) == 0b0011);
}

void TestThumbAlu()
{
    Machine m(kV4);

    // Runs a format-4 instruction on r0 and r1 from the flags `nzcv`.
    const auto alu = [&](uint16_t op, uint32_t a, uint32_t b, uint32_t nzcv)
    {
        m.cpu.SetReg(0, a);
        m.cpu.SetReg(1, b);
        SetNzcv(m.cpu, nzcv);
        RunThumb(m, {op});
    };

    alu(0x4008, 0xF0F0, 0xFF00, 0b0011); // ands r0, r1
    XSFLIB_CHECK(m.cpu.Reg(0) == 0xF000 && Nzcv(m.cpu) == 0b0011);

    alu(0x4048, 0xFFFFFFFF, 0x0F, 0); // eors r0, r1
    XSFLIB_CHECK(m.cpu.Reg(0) == 0xFFFFFFF0 && Nzcv(m.cpu) == 0b1000);

    // Shifts by a register.
    alu(0x4088, 1, 33, 0b0010); // lsls r0, r1
    XSFLIB_CHECK(m.cpu.Reg(0) == 0 && Nzcv(m.cpu) == 0b0100);

    alu(0x4088, 1, 32, 0); // lsls r0, r1
    XSFLIB_CHECK(m.cpu.Reg(0) == 0 && Nzcv(m.cpu) == 0b0110);

    alu(0x4088, 1, 0x100, 0b0010); // lsls r0, r1
    XSFLIB_CHECK(m.cpu.Reg(0) == 1 && Nzcv(m.cpu) == 0b0010);

    alu(0x40C8, 0x80000000, 32, 0); // lsrs r0, r1
    XSFLIB_CHECK(m.cpu.Reg(0) == 0 && Nzcv(m.cpu) == 0b0110);

    alu(0x4108, 0x80000000, 40, 0); // asrs r0, r1
    XSFLIB_CHECK(m.cpu.Reg(0) == 0xFFFFFFFF && Nzcv(m.cpu) == 0b1010);

    alu(0x41C8, 0x80000001, 1, 0); // rors r0, r1
    XSFLIB_CHECK(m.cpu.Reg(0) == 0xC0000000 && Nzcv(m.cpu) == 0b1010);

    alu(0x41C8, 0x80000001, 32, 0); // rors r0, r1
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x80000001 && Nzcv(m.cpu) == 0b1010);

    // Arithmetic with the carry.
    alu(0x4148, 0xFFFFFFFF, 0, 0b0010); // adcs r0, r1
    XSFLIB_CHECK(m.cpu.Reg(0) == 0 && Nzcv(m.cpu) == 0b0110);

    alu(0x4188, 5, 3, 0); // sbcs r0, r1
    XSFLIB_CHECK(m.cpu.Reg(0) == 1 && Nzcv(m.cpu) == 0b0010);

    alu(0x4248, 0, 1, 0); // rsbs r0, r1, #0
    XSFLIB_CHECK(m.cpu.Reg(0) == 0xFFFFFFFF && Nzcv(m.cpu) == 0b1000);

    alu(0x4248, 0, 0, 0); // rsbs r0, r1, #0
    XSFLIB_CHECK(m.cpu.Reg(0) == 0 && Nzcv(m.cpu) == 0b0110);

    alu(0x4248, 0, 0x80000000, 0); // rsbs r0, r1, #0
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x80000000 && Nzcv(m.cpu) == 0b1001);

    // The comparisons leave r0.
    alu(0x4208, 0x0F, 0xF0, 0); // tst r0, r1
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x0F && Nzcv(m.cpu) == 0b0100);

    alu(0x4288, 1, 2, 0); // cmp r0, r1
    XSFLIB_CHECK(m.cpu.Reg(0) == 1 && Nzcv(m.cpu) == 0b1000);

    alu(0x42C8, 0x7FFFFFFF, 1, 0); // cmn r0, r1
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x7FFFFFFF && Nzcv(m.cpu) == 0b1001);

    alu(0x4308, 0x0F, 0xF0, 0); // orrs r0, r1
    XSFLIB_CHECK(m.cpu.Reg(0) == 0xFF && Nzcv(m.cpu) == 0b0000);

    alu(0x4348, 6, 7, 0b0011); // muls r0, r1, r0
    XSFLIB_CHECK(m.cpu.Reg(0) == 42 && Nzcv(m.cpu) == 0b0011);

    alu(0x4388, 0xFF, 0x0F, 0); // bics r0, r1
    XSFLIB_CHECK(m.cpu.Reg(0) == 0xF0);

    alu(0x43C8, 5, 0, 0); // mvns r0, r1
    XSFLIB_CHECK(m.cpu.Reg(0) == 0xFFFFFFFF && Nzcv(m.cpu) == 0b1000);
}

void TestThumbHighRegisters()
{
    Machine m(kV4);

    // ADD and MOV leave the flags. CMP sets them.
    m.cpu.SetReg(8, 5);
    m.cpu.SetReg(1, 6);
    SetNzcv(m.cpu, 0b0101);
    RunThumb(m, {0x4488}); // add r8, r1
    XSFLIB_CHECK(m.cpu.Reg(8) == 11 && Nzcv(m.cpu) == 0b0101);

    m.cpu.SetReg(8, 6);
    RunThumb(m, {0x4588}); // cmp r8, r1
    XSFLIB_CHECK(Nzcv(m.cpu) == 0b0110);

    // r15 reads as the address plus 4. ADD and MOV to r15 stay in Thumb state.
    RunThumb(m, {0x4678}); // mov r0, pc
    XSFLIB_CHECK(m.cpu.Reg(0) == kCode + 4);

    m.cpu.SetReg(1, 0x2001);
    XSFLIB_CHECK(RunThumbTo(m, {0x468F}, 0x2000) == StopReason::kStopAddress); // mov pc, r1
    XSFLIB_CHECK(m.cpu.Pc() == 0x2001);

    m.cpu.SetReg(1, 0x100);
    XSFLIB_CHECK(RunThumbTo(m, {0x448F}, kCode + 0x104) == StopReason::kStopAddress); // add pc, r1
    XSFLIB_CHECK(m.cpu.Pc() == kCode + 0x105);

    // BX switches by bit 0. BX PC goes to ARM state at the next word.
    XSFLIB_CHECK(RunThumbTo(m, {0x4778}, kCode + 4) == StopReason::kStopAddress); // bx pc
    XSFLIB_CHECK(m.cpu.Pc() == kCode + 4);

    m.cpu.SetReg(1, 0x2000);
    XSFLIB_CHECK(RunThumbTo(m, {0x4708}, 0x2000) == StopReason::kStopAddress); // bx r1
    XSFLIB_CHECK(m.cpu.Pc() == 0x2000);
}

void TestThumbTransfers()
{
    for (const Architecture architecture : {kV4, kV5})
    {
        Machine m(architecture);
        const bool v4 = architecture == kV4;

        // Format 6: the PC with bit 1 cleared, from either halfword of a word.
        m.bus.Put32(kCode + 8, {0xCAFEF00D});
        RunThumb(m, {0x4801}); // ldr r0, [pc, #4]
        XSFLIB_CHECK(m.cpu.Reg(0) == 0xCAFEF00D);

        m.cpu.SetReg(0, 0);
        XSFLIB_CHECK(RunThumbAt(m, kCode + 2, {0x4801}, kCode + 4) == StopReason::kStopAddress); // ldr r0, [pc, #4]
        XSFLIB_CHECK(m.cpu.Reg(0) == 0xCAFEF00D);

        // Formats 7 and 8: register offsets.
        m.cpu.SetReg(1, kData);
        m.cpu.SetReg(2, 1);
        RunThumb(m, {0x5888}); // ldr r0, [r1, r2]
        XSFLIB_CHECK(m.cpu.Reg(0) == 0x11443322);

        m.cpu.SetReg(2, 7);
        RunThumb(m, {0x5C88}); // ldrb r0, [r1, r2]
        XSFLIB_CHECK(m.cpu.Reg(0) == 0x88);

        RunThumb(m, {0x5688}); // ldrsb r0, [r1, r2]
        XSFLIB_CHECK(m.cpu.Reg(0) == 0xFFFFFF88);

        RunThumb(m, {0x5E88}); // ldrsh r0, [r1, r2]
        XSFLIB_CHECK(m.cpu.Reg(0) == (v4 ? 0xFFFFFF88 : 0xFFFF8877));

        m.cpu.SetReg(2, 6);
        RunThumb(m, {0x5A88}); // ldrh r0, [r1, r2]
        XSFLIB_CHECK(m.cpu.Reg(0) == 0x8877);

        RunThumb(m, {0x5E88}); // ldrsh r0, [r1, r2]
        XSFLIB_CHECK(m.cpu.Reg(0) == 0xFFFF8877);

        m.cpu.SetReg(2, 1);
        RunThumb(m, {0x5A88}); // ldrh r0, [r1, r2]
        XSFLIB_CHECK(m.cpu.Reg(0) == (v4 ? 0x11000022 : 0x2211));

        m.cpu.SetReg(0, 0xA1B2C3D4);
        m.cpu.SetReg(2, 6);
        RunThumb(m, {0x5088}); // str r0, [r1, r2]
        XSFLIB_CHECK(m.bus.Get32(kData + 4) == 0xA1B2C3D4);

        m.cpu.SetReg(2, 3);
        RunThumb(m, {0x5288}); // strh r0, [r1, r2]
        XSFLIB_CHECK(m.bus.Get32(kData) == 0xC3D42211);

        RunThumb(m, {0x5488}); // strb r0, [r1, r2]
        XSFLIB_CHECK(m.bus.Get32(kData) == 0xD4D42211);

        // Formats 9, 10 and 11: immediate offsets in words, bytes and halfwords, and from SP.
        RunThumb(m, {0x6848}); // ldr r0, [r1, #4]
        XSFLIB_CHECK(m.cpu.Reg(0) == 0xA1B2C3D4);

        RunThumb(m, {0x78C8}); // ldrb r0, [r1, #3]
        XSFLIB_CHECK(m.cpu.Reg(0) == 0xD4);

        RunThumb(m, {0x8848}); // ldrh r0, [r1, #2]
        XSFLIB_CHECK(m.cpu.Reg(0) == 0xD4D4);

        m.cpu.SetReg(0, 0x11223344);
        RunThumb(m, {0x6048}); // str r0, [r1, #4]
        RunThumb(m, {0x70C8}); // strb r0, [r1, #3]
        RunThumb(m, {0x8048}); // strh r0, [r1, #2]
        XSFLIB_CHECK(m.bus.Get32(kData + 4) == 0x11223344 && m.bus.Get32(kData) == 0x33442211);

        m.cpu.SetReg(13, kData);
        RunThumb(m, {0x9802}); // ldr r0, [sp, #8]
        XSFLIB_CHECK(m.cpu.Reg(0) == 0xCCBBAA99);

        RunThumb(m, {0x9002}); // str r0, [sp, #8]
        XSFLIB_CHECK(m.bus.Get32(kData + 8) == 0xCCBBAA99);

        // Format 12: addresses from the PC with bit 1 cleared, and from SP. Format 13: SP moves.
        XSFLIB_CHECK(RunThumbAt(m, kCode + 2, {0xA002}, kCode + 4) == StopReason::kStopAddress); // add r0, pc, #8
        XSFLIB_CHECK(m.cpu.Reg(0) == kCode + 12);

        RunThumb(m, {0xA802}); // add r0, sp, #8
        XSFLIB_CHECK(m.cpu.Reg(0) == kData + 8);

        RunThumb(m, {0xB004}); // add sp, #16
        XSFLIB_CHECK(m.cpu.Reg(13) == kData + 16);

        RunThumb(m, {0xB084}); // sub sp, #16
        XSFLIB_CHECK(m.cpu.Reg(13) == kData);
    }
}

void TestThumbStack()
{
    for (const Architecture architecture : {kV4, kV5})
    {
        Machine m(architecture);
        const bool v4 = architecture == kV4;

        m.cpu.SetReg(0, 1);
        m.cpu.SetReg(1, 2);
        m.cpu.SetReg(14, 3);
        RunThumb(m, {0xB503}); // push {r0, r1, lr}
        XSFLIB_CHECK(m.cpu.Reg(13) == kStack - 12);
        XSFLIB_CHECK(m.bus.Get32(kStack - 12) == 1 && m.bus.Get32(kStack - 8) == 2 && m.bus.Get32(kStack - 4) == 3);

        m.bus.Put32(kStack - 4, {0x2001});
        XSFLIB_CHECK(RunThumbTo(m, {0xBD0C}, 0x2000) == StopReason::kStopAddress); // pop {r2, r3, pc}
        XSFLIB_CHECK(m.cpu.Reg(2) == 1 && m.cpu.Reg(3) == 2 && m.cpu.Reg(13) == kStack && m.cpu.Pc() == 0x2001);

        // An empty list moves SP by 0x40, and only the ARM7TDMI stores r15.
        m.bus.Put32(kStack - 0x40, {0x12345678});
        RunThumb(m, {0xB400}); // push {}
        XSFLIB_CHECK(m.cpu.Reg(13) == kStack - 0x40);
        XSFLIB_CHECK((m.bus.Get32(kStack - 0x40) != 0x12345678) == v4);
    }
}

void TestThumbBlockTransfer()
{
    for (const Architecture architecture : {kV4, kV5})
    {
        Machine m(architecture);
        const bool v4 = architecture == kV4;

        m.cpu.SetReg(0, kData);
        m.cpu.SetReg(1, 0x11);
        m.cpu.SetReg(2, 0x22);
        RunThumb(m, {0xC006}); // stmia r0!, {r1, r2}
        XSFLIB_CHECK(m.bus.Get32(kData) == 0x11 && m.bus.Get32(kData + 4) == 0x22 && m.cpu.Reg(0) == kData + 8);

        m.cpu.SetReg(0, kData);
        m.cpu.SetReg(1, 0);
        m.cpu.SetReg(2, 0);
        RunThumb(m, {0xC806}); // ldmia r0!, {r1, r2}
        XSFLIB_CHECK(m.cpu.Reg(1) == 0x11 && m.cpu.Reg(2) == 0x22 && m.cpu.Reg(0) == kData + 8);

        // STMIA with the base in the list: the old base when it's first, else the ARM7TDMI's new base.
        m.cpu.SetReg(0, 0x33);
        m.cpu.SetReg(1, kData);
        RunThumb(m, {0xC103}); // stmia r1!, {r0, r1}
        XSFLIB_CHECK(m.bus.Get32(kData + 4) == (v4 ? kData + 8 : kData) && m.cpu.Reg(1) == kData + 8);

        m.cpu.SetReg(0, kData);
        RunThumb(m, {0xC003}); // stmia r0!, {r0, r1}
        XSFLIB_CHECK(m.bus.Get32(kData) == kData && m.cpu.Reg(0) == kData + 8);

        // LDMIA with the base in the list keeps the loaded value on both.
        m.bus.Put32(kData, {0xB0, 0xB1});
        m.cpu.SetReg(0, kData);
        RunThumb(m, {0xC803}); // ldmia r0!, {r0, r1}
        XSFLIB_CHECK(m.cpu.Reg(0) == 0xB0 && m.cpu.Reg(1) == 0xB1);

        // An empty list moves the base by 0x40, and only the ARM7TDMI stores r15.
        m.cpu.SetReg(0, kData);
        RunThumb(m, {0xC000}); // stmia r0!, {}
        XSFLIB_CHECK(m.cpu.Reg(0) == kData + 0x40 && (m.bus.Get32(kData) != 0xB0) == v4);
    }
}

void TestThumbBranches()
{
    Machine m(kV4);
    m.cpu.SetReg(1, 0);
    m.cpu.SetReg(2, 0);

    const StopReason reason = RunThumbTo(m,
                                         {
                                             0x2000, // movs r0, #0
                                             0x3001, // loop: adds r0, #1
                                             0x2803, // cmp r0, #3
                                             0xD1FC, // bne loop
                                             0xD000, // beq skip
                                             0x2101, // movs r1, #1
                                             0xE000, // skip: b over
                                             0x2201, // movs r2, #1
                                             0xF000,
                                             0xF801, // over: bl function
                                             0xE001, // b end
                                             0x2307, // function: movs r3, #7
                                             0x4770, // bx lr
                                                     // end:
                                         },
                                         kCode + 0x1A);

    XSFLIB_CHECK(reason == StopReason::kStopAddress);
    XSFLIB_CHECK(m.cpu.Reg(0) == 3 && m.cpu.Reg(1) == 0 && m.cpu.Reg(2) == 0 && m.cpu.Reg(3) == 7);
    XSFLIB_CHECK(m.cpu.Reg(14) == kCode + 0x15);
}

void TestIo()
{
    Machine m(kV4);
    m.bus.SetIo32(kIo, 0x44332211);
    m.bus.SetIo32(kIo + 4, 0x88776655);

    // The bus gets aligned addresses. The CPU rotates an unaligned load itself.
    m.cpu.SetReg(1, kIo + 1);

    RunArm(m, {0xE5910000}); // ldr r0, [r1]
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x11443322);

    RunArm(m, {0xE1D100B0}); // ldrh r0, [r1]
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x11000022);

    m.cpu.SetReg(1, kIo + 2);

    RunArm(m, {0xE1D100F0}); // ldrsh r0, [r1]
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x4433);

    m.cpu.SetReg(0, 0xBEEF);
    m.cpu.SetReg(1, kIo + 3);

    RunArm(m, {0xE1C100B0}); // strh r0, [r1]
    m.cpu.SetReg(0, 0x1FF);

    RunArm(m, {0xE5C10000}); // strb r0, [r1]
    XSFLIB_CHECK(m.bus.Accesses() == (std::vector<TestBus::Access>{{kIo, 4, 0x44332211, false},
                                                                   {kIo, 2, 0x2211, false},
                                                                   {kIo + 2, 2, 0x4433, false},
                                                                   {kIo + 2, 2, 0xBEEF, true},
                                                                   {kIo + 3, 1, 0xFF, true}}));

    // LDMDB reads from the lowest address up. SWP reads, then writes.
    m.bus.ClearAccesses();
    m.cpu.SetReg(0, kIo + 0x10);

    RunArm(m, {0xE9300006}); // ldmdb r0!, {r1, r2}
    m.cpu.SetReg(1, kIo + 4);
    m.cpu.SetReg(2, 0x99);

    RunArm(m, {0xE1010092}); // swp r0, r2, [r1]
    XSFLIB_CHECK(m.bus.Accesses() == (std::vector<TestBus::Access>{{kIo + 8, 4, 0, false},
                                                                   {kIo + 12, 4, 0, false},
                                                                   {kIo + 4, 4, 0x88776655, false},
                                                                   {kIo + 4, 4, 0x99, true}}));
}

void TestCycles()
{
    Machine m(kV4);
    m.cpu.SetReg(2, kData);
    m.bus.Put32(kCode, {
                           0xE1A00000, // nop
                           0x03A00001, // moveq r0, #1
                           0xE5921000, // ldr r1, [r2]
                           0xE8920038, // ldmia r2, {r3-r5}
                       });
    m.cpu.SetStopAddress(kCode + 16);
    SetNzcv(m.cpu, 0);

    // One cycle for each instruction, one for each access.
    XSFLIB_CHECK(m.cpu.Run(2) == StopReason::kCycles && m.cpu.Cycles() == 2 && m.cpu.Pc() == kCode + 8);
    XSFLIB_CHECK(m.cpu.Run(100) == StopReason::kStopAddress && m.cpu.Cycles() == 8);

    // At the stop address, Run returns before it runs anything.
    XSFLIB_CHECK(m.cpu.Run(100) == StopReason::kStopAddress && m.cpu.Cycles() == 8);
}

void TestInvalidatePages()
{
    Machine m(kV4);
    m.bus.Put32(kSwappedPage, {0x11111111});
    m.cpu.SetReg(1, kSwappedPage);

    RunArm(m, {0xE5910000}); // ldr r0, [r1]
    XSFLIB_CHECK(m.cpu.Reg(0) == 0x11111111);

    m.bus.SwapPage();
    m.bus.Put32(kSwappedPage, {0x22222222});
    m.cpu.InvalidatePages();

    RunArm(m, {0xE5910000}); // ldr r0, [r1]

    XSFLIB_CHECK(m.cpu.Reg(0) == 0x22222222);
}

void TestState()
{
    Machine m(kV5);
    XSFLIB_CHECK(m.cpu.GetArchitecture() == kV5);

    // Jump sets the mode and the state. SetReg(15) keeps the state.
    m.cpu.Jump(0x2001, Mode::kSupervisor);
    XSFLIB_CHECK(m.cpu.Pc() == 0x2001 && m.cpu.Reg(15) == 0x2000 && m.cpu.Cpsr() == 0xF3);

    m.cpu.SetReg(15, 0x3003);
    XSFLIB_CHECK(m.cpu.Pc() == 0x3003 && m.cpu.Reg(15) == 0x3002);

    m.cpu.Jump(0x4002, Mode::kSystem);
    XSFLIB_CHECK(m.cpu.Pc() == 0x4000 && m.cpu.Cpsr() == kSystemCpsr);

    // A new CPU starts in supervisor mode at 0.
    TestBus bus;
    Cpu cpu(bus, kV4);
    XSFLIB_CHECK(cpu.Pc() == 0 && cpu.Cpsr() == 0xD3 && cpu.Cycles() == 0 && !cpu.Halted());
}

} // namespace

int main()
{
    TestArithmetic();
    TestLogical();
    TestShifts();
    TestPcOperands();
    TestMultiply();
    TestPsrTransfer();
    TestBanks();
    TestSingleTransfer();
    TestHalfwordTransfer();
    TestDoublewordTransfer();
    TestSwap();
    TestBlockTransfer();
    TestBaseInList();
    TestEmptyList();
    TestUserBank();
    TestExceptionReturns();
    TestBranches();
    TestConditions();
    TestStateChanges();
    TestInterworking();
    TestBlx();
    TestCountLeadingZeros();
    TestSaturatingArithmetic();
    TestHalfwordMultiplies();
    TestUndefinedInstructions();
    TestBreakpoint();
    TestSwi();
    TestIrq();
    TestHalt();
    TestCall();
    TestCp15Registers();
    TestDtcm();
    TestItcm();
    TestTcmOverlap();
    TestVectors();
    TestThumbArithmetic();
    TestThumbAlu();
    TestThumbHighRegisters();
    TestThumbTransfers();
    TestThumbStack();
    TestThumbBlockTransfer();
    TestThumbBranches();
    TestIo();
    TestCycles();
    TestInvalidatePages();
    TestState();

    return xsflib::testing::Summary("ARM CPU");
}
