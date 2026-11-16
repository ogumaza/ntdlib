// SPDX-License-Identifier: MIT

// A reader of ARM routines for searches by shape: it reads a routine's ARM-state code as a list of steps, the calls,
// the loads, the compares, the branches and the return, and follows the values that the registers hold. A rule on
// those steps holds for the many ways in which compilers write the same routine: a constant from a mov, a mvn or a
// literal pool, an argument moved through other registers, and instructions in another order.
//
// The reader follows the path on which no conditional instruction runs, from the routine's first instruction in
// order. A conditional instruction gives a step, and leaves the values as they were. The routines that the searches
// read take conditional instructions on their early exits. The reader stops at the first unconditional return, at an
// unconditional branch, at another routine's start or at its limit. The encodings follow ARM's architecture reference
// manual for ARMv5TE.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ntdlib::arm
{

// A value that a register holds at a step, as far as the routine's code shows it.
struct Value
{
    enum class Kind : uint8_t
    {
        kUnknown,
        kConstant, // `number` is the value
        kArgument, // `number` is the register that held the value at the routine's start: 0 to 3
        kResult,   // `number` is the call, counted from 0 in the routine's order, that returned the value in r0
    };

    friend bool operator==(const Value&, const Value&) = default;

    static Value Constant(uint32_t value);
    static Value Argument(uint32_t reg);
    static Value Result(uint32_t call);

    Kind kind = Kind::kUnknown;
    uint32_t number = 0;
};

// The condition of an instruction that always runs.
constexpr uint32_t kAlways = 0xE;

// The condition "not equal".
constexpr uint32_t kNotEqual = 0x1;

enum class StepKind : uint8_t
{
    kCall,    // bl, blx with an offset or with a register
    kLoad,    // ldr, ldrb, ldrh, ldrsb and ldrsh with a base register other than pc
    kCompare, // cmp
    kBranch,  // b, or bx and other writes of pc that aren't returns
    kReturn,  // bx lr, mov pc, lr, ldmfd sp!, {..., pc} and ldr pc, [sp], #4
};

struct Step
{
    friend bool operator==(const Step&, const Step&) = default;

    StepKind kind = StepKind::kCall;
    uint32_t address = 0;
    uint32_t condition = kAlways;

    // kCall and kBranch: where the code goes, with bit 0 set for Thumb code. Nothing where a register gives it.
    std::optional<uint32_t> target;

    // kCall: the values of r0 to r3 at the call.
    std::array<Value, 4> arguments = {};

    // kLoad: the base register's value, the offset from it and the size in bytes. The offset is nothing for a load
    // indexed by a register, and 0 for one that moves its base after the access. kCompare: the compared register's
    // value, and the immediate, or nothing for a register.
    Value base;
    std::optional<int32_t> offset;
    uint32_t size = 0;
    std::optional<uint32_t> immediate;
};

// A routine's steps, in the order that the reader follows.
struct Routine
{
    uint32_t address = 0;
    std::vector<Step> steps;
    bool returns = false; // the last step is an unconditional return

    // The calls, in order: the steps of kind kCall.
    std::vector<const Step*> Calls() const;
};

// The routine at `address` in `code`, ARM code that starts at `base`, read for at most `limit` instructions. A
// literal that a load from pc reads comes from `code`. A routine that starts outside `code` has no steps.
Routine ReadRoutine(std::span<const uint8_t> code, uint32_t base, uint32_t address, std::size_t limit);

// The addresses in `code` of the instructions that start a routine by saving lr: stmfd sp!, {..., lr} or str lr,
// [sp, #-4]!.
std::vector<uint32_t> RoutineStarts(std::span<const uint8_t> code, uint32_t base);

} // namespace ntdlib::arm
