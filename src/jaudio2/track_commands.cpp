// SPDX-License-Identifier: MIT

// The tracks' parser, their registers and the actions of their commands (JASSeqParser).

#include <array>
#include <cstddef>
#include <cstdint>

#include "jaudio2/track_constants.h"
#include "ntdlib/jaudio/calc.h"
#include "ntdlib/jaudio/sequence_commands.h"
#include "ntdlib/jaudio2/calc.h"
#include "ntdlib/jaudio2/iir_table.h"
#include "ntdlib/jaudio2/track.h"

namespace ntdlib::jaudio2
{

using detail::kEnvOsc;

namespace
{

// The key of a beat for the rhythm parser (AudRhythmSeqParser's first key of a rhythm type, 0x18 from its
// constructor at 0x2363f0).
constexpr uint32_t kBeatKey = 0x18;

// The registers that JASRegisterParam keeps, and the flag register.
constexpr uint32_t kRegisters = 14;
constexpr uint32_t kRegFlag = 3;

// A parameter's value from a byte, as paramE gives it: the byte in the high half, and below it twice the byte when
// the byte's top bit is clear. Then a fraction of 32767.
float ByteParam(uint32_t value)
{
    const uint32_t low = (value & 0x80) != 0 ? 0 : value << 1;

    return static_cast<float>(static_cast<int16_t>(static_cast<uint16_t>(low | value << 8))) / 32767.0f;
}

float HalfParam(uint32_t value)
{
    return static_cast<float>(static_cast<int16_t>(static_cast<uint16_t>(value))) / 32767.0f;
}

} // namespace

uint32_t Tracks::ReadMidiValue(Track& track)
{
    const Sequence& s = *track.sequence;
    uint32_t value = s.U8(track.cursor++);
    if ((value & 0x80) == 0)
    {
        return value;
    }

    value &= 0x7f;
    for (int i = 0; i < 3; i++)
    {
        const uint32_t byte = s.U8(track.cursor++);
        value = value << 7 | (byte & 0x7f);
        if ((byte & 0x80) == 0)
        {
            return value;
        }
        if (i == 2)
        {
            return 0; // a fourth byte with its top bit set
        }
    }

    return value;
}

int32_t Tracks::Parse(uint32_t index)
{
    Track& track = tracks_[index];
    const Sequence& s = *track.sequence;
    const uint32_t command = s.U8(track.cursor++);
    if ((command & 0x80) == 0)
    {
        // A note: its flags (the note id in the low three bits) and its velocity. A gate note (id 0) has its time.
        const uint32_t flags = s.U8(track.cursor);
        const uint32_t velocity = s.U8(track.cursor + 1);
        track.cursor += 2;
        const uint32_t note = flags & 7;
        if (note == 0)
        {
            const uint32_t time = ReadMidiValue(track);
            NoteOnGate(index, command, velocity, time, flags);
        }
        else
        {
            // The rhythm parser's execNoteOnMidi (0x10d540). AudBgmTempoAdjuster gives a beat a rate of 1 in a piece
            // that plays beside no stream. The tempo then goes to the root track unchanged.
            if (track.rhythm_parser && command == kBeatKey)
            {
                SetTempo(tracks_[Root(index)], static_cast<uint32_t>(static_cast<int32_t>(rhythm_tempo_)));
            }
            NoteOn(index, note, command, velocity);
        }

        return 0;
    }

    if ((command & 0xf0) == 0x90)
    {
        // The next command with some of its arguments from registers: a bit for each, from the top bit down.
        const uint32_t count = (command & 7) + 1;
        const uint32_t bits = s.U8(track.cursor++);
        uint32_t mask = 0;
        for (uint32_t i = 0; i < count; i++)
        {
            if ((bits & (0x80u >> i)) != 0)
            {
                mask |= 3u << (2 * i);
            }
        }
        const uint32_t next = s.U8(track.cursor++);
        return ParseCommand(index, next, mask);
    }

    if ((command & 0xf0) == 0x80)
    {
        if ((command & 7) != 0)
        {
            NoteOff(index, command & 7, 0);
        }

        return 0;
    }

    // The rhythm parser's parse (0x12ca20) keeps a tempo command's tempo before the command runs.
    if (track.rhythm_parser && command == 0xe0)
    {
        rhythm_tempo_ = static_cast<float>(s.U16(track.cursor));
    }

    return ParseCommand(index, command, 0);
}

int32_t Tracks::ParseCommand(uint32_t index, uint32_t command, uint32_t register_mask)
{
    Track& track = tracks_[index];
    const Sequence& s = *track.sequence;
    command &= 0xff;
    if (command == 0xb0)
    {
        // JASSeqParser's extended commands: the next byte names one. None reads arguments, and only 01 has an
        // action, a print of the track's state.
        track.cursor++;
        return 0;
    }

    // JASSeqParser's sCmdInfo from 0xa0 on gives each command's argument count and widths. A command without an action
    // reads its arguments and does nothing.
    namespace nj = ntdlib::jaudio;
    const nj::SequenceCommand info = command >= nj::kFirstSequenceCommand
                                         ? nj::kSequenceCommands[command - nj::kFirstSequenceCommand]
                                         : nj::SequenceCommand{};

    Args args;
    uint32_t widths = info.types | register_mask;
    for (uint32_t i = 0; i < info.count && i < args.values.size(); i++)
    {
        uint32_t value = 0;
        switch (widths & 3)
        {
        case 0:
            value = s.U8(track.cursor);
            track.cursor += 1;
            break;

        case 1:
            value = s.U16(track.cursor);
            track.cursor += 2;
            break;

        case 2:
            value = s.U24(track.cursor);
            track.cursor += 3;
            break;

        default:
            value = ReadReg(track, s.U8(track.cursor));
            track.cursor += 1;
            break;
        }
        widths >>= 2;
        args.values[i] = value;
    }

    return Execute(index, command, args);
}

uint32_t Tracks::RegisterRead(const Track& track, uint32_t reg)
{
    const std::array<uint16_t, kRegisters>& r = track.registers;
    if (reg < kRegisters)
    {
        return r[reg];
    }

    switch (reg)
    {
    case 0x20:
        return (r[0] & 0xffu) << 8 | (r[1] & 0xffu);

    case 0x21:
        return uint32_t{r[4]} << 16 | r[5];

    case 0x22:
        return r[4] >> 8;

    case 0x23:
        return r[4] & 0xffu;

    case 0x24:
        return r[5] >> 8;

    case 0x25:
        return r[5] & 0xffu;

    case 0x26:
        return uint32_t{r[6]} << 16 | r[7];

    case 0x27:
        return uint32_t{r[8]} << 16 | r[9];

    // The game's read (0x7548c in Super Mario Galaxy's main) returns registers 10 and 11 for both 0x28 and 0x29. A
    // write of 0x29 sets registers 12 and 13.
    case 0x28:
    case 0x29:
        return uint32_t{r[10]} << 16 | r[11];

    default:
        return 0;
    }
}

void Tracks::RegisterWrite(Track& track, uint32_t reg, uint32_t value)
{
    std::array<uint16_t, kRegisters>& r = track.registers;
    if (reg < kRegisters)
    {
        r[reg] = static_cast<uint16_t>(reg <= 2 ? value & 0xff : value);
        return;
    }

    const auto pair = [&r, value](std::size_t high)
    {
        r[high] = static_cast<uint16_t>(value >> 16);
        r[high + 1] = static_cast<uint16_t>(value);
    };
    switch (reg)
    {
    case 0x20:
        r[0] = static_cast<uint16_t>((value >> 8) & 0xff);
        r[1] = static_cast<uint16_t>(value & 0xff);
        break;

    case 0x21:
        pair(4);
        break;

    case 0x22:
        r[4] = static_cast<uint16_t>((r[4] & 0x00ff) | (value & 0xff) << 8);
        break;

    case 0x23:
        r[4] = static_cast<uint16_t>((r[4] & 0xff00) | (value & 0xff));
        break;

    case 0x24:
        r[5] = static_cast<uint16_t>((r[5] & 0x00ff) | (value & 0xff) << 8);
        break;

    case 0x25:
        r[5] = static_cast<uint16_t>((r[5] & 0xff00) | (value & 0xff));
        break;

    case 0x26:
        pair(6);
        break;

    case 0x27:
        pair(8);
        break;

    case 0x28:
        pair(10);
        break;

    case 0x29:
        pair(12);
        break;

    default:
        break;
    }
}

uint32_t Tracks::ReadReg(const Track& track, uint32_t reg) const
{
    if (reg <= 0x3f)
    {
        return RegisterRead(track, reg);
    }
    if (reg >= 0x40 && reg < 0x50)
    {
        return track.ports[reg - 0x40];
    }

    switch (reg)
    {
    case 0x60:
        {
            // The children that play.
            uint32_t bits = 0;
            for (uint32_t i = 0; i < track.children.size(); i++)
            {
                const uint32_t child = track.children[i];
                if (child != Track::kNone && tracks_[child].status == Track::kRun)
                {
                    bits |= 1u << i;
                }
            }

            return bits;
        }

    case 0x61:
        return track.depth != 0 ? track.loop_counts[track.depth - 1] : 0;

    case 0x62:
        return track.timebase;

    case 0x63:
        return static_cast<uint32_t>(static_cast<int32_t>(track.transpose));

    case 0x64:
        return track.bend_sense;

    case 0x65:
        return track.gate_rate;

    case 0x66:
        return track.skip_samples;

    case 0x67:
        return track.bank;

    case 0x68:
        return track.program;

    case 0x69:
        return ToUint32(track.pan_power * 32767.0f);

    case 0x6a:
        // The game's read (0x7518c in Super Mario Galaxy's main) ORs the two priorities without a shift. A write of
        // 0x6a puts the release priority in the high byte.
        return uint32_t{track.note_on_priority} | track.release_priority;

    case 0x6b:
        return track.note_on_priority;

    case 0x6c:
        return track.release_priority;

    case 0x6d:
        return track.direct_release;

    case 0x6e:
        return ToUint32(track.vibrato_depth * 1524.0f);

    case 0x6f:
        return ToUint32(track.vibrato_depth * 12192.0f);

    case 0x70:
        return ToUint32(track.tremolo_depth * 256.0f);

    case 0x71:
        return ToUint32(track.vibrato_pitch * 64.0f);

    case 0x72:
        return ToUint32(track.tremolo_pitch * 64.0f);

    case 0x73:
        return track.vibrato_delay;

    case 0x74:
        return track.tremolo_delay;

    default:
        return 0;
    }
}

void Tracks::WriteReg(uint32_t index, uint32_t reg, uint32_t value)
{
    // Every write sets the flag register too.
    Track& track = tracks_[index];
    RegisterWrite(track, kRegFlag, value);
    if (reg <= 0x3f)
    {
        RegisterWrite(track, reg, value);
        return;
    }
    if (reg >= 0x40 && reg < 0x50)
    {
        track.ports[reg - 0x40] = static_cast<uint16_t>(value);
        return;
    }

    switch (reg)
    {
    case 0x62:
        track.timebase = static_cast<uint16_t>(value);
        track.flags |= Track::kInvalid;
        UpdateTempo(track);
        break;

    case 0x63:
        track.transpose = static_cast<int8_t>(static_cast<uint8_t>(value));
        break;

    case 0x64:
        track.bend_sense = static_cast<uint8_t>(value);
        break;

    case 0x65:
        track.gate_rate = static_cast<uint8_t>(value);
        break;

    case 0x66:
        track.skip_samples = value;
        break;

    case 0x67:
        track.bank = static_cast<uint16_t>(value & 0xff);
        break;

    case 0x68:
        track.program = static_cast<uint16_t>(value & 0xff);
        break;

    case 0x69:
        track.pan_power = static_cast<float>(value) / 32767.0f;
        break;

    case 0x6a:
        track.note_on_priority = static_cast<uint8_t>(value);
        track.release_priority = static_cast<uint8_t>(value >> 8);
        break;

    case 0x6b:
        track.note_on_priority = static_cast<uint8_t>(value);
        break;

    case 0x6c:
        track.release_priority = static_cast<uint8_t>(value);
        break;

    case 0x6d:
        track.direct_release = static_cast<uint16_t>(value);
        break;

    case 0x6e:
        track.vibrato_depth = static_cast<float>(value) / 1524.0f;
        break;

    case 0x6f:
        track.vibrato_depth = static_cast<float>(value) / 12192.0f;
        break;

    case 0x70:
        track.tremolo_depth = static_cast<float>(value) * (1.0f / 256.0f);
        break;

    case 0x71:
        track.vibrato_pitch = static_cast<float>(value) * (1.0f / 64.0f);
        break;

    case 0x72:
        track.tremolo_pitch = static_cast<float>(value) * (1.0f / 64.0f);
        break;

    case 0x73:
        track.vibrato_delay = static_cast<uint16_t>(value);
        break;

    case 0x74:
        track.tremolo_delay = static_cast<uint16_t>(value);
        break;

    default:
        break;
    }
}

bool Tracks::Condition(const Track& track, uint32_t condition)
{
    const uint32_t flag = track.registers[kRegFlag];
    switch (condition)
    {
    case 0:
        return true;
    case 1:
        return flag == 0;
    case 2:
        return flag != 0;
    case 3:
        return flag == 1;
    case 4:
        return (flag & 0x8000) != 0;
    case 5:
        return (flag & 0x8000) == 0;
    default:
        return false;
    }
}

void Tracks::JumpBack(uint32_t index, uint32_t target)
{
    // The cursor has moved past the jump. A jump to the jump itself or before it goes back.
    const Track& track = tracks_[index];
    if (!loop_hook_ || track.depth != 0 || target >= track.cursor)
    {
        return;
    }

    const bool played = notes_[index] != loop_notes_[index];
    loop_notes_[index] = notes_[index];
    looped_[index] = true;
    quiet_[index] = !played;
    loop_hook_(index, target, played);
}

int32_t Tracks::Execute(uint32_t index, uint32_t command, const Args& a)
{
    // The commands by their groups in the parser's table.
    if (command < 0xc0)
    {
        NoteCommand(index, command, a);
    }
    else if (command < 0xcb)
    {
        FlowCommand(index, command, a);
    }
    else if (command < 0xd0)
    {
        LoopCommand(tracks_[index], command, a);
    }
    else if (command < 0xd8)
    {
        PortCommand(index, command, a);
    }
    else if (command < 0xe0)
    {
        RegisterCommand(index, command, a);
    }
    else if (command < 0xeb)
    {
        SettingCommand(tracks_[index], command, a);
    }
    else if (command < 0xf0)
    {
        FilterCommand(tracks_[index], command, a);
    }
    else
    {
        return TimerCommand(tracks_[index], command, a);
    }

    return 0;
}

// Notes and the track's parameters (0xb1 to 0xbb).
void Tracks::NoteCommand(uint32_t index, uint32_t command, const Args& a)
{
    Track& track = tracks_[index];

    switch (command)
    {
    case 0xb1:
        NoteOn(index, a[1], a[0], a[2]);
        break;

    case 0xb2:
        NoteOff(index, a[0], 0);
        break;

    case 0xb3:
        NoteOnGate(index, a[1], a[2], a[3], a[0]);
        break;

    case 0xb4:
        if (a[0] <= 0xff)
        {
            SetLatestKey(index, a[0]);
        }
        break;

    case 0xb8:
        SetParam(track, a[0], ByteParam(a[1]), 0);
        break;

    case 0xb9:
        SetParam(track, a[0], HalfParam(a[1]), 0);
        break;

    case 0xba:
        SetParam(track, a[0], ByteParam(a[1]), a[2]);
        break;

    case 0xbb:
        SetParam(track, a[0], HalfParam(a[1]), a[2]);
        break;

    default:
        break;
    }
}

void Tracks::Call(Track& track, uint32_t offset)
{
    if (track.depth < track.stack.size())
    {
        track.stack[track.depth++] = track.cursor;
        track.cursor = offset;
    }
}

void Tracks::Return(Track& track)
{
    if (track.depth != 0)
    {
        track.cursor = track.stack[--track.depth];
    }
}

// Children, calls, returns and jumps (0xc1 to 0xca).
void Tracks::FlowCommand(uint32_t index, uint32_t command, const Args& a)
{
    Track& track = tracks_[index];

    switch (command)
    {
    case 0xc1:
        {
            const uint32_t child = OpenChild(index, a[0]);
            if (child != Track::kNone)
            {
                Track& t = tracks_[child];
                t.sequence = track.sequence;
                t.cursor = a[1];
                t.depth = 0;
                t.stack.fill(0);
                t.loop_counts.fill(0);
                t.status = Track::kRun;
            }
            break;
        }

    case 0xc2:
        CloseChild(index, a[0]);
        break;

    case 0xc3:
        Call(track, a[0]);
        break;

    case 0xc4:
        if (Condition(track, a[0]))
        {
            Call(track, a[1]);
        }
        break;

    case 0xc5:
        Return(track);
        break;

    case 0xc6:
        if (Condition(track, a[0]))
        {
            Return(track);
        }
        break;

    case 0xc7:
        JumpBack(index, a[0]);
        track.cursor = a[0];
        break;

    case 0xc8:
        if (Condition(track, a[0]))
        {
            JumpBack(index, a[1]);
            track.cursor = a[1];
        }
        break;

    case 0xc9:
        track.cursor = track.sequence->U24(a[1] + 3 * a[0]);
        break;

    case 0xca:
        Call(track, track.sequence->U24(a[1] + 3 * a[0]));
        break;

    default:
        break;
    }
}

// The loops (0xcb and 0xcc).
void Tracks::LoopCommand(Track& track, uint32_t command, const Args& a)
{
    switch (command)
    {
    case 0xcb:
        if (track.depth < track.stack.size())
        {
            track.stack[track.depth] = track.cursor;
            track.loop_counts[track.depth] = static_cast<uint16_t>(a[0]);
            track.depth++;
        }
        break;

    case 0xcc:
        if (track.depth != 0)
        {
            const uint32_t top = track.depth - 1;
            const uint16_t count = track.loop_counts[top];
            if (count >= 2)
            {
                track.loop_counts[top] = static_cast<uint16_t>(count - 1);
                track.cursor = track.stack[top];
            }
            else
            {
                track.depth = top;
            }
        }
        break;

    default:
        break;
    }
}

// The ports of the track, its parent and its children (0xd0 to 0xd7).
void Tracks::PortCommand(uint32_t index, uint32_t command, const Args& a)
{
    Track& track = tracks_[index];
    const auto child_of = [&track](uint32_t value)
    {
        return track.children[(value >> 4) & 15];
    };

    switch (command)
    {
    case 0xd0:
        {
            // readPort: the port's value from the game, which clears its flag.
            uint32_t value = 0;
            if (a[0] < track.ports.size())
            {
                track.import_flags = static_cast<uint16_t>(track.import_flags & ~(1u << a[0]));
                value = track.ports[a[0]];
            }
            WriteReg(index, a[1], value & 0xffff);
            break;
        }

    case 0xd1:
        if (a[0] < track.ports.size())
        {
            track.export_flags = static_cast<uint16_t>(track.export_flags | 1u << a[0]);
            track.ports[a[0]] = static_cast<uint16_t>(a[1]);
        }
        break;

    case 0xd2:
        RegisterWrite(track, kRegFlag, (uint32_t{track.import_flags} >> (a[0] & 31)) & 1);
        break;

    case 0xd3:
        RegisterWrite(track, kRegFlag, (uint32_t{track.export_flags} >> (a[0] & 31)) & 1);
        break;

    case 0xd4:
        if (track.parent != Track::kNone)
        {
            WritePort(track.parent, a[0], a[1]);
        }
        break;

    case 0xd5:
        if (const uint32_t child = child_of(a[0]); child != Track::kNone)
        {
            WritePort(child, a[0] & 15, a[1]);
        }
        break;

    case 0xd6:
    case 0xd7:
        {
            const uint32_t from = command == 0xd6 ? track.parent : child_of(a[0]);
            const uint32_t port = command == 0xd6 ? a[0] : a[0] & 15;
            if (from != Track::kNone)
            {
                Track& other = tracks_[from];
                uint32_t value = 0;
                if (port < other.ports.size())
                {
                    other.export_flags = static_cast<uint16_t>(other.export_flags & ~(1u << port));
                    value = other.ports[port];
                }
                WriteReg(index, a[1], value & 0xffff);
            }
            break;
        }

    default:
        break;
    }
}

// The registers (0xd8 to 0xdc).
void Tracks::RegisterCommand(uint32_t index, uint32_t command, const Args& a)
{
    const Track& track = tracks_[index];

    switch (command)
    {
    case 0xd8:
        WriteReg(index, a[0], a[1]);
        break;

    case 0xd9:
    case 0xda:
        RegisterArithmetic(index, a[0], a[1], a[2]);
        break;

    case 0xdb:
        if (a[0] == 0xb)
        {
            WriteReg(index, a[1], 0u - ReadReg(track, a[1]));
        }
        break;

    case 0xdc:
        LoadFromTable(index, a);
        break;

    default:
        break;
    }
}

// 0xd9 and 0xda: operation `op` on register `reg` with `value`.
void Tracks::RegisterArithmetic(uint32_t index, uint32_t op, uint32_t reg, uint32_t value)
{
    const Track& track = tracks_[index];
    switch (op)
    {
    case 0:
        WriteReg(index, reg, value);
        break;

    case 1:
        WriteReg(index, reg, ReadReg(track, reg) + value);
        break;

    case 2:
        WriteReg(index, reg, ReadReg(track, reg) - value);
        break;

    case 3:
        WriteReg(index, kRegFlag, ReadReg(track, reg) - value);
        break;

    case 4:
        WriteReg(index, 0x21, ReadReg(track, reg) * value);
        break;

    case 5:
        WriteReg(index, reg, ReadReg(track, reg) & value);
        break;

    case 6:
        WriteReg(index, reg, ReadReg(track, reg) | value);
        break;

    case 7:
        WriteReg(index, reg, ReadReg(track, reg) ^ value);
        break;

    case 8:
        {
            seq_random_ = ntdlib::jaudio::NextFastRandom(seq_random_);
            const uint32_t r = seq_random_ >> 9;
            WriteReg(index, reg, value != 0 ? r % value : r);
            break;
        }

    case 9:
        WriteReg(index, reg, ReadReg(track, reg) << (value & 31));
        break;

    case 10:
        WriteReg(index, reg, ReadReg(track, reg) >> (value & 31));
        break;

    default:
        break;
    }
}

// 0xdc: register a[1] from entry a[3] of the table at a[2] in the sequence, of the width that a[0] picks.
void Tracks::LoadFromTable(uint32_t index, const Args& a)
{
    const Sequence& s = *tracks_[index].sequence;
    const uint32_t table = a[2];
    const uint32_t at = a[3];
    const auto be = [&s](uint32_t offset, int bytes)
    {
        uint32_t value = 0;
        for (int i = 0; i < bytes; i++)
        {
            value = value << 8 | s.U8(offset + static_cast<uint32_t>(i));
        }

        return value;
    };

    switch (a[0])
    {
    case 0xc:
        WriteReg(index, a[1], s.U8(table + at));
        break;
    case 0xd:
        WriteReg(index, a[1], be(table + 2 * at, 2));
        break;
    case 0xe:
        WriteReg(index, a[1], be(table + 3 * at, 3));
        break;
    case 0xf:
        WriteReg(index, a[1], be(table + 4 * at, 4));
        break;
    case 0x10:
        WriteReg(index, a[1], be(table + at, 4));
        break;
    default:
        break;
    }
}

// The tempo, the bank and program, the oscillators and the mix (0xe0 to 0xea).
void Tracks::SettingCommand(Track& track, uint32_t command, const Args& a)
{
    const Sequence& s = *track.sequence;

    switch (command)
    {
    case 0xe0:
        SetTempo(track, a[0]);
        break;

    case 0xe1:
        track.bank = static_cast<uint16_t>((a[0] & 0xffff) >> 8);
        track.program = static_cast<uint16_t>(a[0] & 0xff);
        break;

    case 0xe2:
        track.bank = static_cast<uint16_t>(a[0] & 0xff);
        break;

    case 0xe3:
        track.program = static_cast<uint16_t>(a[0] & 0xff);
        break;

    case 0xe7:
        if (a[0] < track.oscillators.size())
        {
            track.oscillators[a[0]].scale = static_cast<float>(a[1]) / 16383.0f;
        }
        break;

    case 0xe8:
        if (a[0] < track.oscillators.size())
        {
            // envSet: the table in the sequence. A pitch envelope loses its release table.
            track.oscillators[a[0]].table = s.Table(a[1]);
            track.oscillator_tables[a[0]] = true;
            if (a[0] != 0)
            {
                track.oscillators[a[0]].release = {};
            }
        }
        break;

    case 0xe9:
        // simpleADSR: the volume's envelope from the track's points, with a direct release.
        track.adsr[0].time = static_cast<int16_t>(a[0]);
        track.adsr[1].time = static_cast<int16_t>(a[1]);
        track.adsr[2].time = static_cast<int16_t>(a[2]);
        track.adsr[2].value = static_cast<int16_t>(a[3]);
        track.direct_release = static_cast<uint16_t>(a[4]);
        track.oscillators[0] = kEnvOsc;
        track.oscillators[0].table = track.adsr;
        track.oscillator_tables[0] = true;
        break;

    case 0xea:
        if (a[0] < track.mix_configs.size())
        {
            track.mix_configs[a[0]] = static_cast<uint16_t>(a[1]);
        }
        break;

    default:
        break;
    }
}

// The filters (0xeb to 0xed).
void Tracks::FilterCommand(Track& track, uint32_t command, const Args& a)
{
    const Sequence& s = *track.sequence;

    switch (command)
    {
    case 0xeb:
        if (a[0] <= 0x7f)
        {
            for (std::size_t i = 0; i < track.iir.size(); i++)
            {
                track.iir[i] = static_cast<uint16_t>(kCutoffToIir[4 * a[0] + i]);
            }
            track.filter_mode |= 0x20;
        }
        break;

    case 0xec:
        // IIRSet: four coefficients. The game copies four more from its stack. The DSP doesn't read them. The model
        // leaves them 0.
        track.iir = {static_cast<uint16_t>(a[0]),
                     static_cast<uint16_t>(a[1]),
                     static_cast<uint16_t>(a[2]),
                     static_cast<uint16_t>(a[3]),
                     0,
                     0,
                     0,
                     0};
        track.filter_mode |= 0x20;
        break;

    case 0xed:
        for (uint32_t i = 0; i < track.fir.size(); i++)
        {
            track.fir[i] = s.Native16(a[0] + 2 * i);
        }
        track.filter_mode = static_cast<uint8_t>((track.filter_mode & 0x20) | 8);
        break;

    default:
        break;
    }
}

// Waits, interrupts, timers and the end (0xf0 to 0xff). Returns -1 for the end.
int32_t Tracks::TimerCommand(Track& track, uint32_t command, const Args& a)
{
    switch (command)
    {
    case 0xf0:
        track.timer = static_cast<int32_t>(ReadMidiValue(track));
        break;

    case 0xf1:
        track.timer = static_cast<int32_t>(a[0]);
        break;

    case 0xf3:
        track.interrupt_table = a[0];
        break;

    case 0xf4:
        track.mask = static_cast<uint16_t>(track.mask | a[0]);
        break;

    case 0xf5:
        track.mask = static_cast<uint16_t>(track.mask & ~a[0]);
        break;

    case 0xf6:
        if (track.interrupted)
        {
            track.cursor = track.swap;
            track.interrupted = false;
            track.swap = 0;
        }
        CheckInterrupt(track);
        break;

    case 0xf7:
        track.interrupted = false;
        track.swap = 0;
        CheckInterrupt(track);
        break;

    case 0xf8:
        track.interrupt_timer = static_cast<uint8_t>(a[0]);
        track.timer_count = a[1];
        track.timer_reload = a[1];
        break;

    case 0xf9:
        // syncCPU: the game's callbacks answer from the game's state. Without a callback, the command gets 0xffff. The
        // model has none of that state.
        RegisterWrite(track, kRegFlag, 0xffff);
        break;

    case 0xfd:
        Printf(track);
        break;

    case 0xff:
        return -1;

    default:
        break;
    }

    return 0;
}

void Tracks::Printf(Track& track)
{
    // The format, up to 0x80 characters or its end, then a byte for each of its conversions. The game prints them.
    const Sequence& s = *track.sequence;
    uint32_t conversions = 0;
    for (uint32_t i = 0; i < 0x80; i++)
    {
        const uint32_t c = s.U8(track.cursor++);
        if (c == '%')
        {
            i++;
            if (s.U8(track.cursor++) == 0)
            {
                break;
            }
            conversions++;
        }
        else if (c == '\\')
        {
            i++;
            if (s.U8(track.cursor++) == 0)
            {
                break;
            }
        }
        else if (c == 0)
        {
            break;
        }
    }
    track.cursor += conversions;
}

void Tracks::SetParam(Track& track, uint32_t param, float value, uint32_t count)
{
    if (param >= track.params.size())
    {
        return;
    }

    TrackParam& p = track.params[param];
    p.target = value;
    if (count == 0)
    {
        p.value = value;
    }
    p.count = count;
}

void Tracks::SetTempo(Track& track, uint32_t tempo)
{
    track.tempo = static_cast<uint16_t>(tempo);
    track.flags |= Track::kInvalid;
    UpdateTempo(track);
}

void Tracks::SetLatestKey(uint32_t index, uint32_t key)
{
    tracks_[index].latest_key = static_cast<uint8_t>(key + static_cast<uint32_t>(TransposeSum(index)));
}

void Tracks::WritePort(uint32_t index, uint32_t port, uint32_t value)
{
    // A write to another track's port, as the game would: its flag, and interrupt 2 or 3 for port 0 or 1.
    Track& track = tracks_[index];
    if (port < track.ports.size())
    {
        track.import_flags = static_cast<uint16_t>(track.import_flags | 1u << port);
        track.ports[port] = static_cast<uint16_t>(value);
    }
    if (port <= 1)
    {
        const uint32_t bit = 1u << (port == 0 ? 2 : 3);
        if ((track.mask & bit) != 0)
        {
            track.pending = static_cast<uint16_t>(track.pending | bit);
        }
    }
}

} // namespace ntdlib::jaudio2
