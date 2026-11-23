// SPDX-License-Identifier: MIT

// Tests of the ARM interpreter's loads and stores in ARM state: single, halfword, doubleword and block transfers, the
// swap, and the user bank. arm_test.cpp says how the tests are built.

#include "arm_test_util.h"

namespace arm_tests
{
namespace
{

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

} // namespace

int RunTests()
{
    TestSingleTransfer();
    TestHalfwordTransfer();
    TestDoublewordTransfer();
    TestSwap();
    TestBlockTransfer();
    TestBaseInList();
    TestEmptyList();
    TestUserBank();

    return xsflib::testing::Summary("ARM CPU transfers");
}

} // namespace arm_tests

int main()
{
    return arm_tests::RunTests();
}
