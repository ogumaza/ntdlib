// SPDX-License-Identifier: MIT

// Tests of the reader of ARM routines (ntdlib/arm/routine.h), on code that the ARM assembler writes, with a few
// instructions that it doesn't write given as words.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/arm/assembler.h"
#include "ntdlib/arm/routine.h"

namespace
{

using ntdlib::arm::ArmAssembler;
using ntdlib::arm::Routine;
using ntdlib::arm::Step;
using ntdlib::arm::StepKind;
using ntdlib::arm::Value;

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

} // namespace

int main()
{
    TestValues();
    TestEnds();
    TestBlxAndStarts();

    return xsflib::testing::Summary("ARM routines");
}
