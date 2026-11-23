// SPDX-License-Identifier: MIT

// The walk of a JAudio1 sequence. The decoder mirrors the model's parser byte for byte: Tracks::ParseSeq and what it
// calls (CmdNoteOn, CmdWait, CmdNoteOff, CmdSetParam, ReadRegOperand, CmdRegisters, CmdProcess, CmdCall, CmdJmp and
// CmdPrintf). It reads the parser's table of arguments (kArgs).

#include "ntdlib/jaudio1/sequence_walk.h"

#include <cstdint>
#include <span>

#include "jaudio1/track_internal.h"

namespace ntdlib::jaudio1
{

namespace
{

namespace nj = ntdlib::jaudio;

constexpr uint16_t kAny = nj::InstrumentUse::kAny;

// The registers that hold the bank and the program: register 6 both (the bank in its high byte), and 0x20 and 0x21
// one each (StoreRegResult).
constexpr uint32_t kRegBank = 0x20;
constexpr uint32_t kRegProgram = 0x21;

using Args = nj::SequenceWalk::Args;

// The decoder of JAudio1's commands for one path.
class Step
{
public:
    Step(nj::SequenceWalk& walk, const nj::SequenceWalk::Path& path) : walk_(walk), path_(path), at_(path.cursor)
    {
    }

    void Run();

private:
    uint32_t Byte()
    {
        return walk_.U8(path_.cursor++);
    }

    void NoteOn();
    void NoteOff(uint32_t command);
    void SetParam(uint32_t flags);
    void WriteRegParam(uint32_t command);
    void Registers(uint32_t command);
    void Process(uint32_t command, uint32_t register_mask);
    void Execute(uint32_t command, const Args& a);

    // CmdCall and CmdJmp: a flag, then a 24-bit target unless the flag's bit 7 takes it from a register or a table.
    void CallOrJump(uint32_t command);

    void Printf();

    // A store of `value` into register 6, 0x20 or 0x21. Another register changes nothing in the walk.
    void Store(uint32_t reg, bool known, uint32_t value);

    void Stop(uint32_t command, const char* what)
    {
        walk_.Stop(at_, command, what);
    }

    nj::SequenceWalk& walk_;
    nj::SequenceWalk::Path path_;
    uint32_t at_;
};

void Step::Run()
{
    // Tracks::ParseSeq: one command.
    const uint32_t command = Byte();
    if ((command & 0x80) == 0)
    {
        NoteOn();
        return;
    }
    if ((command & ~8u) == 0x80)
    {
        // CmdWait: a byte, or 16 bits for 0x88.
        path_.cursor += command != 0x80 ? 2 : 1;
        walk_.Add(path_);
        return;
    }
    if (command == 0xf9 || (command & 0xf0) == 0x80)
    {
        NoteOff(command);
        return;
    }

    switch (command & 0xf0)
    {
    case 0x90:
        SetParam(command & 0xf);
        return;

    case 0xa0:
        WriteRegParam(command & 0xf);
        return;

    case 0xb0:
        Registers(command);
        return;

    default:
        Process(command, 0);
        return;
    }
}

void Step::NoteOn()
{
    // CmdNoteOn: the flags and the velocity. A note id other than 0 may read the byte of its skip. Note 0 reads its
    // gate's percentage and as many bytes of its length as the flags count.
    const uint32_t flags = Byte();
    path_.cursor++;
    const uint32_t note = flags & 7;
    const uint32_t count = (flags >> 3) & 3;
    const uint32_t gate_flags = (flags >> 5) & 3;
    if (note != 0)
    {
        if ((gate_flags & 1) != 0)
        {
            path_.cursor++;
        }
    }
    else
    {
        path_.cursor += 1 + count;
    }

    walk_.Note(path_);
    walk_.Add(path_);
}

void Step::NoteOff(uint32_t command)
{
    // CmdNoteOff: a release byte follows where the flags' bit 3 is set. For 0xf9, a register decides that bit unless
    // the byte's bit 7 sets it.
    bool release = (command & 8) != 0;
    if (command == 0xf9)
    {
        if ((Byte() & 0x80) == 0)
        {
            Stop(command, "a note's end whose length a register gives");
            return;
        }
        release = true;
    }
    if (release)
    {
        path_.cursor++;
    }

    walk_.Add(path_);
}

void Step::SetParam(uint32_t flags)
{
    // CmdSetParam: the level's number, its value (16 bits for kind 3, a byte otherwise), then its time (none for
    // kind 0, 16 bits for kind 3, a byte otherwise).
    path_.cursor++;
    path_.cursor += ((flags >> 2) & 3) == 3 ? 2 : 1;
    const uint32_t time = flags & 3;
    path_.cursor += time == 0 ? 0 : time == 3 ? 2 : 1;

    walk_.Add(path_);
}

void Step::WriteRegParam(uint32_t command)
{
    // ReadRegOperand, then ApplyRegOperation and StoreRegResult on the bank's and the program's registers.
    uint32_t operation = command;
    uint32_t operand_kind = 0;
    if (command == 9)
    {
        const uint32_t byte = Byte();
        operand_kind = (byte & 0xc) == 8 ? 0x10 : byte & 0xc;
        operation = byte & 0xf0;
    }
    else if (command == 0xa)
    {
        operand_kind = Byte() & 0xc;
    }
    else if (command != 0xb)
    {
        operand_kind = command & 0xc;
        operation = command & 3;
    }

    const uint32_t target = Byte();
    if (operation == 0xa)
    {
        path_.cursor++;
    }

    bool known = true;
    uint32_t operand = 0;
    switch (operand_kind >> 2)
    {
    case 0:
        known = false;
        path_.cursor++;
        break;

    case 1:
        operand = Byte();
        break;

    case 2:
        {
            const uint32_t byte = Byte();
            operand = byte << 8 | ((byte & 0x80) == 0 ? byte << 1 : 0);
            break;
        }

    case 3:
        operand = walk_.U16(path_.cursor);
        path_.cursor += 2;
        break;

    default:
        operand = 0xffff;
        break;
    }

    // Operations 2 and 3 store nothing. Operation 0 stores its operand. Any other stores a value unknown to the walk.
    if (operation != 2 && operation != 3)
    {
        Store(target, operation == 0 && known, operand);
    }

    walk_.Add(path_);
}

void Step::Store(uint32_t reg, bool known, uint32_t value)
{
    const auto byte = [known](uint32_t v)
    {
        return known ? static_cast<uint16_t>(v & 0xff) : kAny;
    };
    switch (reg)
    {
    case 6:
        path_.bank = byte(value >> 8);
        path_.program = byte(value);
        break;

    case kRegBank:
        path_.bank = byte(value);
        break;

    case kRegProgram:
        path_.program = byte(value);
        break;

    default:
        break;
    }
}

void Step::Registers(uint32_t command)
{
    // CmdRegisters: the command from the next byte, then a mask of the arguments that come from registers. 0xb8 to
    // 0xbf take the command from a register.
    if ((command & 8) != 0)
    {
        Stop(command, "a command from a register");
        return;
    }

    const uint32_t inner = Byte();
    const uint32_t bits = Byte();
    uint32_t mask = 0;
    for (uint32_t i = 0; i <= (command & 7); i++)
    {
        if (((bits << i) & 0x80) != 0)
        {
            mask |= 3u << (2 * i);
        }
    }

    Process(inner, mask);
}

void Step::Process(uint32_t command, uint32_t register_mask)
{
    // CmdProcess: the arguments by the table's widths, or from registers where the mask says so. A command below 0xc0
    // reads nothing and does nothing, and so does a command without a handler once it has read its arguments.
    command &= 0xff;
    if (command < 0xc0)
    {
        walk_.Add(path_);
        return;
    }

    const ArgSpec& spec = kArgs[command - 0xc0];
    const Args args = walk_.ReadArgs(path_, spec.count, spec.types | register_mask);

    if (((kNullCommands >> (command - 0xc0)) & 1) != 0)
    {
        walk_.Add(path_);
        return;
    }

    Execute(command, args);
}

void Step::CallOrJump(uint32_t command)
{
    const uint32_t flag = Byte();
    if ((flag & 0x80) != 0)
    {
        Stop(command, "a target from a register or a table");
        return;
    }

    const uint32_t target = walk_.U24(path_.cursor);
    path_.cursor += 3;
    if (command == 0xc4)
    {
        walk_.Call(path_, target);
    }
    else
    {
        nj::SequenceWalk::Path jump = path_;
        jump.cursor = target;
        walk_.Add(jump);
    }

    // conditionCheck: condition 0 always holds.
    if ((flag & 0xf) != 0)
    {
        walk_.Add(path_);
    }
}

void Step::Printf()
{
    // CmdPrintf: the format, at most 128 characters, then a byte for each conversion.
    uint32_t conversions = 0;
    for (uint32_t i = 0; i < 128; i++)
    {
        const uint32_t c = Byte();
        if (c == 0)
        {
            break;
        }
        if (c == '\\')
        {
            if (Byte() == 0)
            {
                break;
            }
            continue;
        }
        if (c != '%')
        {
            continue;
        }

        i++;
        if (Byte() == 0)
        {
            break;
        }
        conversions++;
    }
    path_.cursor += conversions;
}

void Step::Execute(uint32_t command, const Args& a)
{
    switch (command)
    {
    case 0xc1:
        {
            // openTrack: the child inherits the bank and the program unless the flags' bit 7 (inherit & 2) keeps it
            // from inheriting. It then starts with bank 0 and program 0xf0, as JASTrack::init leaves register 6.
            if (!a.Known(1))
            {
                Stop(command, "a child track's start from a register");
                return;
            }
            const bool inherits = !a.Known(0) || ((a[0] >> 6) & 2) == 0;
            const bool resets = !a.Known(0) || ((a[0] >> 6) & 2) != 0;
            if (inherits)
            {
                walk_.Child(a[1], path_.bank, path_.program);
            }
            if (resets)
            {
                walk_.Child(a[1], 0, 0xf0);
            }
            break;
        }

    case 0xc2:
        // openTrackBros: a child of the parent. It inherits from another track.
        if (!a.Known(1))
        {
            Stop(command, "a child track's start from a register");
            return;
        }
        walk_.Child(a[1], kAny, kAny);
        break;

    case 0xc4:
    case 0xc8:
        CallOrJump(command);
        return;

    case 0xc6:
        // ret: at depth 0 the track ends.
        walk_.Return(path_, false);
        if (!a.Known(0) || (a[0] & 0xf) != 0)
        {
            break;
        }
        return;

    case 0xc9:
        walk_.LoopStart(path_);
        return;

    case 0xca:
        walk_.LoopEnd(path_);
        return;

    case 0xcb:
        // readPort: WriteRegDirect ignores 0x20 and 0x21.
        if (!a.Known(1) || (a[1] & 0xff) == 6)
        {
            path_.bank = kAny;
            path_.program = kAny;
        }
        break;

    case 0xdf:
        Stop(command, "interrupts");
        return;

    case 0xfb:
        Printf();
        break;

    case 0xff:
        return;

    default:
        break;
    }

    walk_.Add(path_);
}

} // namespace

nj::SequenceInstruments WalkSequence(std::span<const uint8_t> data, uint32_t start)
{
    // JASTrack::init: register 6 starts at 0x00f0, bank 0 and program 0xf0.
    nj::SequenceWalk walk(data, {.cursor = start, .bank = 0, .program = 0xf0});
    nj::SequenceWalk::Path path;
    while (walk.Next(path))
    {
        Step(walk, path).Run();
    }

    return walk.Finish();
}

} // namespace ntdlib::jaudio1
