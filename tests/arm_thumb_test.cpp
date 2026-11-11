// SPDX-License-Identifier: MIT

// Tests of the ARM interpreter's Thumb instructions. arm_test.cpp says how the tests are built.

#include "arm_test_util.h"

using xsflib::testing::ErrorOf;

namespace arm_tests
{
namespace
{

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

} // namespace

int RunTests()
{
    TestThumbArithmetic();
    TestThumbAlu();
    TestThumbHighRegisters();
    TestThumbTransfers();
    TestThumbStack();
    TestThumbBlockTransfer();
    TestThumbBranches();

    return xsflib::testing::Summary("ARM CPU Thumb");
}

} // namespace arm_tests

int main()
{
    return arm_tests::RunTests();
}
