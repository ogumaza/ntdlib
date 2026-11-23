// SPDX-License-Identifier: MIT

#include "ntdlib/ntr/sound_routines.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <set>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/arm/routine.h"
#include "ntdlib/ntr/code.h"

namespace ntdlib::ntr
{

namespace
{

using arm::IsDirectCall;
using arm::Routine;
using arm::Step;
using arm::StepKind;
using arm::Value;

// The instructions that the reader reads of each routine, and the most that each rule's routine takes.
constexpr std::size_t kReadLimit = 64;
constexpr uint32_t kUpdateBytes = 4 * 20;
constexpr uint32_t kLoadBytes = 4 * 16;
constexpr uint32_t kWaitBytes = 4 * 10;

// The update's calls between the loop and the flush.
constexpr std::size_t kFewestUpdateCalls = 2;
constexpr std::size_t kMostUpdateCalls = 4;

// The record of a sequence: the bank (a half at 4), the priority (a byte at 8) and the player (a byte at 9).
constexpr int32_t kBankOffset = 4;
constexpr int32_t kPriorityOffset = 8;
constexpr int32_t kPlayerOffset = 9;

// The load's arguments: the sequence, the loads of every kind (0xFF), the heap and 1.
constexpr uint32_t kEveryKind = 0xFF;

// The calls of the loader's callee among which it looks for the sequence's record.
constexpr std::size_t kLoadGetInfoCalls = 3;

// OS_WaitIrq's arguments: the interrupt of the vertical blank, and whether to clear its check bit first.
constexpr uint32_t kVBlankIrq = 1;

// The interrupt check word: DTCM + 0x3FF8, in a DTCM of 16 KiB.
constexpr uint32_t kDtcmBytes = 0x4000;
constexpr uint32_t kIrqCheckOffset = 0x3FF8;

// The command routine's stores: the id and the four arguments of the command, from offset 4 on.
constexpr int32_t kCommandFields = 5;
constexpr int32_t kCommandFirstField = 4;

// The size of a routine that returns: from its start to its return's end. Thumb's returns take two bytes.
uint32_t Bytes(const Routine& routine)
{
    const bool thumb = (routine.address & 1) != 0;
    return routine.steps.back().address + (thumb ? 2 : 4) - (routine.address & ~1u);
}

// The address that a load or a store reaches when its base is a constant.
std::optional<uint32_t> ConstantAddress(const Step& step)
{
    if (step.base.kind != Value::Kind::kConstant || !step.offset)
    {
        return std::nullopt;
    }

    return step.base.number + static_cast<uint32_t>(*step.offset);
}

// The update: a call with 0 that a bne repeats after a compare of its result with 0, then two to four calls, a call
// with 0 and the return. No other step: five steps and the calls. The calls may go to Thumb code. The calls between
// take none of the routine's arguments: the update takes none.
std::optional<SoundUpdate> MatchUpdate(const Routine& routine)
{
    std::vector<Step> steps;
    std::ranges::copy_if(routine.steps, std::back_inserter(steps),
                         [](const Step& step) { return step.kind != StepKind::kStore; });
    const std::size_t count = steps.size();
    if (!routine.returns || count < 5 + kFewestUpdateCalls || count > 5 + kMostUpdateCalls ||
        Bytes(routine) > kUpdateBytes)
    {
        return std::nullopt;
    }

    const Step& receive = steps[0];
    const Step& compare = steps[1];
    const Step& loop = steps[2];
    const Step& flush = steps[count - 2];
    if (!IsDirectCall(receive) || receive.arguments[0] != Value::Constant(0) || compare.kind != StepKind::kCompare ||
        compare.base != Value::Result(0) || compare.immediate != 0u || loop.kind != StepKind::kBranch ||
        loop.condition != arm::kNotEqual || !loop.target || *loop.target < routine.address ||
        *loop.target > receive.address || !IsDirectCall(flush) || flush.arguments[0] != Value::Constant(0))
    {
        return std::nullopt;
    }

    const auto argument = [](const Value& value)
    {
        return value.kind == Value::Kind::kArgument;
    };
    SoundUpdate update{.address = routine.address, .receive = *receive.target, .flush = *flush.target};
    for (std::size_t i = 3; i < count - 2; i++)
    {
        if (!IsDirectCall(steps[i]) || std::ranges::any_of(steps[i].arguments, argument))
        {
            return std::nullopt;
        }
        update.calls.push_back(*steps[i].target);
    }

    return update;
}

// The start: two calls, the first with the second argument and the second with the first. The three loads of the record
// that the first returns are between them, and the routine has no other load.
std::optional<SequenceStart> MatchStart(const Routine& routine)
{
    const std::vector<const Step*> calls = routine.Calls();
    if (!routine.returns || calls.size() != 2 || !IsDirectCall(*calls[0]) || !IsDirectCall(*calls[1]) ||
        calls[0]->arguments[0] != Value::Argument(1) || calls[1]->arguments[0] != Value::Argument(0))
    {
        return std::nullopt;
    }

    std::set<std::pair<int32_t, uint32_t>> fields;
    for (const Step& step : routine.steps)
    {
        if (step.kind != StepKind::kLoad)
        {
            continue;
        }
        if (step.address < calls[0]->address || step.address > calls[1]->address || step.base != Value::Result(0) ||
            !step.offset)
        {
            return std::nullopt;
        }
        fields.insert({*step.offset, step.size});
    }

    const std::set<std::pair<int32_t, uint32_t>> record = {{kBankOffset, 2}, {kPriorityOffset, 1}, {kPlayerOffset, 1}};
    if (fields != record)
    {
        return std::nullopt;
    }

    return SequenceStart{.address = routine.address, .get_info = *calls[0]->target, .start = *calls[1]->target};
}

// The start's ex form: two calls, the first with the fifth argument (the sequence, on the stack) and the second with
// the first four. Between them, each of the player (argument 1), the bank (argument 2) and the priority (argument 3) is
// compared with 0 and then loaded from the record that the first call returns, under a condition. The routine has no
// other load.
std::optional<SequenceStart> MatchStartEx(const Routine& routine)
{
    constexpr uint32_t kSequenceArgument = 4;
    const std::vector<const Step*> calls = routine.Calls();
    const std::array<Value, 4> passed = {Value::Argument(0), Value::Argument(1), Value::Argument(2),
                                         Value::Argument(3)};
    if (!routine.returns || calls.size() != 2 || !IsDirectCall(*calls[0]) || !IsDirectCall(*calls[1]) ||
        calls[0]->arguments[0] != Value::Argument(kSequenceArgument) || calls[1]->arguments != passed)
    {
        return std::nullopt;
    }

    // Each field of the record, by the argument that a negative value of reads it.
    const std::set<std::tuple<uint32_t, int32_t, uint32_t>> record = {
        {1, kPlayerOffset, 1}, {2, kBankOffset, 2}, {3, kPriorityOffset, 1}};
    std::set<std::tuple<uint32_t, int32_t, uint32_t>> fields;
    const std::vector<Step>& steps = routine.steps;
    for (std::size_t i = 0; i < steps.size(); i++)
    {
        const Step& step = steps[i];
        if (step.kind != StepKind::kLoad)
        {
            continue;
        }

        const Step* compare = i > 0 ? &steps[i - 1] : nullptr;
        if (step.address < calls[0]->address || step.address > calls[1]->address || step.base != Value::Result(0) ||
            !step.offset || step.condition == arm::kAlways || !compare || compare->kind != StepKind::kCompare ||
            compare->base.kind != Value::Kind::kArgument || compare->immediate != 0u)
        {
            return std::nullopt;
        }
        fields.insert({compare->base.number, *step.offset, step.size});
    }
    if (fields != record)
    {
        return std::nullopt;
    }

    return SequenceStart{
        .address = routine.address, .get_info = *calls[0]->target, .start = *calls[1]->target, .ex = true};
}

// The load's wrapper: one call with the sequence, 0xFF, the heap and 1, and the return, within 16 instructions.
std::optional<SequenceLoad> MatchLoadWrapper(const Routine& routine)
{
    const std::vector<const Step*> calls = routine.Calls();
    if (!routine.returns || calls.size() != 1 || !IsDirectCall(*calls[0]) || Bytes(routine) > kLoadBytes)
    {
        return std::nullopt;
    }

    const std::array<Value, 4> arguments = {Value::Argument(0), Value::Constant(kEveryKind), Value::Argument(1),
                                            Value::Constant(1)};
    if (calls[0]->arguments != arguments)
    {
        return std::nullopt;
    }

    return SequenceLoad{.wrapper = routine.address, .load = *calls[0]->target};
}

// The wait: two calls, (1) and (1, 1), and the return, with no load, within ten instructions.
bool MatchWait(const Routine& routine)
{
    const std::vector<const Step*> calls = routine.Calls();
    const bool loads =
        std::ranges::any_of(routine.steps, [](const Step& step) { return step.kind == StepKind::kLoad; });

    return routine.returns && !loads && Bytes(routine) <= kWaitBytes && calls.size() == 2 && IsDirectCall(*calls[0]) &&
           IsDirectCall(*calls[1]) && calls[0]->arguments[0] == Value::Constant(1) &&
           calls[1]->arguments[0] == Value::Constant(1) && calls[1]->arguments[1] == Value::Constant(1);
}

// OS_WaitIrq(clear, mask): a compare of its first argument with 0, then a load of the interrupt check word, a store of
// the same word, a later load of it, and a call after that load.
bool MatchWaitIrq(const Routine& routine)
{
    const std::vector<Step>& steps = routine.steps;
    const auto compare = std::ranges::find_if(
        steps, [](const Step& step)
        { return step.kind == StepKind::kCompare && step.base == Value::Argument(0) && step.immediate == 0u; });
    std::optional<uint32_t> check;
    bool stored = false;
    bool tested = false;
    for (auto step = compare; step != steps.end(); ++step)
    {
        const std::optional<uint32_t> address = ConstantAddress(*step);
        if (!check && step->kind == StepKind::kLoad && address && *address % kDtcmBytes == kIrqCheckOffset &&
            step->size == 4)
        {
            check = address;
        }
        else if (check && !stored && step->kind == StepKind::kStore && address == check)
        {
            stored = true;
        }
        else if (stored && !tested && step->kind == StepKind::kLoad && address == check)
        {
            tested = true;
        }
        else if (tested && IsDirectCall(*step))
        {
            return true;
        }
    }

    return false;
}

// The command routine: a call with 1, then the five stores of the arguments through its result, then a call with
// the result.
bool MatchCommand(const Routine& routine)
{
    const std::vector<const Step*> calls = routine.Calls();
    if (!routine.returns || calls.size() != 2 || !IsDirectCall(*calls[0]) || !IsDirectCall(*calls[1]) ||
        calls[0]->arguments[0] != Value::Constant(1) || calls[1]->arguments[0] != Value::Result(0))
    {
        return false;
    }

    // Each store's offset and the argument that it stores.
    std::set<std::pair<int32_t, uint32_t>> stores;
    for (const Step& step : routine.steps)
    {
        if (step.kind != StepKind::kStore || step.address < calls[0]->address || step.address > calls[1]->address)
        {
            continue;
        }
        if (step.base != Value::Result(0) || !step.offset || step.size != 4 || step.condition != arm::kAlways)
        {
            return false;
        }
        if (step.value.kind != Value::Kind::kArgument)
        {
            return false;
        }
        stores.insert({*step.offset, step.value.number});
    }

    std::set<std::pair<int32_t, uint32_t>> expected;
    for (int32_t field = 0; field < kCommandFields; field++)
    {
        expected.insert({kCommandFirstField + 4 * field, static_cast<uint32_t>(field)});
    }

    return stores == expected;
}

// Whether `routine` loads the sequence that its first argument gives: a call of one of `get_infos` with the argument,
// among its first calls, then a call that passes the routine's second to fourth arguments on (the load of the bank).
bool LoadsSequence(const Routine& routine, const std::set<uint32_t>& get_infos)
{
    const std::vector<const Step*> calls = routine.Calls();
    const std::array<Value, 3> passed = {Value::Argument(1), Value::Argument(2), Value::Argument(3)};
    for (std::size_t i = 0; i < calls.size() && i < kLoadGetInfoCalls; i++)
    {
        if (!IsDirectCall(*calls[i]) || !get_infos.contains(*calls[i]->target) ||
            calls[i]->arguments[0] != Value::Argument(0))
        {
            continue;
        }

        const auto passes_on = [&passed](const Step* call)
        {
            return std::ranges::equal(std::span(call->arguments).subspan(1), passed);
        };
        return std::any_of(calls.begin() + static_cast<std::ptrdiff_t>(i) + 1, calls.end(), passes_on);
    }

    return false;
}

// The loads: each wrapper whose callee loads the sequence. Without one, each routine that loads the sequence that its
// first argument gives.
std::vector<SequenceLoad> FindLoads(const LoadedCode& arm9, const std::vector<Routine>& routines,
                                    const std::vector<SequenceLoad>& wrappers, const SoundRoutines& found)
{
    std::set<uint32_t> get_infos;
    for (const SequenceStart& start : found.starts)
    {
        get_infos.insert(start.get_info);
    }

    std::vector<SequenceLoad> loads;
    for (const SequenceLoad& wrapper : wrappers)
    {
        if (LoadsSequence(arm::ReadRoutine(arm9.bytes, arm9.address, wrapper.load, kReadLimit), get_infos))
        {
            loads.push_back(wrapper);
        }
    }
    if (!loads.empty())
    {
        return loads;
    }

    for (const Routine& routine : routines)
    {
        if (LoadsSequence(routine, get_infos))
        {
            loads.push_back({.load = routine.address});
        }
    }

    return loads;
}

} // namespace

SoundRoutines FindSoundRoutines(const LoadedCode& arm9, uint32_t entry)
{
    SoundRoutines found;
    found.decompressor = FindStartupDecompressor(arm9, entry);

    std::vector<uint32_t> starts = arm::RoutineStarts(arm9.bytes, arm9.address);
    const std::vector<uint32_t> thumb_starts = arm::ThumbRoutineStarts(arm9.bytes, arm9.address);
    starts.insert(starts.end(), thumb_starts.begin(), thumb_starts.end());

    std::vector<Routine> routines;
    std::vector<SequenceLoad> wrappers;
    std::vector<SequenceStart> ex_starts;
    std::vector<uint32_t> irq_waits;
    for (const uint32_t address : starts)
    {
        Routine routine = arm::ReadRoutine(arm9.bytes, arm9.address, address, kReadLimit);
        if (routine.steps.empty())
        {
            continue;
        }
        if (std::optional<SoundUpdate> update = MatchUpdate(routine))
        {
            found.updates.push_back(std::move(*update));
        }
        if (const std::optional<SequenceStart> start = MatchStart(routine))
        {
            found.starts.push_back(*start);
        }
        if (const std::optional<SequenceStart> start = MatchStartEx(routine))
        {
            ex_starts.push_back(*start);
        }
        if (const std::optional<SequenceLoad> wrapper = MatchLoadWrapper(routine))
        {
            wrappers.push_back(*wrapper);
        }
        if (MatchWait(routine))
        {
            found.waits.push_back({.address = address});
        }
        if (MatchWaitIrq(routine))
        {
            irq_waits.push_back(address);
        }
        if (MatchCommand(routine))
        {
            found.commands.push_back(address);
        }
        routines.push_back(std::move(routine));
    }

    // The fallbacks: the ex form of the start, and OS_WaitIrq(1, 1) as the wait.
    if (found.starts.empty())
    {
        found.starts = std::move(ex_starts);
    }
    if (found.waits.empty())
    {
        for (const uint32_t address : irq_waits)
        {
            found.waits.push_back({.address = address, .arguments = {kVBlankIrq, kVBlankIrq}});
        }
    }
    found.loads = FindLoads(arm9, routines, wrappers, found);

    return found;
}

} // namespace ntdlib::ntr
