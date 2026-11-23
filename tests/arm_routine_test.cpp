// SPDX-License-Identifier: MIT

// Tests of the reader of ARM routines (ntdlib/arm/routine.h), on code that the ARM assembler writes, with a few
// instructions that it doesn't write given as words, and on Thumb code (thumb_code.h). Tests of the decoder of branches
// (ntdlib/arm/branch.h).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/arm/assembler.h"
#include "ntdlib/arm/branch.h"
#include "ntdlib/arm/routine.h"
#include "thumb_code.h"

namespace
{

using ntdlib::arm::ArmAssembler;
using ntdlib::arm::Routine;
using ntdlib::arm::Step;
using ntdlib::arm::StepKind;
using ntdlib::arm::Value;
using ntdlib::testing::ThumbCode;
namespace testing = ntdlib::testing;

constexpr uint32_t kBase = 0x02000000;
constexpr std::size_t kLimit = 64;

// moveq r0, #0, ldmfdeq sp!, {r4, pc}, str lr, [sp, #-4]! and blx to Thumb code 14 bytes on.
constexpr uint32_t kMoveqR0 = 0x03A00000;
constexpr uint32_t kPopeqR4Pc = 0x08BD8010;
constexpr uint32_t kStrLr = 0xE52DE004;
constexpr uint32_t kBlxThumb = 0xFB000001;

// A routine that moves its second argument through r4, makes constants with a mvn and a literal, calls a routine,
// reads its result, and returns early when the result is 0.
std::vector<uint8_t> ValuesCode(uint32_t& callee)
{
    ArmAssembler a(kBase);
    const int target = a.NewLabel();
    a.Push(0x4010);
    a.MovRegister(4, 1);
    a.Word(0xE3E01000); // mvn r1, #0
    a.LoadConstant(3, 0x12345678);
    a.MovRegister(2, 4);
    a.Bl(target);
    a.Ldrh(1, 0, 4);
    a.Ldrb(2, 0, 9);
    a.Cmp(0, 0);
    a.Word(kMoveqR0);
    a.Word(kPopeqR4Pc);
    a.MovRegister(0, 4);
    a.Bl(target);
    a.Pop(0x8010);
    a.Mov(0, 1);
    a.Bind(target);
    callee = a.Here();
    a.Bx(14);

    return a.Finish();
}

void TestValues()
{
    uint32_t callee = 0;
    const std::vector<uint8_t> code = ValuesCode(callee);

    const Routine routine = ntdlib::arm::ReadRoutine(code, kBase, kBase, kLimit);

    const std::vector<Step>& steps = routine.steps;
    XSFLIB_CHECK(routine.returns);
    XSFLIB_CHECK(steps.size() == 7);
    if (steps.size() != 7)
    {
        return;
    }
    XSFLIB_CHECK(steps[0].kind == StepKind::kCall && steps[0].target == callee);
    XSFLIB_CHECK(steps[0].arguments[0] == Value::Argument(0));
    XSFLIB_CHECK(steps[0].arguments[1] == Value::Constant(0xFFFFFFFF));
    XSFLIB_CHECK(steps[0].arguments[2] == Value::Argument(1));
    XSFLIB_CHECK(steps[0].arguments[3] == Value::Constant(0x12345678));
    XSFLIB_CHECK(steps[1].kind == StepKind::kLoad && steps[1].base == Value::Result(0));
    XSFLIB_CHECK(steps[1].offset == 4 && steps[1].size == 2);
    XSFLIB_CHECK(steps[2].kind == StepKind::kLoad && steps[2].offset == 9 && steps[2].size == 1);
    XSFLIB_CHECK(steps[3].kind == StepKind::kCompare && steps[3].base == Value::Result(0) && steps[3].immediate == 0u);
    XSFLIB_CHECK(steps[4].kind == StepKind::kReturn && steps[4].condition == 0);
    XSFLIB_CHECK(steps[5].kind == StepKind::kCall && steps[5].arguments[0] == Value::Argument(1));
    XSFLIB_CHECK(steps[5].arguments[1] == Value() && steps[5].arguments[3] == Value());
    XSFLIB_CHECK(steps[6].kind == StepKind::kReturn && steps[6].condition == ntdlib::arm::kAlways);
    XSFLIB_CHECK(routine.Calls().size() == 2 && routine.Calls()[1] == &steps[5]);
}

// A branch that always runs ends the path. So do another routine's start and the limit.
void TestEnds()
{
    ArmAssembler a(kBase);
    const int next = a.NewLabel();
    a.Push(0x4000);
    a.Mov(0, 1);
    a.B(next);
    a.Mov(0, 2);
    a.Bind(next);
    a.Word(kStrLr);
    a.Mov(0, 3);
    a.Pop(0x8000);
    const std::vector<uint8_t> code = a.Finish();

    const Routine branch = ntdlib::arm::ReadRoutine(code, kBase, kBase, kLimit);
    const Routine next_start = ntdlib::arm::ReadRoutine(code, kBase, kBase + 12, kLimit);
    const Routine limited = ntdlib::arm::ReadRoutine(code, kBase, kBase + 16, 2);
    const Routine outside = ntdlib::arm::ReadRoutine(code, kBase, kBase + 0x100, kLimit);

    XSFLIB_CHECK(!branch.returns && branch.steps.size() == 1);
    XSFLIB_CHECK(branch.steps[0].kind == StepKind::kBranch && branch.steps[0].target == kBase + 16);
    XSFLIB_CHECK(next_start.steps.empty() && !next_start.returns);
    XSFLIB_CHECK(limited.steps.empty() && !limited.returns);
    XSFLIB_CHECK(outside.steps.empty());
}

// blx to Thumb code, and the starts of routines.
void TestBlxAndStarts()
{
    ArmAssembler a(kBase);
    a.Push(0x4000);
    a.Word(kBlxThumb);
    a.Pop(0x8000);
    a.Word(kStrLr);
    a.Bx(14);
    const std::vector<uint8_t> code = a.Finish();

    const Routine routine = ntdlib::arm::ReadRoutine(code, kBase, kBase, kLimit);
    const std::vector<uint32_t> starts = ntdlib::arm::RoutineStarts(code, kBase);

    XSFLIB_CHECK(routine.returns && routine.Calls().size() == 1);
    XSFLIB_CHECK(routine.Calls().at(0)->target == ((kBase + 4 + 8 + 4 + 2) | 1));
    XSFLIB_CHECK(starts == (std::vector<uint32_t>{kBase, kBase + 12}));
}

// The stack: a load of a word that the caller passed on the stack gives that argument, after pushes and a move of sp.
// Stores give steps with the stored value, and a load or a store through sp gives none.
void TestStackAndStores()
{
    ArmAssembler a(kBase);
    const int callee = a.NewLabel();
    a.Push(0x40F8);   // stmfd sp!, {r3, r4, r5, r6, r7, lr}: 24 bytes
    a.Sub(13, 13, 8); // 8 more
    a.Ldr(4, 13, 0x24);
    a.Str(4, 13, 0);
    a.MovRegister(5, 1);
    a.Bl(callee);
    a.Str(5, 0, 8);
    a.Str(4, 0, 0x14);
    a.Add(13, 13, 8);
    a.Pop(0x80F8);
    a.Bind(callee);
    a.Bx(14);
    const std::vector<uint8_t> code = a.Finish();

    const Routine routine = ntdlib::arm::ReadRoutine(code, kBase, kBase, kLimit);

    const std::vector<Step>& steps = routine.steps;
    XSFLIB_CHECK(routine.returns && steps.size() == 4);
    if (steps.size() != 4)
    {
        return;
    }
    XSFLIB_CHECK(steps[0].kind == StepKind::kCall);
    XSFLIB_CHECK(steps[1].kind == StepKind::kStore && steps[1].base == Value::Result(0) && steps[1].offset == 8);
    XSFLIB_CHECK(steps[1].size == 4 && steps[1].value == Value::Argument(1));
    XSFLIB_CHECK(steps[2].kind == StepKind::kStore && steps[2].offset == 0x14);
    XSFLIB_CHECK(steps[2].value == Value::Argument(5));
}

// Thumb code: the same steps as ARM code. An early exit that a forward conditional branch skips runs only under the
// opposite condition. Its return therefore doesn't end the path. Code that a forward branch skips and that falls
// through to the target is on the path. bl and blx, and the stack, as in ARM code.
void TestThumb()
{
    ThumbCode t(kBase);
    t.Bind("routine");
    t.Half(testing::ThumbPush(0x38)); // push {r3, r4, r5, lr}: 16 bytes
    t.Half(testing::ThumbSubSp(8));
    t.Half(testing::ThumbCopy(4, 1));
    t.Half(testing::ThumbCopy(0, 4));
    t.Bl("thumb");
    t.Half(testing::ThumbCopy(3, 0));
    t.B(testing::kThumbNe, "go_on");
    t.Half(testing::ThumbAddSp(8));
    t.Half(testing::ThumbMovs(0, 0));
    t.Half(testing::ThumbPop(0x38));
    t.Bind("go_on");
    t.Half(testing::ThumbLdrb(1, 3, 9));
    t.Half(testing::ThumbLdrh(2, 3, 4));
    t.Half(testing::ThumbLdrSp(3, 0x18));
    t.Half(testing::ThumbStr(4, 0, 0x10));
    t.Half(testing::ThumbCmp(4, 0));
    t.B(testing::kThumbGe, "skip");
    t.Half(testing::ThumbLdrb(4, 0, 8));
    t.Bind("skip");
    t.Half(testing::ThumbCopy(1, 4));
    t.Blx("arm");
    t.Half(testing::ThumbPop(0x38));
    t.Bind("thumb");
    t.Half(testing::kThumbBxLr);
    t.Align();
    t.Bind("arm");
    t.Half(0xFF1E); // bx lr in ARM code
    t.Half(0xE12F);
    const std::vector<uint8_t> code = t.Finish();

    const Routine routine = ntdlib::arm::ReadRoutine(code, kBase, kBase | 1, kLimit);
    const std::vector<uint32_t> starts = ntdlib::arm::ThumbRoutineStarts(code, kBase);

    const std::vector<Step>& steps = routine.steps;
    XSFLIB_CHECK(routine.address == (kBase | 1) && routine.returns);
    XSFLIB_CHECK(steps.size() == 11);
    if (steps.size() != 11)
    {
        return;
    }
    XSFLIB_CHECK(steps[0].kind == StepKind::kCall && steps[0].target == (t.At("thumb") | 1));
    XSFLIB_CHECK(steps[0].arguments[0] == Value::Argument(1));
    XSFLIB_CHECK(steps[1].kind == StepKind::kBranch && steps[1].condition == testing::kThumbNe);
    XSFLIB_CHECK(steps[2].kind == StepKind::kReturn && steps[2].condition == testing::kThumbEq);
    XSFLIB_CHECK(steps[3].kind == StepKind::kLoad && steps[3].base == Value::Result(0) && steps[3].offset == 9);
    XSFLIB_CHECK(steps[4].kind == StepKind::kLoad && steps[4].offset == 4 && steps[4].size == 2);
    XSFLIB_CHECK(steps[5].kind == StepKind::kStore && steps[5].value == Value::Argument(1) && steps[5].offset == 0x10);
    XSFLIB_CHECK(steps[6].kind == StepKind::kCompare && steps[6].base == Value::Argument(1));
    XSFLIB_CHECK(steps[7].kind == StepKind::kBranch && steps[7].condition == testing::kThumbGe);
    XSFLIB_CHECK(steps[8].kind == StepKind::kLoad && steps[8].condition == ntdlib::arm::kAlways);
    XSFLIB_CHECK(steps[9].kind == StepKind::kCall && steps[9].target == t.At("arm"));
    XSFLIB_CHECK(steps[9].arguments[0] == Value::Result(0) && steps[9].arguments[1] == Value());
    XSFLIB_CHECK(steps[9].arguments[3] == Value::Argument(4));
    XSFLIB_CHECK(steps[10].kind == StepKind::kReturn && steps[10].condition == ntdlib::arm::kAlways);
    XSFLIB_CHECK(!starts.empty() && starts.front() == (kBase | 1));
}

// ARM code with a call, a load, a conditional call, a call through a register and a blx to Thumb code.
std::vector<uint8_t> ArmCalls()
{
    constexpr uint32_t kBleq = 0x0B000000; // bleq to 8 bytes on
    constexpr uint32_t kBlxR3 = 0xE12FFF33;
    ArmAssembler a(kBase);
    const int callee = a.NewLabel();
    a.Push(0x4000);
    a.Bl(callee);
    a.Ldrb(1, 0, 9);
    a.Word(kBleq);
    a.Word(kBlxR3);
    a.Word(kBlxThumb);
    a.Pop(0x8000);
    a.Bind(callee);
    a.Bx(14);

    return a.Finish();
}

// Thumb code with a bl to Thumb code, a call through a register and a blx to ARM code.
std::vector<uint8_t> ThumbCalls()
{
    constexpr uint16_t kThumbBlxR3 = 0x4798;
    ThumbCode t(kBase);
    t.Half(testing::ThumbPush(0));
    t.Bl("thumb");
    t.Half(kThumbBlxR3);
    t.Blx("arm");
    t.Half(testing::ThumbPop(0));
    t.Bind("thumb");
    t.Half(testing::kThumbBxLr);
    t.Align();
    t.Bind("arm");
    t.Half(0xFF1E); // bx lr in ARM code
    t.Half(0xE12F);

    return t.Finish();
}

// IsDirectCall: a call that always runs, to an address in the code, in ARM or in Thumb code. A conditional call, a call
// through a register and a step of another kind aren't.
void TestDirectCalls()
{
    const std::vector<uint8_t> arm_code = ArmCalls();
    const std::vector<uint8_t> thumb_code = ThumbCalls();
    const Step branch = {.kind = StepKind::kBranch, .target = kBase};

    const Routine arm = ntdlib::arm::ReadRoutine(arm_code, kBase, kBase, kLimit);
    const Routine thumb = ntdlib::arm::ReadRoutine(thumb_code, kBase, kBase | 1, kLimit);

    const std::vector<const Step*> arm_calls = arm.Calls();
    const std::vector<const Step*> thumb_calls = thumb.Calls();
    XSFLIB_CHECK(arm.returns && arm_calls.size() == 4 && arm.steps.size() == 6);
    XSFLIB_CHECK(thumb.returns && thumb_calls.size() == 3);
    if (arm_calls.size() != 4 || arm.steps.size() != 6 || thumb_calls.size() != 3)
    {
        return;
    }

    XSFLIB_CHECK(ntdlib::arm::IsDirectCall(*arm_calls[0]));
    XSFLIB_CHECK(!ntdlib::arm::IsDirectCall(arm.steps[1]));
    XSFLIB_CHECK(!ntdlib::arm::IsDirectCall(*arm_calls[1]));
    XSFLIB_CHECK(!ntdlib::arm::IsDirectCall(*arm_calls[2]));
    XSFLIB_CHECK(ntdlib::arm::IsDirectCall(*arm_calls[3]));
    XSFLIB_CHECK(ntdlib::arm::IsDirectCall(*thumb_calls[0]));
    XSFLIB_CHECK(!ntdlib::arm::IsDirectCall(*thumb_calls[1]));
    XSFLIB_CHECK(ntdlib::arm::IsDirectCall(*thumb_calls[2]));
    XSFLIB_CHECK(!ntdlib::arm::IsDirectCall(branch));
}

// B and BL under any condition are branches, and BLX with an offset (condition 0xF) isn't. The offset counts words
// from the branch's address plus 8, forward and back. BLX's bit 24 adds a halfword.
void TestBranches()
{
    constexpr uint32_t kB = 0xEA000002;       // b 2 words on from pc
    constexpr uint32_t kBlne = 0x1BFFFFFE;    // blne back to its address
    constexpr uint32_t kBack = 0xEAFFFFF0;    // b 16 words back from pc
    constexpr uint32_t kMov = 0xE1A00000;     // mov r0, r0
    constexpr uint32_t kBlxBack = 0xFAFFFFFE; // blx back to its address

    XSFLIB_CHECK(ntdlib::arm::IsBranch(kB) && !ntdlib::arm::IsLink(kB));
    XSFLIB_CHECK(ntdlib::arm::IsBranch(kBlne) && ntdlib::arm::IsLink(kBlne));
    XSFLIB_CHECK(!ntdlib::arm::IsBranch(kBlxThumb) && !ntdlib::arm::IsBranch(kMov));
    XSFLIB_CHECK(ntdlib::arm::BranchOffset(kB) == 8 && ntdlib::arm::BranchOffset(kBack) == -64);
    XSFLIB_CHECK(ntdlib::arm::BranchTarget(kBase, kB) == kBase + 16);
    XSFLIB_CHECK(ntdlib::arm::BranchTarget(kBase + 0x40, kBlne) == kBase + 0x40);
    XSFLIB_CHECK(ntdlib::arm::BranchTarget(kBase + 0x100, kBack) == kBase + 0xC8);
    XSFLIB_CHECK(ntdlib::arm::IsBlxImmediate(kBlxThumb) && ntdlib::arm::IsBlxImmediate(kBlxBack));
    XSFLIB_CHECK(!ntdlib::arm::IsBlxImmediate(kB) && !ntdlib::arm::IsBlxImmediate(kBlne));
    XSFLIB_CHECK(ntdlib::arm::BlxOffset(kBlxThumb) == 6 && ntdlib::arm::BlxOffset(kBlxBack) == -8);
    XSFLIB_CHECK(ntdlib::arm::BlxTarget(kBase, kBlxThumb) == kBase + 14);
    XSFLIB_CHECK(ntdlib::arm::BlxTarget(kBase + 0x40, kBlxBack) == kBase + 0x40);
}

} // namespace

int main()
{
    TestValues();
    TestEnds();
    TestBlxAndStarts();
    TestStackAndStores();
    TestThumb();
    TestDirectCalls();
    TestBranches();

    return xsflib::testing::Summary("ARM routines");
}
