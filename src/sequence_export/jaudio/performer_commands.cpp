// SPDX-License-Identifier: MIT

// JAudio2's sequence commands (see performer.h): the parser (JASSeqParser) and the commands that it runs.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include "ntdlib/jaudio/sequence_commands.h"
#include "sequence_export/jaudio/performer.h"

namespace ntdlib::sequence_export::jaudio
{
namespace
{

// JAudio2's commands from 0xa0.
constexpr const std::array<CommandInfo, 96>& kCommands = ntdlib::jaudio::kSequenceCommands;

// JASSeqParser::execNoteOnGate (0x80035050) turns a gate note's flags into JASTrack::gateOn's. Bit 7 sweeps from the
// latest key (1), bit 6 ties the next note to this one (2), and a length of 0 waits for the note to end (4).
uint32_t GateFlags(uint8_t flags, uint32_t length)
{
    return ((flags & 0x80) ? 1u : 0u) | ((flags & 0x40) ? 2u : 0u) | (length == 0 ? 4u : 0u);
}

} // namespace

Outcome Performer::Parse(int t)
{
    // JASSeqParser::parse.
    if (sound_.jaudio1)
    {
        return Parse1(t);
    }

    Track& tr = TrackAt(t);
    tr.visited[tr.pc] = {tick_, commands_run_++};
    const uint8_t b = ReadByte(t);
    if (!(b & 0x80))
    {
        // A note: flags (the note id in the low three bits), velocity, and a gate note's length.
        const uint8_t flags = ReadByte(t);
        const uint8_t velocity = ReadByte(t);
        if ((flags & 7) == 0)
        {
            const uint32_t length = ReadMidiValue(t);
            GateOn(t, b, velocity, static_cast<float>(length), GateFlags(flags, length));
            return WaitForGateNote(t, length);
        }

        NoteOn(t, flags & 7, b, velocity);
        return Outcome::kContinue;
    }

    switch (b & 0xf0)
    {
    case 0x80:
        if (b & 7)
        {
            NoteOff(t, b & 7, 0);
        }
        return Outcome::kContinue;

    case 0x90:
        {
            // A register prefix: the next command reads the flagged arguments from registers.
            const uint16_t types = RegisterTypes(ReadByte(t), (b & 7) + 1);
            const uint8_t cmd = ReadByte(t);
            return Command(t, cmd, types);
        }

    default:
        return Command(t, b, 0);
    }
}

// Read the arguments of a command whose table entry is `info`. `override_types` marks the arguments that a register
// prefix reads from registers. A register's argument is its number as a byte. `read_reg` reads the register: ReadReg,
// or ReadReg1 for JAudio1.
Performer::Arguments Performer::ReadArguments(int t, const CommandInfo& info, uint16_t override_types,
                                              uint32_t (Performer::*read_reg)(int, uint32_t))
{
    uint16_t types = info.types | override_types;
    Arguments a{};
    for (int i = 0; i < info.count; i++, types = static_cast<uint16_t>(types >> 2))
    {
        switch (types & 3)
        {
        case 0:
            a[static_cast<std::size_t>(i)] = ReadByte(t);
            break;
        case 1:
            a[static_cast<std::size_t>(i)] = Read16(t);
            break;
        case 2:
            a[static_cast<std::size_t>(i)] = Read24(t);
            break;
        default:
            a[static_cast<std::size_t>(i)] = (this->*read_reg)(t, ReadByte(t));
            break;
        }
    }

    return a;
}

Outcome Performer::Command(int t, uint8_t cmd, uint16_t override_types)
{
    // JASSeqParser::parseCommand: read the arguments the command table gives, then run the command.
    if (cmd == 0xb0)
    {
        ReadByte(t); // an extended command: only a debugging dump
        return Outcome::kContinue;
    }
    if (cmd < 0xa0)
    {
        return Outcome::kContinue;
    }

    const Arguments a = ReadArguments(t, kCommands[cmd - 0xa0u], override_types, &Performer::ReadReg);
    switch (cmd & 0xf0)
    {
    case 0xa0:
    case 0xb0:
        return NoteCommand(t, cmd, a);

    case 0xc0:
        FlowCommand(t, cmd, a);
        return Outcome::kContinue;

    case 0xd0:
        PortAndRegisterCommand(t, cmd, a);
        return Outcome::kContinue;

    case 0xe0:
        SettingCommand(t, cmd, a);
        return Outcome::kContinue;

    default:
        return ControlCommand(t, cmd, a);
    }
}

Outcome Performer::NoteCommand(int t, uint8_t cmd, const Arguments& a)
{
    Track& tr = TrackAt(t);
    switch (cmd)
    {
    case 0xb1:
        NoteOn(t, a[1], static_cast<int>(a[0]), static_cast<int>(a[2]));
        break;

    case 0xb2:
        NoteOff(t, a[0], 0);
        break;

    case 0xb3:
        GateOn(t, static_cast<int>(a[1]), static_cast<int>(a[2]), static_cast<float>(a[3]),
               GateFlags(static_cast<uint8_t>(a[0]), a[3]));
        return WaitForGateNote(t, a[3]);

    case 0xb4:
        if (a[0] < 256)
        {
            tr.latest_key = static_cast<uint8_t>(a[0] + TransposeTotal(t));
        }
        break;

    case 0xb8:
    case 0xba:
        SetParam(t, a[0], static_cast<float>(SpreadByte(a[1])) / 32767.0f, cmd == 0xba ? a[2] : 0);
        break;

    case 0xb9:
    case 0xbb:
        SetParam(t, a[0], static_cast<float>(static_cast<int16_t>(a[1])) / 32767.0f, cmd == 0xbb ? a[2] : 0);
        break;

    default:
        break;
    }

    return Outcome::kContinue;
}

void Performer::FlowCommand(int t, uint8_t cmd, const Arguments& a)
{
    switch (cmd)
    {
    case 0xc1:
        if (a[0] < kChildren)
        {
            OpenChild(t, a[0], a[1]);
        }
        break;

    case 0xc2:
        if (a[0] < kChildren)
        {
            CloseChild(t, a[0]);
        }
        break;

    case 0xc3:
        Call(t, a[0]);
        break;

    case 0xc4:
        if (Condition(t, a[0]))
        {
            Call(t, a[1]);
        }
        break;

    case 0xc5:
        Return(t);
        break;

    case 0xc6:
        if (Condition(t, a[0]))
        {
            Return(t);
        }
        break;

    case 0xc7:
        Jump(t, a[0]);
        break;

    case 0xc8:
        if (Condition(t, a[0]))
        {
            Jump(t, a[1]);
        }
        break;

    case 0xc9:
        Jump(t, Data24(a[1] + 3 * a[0]));
        break;

    case 0xca:
        Call(t, Data24(a[1] + 3 * a[0]));
        break;

    case 0xcb:
        LoopStart(t, a[0]);
        break;

    case 0xcc:
        LoopEnd(t);
        break;

    default:
        break;
    }
}

void Performer::PortAndRegisterCommand(int t, uint8_t cmd, const Arguments& a)
{
    Track& tr = TrackAt(t);
    switch (cmd)
    {
    case 0xd0:
        tr.imported = static_cast<uint16_t>(tr.imported & ~(1u << (a[0] & 15)));
        NotePortRead(t, a[0] & 15);
        WriteReg(t, a[1], tr.ports[a[0] & 15]);
        break;

    case 0xd1:
        tr.exported = static_cast<uint16_t>(tr.exported | (1u << (a[0] & 15)));
        NotePortWrite(t, a[0] & 15);
        tr.ports[a[0] & 15] = static_cast<uint16_t>(a[1]);
        break;

    case 0xd2:
        WriteRegister(t, 3, (tr.imported >> (a[0] & 15)) & 1);
        break;

    case 0xd3:
        WriteRegister(t, 3, (tr.exported >> (a[0] & 15)) & 1);
        break;

    case 0xd4:
    case 0xd5:
        // A write to the parent's port, or to a child's.
        if (const int other = PortTrack(t, cmd == 0xd5, a[0]); other >= 0)
        {
            WritePortOf(t, other, a[0] & 15, a[1]);
        }
        break;

    case 0xd6:
    case 0xd7:
        // A read of the parent's port, or of a child's. The game deletes a child once it has finished or closed. A read
        // of its port then writes nothing.
        if (const int other = PortTrack(t, cmd == 0xd7, a[0]); other >= 0 && TrackAt(other).open)
        {
            const uint32_t port = a[0] & 15;
            Track& o = TrackAt(other);
            o.exported = static_cast<uint16_t>(o.exported & ~(1u << port));
            NotePortRead(other, port);
            WriteReg(t, a[1], o.ports[port]);
        }
        break;

    case 0xd8:
        WriteReg(t, a[0], a[1]);
        break;

    case 0xd9:
    case 0xda:
        RegisterArithmetic(t, a);
        break;

    case 0xdb:
        if (a[0] == 11)
        {
            WriteReg(t, a[1], static_cast<uint32_t>(-static_cast<int32_t>(ReadReg(t, a[1]))));
        }
        break;

    case 0xdc:
        LoadFromTable(t, a);
        break;

    default:
        break;
    }
}

void Performer::RegisterArithmetic(int t, const Arguments& a)
{
    // JASSeqParser::cmdReg: arithmetic on a register, with a value or another register's value.
    uint32_t reg = a[1];
    uint32_t v = a[2];
    switch (a[0])
    {
    case 0:
        break;

    case 1:
        v += ReadReg(t, reg);
        break;

    case 2:
        v = ReadReg(t, reg) - v;
        break;

    case 3:
        v = ReadReg(t, reg) - v;
        reg = 3;
        break;

    case 4:
        v *= ReadReg(t, reg);
        reg = 0x21;
        break;

    case 5:
        v &= ReadReg(t, reg);
        break;

    case 6:
        v |= ReadReg(t, reg);
        break;

    case 7:
        v ^= ReadReg(t, reg);
        break;

    case 8:
        last_draw_ = tick_;
        v = v != 0 ? (rng_.Next() >> 9) % v : 0;
        break;

    case 9:
        v = ReadReg(t, reg) << (v & 31);
        break;

    case 10:
        v = ReadReg(t, reg) >> (v & 31);
        break;

    default:
        return;
    }
    WriteReg(t, reg, v);
}

void Performer::LoadFromTable(int t, const Arguments& a)
{
    uint32_t v = 0;
    switch (a[0])
    {
    case 12:
        v = Data8(a[2] + a[3]);
        break;
    case 13:
        v = Data16(a[2] + 2 * a[3]);
        break;
    case 14:
        v = Data24(a[2] + 3 * a[3]);
        break;
    case 15:
        v = Data32(a[2] + 4 * a[3]);
        break;
    case 16:
        v = Data32(a[2] + a[3]);
        break;
    default:
        return;
    }
    WriteReg(t, a[1], v);
}

void Performer::SettingCommand(int t, uint8_t cmd, const Arguments& a)
{
    Track& tr = TrackAt(t);
    switch (cmd)
    {
    case 0xe0:
        // Only the first track's tempo counts (JASTrack::setTempo asserts it).
        if (tr.parent < 0)
        {
            tempo_ = static_cast<uint16_t>(a[0]);
            tr.dirty = true;
            EmitTempo();
        }
        break;

    case 0xe1:
        tr.bank = static_cast<uint16_t>((a[0] >> 8) & 0xff);
        tr.program = static_cast<uint16_t>(a[0] & 0xff);
        break;

    case 0xe2:
        tr.bank = static_cast<uint16_t>(a[0] & 0xff);
        break;

    case 0xe3:
        tr.program = static_cast<uint16_t>(a[0] & 0xff);
        break;

    case 0xe7:
        {
            // JASTrack::setOscScale: the scale of the track's oscillator 0 (volume) or 1 (pitch), in 16383ths. The
            // scale multiplies the oscillator's output. A note plays the track's oscillator only once the track has set
            // its table (JASTrack::overwriteOsc). The note reads the scale as it plays. A new scale reaches it at once.
            if (a[0] >= 2)
            {
                break;
            }

            tr.osc_scale[a[0]] = static_cast<float>(a[1]) / 16383.0f;
            if (tr.osc[a[0]])
            {
                tr.osc[a[0]]->scale = tr.osc_scale[a[0]];
            }
            break;
        }

    case 0xe8:
        SetOscTable(t, a);
        break;

    case 0xe9:
        SetOscAdsr(t, a);
        break;

    case 0xea:
    case 0xed:
        break; // bus connections and FIR filters: the effects that the game sets up

    case 0xeb:
        tr.iir = a[0] < 127;
        tr.filter_entry = tr.iir ? static_cast<uint8_t>(a[0]) : kOpenTrackFilter;
        break;

    case 0xec:
        tr.iir = !(static_cast<int16_t>(a[0]) == 0x7fff && a[1] == 0 && a[2] == 0 && a[3] == 0);
        tr.filter_entry = kOpenTrackFilter;
        break;

    default:
        break;
    }
}

void Performer::SetOscTable(int t, const Arguments& a)
{
    // JASTrack::setOscTable: the track's table for oscillator 0 (volume) or 1 (pitch), from the sequence. The
    // oscillator keeps its scale.
    if (a[0] >= 2)
    {
        return;
    }

    Track& tr = TrackAt(t);
    Oscillator osc;
    osc.target = static_cast<uint8_t>(a[0]);
    osc.scale = tr.osc_scale[a[0]];
    for (uint32_t at = a[1], n = 0; n < 0x10000; at += 6, n++)
    {
        const OscPoint p{static_cast<int16_t>(Data16(at)), static_cast<int16_t>(Data16(at + 2)),
                         static_cast<int16_t>(Data16(at + 4))};
        osc.attack.push_back(p);
        if (p.mode > 10)
        {
            break;
        }
    }
    if (a[0] == 0 && tr.osc[0])
    {
        osc.release = tr.osc[0]->release; // osc 0 keeps its release table
    }
    tr.osc[a[0]] = std::move(osc);
}

void Performer::SetOscAdsr(int t, const Arguments& a)
{
    // JASTrack::setOscAdsr: oscillator 0 becomes the track's four-point table (sAdsTable) with these times and
    // sustain, and the direct release. Oscillator 0 starts from sEnvOsc's settings, with a scale of 1.
    Track& tr = TrackAt(t);
    tr.osc_scale[0] = 1.0f;
    Oscillator osc;
    osc.target = 0;
    osc.attack = {{0, static_cast<int16_t>(a[0]), 0x7fff},
                  {0, static_cast<int16_t>(a[1]), 0x7fff},
                  {0, static_cast<int16_t>(a[2]), static_cast<int16_t>(a[3])},
                  {0xe, 0, 0}};
    tr.osc[0] = std::move(osc);
    tr.direct_release = static_cast<uint16_t>(a[4]);
}

Outcome Performer::ControlCommand(int t, uint8_t cmd, const Arguments& a)
{
    Track& tr = TrackAt(t);
    switch (cmd)
    {
    case 0xf0:
        tr.wait = static_cast<int32_t>(ReadMidiValue(t));
        return tr.wait != 0 ? Outcome::kWait : Outcome::kContinue;

    case 0xf1:
        tr.wait = static_cast<int32_t>(a[0]);
        return tr.wait != 0 ? Outcome::kWait : Outcome::kContinue;

    case 0xf3:
        tr.intr_table = a[0];
        break;

    case 0xf4:
        tr.intr_mask = static_cast<uint16_t>(tr.intr_mask | a[0]);
        break;

    case 0xf5:
        tr.intr_mask = static_cast<uint16_t>(tr.intr_mask & ~a[0]);
        break;

    case 0xf6:
        if (tr.intr_return)
        {
            tr.pc = *tr.intr_return;
            tr.intr_return.reset();
        }
        CheckInterrupt(t);
        break;

    case 0xf7:
        tr.intr_return.reset();
        CheckInterrupt(t);
        break;

    case 0xf8:
        tr.timer_repeats = static_cast<uint8_t>(a[0]);
        tr.timer = a[1];
        tr.timer_reload = a[1];
        break;

    case 0xf9:
        // A game can answer through a callback (SequenceSound::callback). The command's handler writes the answer to
        // the flag register. Four Swords Adventures registers none. Its handler (0x80034654) then writes 0xffff.
        if (sound_.callback)
        {
            const auto answer = sound_.callback->find(static_cast<uint16_t>(a[0]));
            WriteRegister(t, 3, answer != sound_.callback->end() ? answer->second : 0);
        }
        else
        {
            WriteRegister(t, 3, 0xffff);
        }
        break;

    case 0xfd:
        SkipPrint(t);
        break;

    case 0xff:
        return Outcome::kFinish;

    default:
        break;
    }

    return Outcome::kContinue;
}

} // namespace ntdlib::sequence_export::jaudio
