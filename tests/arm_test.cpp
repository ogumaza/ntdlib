// SPDX-License-Identifier: MIT

// Tests of the ARM interpreter (ntdlib/arm/cpu.h) on the ARM7TDMI (ARMv4T) and the ARM946E-S (ARMv5TE), with programs
// that LLVM's assembler built. Each instruction's word comes with its assembly. The expected values follow ARM's
// architecture reference manual and GBATEK's ARM CPU reference, worked out by hand.
// This file tests the ARM instructions. arm_system_test.cpp tests the exceptions, the coprocessor, the memory and the
// state, and arm_thumb_test.cpp the Thumb instructions.

#include "arm_test_util.h"

using xsflib::testing::ErrorOf;

namespace arm_tests
{
namespace
{

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

} // namespace

int RunTests()
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

    return xsflib::testing::Summary("ARM CPU");
}

} // namespace arm_tests

int main()
{
    return arm_tests::RunTests();
}
