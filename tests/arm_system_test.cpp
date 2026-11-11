// SPDX-License-Identifier: MIT

// Tests of the ARM interpreter's exceptions, calls, CP15 registers, tightly coupled memory, vectors, I/O, cycles,
// pages and state. arm_test.cpp says how the tests are built.

#include "arm_test_util.h"

using xsflib::testing::ErrorOf;

namespace arm_tests
{
namespace
{

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

int RunTests()
{
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
    TestIo();
    TestCycles();
    TestInvalidatePages();
    TestState();

    return xsflib::testing::Summary("ARM CPU system");
}

} // namespace arm_tests

int main()
{
    return arm_tests::RunTests();
}
