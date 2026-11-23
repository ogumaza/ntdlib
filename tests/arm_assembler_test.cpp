// SPDX-License-Identifier: MIT

// Tests of the ARM assembler (ntdlib/arm/assembler.h). The expected words are the encodings that ARM's architecture
// reference manual gives each instruction, as GNU's and LLVM's assemblers emit them.

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/arm/assembler.h"

namespace
{

using ntdlib::arm::ArmAssembler;
using ntdlib::arm::Condition;
using ntdlib::arm::kLr;
using ntdlib::arm::kPc;
using ntdlib::arm::kSp;

constexpr uint32_t kBase = 0x08000100;

// The words of the code that `write` assembles at kBase.
std::vector<uint32_t> Words(const std::function<void(ArmAssembler&)>& write)
{
    ArmAssembler a(kBase);
    write(a);
    const std::vector<uint8_t> bytes = a.Finish();

    std::vector<uint32_t> words;
    for (size_t i = 0; i + 4 <= bytes.size(); i += 4)
    {
        words.push_back(xsflib::LoadLe32(&bytes[i]));
    }

    return words;
}

uint32_t One(const std::function<void(ArmAssembler&)>& write)
{
    const std::vector<uint32_t> words = Words(write);
    return words.size() == 1 ? words[0] : 0;
}

void TestRotatedImmediate()
{
    XSFLIB_CHECK(ntdlib::arm::RotatedImmediate(0xFF) == 0x0FFu);
    XSFLIB_CHECK(ntdlib::arm::RotatedImmediate(0x04000000) == 0x301u);
    XSFLIB_CHECK(ntdlib::arm::RotatedImmediate(0x200) == 0xC02u);
    XSFLIB_CHECK(ntdlib::arm::RotatedImmediate(0x104) == 0xF41u);
    XSFLIB_CHECK(!ntdlib::arm::RotatedImmediate(0x101));
    XSFLIB_CHECK(!ntdlib::arm::RotatedImmediate(0x03007F00));
}

void TestDataProcessing()
{
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.Mov(0, 0x12); }) == 0xE3A00012);
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.LoadConstant(3, 0x04000000); }) == 0xE3A03301);
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.LoadConstant(0, 0xFFFFFFFF); }) == 0xE3E00000);
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.Add(3, 3, 0x200); }) == 0xE2833C02);
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.Sub(2, 2, 1, true); }) == 0xE2522001);
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.Orr(0, 0, 8); }) == 0xE3800008);
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.And(1, 2, 0xC0); }) == 0xE20210C0);
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.Tst(0, 1); }) == 0xE3100001);
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.AndShifted(0, 0, 0, 16); }) == 0xE0000820);
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.AndRegister(1, 2, 3); }) == 0xE0021003);
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.OrrRegister(2, 2, 0); }) == 0xE1822000);
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.MovRegister(kLr, kPc); }) == 0xE1A0E00F);
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.Cmp(0, 100); }) == 0xE3500064);
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.CmpRegister(0, 2); }) == 0xE1500002);
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.SubRegister(0, 0, 2); }) == 0xE0400002);
}

void TestTransfers()
{
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.Ldr(2, 3); }) == 0xE5932000);
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.Str(0, 1); }) == 0xE5810000);
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.Ldr(0, 1, -4); }) == 0xE5110004);
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.Strb(0, 1); }) == 0xE5C10000);
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.Ldrb(0, 1, 2); }) == 0xE5D10002);
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.Ldrh(1, 3, 8); }) == 0xE1D310B8);
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.Strh(0, 3, 8); }) == 0xE1C300B8);
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.Strh(1, 0, 0x88); }) == 0xE1C018B8);
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.LdrPost(3, 0, 4); }) == 0xE4903004);
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.LdrIndexed(0, 0, 12, 2); }) == 0xE790010C);
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.StrPost(3, 1, 4); }) == 0xE4813004);
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.LdrbPost(3, 0, 1); }) == 0xE4D03001);
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.StrbPost(3, 1, 1); }) == 0xE4C13001);
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.Push(0x400F); }) == 0xE92D400F);
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.Pop(0x400F); }) == 0xE8BD400F);
}

void TestControl()
{
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.SetControl(0xD2); }) == 0xE321F0D2);
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.SetControl(0x1F); }) == 0xE321F01F);
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.Bx(1); }) == 0xE12FFF11);
    XSFLIB_CHECK(One([](ArmAssembler& a) { a.Swi(5); }) == 0xEF050000);
    XSFLIB_CHECK(Words([](ArmAssembler& a) { a.CallRegister(12); }) == (std::vector<uint32_t>{0xE1A0E00F, 0xE12FFF1C}));
    XSFLIB_CHECK(Words([](ArmAssembler& a) { a.CallThumb(12); }) ==
                 (std::vector<uint32_t>{0xE28FE001, 0xE12FFF1C, 0x46C04778}));
}

void TestBranches()
{
    // A branch back to itself, one back two words, and one forward past a word under a condition.
    const std::vector<uint32_t> words = Words(
        [](ArmAssembler& a)
        {
            const int top = a.NewLabel();
            const int ahead = a.NewLabel();
            a.Bind(top);
            a.B(top);
            a.Mov(0, 0);
            a.B(top);
            a.B(ahead, Condition::kNe);
            a.Mov(0, 1);
            a.Bind(ahead);
            a.Mov(0, 2);
        });

    XSFLIB_REQUIRE(words.size() == 6);
    XSFLIB_CHECK(words[0] == 0xEAFFFFFE);
    XSFLIB_CHECK(words[2] == 0xEAFFFFFC);
    XSFLIB_CHECK(words[3] == 0x1A000000);

    // A call forward past a word, and the unsigned conditions back to the start.
    const std::vector<uint32_t> calls = Words(
        [](ArmAssembler& a)
        {
            const int top = a.NewLabel();
            const int routine = a.NewLabel();
            a.Bind(top);
            a.Bl(routine);
            a.Mov(0, 0);
            a.Bind(routine);
            a.B(top, Condition::kCc);
            a.B(top, Condition::kCs);
        });

    XSFLIB_REQUIRE(calls.size() == 4);
    XSFLIB_CHECK(calls[0] == 0xEB000000);
    XSFLIB_CHECK(calls[2] == 0x3AFFFFFC);
    XSFLIB_CHECK(calls[3] == 0x2AFFFFFB);
}

void TestLiterals()
{
    // Two loads of one constant share its word in the pool, after the code. A label's address is a literal too.
    std::vector<uint32_t> words = Words(
        [](ArmAssembler& a)
        {
            const int here = a.NewLabel();
            a.LoadConstant(kSp, 0x03007F00);
            a.Bind(here);
            a.LoadConstant(1, 0x03007F00);
            a.LoadAddress(2, here);
        });

    XSFLIB_REQUIRE(words.size() == 5);
    XSFLIB_CHECK(words[0] == 0xE59FD004); // ldr sp, [pc, #4]: the word at 0x0C
    XSFLIB_CHECK(words[1] == 0xE59F1000);
    XSFLIB_CHECK(words[2] == 0xE59F2000); // the word at 0x10
    XSFLIB_CHECK(words[3] == 0x03007F00);
    XSFLIB_CHECK(words[4] == kBase + 4);

    // A pool that Pool() places before data that is past a load's reach. The label's address resolves after it.
    words = Words(
        [](ArmAssembler& a)
        {
            const int data = a.NewLabel();
            a.LoadConstant(0, 0x12345678);
            a.LoadAddress(1, data);
            a.Pool();
            a.Bind(data);
            for (int i = 0; i < 1100; i++)
            {
                a.Word(i);
            }
        });

    XSFLIB_REQUIRE(words.size() == 4 + 1100);
    XSFLIB_CHECK(words[0] == 0xE59F0000); // ldr r0, [pc, #0]: the word at 0x08
    XSFLIB_CHECK(words[1] == 0xE59F1000); // the word at 0x0C
    XSFLIB_CHECK(words[2] == 0x12345678 && words[3] == kBase + 16);
    XSFLIB_CHECK(words[4] == 0 && words[1103] == 1099);
}

void TestErrors()
{
    XSFLIB_CHECK(!xsflib::testing::ErrorOf<std::logic_error>([] { ArmAssembler a(kBase + 2); }).empty());
    XSFLIB_CHECK(
        !xsflib::testing::ErrorOf<std::logic_error>([] { Words([](ArmAssembler& a) { a.Mov(0, 0x101); }); }).empty());
    XSFLIB_CHECK(
        !xsflib::testing::ErrorOf<std::logic_error>([] { Words([](ArmAssembler& a) { a.Ldrh(0, 1, 256); }); }).empty());
    XSFLIB_CHECK(
        !xsflib::testing::ErrorOf<std::logic_error>([] { Words([](ArmAssembler& a) { a.B(a.NewLabel()); }); }).empty());

    // A literal more than 4095 bytes past its load.
    const auto far = [](ArmAssembler& a)
    {
        a.LoadConstant(0, 0x12345678);
        for (int i = 0; i < 1030; i++)
        {
            a.Word(0);
        }
    };
    XSFLIB_CHECK(!xsflib::testing::ErrorOf<std::logic_error>([&] { Words(far); }).empty());

    // A load after the pool, and a second pool.
    XSFLIB_CHECK(!xsflib::testing::ErrorOf<std::logic_error>(
                      []
                      {
                          Words(
                              [](ArmAssembler& a)
                              {
                                  a.Pool();
                                  a.LoadConstant(0, 0x12345678);
                              });
                      })
                      .empty());
    XSFLIB_CHECK(!xsflib::testing::ErrorOf<std::logic_error>(
                      []
                      {
                          Words(
                              [](ArmAssembler& a)
                              {
                                  a.Pool();
                                  a.Pool();
                              });
                      })
                      .empty());
}

} // namespace

int main()
{
    TestRotatedImmediate();
    TestDataProcessing();
    TestTransfers();
    TestControl();
    TestBranches();
    TestLiterals();
    TestErrors();

    return xsflib::testing::Summary("ARM assembler");
}
