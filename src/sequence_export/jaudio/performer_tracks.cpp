// SPDX-License-Identifier: MIT

// The performer's tracks (see performer.h): their opening and closing, their ticks, the reader of their commands,
// the call stack and loops, the registers and ports, the interrupts and timers, and the settings that they pass
// down to their channels.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "sequence_export/jaudio/performer.h"

namespace ntdlib::sequence_export::jaudio
{
namespace
{

// Commands a track may run in one tick. JAudio2 parses until a command waits. A track that loops without waiting hangs
// the game. The performer stops such a track instead.
constexpr int kParseLimit = 100000;

// A track of `parent` as it opens: no children and no notes, at full volume and the pan's middle, with nothing moving.
Track FreshTrack(int parent)
{
    Track tr;
    tr.parent = parent;
    tr.children.fill(-1);
    tr.slots.fill(-1);
    tr.move[0].value = 1.0f; // volume
    tr.move[3].value = 0.5f; // pan
    for (MoveParam& m : tr.move)
    {
        m.target = m.value;
    }

    return tr;
}

} // namespace

int Performer::NewTrack(int parent, int number)
{
    if (tracks_.size() >= kMaxTracks)
    {
        throw FormatError("sequence opens too many tracks");
    }

    Track tr = FreshTrack(parent);
    tr.voice = parent < 0 ? in_voice_ : TrackAt(parent).voice;
    const std::string prefix = tr.voice < 0 ? "Track " : "Effect " + std::to_string(tr.voice) + " track ";
    tr.name = parent < 0 ? (tr.voice < 0 ? "Root" : "Effect " + std::to_string(tr.voice))
                         : (TrackAt(parent).parent < 0 ? prefix : TrackAt(parent).name + ".") + std::to_string(number);

    // Child `number` of the root's child n opens muted if its bit of SequenceSound::tracks is clear. The game's code
    // muted it before the sound's first tick, and the first open keeps the mute.
    const int grandparent = parent < 0 ? -1 : TrackAt(parent).parent;
    std::optional<std::size_t> handle_index;
    if (tr.voice < 0 && grandparent >= 0 && TrackAt(grandparent).parent < 0)
    {
        const std::array<int, kChildren>& top = TrackAt(grandparent).children;
        const auto n = static_cast<std::size_t>(std::find(top.begin(), top.end(), parent) - top.begin());
        tr.muted = n < top.size() && (sound_.tracks[n] >> number & 1) == 0;
        if (!sound_.jaudio1 && n < top.size())
        {
            handle_index = 16 * n + static_cast<std::size_t>(number);
        }
    }

    // The sound's handle reaches the root's children in JAudio1, and child m of the root's child n, as 16 * n + m, in
    // JAudio2 (SequenceSound::child_volumes). A track at 0 is silent. A silent track starts no notes, and its
    // controllers stay as they are at full volume. In a sequence of Pikmin's library, the volumes go to the root's
    // children through their external buffers instead (OpenChild).
    if (tr.voice < 0 && sound_.jaudio1 && !sound_.jac && parent >= 0 && TrackAt(parent).parent < 0)
    {
        handle_index = static_cast<std::size_t>(number);
    }
    const auto volume = handle_index && *handle_index < 256
                            ? sound_.child_volumes.find(static_cast<uint8_t>(*handle_index))
                            : sound_.child_volumes.end();
    if (volume != sound_.child_volumes.end())
    {
        // setChildTrackVolume (0x802b3fec in Twilight Princess's main.dol) keeps a volume between 0 and 1.
        const float level = std::clamp(volume->second, 0.0f, 1.0f);
        tr.silent = level == 0.0f;
        if (level > 0.0f && level < 1.0f)
        {
            if (sound_.jaudio1)
            {
                Approximate("a track volume between 0 and 1 through the sound's handle played as 1");
            }
            else
            {
                tr.handle_volume = level;
            }
        }
    }

    tracks_.push_back(std::move(tr));

    return static_cast<int>(tracks_.size() - 1);
}

bool Performer::Muted(int t) const
{
    for (int u = t; u >= 0; u = TrackAt(u).parent)
    {
        if (TrackAt(u).muted || TrackAt(u).silent)
        {
            return true;
        }
    }

    return false;
}

int Performer::OpenChild(int t, uint32_t number, uint32_t start)
{
    // JASTrack::openChild: a running child is closed and starts again; the new child inherits its parent's bank,
    // program, priorities, skip, filters and bus connections. It reads on from `start`.
    Track& parent = TrackAt(t);
    int child = parent.children[number];
    if (child >= 0 && TrackAt(child).open)
    {
        UpdateSeq(0, false, 1.0f);
        CloseTrack(child);
    }
    if (child < 0)
    {
        child = NewTrack(t, static_cast<int>(number));
        TrackAt(t).children[number] = child;
    }
    else
    {
        // Start afresh. Keep the MIDI track it records into.
        Track& c = TrackAt(child);
        Track fresh = FreshTrack(t);
        fresh.children = c.children;
        fresh.slot = c.slot;
        fresh.name = c.name;
        fresh.silent = c.silent;
        fresh.handle_volume = c.handle_volume;
        fresh.output = c.output;
        fresh.visited = c.visited;
        fresh.voice = c.voice;
        c = std::move(fresh);
    }

    Track& c = TrackAt(child);
    const Track& p = TrackAt(t);
    // The later JAudio1's openChild copies the parent's tick rate (0x800a155c in Pikmin 2's main.dol).
    if (sound_.later_jaudio1)
    {
        c.tick_rate = p.tick_rate;
    }
    // Jaq_OpenTrack (0x80014780 in Pikmin's main.dol) gives a child the external buffer that its parent holds for its
    // number, each time the child opens.
    const auto ext = sound_.child_volumes.find(static_cast<uint8_t>(number));
    if (sound_.jac && p.parent < 0 && ext != sound_.child_volumes.end())
    {
        c.ext_volume = ext->second;
        c.muted = c.muted || ext->second == 0.0f;
    }
    if (p.parent < 0 && (sound_.muted_children >> number & 1) != 0)
    {
        c.muted = true;
    }
    c.bank = p.bank;
    c.program = p.program;
    c.note_on_prio = p.note_on_prio;
    c.release_prio = p.release_prio;
    c.skip_samples = p.skip_samples;
    c.iir = p.iir;
    c.filter_entry = p.filter_entry;
    c.opened = tick_;
    c.pc = start;
    c.open = true;
    c.dirty = true;

    return child;
}

void Performer::CloseTrack(int t)
{
    // JASTrack::close: close the children, and release the track's channels.
    Track& tr = TrackAt(t);
    for (int child : tr.children)
    {
        if (child >= 0 && TrackAt(child).open)
        {
            CloseTrack(child);
        }
    }

    for (int& slot : TrackAt(t).slots)
    {
        if (slot >= 0)
        {
            ReleaseChannel(static_cast<std::size_t>(slot), 0);
            Detach(static_cast<std::size_t>(slot));
            slot = -1;
        }
    }

    TrackAt(t).open = false;
}

void Performer::CloseChild(int t, uint32_t number)
{
    const int child = TrackAt(t).children[number];
    if (child >= 0 && TrackAt(child).open)
    {
        UpdateSeq(0, false, 1.0f);
        CloseTrack(child);
    }
}

int Performer::TickProc(int t)
{
    // Pikmin's child track with a tempo of its own skips the parent's ticks that leave its count below 1 (0x80012ec0 in
    // the USA release's main.dol).
    if (sound_.jac && TrackAt(t).parent >= 0 && TrackAt(t).own_tempo)
    {
        Track& tr = TrackAt(t);
        tr.tempo_count += TempoShare(t);
        if (tr.tempo_count < 1.0f)
        {
            return 0;
        }
        tr.tempo_count -= 1.0f;
    }

    // JASTrack::tickProc: run the track's tick, move its timed parameters, then its children's ticks.
    const int ret = SeqTick(t);
    UpdateTimedParams(t);
    TrackAt(t).dirty = true;
    if (ret < 0)
    {
        return -1;
    }

    for (int i = 0; i < kChildren && !stop_; i++)
    {
        const int child = TrackAt(t).children[static_cast<std::size_t>(i)];
        if (child >= 0 && TrackAt(child).open && TickProc(child) < 0)
        {
            UpdateSeq(0, false, 1.0f);
            CloseTrack(child);
        }
    }

    return 0;
}

int Performer::SeqTick(int t)
{
    // JASSeqCtrl::tickProc.
    if (sound_.jaudio1)
    {
        return SeqTick1(t);
    }

    Interrupt(t, kIntrTick);
    TimerProcess(t);
    if (WaitsForGateNote(t))
    {
        return 0;
    }

    Track& tr = TrackAt(t);
    if (tr.wait > 0)
    {
        tr.wait--;
    }
    CheckInterrupt(t);

    return ParseToWait(t);
}

Outcome Performer::WaitForGateNote(int t, uint32_t length)
{
    Track& tr = TrackAt(t);
    if (length != 0)
    {
        tr.wait = static_cast<int32_t>(length);
    }
    else
    {
        tr.wait_note_finish = true;
    }

    return Outcome::kWait;
}

bool Performer::WaitsForGateNote(int t)
{
    // The wait lasts while the note holds slot 0.
    Track& tr = TrackAt(t);
    if (tr.wait_note_finish && tr.slots[0] >= 0)
    {
        return true;
    }

    tr.wait_note_finish = false;

    return false;
}

int Performer::ParseToWait(int t)
{
    // JAudio2 reads on through a wait while an interrupt runs. JAudio1 puts the wait aside while one runs
    // (CheckInterrupt1).
    const auto reads_on = [this, t]
    {
        const Track& tr = TrackAt(t);
        return (tr.wait == 0 || (!sound_.jaudio1 && tr.intr_return)) && !tr.wait_note_finish && !stop_;
    };
    int commands = 0;
    while (reads_on())
    {
        if (++commands > kParseLimit)
        {
            out_.warnings.push_back(TrackAt(t).name + " runs without waiting. It would hang the game. " + program_ +
                                    " stopped it");
            return -1;
        }

        if (Parse(t) == Outcome::kFinish)
        {
            return -1;
        }
    }

    // A wait for a note to end is a wait too, as far as loops go.
    const Track& after = TrackAt(t);
    if (after.wait > 0 || after.wait_note_finish)
    {
        TrackAt(t).last_wait = tick_;
    }

    return 0;
}

uint32_t Performer::Data8(uint32_t offset) const
{
    return offset < data_.size() ? data_[offset] : 0xff;
}

uint32_t Performer::Data16(uint32_t offset) const
{
    return Data8(offset) << 8 | Data8(offset + 1);
}

uint32_t Performer::Data24(uint32_t offset) const
{
    return Data8(offset) << 16 | Data8(offset + 1) << 8 | Data8(offset + 2);
}

uint32_t Performer::Data32(uint32_t offset) const
{
    return Data16(offset) << 16 | Data16(offset + 2);
}

uint8_t Performer::ReadByte(int t)
{
    // Past the end of the data, read 0xff: finish.
    Track& tr = TrackAt(t);
    const auto b = static_cast<uint8_t>(Data8(tr.pc));
    tr.pc++;

    return b;
}

uint16_t Performer::Read16(int t)
{
    const uint32_t hi = ReadByte(t);
    return static_cast<uint16_t>(hi << 8 | ReadByte(t));
}

uint32_t Performer::Read24(int t)
{
    const uint32_t hi = Read16(t);
    return hi << 8 | ReadByte(t);
}

uint32_t Performer::ReadMidiValue(int t)
{
    // JASSeqReader::readMidiValue: up to four bytes of seven bits; a longer value reads as 0.
    uint32_t value = ReadByte(t);
    if (!(value & 0x80))
    {
        return value;
    }

    value &= 0x7f;
    for (int i = 0;; i++)
    {
        if (i > 2)
        {
            return 0;
        }

        const uint8_t b = ReadByte(t);
        value = value << 7 | (b & 0x7f);
        if (!(b & 0x80))
        {
            return value;
        }
    }
}

void Performer::SkipPrint(int t)
{
    // A debugging print: its text, then a byte for each % in it.
    int args = 0;
    for (int i = 0; i < 128; i++)
    {
        const uint8_t c = ReadByte(t);
        if (c == 0)
        {
            break;
        }

        if (c == '\\')
        {
            if (ReadByte(t) == 0)
            {
                break;
            }
        }
        else if (c == '%')
        {
            if (ReadByte(t) == 0)
            {
                break;
            }

            args++;
        }
    }
    for (int i = 0; i < args; i++)
    {
        ReadByte(t);
    }
}

void Performer::Jump(int t, uint32_t target)
{
    // The track's position is the next command. A jump there goes nowhere and isn't a jump back.
    Track& tr = TrackAt(t);
    if (target < tr.pc)
    {
        OnJumpBack(t, target);
    }
    TrackAt(t).pc = target;
}

bool Performer::Call(int t, uint32_t target)
{
    Track& tr = TrackAt(t);
    if (tr.depth >= kStackSize)
    {
        return false;
    }

    tr.return_to[static_cast<std::size_t>(tr.depth++)] = tr.pc;
    tr.pc = target;

    return true;
}

bool Performer::Return(int t)
{
    Track& tr = TrackAt(t);
    if (tr.depth == 0)
    {
        return false;
    }

    tr.pc = tr.return_to[static_cast<std::size_t>(--tr.depth)];

    return true;
}

void Performer::LoopStart(int t, uint32_t count)
{
    // JASSeqReader::loopStart, and JAudio1's TSeqCtrl::loopStart: the count goes on the call stack with the return
    // address.
    Track& tr = TrackAt(t);
    if (tr.depth < kStackSize)
    {
        tr.return_to[static_cast<std::size_t>(tr.depth)] = tr.pc;
        tr.loop_count[static_cast<std::size_t>(tr.depth)] = static_cast<uint16_t>(count);
        tr.depth++;
    }
}

void Performer::LoopEnd(int t)
{
    // JASSeqReader::loopEnd, and JAudio1's TSeqCtrl::loopEnd: a count of 0 or 1 plays the loop once.
    Track& tr = TrackAt(t);
    if (tr.depth == 0)
    {
        return;
    }

    uint16_t& count = tr.loop_count[static_cast<std::size_t>(tr.depth - 1)];
    const uint16_t left = count != 0 ? static_cast<uint16_t>(count - 1) : 0;
    if (left == 0)
    {
        tr.depth--;
        return;
    }

    count = left;
    tr.pc = tr.return_to[static_cast<std::size_t>(tr.depth - 1)];
}

bool Performer::Condition(int t, uint32_t condition)
{
    // JASSeqParser::conditionCheck, on register 3.
    const uint32_t reg = ReadReg(t, 3);
    switch (condition)
    {
    case 0:
        return true;
    case 1:
        return reg == 0;
    case 2:
        return reg != 0;
    case 3:
        return reg == 1;
    case 4:
        return reg >= 0x8000;
    case 5:
        return reg < 0x8000;
    default:
        return false;
    }
}

uint32_t Performer::ReadRegister(int t, uint32_t reg)
{
    // JASRegisterParam::read: 14 registers, and pairs of them read together.
    const auto& r = TrackAt(t).regs;
    if (reg < 14)
    {
        return r[reg];
    }

    switch (reg)
    {
    case 32:
        return (r[0] & 0xff) << 8 | (r[1] & 0xff);
    case 33:
        return static_cast<uint32_t>(r[4]) << 16 | r[5];
    case 34:
        return r[4] >> 8;
    case 35:
        return r[4] & 0xff;
    case 36:
        return r[5] >> 8;
    case 37:
        return r[5] & 0xff;
    case 38:
        return static_cast<uint32_t>(r[6]) << 16 | r[7];
    case 39:
        return static_cast<uint32_t>(r[8]) << 16 | r[9];
    case 40:
    case 41: // the game reads 40's registers for 41 too
        return static_cast<uint32_t>(r[10]) << 16 | r[11];
    default:
        return 0;
    }
}

void Performer::WriteRegister(int t, uint32_t reg, uint32_t value)
{
    auto& r = TrackAt(t).regs;
    if (reg < 14)
    {
        r[reg] = static_cast<uint16_t>(reg <= 2 ? (value & 0xff) : (value & 0xffff));
        return;
    }

    switch (reg)
    {
    case 32:
        r[0] = (value >> 8) & 0xff;
        r[1] = value & 0xff;
        break;

    case 33:
        r[4] = static_cast<uint16_t>(value >> 16);
        r[5] = static_cast<uint16_t>(value);
        break;

    case 34:
        r[4] = static_cast<uint16_t>((r[4] & 0xff) | (value & 0xff) << 8);
        break;

    case 35:
        r[4] = static_cast<uint16_t>((r[4] & 0xff00) | (value & 0xff));
        break;

    case 36:
        r[5] = static_cast<uint16_t>((r[5] & 0xff) | (value & 0xff) << 8);
        break;

    case 37:
        r[5] = static_cast<uint16_t>((r[5] & 0xff00) | (value & 0xff));
        break;

    case 38:
        r[6] = static_cast<uint16_t>(value >> 16);
        r[7] = static_cast<uint16_t>(value);
        break;

    case 39:
        r[8] = static_cast<uint16_t>(value >> 16);
        r[9] = static_cast<uint16_t>(value);
        break;

    case 40:
        r[10] = static_cast<uint16_t>(value >> 16);
        r[11] = static_cast<uint16_t>(value);
        break;

    case 41:
        r[12] = static_cast<uint16_t>(value >> 16);
        r[13] = static_cast<uint16_t>(value);
        break;

    default:
        break;
    }
}

uint32_t Performer::ReadReg(int t, uint32_t reg)
{
    // JASSeqParser::readReg: the registers, the ports from 0x40, and the track's settings from 0x60.
    Track& tr = TrackAt(t);
    if (reg < 0x40)
    {
        return ReadRegister(t, reg);
    }
    if (reg < 0x50)
    {
        NotePortRead(t, reg - 0x40);
        return tr.ports[reg - 0x40];
    }

    switch (reg)
    {
    case 0x60:
        {
            uint32_t running = 0;
            for (int i = 0; i < kChildren; i++)
            {
                const int child = tr.children[static_cast<std::size_t>(i)];
                if (child >= 0 && TrackAt(child).open)
                {
                    running |= 1u << i;
                }
            }
            return running;
        }

    case 0x61:
        return tr.depth > 0 ? tr.loop_count[static_cast<std::size_t>(tr.depth - 1)] : 0;

    case 0x62:
        return timebase_;

    case 0x63:
        return static_cast<uint32_t>(static_cast<int32_t>(tr.transpose));

    case 0x64:
        return tr.bend_sense;

    case 0x65:
        return tr.gate_rate;

    case 0x66:
        return tr.skip_samples;

    case 0x67:
        return tr.bank;

    case 0x68:
        return tr.program;

    case 0x69:
        return static_cast<uint32_t>(static_cast<int32_t>(tr.pan_power * 32767.0f));

    case 0x6a:
        return tr.release_prio | tr.note_on_prio; // the game forgets to shift the release priority

    case 0x6b:
        return tr.note_on_prio;

    case 0x6c:
        return tr.release_prio;

    case 0x6d:
        return tr.direct_release;

    case 0x6e:
        return static_cast<uint32_t>(tr.vib_depth * 1524.0f);

    case 0x6f:
        return static_cast<uint32_t>(tr.vib_depth * 12192.0f);

    case 0x70:
        return static_cast<uint32_t>(tr.trem_depth * 256.0f);

    case 0x71:
        return static_cast<uint32_t>(tr.vib_pitch * 64.0f);

    case 0x72:
        return static_cast<uint32_t>(tr.trem_pitch * 64.0f);

    case 0x73:
        return tr.vib_delay;

    case 0x74:
        return tr.trem_delay;

    default:
        return 0;
    }
}

void Performer::WriteReg(int t, uint32_t reg, uint32_t value)
{
    // JASSeqParser::writeReg: every write also sets register 3, the flag the conditions test.
    WriteRegister(t, 3, value);
    Track& tr = TrackAt(t);
    if (reg < 0x40)
    {
        WriteRegister(t, reg, value);
        return;
    }
    if (reg < 0x50)
    {
        NotePortWrite(t, reg - 0x40);
        tr.ports[reg - 0x40] = static_cast<uint16_t>(value);
        return;
    }

    switch (reg)
    {
    case 0x62:
        // Only the first track's timebase counts (JASTrack::setTimebase asserts it).
        if (tr.parent < 0)
        {
            timebase_ = static_cast<uint16_t>(value);
            tr.dirty = true;
            EmitTempo();
        }
        break;

    case 0x63:
        tr.transpose = static_cast<int8_t>(value);
        break;

    case 0x64:
        tr.bend_sense = static_cast<uint8_t>(value);
        break;

    case 0x65:
        tr.gate_rate = static_cast<uint8_t>(value);
        break;

    case 0x66:
        tr.skip_samples = value;
        break;

    case 0x67:
        tr.bank = static_cast<uint8_t>(value);
        break;

    case 0x68:
        tr.program = static_cast<uint8_t>(value);
        break;

    case 0x69:
        tr.pan_power = static_cast<float>(value) / 32767.0f;
        break;

    case 0x6a:
        tr.release_prio = static_cast<uint8_t>(value >> 8);
        tr.note_on_prio = static_cast<uint8_t>(value);
        break;

    case 0x6b:
        tr.note_on_prio = static_cast<uint8_t>(value);
        break;

    case 0x6c:
        tr.release_prio = static_cast<uint8_t>(value);
        break;

    case 0x6d:
        tr.direct_release = static_cast<uint16_t>(value);
        break;

    case 0x6e:
        tr.vib_depth = static_cast<float>(value) / 1524.0f;
        break;

    case 0x6f:
        tr.vib_depth = static_cast<float>(value) / 12192.0f;
        break;

    case 0x70:
        tr.trem_depth = static_cast<float>(value) / 256.0f;
        break;

    case 0x71:
        tr.vib_pitch = static_cast<float>(value) / 64.0f;
        break;

    case 0x72:
        tr.trem_pitch = static_cast<float>(value) / 64.0f;
        break;

    case 0x73:
        tr.vib_delay = static_cast<uint16_t>(value);
        break;

    case 0x74:
        tr.trem_delay = static_cast<uint16_t>(value);
        break;

    default:
        break;
    }
}

void Performer::Interrupt(int t, int type)
{
    Track& tr = TrackAt(t);
    if (tr.intr_mask & (1u << type))
    {
        tr.intr_pending = static_cast<uint16_t>(tr.intr_pending | (1u << type));
    }
}

void Performer::CheckInterrupt(int t)
{
    // JASSeqCtrl::checkIntr: take the lowest pending interrupt that the mask enables. Do nothing while one is running.
    Track& tr = TrackAt(t);
    if (tr.intr_return)
    {
        return;
    }

    const uint16_t ready = tr.intr_mask & tr.intr_pending;
    for (int i = 0; i < 16; i++)
    {
        if (ready & (1u << i))
        {
            tr.intr_pending = static_cast<uint16_t>(tr.intr_pending & ~(1u << i));
            tr.intr_return = tr.pc;
            tr.pc = Data24(tr.intr_table + 3 * static_cast<uint32_t>(i));
            return;
        }
    }
}

int Performer::PortTrack(int t, bool child, uint32_t port) const
{
    const Track& tr = TrackAt(t);
    return child ? tr.children[(port >> 4) & 15] : tr.parent;
}

void Performer::WritePortOf(int t, int other, uint32_t port, uint32_t value)
{
    TrackAt(t).last_signal = commands_run_;
    WritePortApp(other, port, value);
}

void Performer::NotePortRead(int t, uint32_t port)
{
    if (t == root_track_ && ((first_ports_ >> port) & 1) != 0)
    {
        out_.ports_read = static_cast<uint16_t>(out_.ports_read | (1u << port));
    }
}

void Performer::NotePortWrite(int t, uint32_t port)
{
    if (t == root_track_)
    {
        first_ports_ = static_cast<uint16_t>(first_ports_ & ~(1u << port));
    }
}

void Performer::WritePortApp(int t, uint32_t port, uint32_t value)
{
    NotePortWrite(t, port);
    Track& tr = TrackAt(t);
    tr.imported = static_cast<uint16_t>(tr.imported | (1u << port));
    tr.ports[port] = static_cast<uint16_t>(value);
    if (port == 0)
    {
        Interrupt(t, sound_.jaudio1 ? kIntr1Port0 : kIntrPort0);
    }
    else if (port == 1)
    {
        Interrupt(t, sound_.jaudio1 ? kIntr1Port1 : kIntrPort1);
    }
}

void Performer::TimerProcess(int t)
{
    // JASSeqCtrl::timerProcess: the timer raises its interrupt when it runs out, and starts again for as many repeats
    // as it has left, or for ever with none.
    Track& tr = TrackAt(t);
    if (tr.timer == 0)
    {
        return;
    }
    tr.timer--;
    if (tr.timer != 0)
    {
        return;
    }

    Interrupt(t, sound_.jaudio1 ? kIntr1Timer : kIntrTimer);
    if (tr.timer_repeats != 0)
    {
        tr.timer_repeats--;
        if (tr.timer_repeats != 0)
        {
            tr.timer = tr.timer_reload;
        }
    }
    else
    {
        tr.timer = tr.timer_reload;
    }
}

void Performer::SetParam(int t, uint32_t target, float value, uint32_t count)
{
    // JASTrack::setParam: move to the value over `count` ticks, or at once.
    if (target >= 6)
    {
        // JAudio1's targets 6 to 10 change the width, the rate and the vertex of the track's oscillators (0x80281850 in
        // The Wind Waker's main.dol). The performer leaves those out.
        if (sound_.jaudio1 && target <= 10)
        {
            TrackAt(t).osc1_known[target <= 8 ? 0 : 1] = false;
        }
        return;
    }

    MoveParam& m = TrackAt(t).move[target];
    if (m.target != value || m.value != value)
    {
        Changed(t);
    }
    m.target = value;
    if (count == 0)
    {
        m.value = value;
    }
    m.count = count;
}

void Performer::UpdateTimedParams(int t)
{
    for (MoveParam& m : TrackAt(t).move)
    {
        if (m.count != 0)
        {
            m.value += (m.target - m.value) / static_cast<float>(m.count);
            m.count--;
        }
    }
}

void Performer::UpdateSeq(int t, bool force, float pan_scale)
{
    // JASTrack::updateSeq: recompute the settings of the tracks that changed, and of all their descendants.
    Track& tr = TrackAt(t);
    if (!force)
    {
        force = tr.dirty;
    }
    tr.dirty = false;
    if (force)
    {
        UpdateTrack(t, pan_scale);
    }

    const float child_scale = pan_scale * TrackAt(t).pan_power;
    for (int i = 0; i < kChildren; i++)
    {
        const int child = TrackAt(t).children[static_cast<std::size_t>(i)];
        if (child >= 0 && TrackAt(child).open)
        {
            UpdateSeq(child, force, child_scale);
        }
    }
}

void Performer::UpdateTrack(int t, float pan_scale)
{
    // JASTrack::updateTrack: the track's volume squared and its bend (bend sense / 3 octaves at full range, with 1/3 at
    // 0x8053b270) combine with its parent's settings. The sound table's volume comes in at the first track.
    if (sound_.jaudio1)
    {
        UpdateTrack1(t);
        return;
    }

    Track& tr = TrackAt(t);
    const float volume = tr.move[0].value * tr.move[0].value;
    const float bend = tr.move[1].value * static_cast<float>(tr.bend_sense) * (1.0f / 3.0f);
    const float pan = (tr.move[3].value - 0.5f) * tr.pan_power * pan_scale;
    ChannelParams p;
    if (tr.parent < 0)
    {
        p.volume = volume * SoundVolume(tr);
        p.pitch = 1.0f;
        p.pan = pan;
        p.fx_mix = tr.move[2].value;
        p.bend = bend;
    }
    else
    {
        const ChannelParams& up = TrackAt(tr.parent).params;
        p.volume = up.volume * volume * tr.handle_volume;
        p.pitch = up.pitch;
        p.pan = (up.pan - 0.5f) + pan;
        p.fx_mix = up.fx_mix + tr.move[2].value;
        p.bend = up.bend + bend;
    }
    p.pan += 0.5f;
    tr.params = p;

    // updateTempo (0x80037804): the first track works out its tick rate. A child copies its parent's. setTempo and
    // setTimebase only store their values.
    tr.tick_rate = tr.parent < 0 ? TicksPerSubFrame() : TrackAt(tr.parent).tick_rate;
}

float Performer::TicksPerSubFrame() const
{
    return TickRate(tempo_, timebase_, tempo_rate_);
}

int Performer::TransposeTotal(int t) const
{
    int total = 0;
    for (; t >= 0; t = TrackAt(t).parent)
    {
        total += TrackAt(t).transpose;
    }

    return total;
}

} // namespace ntdlib::sequence_export::jaudio
