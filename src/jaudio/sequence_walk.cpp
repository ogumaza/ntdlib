// SPDX-License-Identifier: MIT

// The paths of a walk of a JAudio sequence, and the decoder of JAudio2's bytecode. The decoder mirrors the model's
// parser (Tracks::Parse and Tracks::ParseCommand in jaudio2/track_commands.cpp) byte for byte.

#include "ntdlib/jaudio/sequence_walk.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include "ntdlib/jaudio/sequence_commands.h"

namespace ntdlib::jaudio
{

SequenceWalk::SequenceWalk(std::span<const uint8_t> data, const Path& start, std::size_t limit)
    : data_(data), limit_(limit)
{
    Add(start);
}

std::size_t SequenceWalk::PathHash::operator()(const Path& path) const
{
    // FNV-1a over the fields.
    uint64_t hash = 0xcbf29ce484222325ull;
    const auto mix = [&hash](uint64_t value)
    {
        hash ^= value;
        hash *= 0x100000001b3ull;
    };
    mix(path.cursor);
    mix(uint64_t{path.bank} << 16 | path.program);
    mix(path.depth);
    for (uint32_t i = 0; i < path.depth; i++)
    {
        mix(path.stack[i]);
    }

    return static_cast<std::size_t>(hash);
}

bool SequenceWalk::Next(Path& path)
{
    if (!result_.complete || work_.empty())
    {
        return false;
    }

    path = work_.back();
    work_.pop_back();
    commands_.insert(path.cursor);

    return true;
}

void SequenceWalk::Add(const Path& path)
{
    if (!result_.complete || !walked_.insert(path).second)
    {
        return;
    }

    if (walked_.size() > limit_)
    {
        Stop("more than " + std::to_string(limit_) + " paths");
        return;
    }

    work_.push_back(path);
}

void SequenceWalk::Note(const Path& path)
{
    if (path.program < 0xf0 || path.program > 0xff)
    {
        result_.uses.insert({path.bank, path.program});
    }
}

void SequenceWalk::Child(uint32_t start, uint16_t bank, uint16_t program)
{
    Add({.cursor = start, .bank = bank, .program = program});
}

void SequenceWalk::Call(Path path, uint32_t target)
{
    if (path.depth < kStackSize)
    {
        path.stack[path.depth++] = path.cursor;
        path.cursor = target;
    }

    Add(path);
}

void SequenceWalk::LoopStart(Path path)
{
    if (path.depth < kStackSize)
    {
        path.stack[path.depth++] = path.cursor;
    }

    Add(path);
}

void SequenceWalk::Return(Path path, bool at_depth_0_goes_on)
{
    if (path.depth == 0)
    {
        if (at_depth_0_goes_on)
        {
            Add(path);
        }
        return;
    }

    path.cursor = path.stack[--path.depth];
    path.stack[path.depth] = 0;
    Add(path);
}

void SequenceWalk::LoopEnd(Path path)
{
    if (path.depth == 0)
    {
        Add(path);
        return;
    }

    Path back = path;
    back.cursor = path.stack[path.depth - 1];
    Add(back);

    path.stack[--path.depth] = 0;
    Add(path);
}

void SequenceWalk::Stop(const std::string& reason)
{
    if (result_.complete)
    {
        result_.complete = false;
        result_.stopped_by = reason;
        work_.clear();
    }
}

SequenceInstruments SequenceWalk::Finish()
{
    result_.commands.assign(commands_.begin(), commands_.end());

    return std::move(result_);
}

namespace
{

constexpr uint16_t kAny = InstrumentUse::kAny;

// The registers of the bank and the program (Tracks::WriteReg).
constexpr uint32_t kRegBank = 0x67;
constexpr uint32_t kRegProgram = 0x68;

// A command's arguments as the walk reads them: each value, and whether it came from a register. A value from a
// register is unknown.
struct Args
{
    std::array<uint32_t, 16> values{};
    std::array<bool, 16> from_register{};

    uint32_t operator[](std::size_t i) const
    {
        return values[i];
    }

    bool Known(std::size_t i) const
    {
        return !from_register[i];
    }
};

std::string Hex(uint32_t value)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text;
    do
    {
        text.insert(text.begin(), kDigits[value & 15]);
        value >>= 4;
    } while (value != 0);

    return "0x" + text;
}

// The decoder of JAudio2's commands for one path.
class Jaudio2Step
{
public:
    Jaudio2Step(SequenceWalk& walk, const SequenceWalk::Path& path) : walk_(walk), path_(path), at_(path.cursor)
    {
    }

    void Run();

private:
    // Tracks::ReadMidiValue: up to 4 bytes, each but the last with its top bit set.
    void SkipMidiValue();

    // Tracks::ParseCommand for `command`, with the arguments in `register_mask` from registers.
    void Command(uint32_t command, uint32_t register_mask);

    // The command's effect once its arguments are read. The path's cursor is past them.
    void Execute(uint32_t command, const Args& a);

    // A write of register `reg` (Tracks::WriteReg). Only the bank's and the program's registers change the walk.
    void WriteRegister(uint32_t reg, bool known, uint32_t value);

    // A write of the register that argument `index` names, with an unknown value. A register's number from a register
    // can be the bank's or the program's.
    void WriteUnknown(const Args& a, std::size_t index);

    // A condition (Tracks::Condition): condition 0 always holds.
    static bool Always(const Args& a, std::size_t index)
    {
        return a.Known(index) && a[index] == 0;
    }

    void Stop(uint32_t command, const char* what)
    {
        walk_.Stop(what + std::string(" at ") + Hex(at_) + " (command " + Hex(command) + ")");
    }

    SequenceWalk& walk_;
    SequenceWalk::Path path_;
    uint32_t at_;
};

void Jaudio2Step::SkipMidiValue()
{
    if ((walk_.U8(path_.cursor++) & 0x80) == 0)
    {
        return;
    }

    for (int i = 0; i < 3; i++)
    {
        if ((walk_.U8(path_.cursor++) & 0x80) == 0)
        {
            return;
        }
    }
}

void Jaudio2Step::Run()
{
    // Tracks::Parse.
    const uint32_t command = walk_.U8(path_.cursor++);
    if ((command & 0x80) == 0)
    {
        const uint32_t flags = walk_.U8(path_.cursor);
        path_.cursor += 2;
        if ((flags & 7) == 0)
        {
            SkipMidiValue();
        }
        walk_.Note(path_);
        walk_.Add(path_);
        return;
    }

    if ((command & 0xf0) == 0x90)
    {
        const uint32_t count = (command & 7) + 1;
        const uint32_t bits = walk_.U8(path_.cursor++);
        uint32_t mask = 0;
        for (uint32_t i = 0; i < count; i++)
        {
            if ((bits & (0x80u >> i)) != 0)
            {
                mask |= 3u << (2 * i);
            }
        }
        Command(walk_.U8(path_.cursor++), mask);
        return;
    }

    if ((command & 0xf0) == 0x80)
    {
        walk_.Add(path_);
        return;
    }

    Command(command, 0);
}

void Jaudio2Step::Command(uint32_t command, uint32_t register_mask)
{
    if (command == 0xb0)
    {
        path_.cursor++;
        walk_.Add(path_);
        return;
    }

    const SequenceCommand info =
        command >= kFirstSequenceCommand ? kSequenceCommands[command - kFirstSequenceCommand] : SequenceCommand{};
    Args args;
    uint32_t widths = info.types | register_mask;
    for (uint32_t i = 0; i < info.count && i < args.values.size(); i++)
    {
        switch (widths & 3)
        {
        case 0:
            args.values[i] = walk_.U8(path_.cursor);
            path_.cursor += 1;
            break;

        case 1:
            args.values[i] = walk_.U16(path_.cursor);
            path_.cursor += 2;
            break;

        case 2:
            args.values[i] = walk_.U24(path_.cursor);
            path_.cursor += 3;
            break;

        default:
            args.from_register[i] = true;
            path_.cursor += 1;
            break;
        }
        widths >>= 2;
    }

    Execute(command, args);
}

void Jaudio2Step::WriteRegister(uint32_t reg, bool known, uint32_t value)
{
    if (reg == kRegBank)
    {
        path_.bank = known ? static_cast<uint16_t>(value & 0xff) : kAny;
    }
    else if (reg == kRegProgram)
    {
        path_.program = known ? static_cast<uint16_t>(value & 0xff) : kAny;
    }
}

void Jaudio2Step::WriteUnknown(const Args& a, std::size_t index)
{
    if (!a.Known(index))
    {
        path_.bank = kAny;
        path_.program = kAny;
        return;
    }

    WriteRegister(a[index], false, 0);
}

void Jaudio2Step::Execute(uint32_t command, const Args& a)
{
    switch (command)
    {
    case 0xb1:
    case 0xb3:
        walk_.Note(path_);
        break;

    case 0xc1:
        if (!a.Known(1))
        {
            Stop(command, "a child track's start from a register");
            return;
        }
        walk_.Child(a[1], path_.bank, path_.program);
        break;

    case 0xc3:
    case 0xc4:
        {
            const std::size_t target = command == 0xc3 ? 0 : 1;
            if (!a.Known(target))
            {
                Stop(command, "a call to a register's value");
                return;
            }
            walk_.Call(path_, a[target]);
            if (command == 0xc3 || Always(a, 0))
            {
                return;
            }
            break;
        }

    case 0xc5:
        walk_.Return(path_, true);
        return;

    case 0xc6:
        walk_.Return(path_, true);
        if (Always(a, 0))
        {
            return;
        }
        break;

    case 0xc7:
    case 0xc8:
        {
            const std::size_t target = command == 0xc7 ? 0 : 1;
            if (!a.Known(target))
            {
                Stop(command, "a jump to a register's value");
                return;
            }
            SequenceWalk::Path jump = path_;
            jump.cursor = a[target];
            walk_.Add(jump);
            if (command == 0xc7 || Always(a, 0))
            {
                return;
            }
            break;
        }

    case 0xc9:
    case 0xca:
        Stop(command, "a jump or a call through a table");
        return;

    case 0xcb:
        walk_.LoopStart(path_);
        return;

    case 0xcc:
        walk_.LoopEnd(path_);
        return;

    case 0xd0:
    case 0xd6:
    case 0xd7:
    case 0xdc:
        WriteUnknown(a, 1);
        break;

    case 0xd8:
        if (!a.Known(0))
        {
            WriteUnknown(a, 0);
        }
        else
        {
            WriteRegister(a[0], a.Known(1), a[1]);
        }
        break;

    case 0xd9:
    case 0xda:
        // Tracks::RegisterArithmetic: operation 0 loads the value, and operations 3 and 4 write other registers.
        if (!a.Known(1))
        {
            WriteUnknown(a, 1);
        }
        else if (a.Known(0) && a[0] == 0)
        {
            WriteRegister(a[1], a.Known(2), a[2]);
        }
        else if (!a.Known(0) || (a[0] != 3 && a[0] != 4))
        {
            WriteRegister(a[1], false, 0);
        }
        break;

    case 0xdb:
        if (!a.Known(0) || a[0] == 0xb)
        {
            WriteUnknown(a, 1);
        }
        break;

    case 0xe1:
        path_.bank = a.Known(0) ? static_cast<uint16_t>((a[0] & 0xffff) >> 8) : kAny;
        path_.program = a.Known(0) ? static_cast<uint16_t>(a[0] & 0xff) : kAny;
        break;

    case 0xe2:
        path_.bank = a.Known(0) ? static_cast<uint16_t>(a[0] & 0xff) : kAny;
        break;

    case 0xe3:
        path_.program = a.Known(0) ? static_cast<uint16_t>(a[0] & 0xff) : kAny;
        break;

    case 0xf0:
        SkipMidiValue();
        break;

    case 0xf4:
        Stop(command, "interrupts");
        return;

    case 0xfd:
        {
            // Tracks::Printf: the format, up to 0x80 characters or its end, then a byte for each conversion.
            uint32_t conversions = 0;
            for (uint32_t i = 0; i < 0x80; i++)
            {
                const uint32_t c = walk_.U8(path_.cursor++);
                if (c == '%')
                {
                    i++;
                    if (walk_.U8(path_.cursor++) == 0)
                    {
                        break;
                    }
                    conversions++;
                }
                else if (c == '\\')
                {
                    i++;
                    if (walk_.U8(path_.cursor++) == 0)
                    {
                        break;
                    }
                }
                else if (c == 0)
                {
                    break;
                }
            }
            path_.cursor += conversions;
            break;
        }

    case 0xff:
        return;

    default:
        break;
    }

    walk_.Add(path_);
}

} // namespace

SequenceInstruments WalkSequence(std::span<const uint8_t> data, uint32_t start)
{
    // Tracks::StartSequence: bank 0 and program 0xf0.
    SequenceWalk walk(data, {.cursor = start, .bank = 0, .program = 0xf0});
    SequenceWalk::Path path;
    while (walk.Next(path))
    {
        Jaudio2Step(walk, path).Run();
    }

    return walk.Finish();
}

} // namespace ntdlib::jaudio
