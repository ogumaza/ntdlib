// SPDX-License-Identifier: MIT

// A static walk of a JAudio sequence's bytecode: the banks and programs that its notes can start with, along every path
// from its start. Each path has a cursor, a bank, a program and the track's stack. A walk that meets something it can't
// follow, such as a jump through a table or an interrupt, is incomplete. The sequence can then play any program of any
// bank. WalkSequence reads JAudio2's sequences. jaudio1/sequence_walk.h reads JAudio1's with the same paths.

#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace ntdlib::jaudio
{

// A bank and a program that a note of a sequence can start with. kAny stands for a number that the sequence takes
// from a register or a port.
struct InstrumentUse
{
    static constexpr uint16_t kAny = 0xffff;

    auto operator<=>(const InstrumentUse&) const = default;

    uint16_t bank = 0;
    uint16_t program = 0;
};

// What a walk of a sequence finds, along every path from its start. An incomplete walk met something that it can't
// follow. The sequence can then play any program of any bank.
struct SequenceInstruments
{
    bool complete = true;
    std::string stopped_by;         // what stopped an incomplete walk, for a test or a report
    std::set<InstrumentUse> uses;   // empty for a sequence that starts no note on a bank
    std::vector<uint32_t> commands; // the offsets of the commands that the walk passed, ascending, each once
};

// Walks a JAudio2 sequence from `start` in `data`, as the model's parser reads it (jaudio2's Tracks::Parse).
SequenceInstruments WalkSequence(std::span<const uint8_t> data, uint32_t start);

// The paths of a walk, for the decoders of each engine's bytecode. A decoder takes the next path, reads the command at
// its cursor, and adds the paths that follow it. A path is walked once. The engines' steps of the stack are the same
// in both: a track holds a stack of 8 cursors, each a call's return or a loop's start.
class SequenceWalk
{
public:
    // The most paths that a walk takes. Past them the walk is incomplete.
    static constexpr std::size_t kLimit = 1000000;

    static constexpr uint32_t kStackSize = 8;

    // A place of a walk: the cursor at a command, the bank and the program (0 to 255, or InstrumentUse::kAny), and the
    // track's stack. The entries above the depth are 0.
    struct Path
    {
        bool operator==(const Path&) const = default;

        uint32_t cursor = 0;
        uint16_t bank = 0;
        uint16_t program = 0;
        uint32_t depth = 0;
        std::array<uint32_t, kStackSize> stack{};
    };

    // A walk from `start` in `data`. A test may give a lower limit.
    SequenceWalk(std::span<const uint8_t> data, const Path& start, std::size_t limit = kLimit);

    // The sequence's bytes. A byte past the end reads 0xff, a sequence's end.
    uint8_t U8(uint32_t offset) const
    {
        return offset < data_.size() ? data_[offset] : 0xff;
    }

    uint16_t U16(uint32_t offset) const
    {
        return static_cast<uint16_t>(U8(offset) << 8 | U8(offset + 1));
    }

    uint32_t U24(uint32_t offset) const
    {
        return static_cast<uint32_t>(U8(offset)) << 16 | static_cast<uint32_t>(U8(offset + 1)) << 8 | U8(offset + 2);
    }

    // Takes the next path to walk into `path`, and records its cursor as a command's offset. False once no path is
    // left, or once the walk has stopped.
    bool Next(Path& path);

    // Adds a path to walk, unless the walk has taken it already.
    void Add(const Path& path);

    // A note on the path's bank and program. Programs 0xf0 to 0xff play the DSP's oscillator and no wave.
    void Note(const Path& path);

    // A child track that opens at `start` with `bank` and `program`, and an empty stack.
    void Child(uint32_t start, uint16_t bank, uint16_t program);

    // The steps of the stack. `path` holds the cursor after the command. A call or a loop's start pushes that cursor
    // while the depth is below 8, and does nothing at depth 8. A return pops the top entry and goes there. A return at
    // depth 0 goes on where `at_depth_0_goes_on`, and ends the path otherwise. A loop's end at depth 0 goes on.
    // Otherwise it goes back to the top entry and keeps the stack, or pops the entry and goes on. The walk takes both.
    void Call(Path path, uint32_t target);
    void LoopStart(Path path);
    void Return(Path path, bool at_depth_0_goes_on);
    void LoopEnd(Path path);

    // Ends the walk as incomplete: `reason` says what it can't follow.
    void Stop(const std::string& reason);

    // The walk's result.
    SequenceInstruments Finish();

private:
    struct PathHash
    {
        std::size_t operator()(const Path& path) const;
    };

    std::span<const uint8_t> data_;
    std::size_t limit_;
    std::vector<Path> work_;
    std::unordered_set<Path, PathHash> walked_;
    std::set<uint32_t> commands_;
    SequenceInstruments result_;
};

} // namespace ntdlib::jaudio
