// SPDX-License-Identifier: MIT

// JAudio1's tracks and sequence commands (see performer.h): TTrack, TSeqParser and TRegisterParam of JASystem,
// with the later JAudio1 of Pikmin 2 and the sound library of Pikmin.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include "sequence_export/jaudio/performer.h"

namespace ntdlib::sequence_export::jaudio
{
namespace
{

// JAudio1's commands from 0xc0 (JASystem::TSeqParser's Arglist in The Wind Waker's main.dol), encoded as JAudio2's
// are. 0xc4 (call) and 0xc8 (jump) read their arguments themselves, and 0xfb (print) reads its text.
constexpr std::array<CommandInfo, 64> MakeCommands1()
{
    std::array<CommandInfo, 64> c{};

    auto set = [&](int cmd, uint8_t count, uint16_t types)
    {
        c[static_cast<std::size_t>(cmd - 0xc0)] = {true, count, types};
    };

    set(0xc1, 2, 0x0008); // open a track
    set(0xc2, 2, 0x0008); // open a track beside this one
    set(0xc3, 1, 0x0002); // nothing, with an address
    set(0xc6, 1, 0x0000); // return if
    set(0xc7, 1, 0x0002); // nothing, with an address
    set(0xc9, 1, 0x0001); // loop start
    set(0xcb, 2, 0x0000); // read a port into a register
    set(0xcc, 2, 0x000c); // write a register's value to a port
    set(0xcd, 1, 0x0000); // check a port's import
    set(0xce, 1, 0x0000); // check a port's export
    set(0xcf, 1, 0x0003); // wait, a register's time
    set(0xd1, 2, 0x000c); // write the parent's port
    set(0xd2, 2, 0x000c); // write a child's port
    set(0xd4, 1, 0x0000); // set the latest key
    set(0xd5, 1, 0x0000); // whether note lengths follow the tempo
    set(0xd6, 1, 0x0000); // a simple oscillator
    set(0xd7, 2, 0x0008); // an envelope table
    set(0xd8, 5, 0x0155); // a simple ADSR
    set(0xd9, 1, 0x0000); // transpose
    set(0xda, 1, 0x0000); // close a track
    set(0xdb, 1, 0x0000); // the game's settings that apply
    set(0xdc, 1, 0x0001); // update the settings
    set(0xdd, 2, 0x0004); // bus connection
    set(0xde, 1, 0x0000); // pause settings
    set(0xdf, 2, 0x0008); // an interrupt's routine
    set(0xe0, 1, 0x0000); // disable an interrupt
    set(0xe4, 2, 0x0004); // interrupt timer
    set(0xe5, 1, 0x0000); // vibrato depth, a byte
    set(0xe6, 1, 0x0001); // vibrato depth, 16 bits
    set(0xe7, 1, 0x0001); // synchronise with the game
    set(0xea, 1, 0x0002); // wait, 24 bits
    set(0xeb, 5, 0x0000); // pan powers
    set(0xec, 4, 0x0055); // IIR coefficients
    set(0xed, 1, 0x0002); // FIR coefficients
    set(0xee, 1, 0x0002); // a buffer for the game's settings
    set(0xef, 3, 0x0000); // how the pan, the effect send and the surround combine
    set(0xf0, 1, 0x0000); // oscillator route
    set(0xf1, 1, 0x0000); // IIR cutoff
    set(0xf2, 3, 0x0028); // an oscillator
    set(0xf3, 1, 0x0000); // volume mode
    set(0xf4, 1, 0x0000); // vibrato speed
    set(0xfa, 1, 0x0001); // check a wave
    set(0xfd, 1, 0x0001); // tempo
    set(0xfe, 1, 0x0001); // timebase

    return c;
}

constexpr std::array<CommandInfo, 64> kCommands1 = MakeCommands1();

// Pikmin's commands from 0xc0 (SequenceSound::jac).
constexpr std::array<CommandInfo, 64> MakeCommandsJac()
{
    std::array<CommandInfo, 64> c = kCommands1;
    c[0xd0 - 0xc0] = {true, 2, 0x0005};
    c[0xd3 - 0xc0] = {true, 2, 0x000f};
    for (const int cmd : {0xe5, 0xe6, 0xf3, 0xf4})
    {
        c[static_cast<std::size_t>(cmd - 0xc0)] = {};
    }

    return c;
}

constexpr std::array<CommandInfo, 64> kCommandsJac = MakeCommandsJac();

// JASystem::Player::extend8to16: a byte as a signed 16-bit value.
uint16_t Extend8(uint8_t b)
{
    return static_cast<uint16_t>(static_cast<int16_t>(static_cast<int8_t>(b)));
}

// JASystem::TTrack::panCalc: a JAudio1 track's pan or effect send by its calc type. Type 0 keeps the track's value
// `value`, type 1 takes its parent's value `parent`, and type 2 mixes the two with the parent's at `weight`.
float PanCalc(float value, float parent, float weight, uint8_t type)
{
    switch (type)
    {
    case 1:
        return parent;
    case 2:
        return value * (1.0f - weight) + parent * weight;
    default:
        return value;
    }
}

} // namespace

void Performer::InitTrack1(int t)
{
    // JASystem::TRegisterParam::init: bank 0, program 0xf0, a bend sense of 12, the pan powers and a priority of 0x40.
    Track& tr = TrackAt(t);
    tr.regs = {};
    tr.regs[6] = 0x00f0;
    tr.regs[7] = 12;
    tr.regs[9] = 1;
    tr.regs[10] = 1;
    tr.regs[11] = 0x7fff;
    tr.regs[12] = 0x4000;
    tr.regs[13] = 0x40;
    tr.bank = 0;
    tr.program = 0xf0;
    tr.bend_sense = 12;
    tr.note_on_prio = 0x40;
}

int Performer::OpenChild1(int t, uint32_t mode_and_number, uint32_t start)
{
    // JASystem::TSeqParser::cmdOpenTrack and TTrack::openChild: the low four bits give the child's number, bits 6 and 7
    // its mode, and bit 5 mode 4. TTrack::inherit gives the child its parent's volume mode. Unless the mode sets bit 1,
    // the child also takes the parent's bank, program, bend sense, pan powers, priority and how its pan combines.
    const uint32_t number = mode_and_number & 15;
    const auto mode = static_cast<uint8_t>((mode_and_number & 0x20) ? 4 : (mode_and_number >> 6) & 3);
    const int child = OpenChild(t, number, start);
    InitTrack1(child);
    Track& c = TrackAt(child);
    const Track& p = TrackAt(t);
    c.open_mode = mode;
    c.volume_mode = p.volume_mode;
    c.tempo = p.tempo;
    c.own_tempo = false;
    c.tempo_count = 0.0f;
    if (!(mode & 2))
    {
        for (std::size_t i = 6; i < 14; i++)
        {
            c.regs[i] = p.regs[i];
        }
        c.parent_calc = p.parent_calc;
        c.channel_calc = p.channel_calc;
        c.bank = p.bank;
        c.program = p.program;
        c.bend_sense = p.bend_sense;
        c.note_on_prio = p.note_on_prio;
    }
    if (mode == 4)
    {
        Approximate("tracks that share their parents' voices get voices apart");
    }

    return child;
}

int Performer::SeqTick1(int t)
{
    // JASystem::TTrack::mainProc: the tick's interrupt, the timer and an interrupt that's due, then the wait, then the
    // commands up to the next wait.
    Interrupt(t, kIntr1Tick);
    TimerProcess(t);
    CheckInterrupt1(t);
    if (WaitsForGateNote(t))
    {
        return 0;
    }

    // The later JAudio1 counts down the wait that a running interrupt put aside too (SequenceSound::later_jaudio1).
    Track& tr = TrackAt(t);
    if (sound_.later_jaudio1 && tr.intr_wait > 0)
    {
        tr.intr_wait--;
    }
    if (tr.wait > 0)
    {
        if (--tr.wait > 0)
        {
            return 0;
        }

        // TNoteMgr::endProcess: the wait for a gate note is over. Its slot is free for the next note, and the note
        // plays on for its length.
        if (tr.gate_ends)
        {
            tr.gate_ends = false;
            tr.slots[0] = -1;
        }
    }

    return ParseToWait(t);
}

Outcome Performer::Parse1(int t)
{
    // JASystem::TSeqParser::parseSeq: below 0x80 a note, 0x80 and 0x88 a wait, the other commands of 0x8x and 0xf9 a
    // note's end, 0x9x a parameter, 0xax register arithmetic, 0xbx a command with arguments from registers, and the
    // command table from 0xc0.
    Track& tr = TrackAt(t);
    tr.visited[tr.pc] = {tick_, commands_run_++};
    const uint8_t b = ReadByte(t);
    if (!(b & 0x80))
    {
        return Note1(t, b);
    }

    if ((b & 0xf0) == 0x80 && !(b & 7))
    {
        // A wait of a byte (0x80) or of 16 bits (0x88).
        const uint32_t ticks = b == 0x80 ? ReadByte(t) : Read16(t);
        TrackAt(t).wait = static_cast<int32_t>(ticks);
        return ticks != 0 ? Outcome::kWait : Outcome::kContinue;
    }

    if ((b & 0xf0) == 0x80 || b == 0xf9)
    {
        NoteOff1(t, b);
        return Outcome::kContinue;
    }

    switch (b & 0xf0)
    {
    case 0x90:
        SetParam1(t, b);
        return Outcome::kContinue;

    case 0xa0:
        RegisterCommand1(t, b & 0xf);
        return Outcome::kContinue;

    case 0xb0:
        {
            // RegCmd_Process: the command, or with bit 3 the register that holds it. Then a byte whose top bits mark
            // the arguments that come from registers: the low three bits give their count, less one.
            uint8_t cmd = ReadByte(t);
            const bool from_register = (b & 8) != 0;
            const int count = (b & 7) + 1;
            if (from_register)
            {
                cmd = static_cast<uint8_t>(ReadReg1(t, cmd));
            }
            uint16_t types = 0;
            if (!from_register || count > 1)
            {
                types = RegisterTypes(ReadByte(t), count);
            }
            return Command1(t, cmd, types);
        }

    default:
        return Command1(t, b, 0);
    }
}

void Performer::NoteOff1(int t, uint8_t b)
{
    // cmdNoteOff: the note id in the low three bits and, with bit 3, a release time. 0xf9 takes the note id from a
    // register, and the top bit of its byte gives the release time. A time above 100 counts in steps of 20.
    uint8_t flag = b;
    if (b == 0xf9)
    {
        const uint8_t r = ReadByte(t);
        flag = static_cast<uint8_t>(0x80 + (ReadReg1(t, r & 7) & 0xff));
        if (r & 0x80)
        {
            flag = static_cast<uint8_t>(flag | 8);
        }
    }
    uint32_t release = 0;
    if (flag & 8)
    {
        release = ReadByte(t);
        if (release > 100)
        {
            release = (release - 98) * 20;
        }
    }
    NoteOff(t, flag & 7, static_cast<uint16_t>(release));
}

void Performer::SetParam1(int t, uint8_t b)
{
    // cmdSetParam: the target, then the value and the time, each read as the command's low bits give.
    const uint8_t target = ReadByte(t);
    int16_t data = 0;
    switch (b & 0xc)
    {
    case 0:
        data = static_cast<int16_t>(ReadReg16(t, ReadByte(t)));
        break;
    case 4:
        data = ReadByte(t);
        break;
    case 8:
        data = SpreadByte(ReadByte(t));
        break;
    default:
        data = static_cast<int16_t>(Read16(t));
        break;
    }

    int32_t time = -1;
    switch (b & 3)
    {
    case 1:
        time = ReadReg16(t, ReadByte(t));
        break;
    case 2:
        time = ReadByte(t);
        break;
    case 3:
        time = Read16(t);
        break;
    default:
        break;
    }

    // Targets 0 to 5 are JAudio2's. The IIR filter's coefficients are 12 to 15.
    if (target >= 12 && target <= 15)
    {
        TrackAt(t).iir = true;
        TrackAt(t).filter_entry = kOpenTrackFilter;
    }
    SetParam(t, target, static_cast<float>(data) / 32767.0f, time > 0 ? static_cast<uint32_t>(time) : 0);
}

Outcome Performer::Note1(int t, uint8_t key)
{
    // JASystem::TSeqParser::cmdNoteOn. The flags give the note id in their low three bits. Bit 7 takes the key from a
    // register. Bits 5 and 6 give how the note connects: bit 6 sweeps from the latest key, and bit 5 ties the next note
    // to this one. A note of id 0 is a gate note: the share of its length that it plays, in percent, then its length in
    // up to three bytes. Bits 3 and 4 give the count. The track waits for that length, or for the note's end with a
    // length of 0. A note of another id plays until its end. Its bits 3 and 4 take the id from a register, and its bit
    // 5 reads a register's sample skip.
    const uint8_t flags = ReadByte(t);
    int k = key;
    if (flags & 0x80)
    {
        k = static_cast<int>(ReadReg1(t, key) & 0xff);
    }
    k += TransposeTotal(t);
    uint8_t connect = (flags >> 5) & 3;
    const int target = k;
    if (connect & 2)
    {
        k = TrackAt(t).latest_key;
    }

    uint32_t velocity = ReadByte(t);
    if (velocity & 0x80)
    {
        velocity = ReadReg1(t, velocity & 0x7f) & 0xff;
    }

    uint32_t note_id = flags & 7;
    if (note_id == 0)
    {
        uint32_t share = ReadByte(t);
        if (share & 0x80)
        {
            share = ReadReg1(t, share & 0x7f) & 0xff;
        }
        uint32_t length = 0;
        const int count = (flags >> 3) & 3;
        for (int i = 0; i < count; i++)
        {
            length = length << 8 | ReadByte(t);
        }
        if (count == 1 && (length & 0x80))
        {
            length = ReadReg1(t, length & 0x7f);
        }

        // seqTimeToDspTime takes the share of the length. GateOn divides it by the tick rate. Flag 1 of GateOn sweeps,
        // flag 2 ties, and flag 4 waits for the note's end.
        Track& tr = TrackAt(t);
        if (!tr.time_relate)
        {
            Approximate("note lengths that don't follow the tempo follow it");
        }
        const uint32_t gate_flags = ((connect & 2) ? 1u : 0u) | ((connect & 1) ? 2u : 0u) | (length == 0 ? 4u : 0u);
        const float time = static_cast<float>(length) * static_cast<float>(share) / 100.0f;
        GateOn(t, target - TransposeTotal(t), static_cast<int>(velocity), time, gate_flags);
        TrackAt(t).gate_ends = length != 0 && connect == 0;
        return WaitForGateNote(t, length);
    }

    if ((flags >> 3) & 3)
    {
        note_id = ReadReg1(t, note_id - 1);
    }
    uint32_t skip = 0;
    if (connect & 1)
    {
        skip = ReadReg1(t, ReadByte(t));
        connect = static_cast<uint8_t>(connect ^ 1);
    }
    if (connect & 2)
    {
        Approximate("sweeps of notes with an id omitted");
    }

    Track& tr = TrackAt(t);
    tr.skip_samples = skip;
    NoteOn(t, note_id, k - TransposeTotal(t), static_cast<int>(velocity));
    Track& again = TrackAt(t);
    again.skip_samples = 0;
    again.tied = false;
    again.latest_key = static_cast<uint8_t>(std::clamp(target, 0, 127));

    return Outcome::kContinue;
}

Outcome Performer::Command1(int t, uint8_t cmd, uint16_t override_types)
{
    // JASystem::TSeqParser::Cmd_Process: read the arguments that the table gives, then run the command.
    if (cmd < 0xc0)
    {
        return Outcome::kContinue;
    }

    if (cmd == 0xc4 || cmd == 0xc8)
    {
        JumpOrCall1(t, cmd);
        return Outcome::kContinue;
    }

    if (cmd == 0xfb)
    {
        SkipPrint(t);
        return Outcome::kContinue;
    }

    const auto& commands = sound_.jac ? kCommandsJac : kCommands1;
    const Arguments a = ReadArguments(t, commands[cmd - 0xc0u], override_types, &Performer::ReadReg1);

    // Pikmin's track names, and its commands that do nothing.
    if (sound_.jac && (cmd == 0xd0 || cmd == 0xd3 || cmd == 0xe5 || cmd == 0xe6 || cmd == 0xf3 || cmd == 0xf4))
    {
        return Outcome::kContinue;
    }

    switch (cmd)
    {
    case 0xc1:
    case 0xc2:
    case 0xc6:
    case 0xc9:
    case 0xca:
    case 0xcf:
    case 0xda:
    case 0xe8:
    case 0xea:
    case 0xff:
        return TrackCommand1(t, cmd, a);

    case 0xcb:
    case 0xcc:
    case 0xcd:
    case 0xce:
    case 0xd1:
    case 0xd2:
        PortCommand1(t, cmd, a);
        return Outcome::kContinue;

    case 0xd6:
    case 0xd7:
    case 0xd8:
    case 0xf0:
    case 0xf2:
        OscillatorCommand1(t, cmd, a);
        return Outcome::kContinue;

    case 0xdf:
    case 0xe0:
    case 0xe1:
    case 0xe2:
    case 0xe3:
    case 0xe4:
        InterruptCommand1(t, cmd, a);
        return Outcome::kContinue;

    case 0xd4:
    case 0xd5:
    case 0xd9:
    case 0xdd:
    case 0xe5:
    case 0xe6:
    case 0xeb:
    case 0xec:
    case 0xef:
    case 0xf1:
    case 0xf3:
    case 0xf4:
    case 0xfd:
    case 0xfe:
        SettingCommand1(t, cmd, a);
        return Outcome::kContinue;

    case 0xe7:
    case 0xfa:
        GameCommand1(t, cmd, a);
        return Outcome::kContinue;

    default:
        // Filters, the game's settings, pauses and updates.
        return Outcome::kContinue;
    }
}

void Performer::JumpOrCall1(int t, uint8_t cmd)
{
    // cmdCall and cmdJmp: a flag byte with the condition in its low four bits. With bit 7, a register gives the
    // target, or with bit 6 the index into a table of 24-bit targets. With bit 5, a register gives the table's
    // offset too.
    const uint8_t flag = ReadByte(t);
    uint32_t target = 0;
    if (flag & 0x80)
    {
        const uint8_t reg = ReadByte(t);
        if (flag & 0x40)
        {
            const uint32_t index = ReadReg16(t, reg);
            const uint32_t table = (flag & 0x20) ? ReadReg16(t, ReadByte(t)) : Read24(t);
            target = Data24(table + 3 * index);
        }
        else
        {
            target = cmd == 0xc4 ? ReadReg16(t, reg) : ReadReg32(t, reg);
        }
    }
    else
    {
        target = Read24(t);
    }
    if (Condition(t, flag & 0xf))
    {
        if (cmd == 0xc4)
        {
            Call(t, target);
        }
        else
        {
            Jump(t, target);
        }
    }
}

Outcome Performer::TrackCommand1(int t, uint8_t cmd, const Arguments& a)
{
    Track& tr = TrackAt(t);
    switch (cmd)
    {
    case 0xc1:
        OpenChild1(t, a[0], a[1]);
        break;

    case 0xc2:
        if (tr.parent >= 0)
        {
            OpenChild1(tr.parent, a[0], a[1]);
        }
        break;

    case 0xc6:
        // cmdRet: a return with nothing to return to ends the track.
        if (Condition(t, a[0]) && !Return(t))
        {
            return Outcome::kFinish;
        }
        break;

    case 0xc9:
        LoopStart(t, a[0]);
        break;

    case 0xca:
        LoopEnd(t);
        break;

    case 0xcf:
    case 0xea:
        tr.wait = static_cast<int32_t>(a[0]);
        return a[0] != 0 ? Outcome::kWait : Outcome::kContinue;

    case 0xda:
        if (a[0] < kChildren)
        {
            CloseChild(t, a[0]);
        }
        break;

    case 0xe8:
        // cmdFlushAll: the track's notes stop at once.
        for (const int slot : tr.slots)
        {
            if (slot >= 0)
            {
                EndChannel(static_cast<std::size_t>(slot));
            }
        }
        break;

    case 0xff:
        return Outcome::kFinish;

    default:
        break;
    }

    return Outcome::kContinue;
}

void Performer::PortCommand1(int t, uint8_t cmd, const Arguments& a)
{
    Track& tr = TrackAt(t);
    switch (cmd)
    {
    case 0xcb:
        // cmdReadPort: the port's value goes to the register, and its import flag clears.
        tr.imported = static_cast<uint16_t>(tr.imported & ~(1u << (a[0] & 15)));
        NotePortRead(t, a[0] & 15);
        WriteRegDirect1(t, a[1], tr.ports[a[0] & 15]);
        break;

    case 0xcc:
        tr.exported = static_cast<uint16_t>(tr.exported | (1u << (a[0] & 15)));
        NotePortWrite(t, a[0] & 15);
        tr.ports[a[0] & 15] = static_cast<uint16_t>(a[1]);
        break;

    case 0xcd:
        StoreRegister1(t, 3, (tr.imported >> (a[0] & 15)) & 1, (tr.imported >> (a[0] & 15)) & 1);
        break;

    case 0xce:
        StoreRegister1(t, 3, (tr.exported >> (a[0] & 15)) & 1, (tr.exported >> (a[0] & 15)) & 1);
        break;

    case 0xd1:
    case 0xd2:
        // writePortAppDirect: a write to the parent's port, or to a child's.
        if (const int other = PortTrack(t, cmd == 0xd2, a[0]); other >= 0)
        {
            WritePortOf(t, other, a[0] & 15, a[1]);
        }
        break;

    default:
        break;
    }
}

void Performer::OscillatorCommand1(int t, uint8_t cmd, const Arguments& a)
{
    Track& tr = TrackAt(t);
    switch (cmd)
    {
    case 0xd6:
    case 0xd7:
    case 0xf2:
        // TTrack's simpleosc (0x8027ee5c in The Wind Waker's main.dol), envtable (0x8027ee8c) and osc (0x8027f65c)
        // give one of the track's oscillators other templates and tables from the sequence's data. The performer
        // leaves those out. A note that a route gives that oscillator keeps the instrument's (RouteOscillators1).
        // simpleosc 1 sets oscillator 0, and 0 and 2 set oscillator 1 (0x80281900). envtable 0 and 1 change
        // oscillator 0 (0x802817e4). Bit 4 of osc's first argument picks the oscillator (0x80281708).
        Approximate("oscillators that tracks set omitted");
        if (cmd == 0xd6 && a[0] < 3)
        {
            tr.osc1_known[a[0] == 1 ? 0 : 1] = false;
        }
        else if (cmd == 0xd7 && a[0] < 2)
        {
            tr.osc1_known[0] = false;
        }
        else if (cmd == 0xf2)
        {
            tr.osc1_known[(a[0] >> 4) & 1] = false;
        }
        break;

    case 0xd8:
        if (!sound_.track_oscillator_routes)
        {
            Approximate("oscillators that tracks set omitted");
            tr.direct_release = static_cast<uint16_t>(a[4]);
            break;
        }
        {
            // TTrack's simple ADSR (0x8027eec0 in The Wind Waker's main.dol): oscillator 0 becomes a volume oscillator
            // with no release table (the template at 0x80367550). Its attack table (TTrack +0x304, from the template at
            // 0x80398370) rises to full over the first argument and holds there for the second. It then moves to the
            // fourth over the third, and holds. The fifth is the notes' direct release.
            const auto arg = [&](std::size_t i)
            {
                return static_cast<int16_t>(a[i]);
            };
            Oscillator osc;
            osc.attack = {{0, arg(0), 0x7fff}, {0, arg(1), 0x7fff}, {0, arg(2), arg(3)}, {14, 0, 0}};
            tr.osc1[0] = std::move(osc);
            tr.osc1_known[0] = true;
            tr.direct_release = static_cast<uint16_t>(a[4]);
            break;
        }

    case 0xf0:
        // TTrack's oscillator route (0x8027f460): the high four bits pick the track's oscillator, and the low four give
        // its route (RouteOscillators1).
        if (!sound_.track_oscillator_routes)
        {
            Approximate("oscillators that tracks set omitted");
        }
        else if ((a[0] >> 4) < tr.osc_routes.size())
        {
            tr.osc_routes[a[0] >> 4] = static_cast<uint8_t>(a[0] & 15);
        }
        break;

    default:
        break;
    }
}

void Performer::InterruptCommand1(int t, uint8_t cmd, const Arguments& a)
{
    Track& tr = TrackAt(t);
    switch (cmd)
    {
    case 0xdf:
        // TIntrMgr::setIntr: the interrupt's routine, and the interrupt enabled.
        if (a[0] < 8)
        {
            tr.intr_addresses[a[0]] = a[1];
            tr.intr_mask = static_cast<uint16_t>(tr.intr_mask | (1u << a[0]));
        }
        break;

    case 0xe0:
        tr.intr_mask = static_cast<uint16_t>(tr.intr_mask & ~(1u << (a[0] & 15)));
        break;

    case 0xe1:
        // cmdClrI: interrupts enabled, and the running one given up without a return.
        tr.intr_enabled = true;
        tr.intr_return.reset();
        break;

    case 0xe2:
        tr.intr_enabled = false;
        break;

    case 0xe3:
        // cmdRetI: back to where the interrupt came in, with the wait put aside.
        tr.intr_enabled = true;
        if (tr.intr_return)
        {
            tr.pc = *tr.intr_return;
            tr.intr_return.reset();
            tr.wait = tr.intr_wait < 0 ? 0 : tr.intr_wait;
            tr.wait_note_finish = tr.intr_wait < 0;
        }
        CheckInterrupt1(t);
        break;

    case 0xe4:
        tr.timer_repeats = static_cast<uint8_t>(a[0]);
        tr.timer = a[1];
        tr.timer_reload = a[1];
        break;

    default:
        break;
    }
}

void Performer::SettingCommand1(int t, uint8_t cmd, const Arguments& a)
{
    Track& tr = TrackAt(t);
    switch (cmd)
    {
    case 0xd4:
        tr.latest_key = static_cast<uint8_t>(std::clamp(static_cast<int>(a[0]) + TransposeTotal(t), 0, 127));
        break;

    case 0xd5:
        tr.time_relate = a[0] != 0;
        break;

    case 0xd9:
        tr.transpose = static_cast<int8_t>(a[0]);
        break;

    case 0xdd:
        // TTrack::connectBus: a mix line of the track's channels and its bus. Mix line 0 at 0xffff puts the channels
        // that the track starts from then on onto the DSP's automatic mixer. A track opens with the default lines
        // (TChannelMgr::init). Its children don't take its lines.
        if (a[0] == 0)
        {
            tr.auto_mixer = static_cast<uint16_t>(a[1]) == 0xffff;
            tr.dirty = true;
        }
        break;

    case 0xe5:
        // TVibrate: its depth times 4 octaves (Player::pitchToCent with a bend sense of 12). A byte spreads over 16
        // bits.
        tr.vib_depth = 4.0f * static_cast<float>((a[0] & 0xff) << 8 | (a[0] & 0xff) << 1) / 393204.0f;
        break;

    case 0xe6:
        tr.vib_depth = 4.0f * static_cast<float>(a[0]) / 393204.0f;
        break;

    case 0xeb:
        // cmdPanPowSet: the pan powers of a note's bank value, effects' value and track value (CalcWeights), then the
        // parent's and the track's in hundredths.
        tr.regs[8] = static_cast<uint16_t>(a[0]);
        tr.regs[9] = static_cast<uint16_t>(a[1]);
        tr.regs[10] = static_cast<uint16_t>(a[2]);
        tr.regs[11] = static_cast<uint16_t>(static_cast<float>(a[3]) * 327.67f);
        tr.regs[12] = static_cast<uint16_t>(static_cast<float>(a[4]) * 327.67f);
        tr.dirty = true;
        break;

    case 0xec:
        tr.iir = !(static_cast<int16_t>(a[0]) == 0x7fff && a[1] == 0 && a[2] == 0 && a[3] == 0);
        tr.filter_entry = kOpenTrackFilter;
        break;

    case 0xef:
        {
            // cmdPanSwSet (0x8027f3bc): the top three bits of each byte pick how the track's value takes its parent's.
            // The low five bits pick the rule for its notes' values (kCalcRules). The game reads a rule past the table
            // from the bytes after it.
            constexpr std::array<uint8_t, 8> kParentCalc{0, 1, 2, 0, 2, 0, 2, 0};
            for (std::size_t i = 0; i < 3; i++)
            {
                tr.parent_calc[i] = kParentCalc[(a[i] >> 5) & 7];
                tr.channel_calc[i] = static_cast<uint8_t>(a[i] & 0x1f);
                if (i < 2 && tr.channel_calc[i] >= kCalcRules.size())
                {
                    Approximate("notes' pan and effect send by a rule past the game's table taken as the sum");
                }
            }
            tr.dirty = true;
            break;
        }

    case 0xf1:
        tr.iir = a[0] < 127;
        tr.filter_entry = tr.iir ? static_cast<uint8_t>(a[0]) : kOpenTrackFilter;
        break;

    case 0xf3:
        tr.volume_mode = static_cast<uint8_t>(a[0]);
        tr.dirty = true;
        break;

    case 0xf4:
        // TVibrate's speed: a byte spread over 16 bits, in quarters of a cycle per update. JAudio2's LFO counts 72nds.
        tr.vib_pitch =
            18.0f * static_cast<float>(static_cast<uint16_t>((a[0] & 0xff) << 8 | (a[0] & 0xff) << 1)) / 294903.0f;
        break;

    case 0xfd:
        // A child track's tempo runs its ticks at a share of its parent's (TTrack +0x388). Only the first track's tempo
        // counts here, but in Pikmin's library (TickProc).
        tr.tempo = static_cast<uint16_t>(a[0]);
        if (tr.parent < 0)
        {
            tempo_ = static_cast<uint16_t>(a[0]);
            tr.dirty = true;
            EmitTempo();
        }
        else if (sound_.jac)
        {
            tr.own_tempo = true;
        }
        else if (a[0] != tempo_)
        {
            Approximate("tempos of child tracks omitted");
        }
        break;

    case 0xfe:
        if (tr.parent < 0)
        {
            timebase_ = static_cast<uint16_t>(a[0]);
            tr.dirty = true;
            EmitTempo();
        }
        break;

    default:
        break;
    }
}

void Performer::GameCommand1(int t, uint8_t cmd, const Arguments& a)
{
    Track& tr = TrackAt(t);
    switch (cmd)
    {
    case 0xe7:
        {
            // The Wind Waker's callback (JAIBasic::setParameterSeqSync) answers 0. Another game's callback may give
            // other answers (SequenceSound::sync_answers), or mute the track (SequenceSound::mute_child_15).
            // A conductor answers its calls first (SequenceSound::conductor).
            const auto id = static_cast<uint16_t>(a[0]);
            std::optional<uint16_t> value = sound_.conductor ? ConductorCall(t, id) : std::nullopt;
            if (!value)
            {
                const auto answer = sound_.sync_answers.find(id);
                value = answer != sound_.sync_answers.end() ? answer->second : 0;
            }
            StoreRegister1(t, 3, *value, *value);
        }
        if (sound_.mute_child_15 && a[0] == 0 && tr.parent >= 0 && TrackAt(tr.parent).children[15] == t)
        {
            tr.muted = true;
        }
        break;

    case 0xfa:
        StoreRegister1(t, 3, 0, 0);
        break;

    default:
        break;
    }
}

void Performer::RegisterCommand1(int t, uint8_t low)
{
    // JASystem::TTrack::writeRegParam: an operation on a register with a value or another register's value. The low
    // four bits give the value's encoding and the operation. With 9 and 10, a byte after the command gives them: 9 the
    // other operations, 10 a load from a table.
    uint32_t encoding = low & 0xc;
    uint32_t operation = low & 3;
    uint32_t table_kind = 0;
    if (low == 11)
    {
        encoding = 0;
        operation = 11;
    }
    else if (low == 10)
    {
        const uint8_t b = ReadByte(t);
        encoding = b & 0xc;
        table_kind = (b >> 4) + 4u;
        operation = 10;
    }
    else if (low == 9)
    {
        const uint8_t b = ReadByte(t);
        encoding = b & 0xc;
        operation = b & 0xf0;
        if (encoding == 8)
        {
            encoding = 16;
        }
    }

    const uint32_t reg = ReadByte(t);
    uint32_t table = 0;
    if (operation == 10)
    {
        table = ReadReg32(t, ReadByte(t));
    }

    int16_t value = ReadRegisterValue1(t, encoding);

    const auto old = static_cast<int16_t>(ReadReg16(t, reg));
    const auto widened = static_cast<uint32_t>(static_cast<int32_t>(old));
    if (encoding == 4 && (operation == 1 || operation == 0x10 || operation == 0x20))
    {
        value = static_cast<int16_t>(Extend8(static_cast<uint8_t>(value)));
    }
    uint32_t loaded = 0;
    switch (operation)
    {
    case 0:
        break;
    case 1:
        value = static_cast<int16_t>(value + old);
        break;
    case 2:
        {
            // A product goes to registers 4 and 5.
            const uint32_t product = widened * static_cast<uint32_t>(static_cast<int32_t>(value));
            WriteRegDirect1(t, 4, static_cast<uint16_t>(product >> 16));
            WriteRegDirect1(t, 5, static_cast<uint16_t>(product));
            return;
        }
    case 3:
        // A comparison sets the flag only.
        StoreRegister1(t, 3, static_cast<uint16_t>(old - value), static_cast<uint16_t>(old - value));
        return;
    case 11:
        value = static_cast<int16_t>(widened - static_cast<uint32_t>(static_cast<int32_t>(value)));
        break;
    case 0x10:
        value = static_cast<int16_t>(value < 0 ? static_cast<uint16_t>(old) >> (-value & 31)
                                               : static_cast<uint16_t>(old) << (value & 31));
        break;
    case 0x20:
        value = static_cast<int16_t>(value < 0 ? old >> (-value & 31) : old << (value & 31));
        break;
    case 0x30:
        value = static_cast<int16_t>(value & old);
        break;
    case 0x40:
        value = static_cast<int16_t>(value | old);
        break;
    case 0x50:
        value = static_cast<int16_t>(value ^ old);
        break;
    case 0x60:
        value = static_cast<int16_t>(-old);
        break;
    case 0x90:
        last_draw_ = tick_;
        value =
            static_cast<uint16_t>(value) != 0 ? static_cast<int16_t>(rng_.Next() % static_cast<uint16_t>(value)) : 0;
        break;
    case 10:
        loaded = LoadTable1(table, table_kind, static_cast<uint32_t>(static_cast<uint16_t>(value)));
        value = static_cast<int16_t>(loaded & 0xffff);
        break;
    default:
        return;
    }

    StoreResult1(t, reg, value, loaded);
}

int16_t Performer::ReadRegisterValue1(int t, uint32_t encoding)
{
    switch (encoding)
    {
    case 0:
        return static_cast<int16_t>(ReadReg16(t, ReadByte(t)));
    case 4:
        return ReadByte(t);
    case 12:
        return static_cast<int16_t>(Read16(t));
    case 8:
        return SpreadByte(ReadByte(t));
    default:
        return -1;
    }
}

uint32_t Performer::LoadTable1(uint32_t table, uint32_t kind, uint32_t index) const
{
    // TTrack::loadTbl: a byte, 16 bits, 24 bits or 32 bits at the index, or 32 bits at a byte offset.
    switch (kind)
    {
    case 4:
        return Data8(table + index);
    case 5:
        return Data16(table + 2 * index);
    case 6:
        return Data24(table + 3 * index);
    case 7:
        return Data32(table + 4 * index);
    case 8:
        return Data32(table + index);
    default:
        return 0;
    }
}

void Performer::StoreResult1(int t, uint32_t reg, int16_t value, uint32_t loaded)
{
    Track& tr = TrackAt(t);
    auto v = static_cast<uint16_t>(value);
    uint16_t flag = v;
    switch (reg)
    {
    case 0:
    case 1:
    case 2:
        v &= 0xff;
        flag = Extend8(static_cast<uint8_t>(v));
        break;
    case 0x20:
        // The bank: the high byte of register 6.
        v = static_cast<uint16_t>((tr.regs[6] & 0xff) | v << 8);
        reg = 6;
        break;
    case 0x21:
        // The program: the low byte of register 6.
        v = static_cast<uint16_t>((tr.regs[6] & 0xff00) | (v & 0xff));
        reg = 6;
        break;
    case 0x22:
        WriteRegDirect1(t, 0, static_cast<uint16_t>(v >> 8));
        reg = 1;
        v &= 0xff;
        flag = v;
        break;
    case 0x2e:
        v = static_cast<uint16_t>((tr.regs[13] & 0xff00) | (v & 0xff));
        reg = 13;
        break;
    case 0x2f:
        v = static_cast<uint16_t>((tr.regs[13] & 0xff) | v << 8);
        reg = 13;
        break;
    case 0x28:
    case 0x29:
    case 0x2a:
    case 0x2b:
        if (sound_.jac || sound_.later_jaudio1)
        {
            tr.regs[16 + 2 * (reg - 0x28)] = static_cast<uint16_t>(loaded >> 16);
            tr.regs[17 + 2 * (reg - 0x28)] = static_cast<uint16_t>(loaded);
        }
        else
        {
            tr.addresses[reg - 0x28] = loaded;
        }
        return;
    default:
        break;
    }
    StoreRegister1(t, reg, v, flag);
}

uint32_t Performer::ReadReg1(int t, uint32_t reg)
{
    // JASystem::TTrack::exchangeRegisterValue: the registers below 0x40, then the ports.
    if (reg < 0x40)
    {
        return ReadReg32(t, reg);
    }

    if (reg - 0x40 >= 16)
    {
        return 0;
    }

    NotePortRead(t, reg - 0x40);
    return TrackAt(t).ports[reg - 0x40];
}

uint16_t Performer::ReadReg16(int t, uint32_t reg)
{
    // JASystem::TTrack::readReg16: the bank and the program, registers 0 and 1 together, the running children, the
    // note ids that hold no note, and the loop's count.
    const Track& tr = TrackAt(t);
    switch (reg)
    {
    case 0x20:
        return static_cast<uint16_t>(tr.regs[6] >> 8);
    case 0x21:
        return static_cast<uint16_t>(tr.regs[6] & 0xff);
    case 0x22:
        return static_cast<uint16_t>((tr.regs[0] & 0xff) << 8 | (tr.regs[1] & 0xff));
    case 0x2c:
        {
            uint16_t running = 0;
            for (int i = 0; i < kChildren; i++)
            {
                const int child = tr.children[static_cast<std::size_t>(i)];
                if (child >= 0 && TrackAt(child).open)
                {
                    running = static_cast<uint16_t>(running | (1u << i));
                }
            }
            return running;
        }
    case 0x2d:
        {
            uint16_t free = 0;
            for (int i = 0; i < kNoteIds; i++)
            {
                if (tr.slots[static_cast<std::size_t>(i)] < 0)
                {
                    free = static_cast<uint16_t>(free | (1u << i));
                }
            }
            return free;
        }
    case 0x30:
        return tr.depth > 0 ? tr.loop_count[static_cast<std::size_t>(tr.depth - 1)] : 0;
    default:
        return reg < Registers1() ? tr.regs[reg] : 0;
    }
}

std::size_t Performer::Registers1() const
{
    return sound_.jac || sound_.later_jaudio1 ? kRegistersJac : kRegisters1;
}

uint32_t Performer::ReadReg32(int t, uint32_t reg)
{
    // JASystem::TTrack::readReg32: the address registers, and registers 4 and 5 together.
    switch (reg)
    {
    case 0x28:
    case 0x29:
    case 0x2a:
    case 0x2b:
        if (sound_.jac || sound_.later_jaudio1)
        {
            const Track& tr = TrackAt(t);
            return static_cast<uint32_t>(tr.regs[16 + 2 * (reg - 0x28)]) << 16 | tr.regs[17 + 2 * (reg - 0x28)];
        }
        return TrackAt(t).addresses[reg - 0x28];
    case 0x23:
        return static_cast<uint32_t>(ReadReg16(t, 4)) << 16 | ReadReg16(t, 5);
    default:
        return ReadReg16(t, reg);
    }
}

void Performer::WriteRegDirect1(int t, uint32_t reg, uint16_t value)
{
    // JASystem::TTrack::writeRegDirect: registers 0 to 2 hold a byte, and the flag takes it sign-extended. The bank and
    // the program can't be written this way. 0x22 writes its high byte to register 0 and its low byte to register 1.
    switch (reg)
    {
    case 0:
    case 1:
    case 2:
        StoreRegister1(t, reg, value & 0xff, Extend8(static_cast<uint8_t>(value)));
        return;
    case 0x20:
    case 0x21:
        return;
    case 0x22:
        WriteRegDirect1(t, 0, static_cast<uint16_t>(value >> 8));
        StoreRegister1(t, 1, value & 0xff, value);
        return;
    default:
        StoreRegister1(t, reg, value, value);
        return;
    }
}

void Performer::StoreRegister1(int t, uint32_t reg, uint16_t value, uint16_t flag)
{
    // The write, then the flag. A new bank and program select what the track's notes play, and the bend sense moves its
    // pitch. Register 13 holds the notes' priority.
    Track& tr = TrackAt(t);
    if (reg < Registers1())
    {
        tr.regs[reg] = value;
    }
    tr.regs[3] = flag;
    switch (reg)
    {
    case 6:
        // A new bank or program clears the oscillators' routes (0x8028369c in The Wind Waker's main.dol).
        tr.bank = static_cast<uint16_t>(value >> 8);
        tr.program = static_cast<uint16_t>(value & 0xff);
        tr.osc_routes.fill(kNoRoute);
        break;
    case 7:
        tr.bend_sense = static_cast<uint8_t>(value);
        tr.dirty = true;
        break;
    case 13:
        tr.note_on_prio = static_cast<uint8_t>(value);
        break;
    default:
        break;
    }
}

void Performer::CheckInterrupt1(int t)
{
    // JASystem::TTrack::tryInterrupt: while interrupts are enabled and none runs, the lowest pending interrupt that is
    // set runs its routine. TSeqCtrl::callIntr puts the track's wait aside until the routine returns.
    Track& tr = TrackAt(t);
    if (tr.intr_return || !tr.intr_enabled)
    {
        return;
    }

    const auto ready = static_cast<uint16_t>(tr.intr_mask & tr.intr_pending);
    for (std::size_t i = 0; i < tr.intr_addresses.size(); i++)
    {
        if (ready & (1u << i))
        {
            tr.intr_pending = static_cast<uint16_t>(tr.intr_pending & ~(1u << i));
            tr.intr_return = tr.pc;
            tr.pc = tr.intr_addresses[i];
            tr.intr_wait = tr.wait_note_finish ? -1 : tr.wait;
            tr.wait = 0;
            tr.wait_note_finish = false;
            return;
        }
    }
}

void Performer::UpdateTrack1(int t)
{
    // JASystem::TTrack::updateTrack: the volume, squared in volume mode 0 where the game's JASystem squares it
    // (SequenceSound::squared_track_volume), and the bend. Both follow JAudio2's
    // (Player::pitchToCent's table follows equal temperament within a cent). A track's pan and effect send take its
    // parent's by the track's calc type, with the track's pan power as the weight. By default a track keeps its values.
    // A track opened in a mode with bit 0 set takes nothing from its parent. Pikmin's library (SequenceSound::jac)
    // plays the volume as it is: Jam_UpdateTrack (0x80010e20 in the USA release's main.dol) and Jam_UpdateTrackAll
    // (0x80010ae0) multiply the volume parameter (+0x14c) into the parent's volume and square nothing.
    Track& tr = TrackAt(t);
    float volume = tr.move[0].value;
    if (SquaresVolume(tr))
    {
        volume *= volume;
    }
    const float bend = tr.move[1].value * static_cast<float>(tr.bend_sense) * (1.0f / 3.0f);
    ChannelParams p;
    p.volume = volume * tr.ext_volume;
    p.pan = tr.move[3].value;
    p.fx_mix = tr.move[2].value;
    p.bend = bend;
    if (tr.parent < 0)
    {
        p.volume *= SoundVolume();
        if (sound_.pitch != 1.0f)
        {
            p.bend += std::log2(sound_.pitch);
        }
    }
    else if (!(tr.open_mode & 1))
    {
        const ChannelParams& up = TrackAt(tr.parent).params;
        const float weight = static_cast<float>(tr.regs[12]) / 32767.0f;
        p.volume *= up.volume;
        p.pitch = up.pitch;
        p.bend += up.bend;
        p.pan = PanCalc(p.pan, up.pan, weight, tr.parent_calc[0]);
        p.fx_mix = PanCalc(p.fx_mix, up.fx_mix, weight, tr.parent_calc[1]);
    }
    tr.params = p;
    tr.tick_rate = tr.parent < 0 ? TicksPerSubFrame() : TrackAt(tr.parent).tick_rate;
    if (sound_.jac && tr.parent >= 0 && tr.own_tempo)
    {
        tr.tick_rate *= TempoShare(t);
    }
}

float Performer::TempoShare(int t) const
{
    const Track& tr = TrackAt(t);
    const uint16_t parent = TrackAt(tr.parent).tempo;

    return parent == 0 ? 1.0f : std::min(static_cast<float>(tr.tempo) / static_cast<float>(parent), 1.0f);
}

} // namespace ntdlib::sequence_export::jaudio
