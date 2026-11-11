// SPDX-License-Identifier: MIT

// The tracks' reads of their sequences, their registers, the conditions, the timing, the ports and the outer
// parameters (see track.cpp).

#include "ntdlib/jac/track.h"

#include <cstddef>
#include <cstdint>

#include "jac/track_internal.h"
#include "ntdlib/jaudio1/calc.h"

namespace ntdlib::jac
{

namespace
{

uint16_t Extend8To16(uint16_t value)
{
    return (value & 0x80) != 0 ? static_cast<uint16_t>(value + 0xff00) : value;
}

} // namespace

// jammain_2.

uint8_t Tracks::ReadByte(Track& track)
{
    return track.sequence->Read8(track.cursor);
}

uint16_t Tracks::Read16(Track& track)
{
    return track.sequence->Read16(track.cursor);
}

uint32_t Tracks::Read24(Track& track)
{
    return track.sequence->Read24(track.cursor);
}

uint16_t Tracks::Register(const Track& track, uint32_t reg)
{
    if (reg < track.registers.size())
    {
        return track.registers[reg];
    }

    // Past the registers, the game reads the pointers of the track's outer parameters (registers 32 to 35) and of its
    // children's (36 to 99), then the ports. A null pointer reads as 0.
    if (reg < 36)
    {
        if (track.outer != Track::kNone)
        {
            unmodelled_++;
        }
        return 0;
    }
    if (reg < 100)
    {
        if (track.child_outer[(reg - 36) / 4] != Track::kNone)
        {
            unmodelled_++;
        }
        return 0;
    }

    unmodelled_++;

    return 0;
}

void Tracks::SetRegister(Track& track, uint32_t reg, uint16_t value)
{
    if (reg < track.registers.size())
    {
        track.registers[reg] = value;
        return;
    }

    unmodelled_++;
}

bool Tracks::Condition(uint32_t index, uint8_t condition)
{
    // __ConditionCheck on register 3.
    return jaudio1::ConditionHolds(ReadRegDirect(index, kRegValue), condition);
}

int32_t Tracks::SeqTimeToDspTime(uint32_t index, int32_t time, uint8_t percent) const
{
    // Jam_SEQtimeToDSPtime: a note's time in ticks, times its gate in percent, in frames.
    const Track& track = tracks_[index];
    float frames = static_cast<float>(time) * static_cast<float>(percent) / 100.0f;
    if (track.time_relate == 1)
    {
        frames = frames / track.tempo_rate;
    }
    else
    {
        frames = frames * 120.0f / static_cast<float>(track.timebase);
    }

    return ToInt32(frames);
}

void Tracks::WriteTimeParam(uint32_t index, uint8_t command)
{
    // Jam_WriteTimeParam (commands 0x90 to 0x9f, 0x26ae90 in the release's main): a timed parameter's target, from a
    // register, a byte, a byte in the high half or a word, and its time in ticks, none, from a register, a byte or a
    // word. The release's code fills the low half of a byte below 0x80 with twice the byte: 0x7f gives 0x7ffe. It
    // divides a target by 32,767.
    Track& track = tracks_[index];
    const uint8_t param = ReadByte(track);
    int16_t target = 0;
    switch (command & 0x0c)
    {
    case 0x00:
        target = static_cast<int16_t>(Register(track, ReadByte(track)));
        break;

    case 0x04:
        target = static_cast<int16_t>(ReadByte(track));
        break;

    case 0x08:
        {
            const uint8_t high = ReadByte(track);
            target = static_cast<int16_t>(high << 8 | (high < 0x80 ? high << 1 : 0));
        }
        break;

    default:
        target = static_cast<int16_t>(Read16(track));
        break;
    }

    int32_t time = 0;
    switch (command & 0x03)
    {
    case 0:
        time = -1;
        break;
    case 1:
        time = Register(track, ReadByte(track));
        break;
    case 2:
        time = ReadByte(track);
        break;
    default:
        time = Read16(track);
        break;
    }

    if (param >= track.timed.size())
    {
        unmodelled_++;
        return;
    }

    MoveParam& move = track.timed[param];
    move.target = static_cast<float>(target) / kShortMax;
    if (time >= 0)
    {
        move.time = static_cast<float>(time);
    }
    if (move.time <= 0.0f)
    {
        TakeTarget(move);
    }
    else
    {
        move.step = (move.target - move.value) / move.time;
    }
}

void Tracks::WriteRegDirect(uint32_t index, uint8_t reg, uint16_t value)
{
    // Jam_WriteRegDirect: registers 0 to 2 hold a byte, and register 3 takes its sign extension. Registers 32 and 33
    // take nothing, and register 34 splits its value into registers 0 and 1. Register 3 takes every value.
    Track& track = tracks_[index];
    uint16_t result = value;
    switch (reg)
    {
    case 0:
    case 1:
    case 2:
        value &= 0xff;
        result = Extend8To16(value);
        break;

    case 32:
    case 33:
        return;

    case 34:
        WriteRegDirect(index, 0, static_cast<uint16_t>(value >> 8));
        result = value;
        value &= 0xff;
        reg = 1;
        break;

    default:
        break;
    }
    SetRegister(track, reg, value);
    track.registers[kRegValue] = result;
}

uint32_t Tracks::LoadTable(uint32_t index, uint32_t offset, uint32_t entry, uint32_t type)
{
    // LoadTbl: entry `entry` of a table of bytes, words, 24-bit words or 32-bit words. Type 8 reads a 32-bit word at
    // the entry's number as a byte offset.
    const Track& track = tracks_[index];
    switch (type)
    {
    case 4:
        return track.sequence->U8(offset + entry);

    case 5:
        return track.sequence->U16(offset + entry * 2);

    case 6:
        return track.sequence->U24(offset + entry * 3);

    case 7:
        return track.sequence->U32(offset + entry * 4);

    case 8:
        return track.sequence->U32(offset + entry);

    default:
        unmodelled_++;
        return 0;
    }
}

void Tracks::WriteRegParam(uint32_t index, uint8_t command)
{
    // Jam_WriteRegParam (commands 0xa0 to 0xaf): an operation on a register. The low nibble of the command gives the
    // operation and the source's kind. 0xa9 and 0xaa read a second byte with the operation, and 0xaa loads from a
    // table. Each test reads the command byte as the one before it left it.
    RegOperation op = ReadRegOperand(index, command);
    if (!ApplyRegOperation(index, op))
    {
        return;
    }
    StoreRegResult(index, op);
}

Tracks::RegOperation Tracks::ReadRegOperand(uint32_t index, uint8_t command)
{
    Track& track = tracks_[index];
    RegOperation op;
    op.source = command & 0x0c;
    op.operation = command & 0x03;
    if ((command & 0x0f) == 0x0b)
    {
        op.source = 0;
        op.operation = 0x0b;
    }
    if ((command & 0x0f) == 0x0a)
    {
        command = ReadByte(track);
        op.source = command & 0x0c;
        op.operation = 0x0a;
        op.type = static_cast<uint32_t>(command >> 4) + 4;
    }
    if ((command & 0x0f) == 0x09)
    {
        command = ReadByte(track);
        op.source = command & 0x0c;
        op.operation = command & 0xf0;
        if (op.source == 8)
        {
            op.source = 0x10;
        }
    }

    op.reg = ReadByte(track);
    if (op.operation == 0x0a)
    {
        op.wide = ReadReg32(index, ReadByte(track));
        op.wide_set = true;
    }

    switch (op.source)
    {
    case 0:
        op.value = static_cast<int16_t>(ReadRegDirect(index, ReadByte(track)));
        break;
    case 4:
        op.value = static_cast<int16_t>(ReadByte(track));
        break;
    case 8:
        op.value = static_cast<int16_t>(ReadByte(track) << 8);
        break;
    case 12:
        op.value = static_cast<int16_t>(Read16(track));
        break;
    default:
        op.value = -1;
        break;
    }

    return op;
}

bool Tracks::ApplyRegOperation(uint32_t index, RegOperation& op)
{
    Track& track = tracks_[index];
    const auto old = static_cast<int16_t>(ReadRegDirect(index, op.reg));
    const auto shift = [](int32_t amount)
    {
        return static_cast<uint32_t>(amount) & 31;
    };
    switch (op.operation)
    {
    case 0x00:
        break;

    case 0x01:
        if (op.source == 4)
        {
            op.value = static_cast<int16_t>(Extend8To16(static_cast<uint16_t>(op.value)));
        }
        op.value = static_cast<int16_t>(old + op.value);
        break;

    case 0x02:
        // The product goes to registers 4 and 5 (Jam_WriteRegXY).
        op.wide = static_cast<uint32_t>(static_cast<int32_t>(old) * static_cast<int32_t>(op.value));
        WriteRegDirect(index, kRegX, static_cast<uint16_t>(op.wide >> 16));
        WriteRegDirect(index, kRegY, static_cast<uint16_t>(op.wide));
        return false;

    case 0x03:
        track.registers[kRegValue] = static_cast<uint16_t>(old - op.value);
        return false;

    case 0x0b:
        op.value = static_cast<int16_t>(old - op.value);
        break;

    case 0x10:
        if (op.source == 4)
        {
            op.value = static_cast<int16_t>(Extend8To16(static_cast<uint16_t>(op.value)));
        }
        op.value = op.value < 0
                       ? static_cast<int16_t>(static_cast<uint32_t>(static_cast<uint16_t>(old)) >> shift(-op.value))
                       : static_cast<int16_t>(static_cast<uint32_t>(static_cast<uint16_t>(old)) << shift(op.value));
        break;

    case 0x20:
        if (op.source == 4)
        {
            op.value = static_cast<int16_t>(Extend8To16(static_cast<uint16_t>(op.value)));
        }
        op.value = op.value < 0
                       ? static_cast<int16_t>(static_cast<int32_t>(old) >> shift(-op.value))
                       : static_cast<int16_t>(static_cast<uint32_t>(static_cast<int32_t>(old)) << shift(op.value));
        break;

    case 0x30:
        op.value = static_cast<int16_t>(old & op.value);
        break;

    case 0x40:
        op.value = static_cast<int16_t>(old | op.value);
        break;

    case 0x50:
        op.value = static_cast<int16_t>(old ^ op.value);
        break;

    case 0x60:
        op.value = static_cast<int16_t>(-old);
        break;

    case 0x90:
        {
            // GetRandom_s32 modulo the source's value. The build's division by 0 gives 0, and the remainder is then the
            // draw.
            op.wide = static_cast<uint32_t>(random_.S32());
            op.wide_set = true;
            const uint32_t divisor = static_cast<uint16_t>(op.value);
            op.value = static_cast<int16_t>(divisor == 0 ? op.wide : op.wide % divisor);
            break;
        }

    case 0x0a:
        // The entry's number is the signed source value.
        op.wide = LoadTable(index, op.wide, static_cast<uint32_t>(static_cast<int32_t>(op.value)), op.type);
        op.value = static_cast<int16_t>(op.wide);
        break;

    default:
        break;
    }

    return true;
}

void Tracks::StoreRegResult(uint32_t index, const RegOperation& op)
{
    Track& track = tracks_[index];
    uint8_t reg = op.reg;
    int16_t value = op.value;

    // Register 3 takes the result. The release's code gives it 0 for a half of the bank's or the priority's register
    // (0x26bffc and 0x26c050 in its main).
    uint16_t result = static_cast<uint16_t>(value);
    switch (reg)
    {
    case 0:
    case 1:
    case 2:
        value = static_cast<int16_t>(value & 0xff);
        result = Extend8To16(static_cast<uint16_t>(value));
        break;

    case 0x21:
        reg = kRegBank;
        value = static_cast<int16_t>((track.registers[kRegBank] & 0xff00) | (value & 0x00ff));
        result = 0;
        break;

    case 0x20:
        reg = kRegBank;
        value = static_cast<int16_t>((track.registers[kRegBank] & 0x00ff) | (value << 8));
        result = 0;
        break;

    case 0x2e:
        reg = kRegPriority;
        value = static_cast<int16_t>((track.registers[kRegPriority] & 0xff00) | (value & 0x00ff));
        result = 0;
        break;

    case 0x2f:
        reg = kRegPriority;
        value = static_cast<int16_t>((track.registers[kRegPriority] & 0x00ff) | (value << 8));
        result = 0;
        break;

    case 0x22:
        WriteRegDirect(index, 0, static_cast<uint16_t>(static_cast<uint16_t>(value) >> 8));
        value = static_cast<int16_t>(value & 0xff);
        result = static_cast<uint16_t>(value);
        reg = 1;
        break;

    case 0x28:
    case 0x29:
    case 0x2a:
    case 0x2b:
        {
            // A 32-bit register takes the draw or the table's word. After the other operations, the game writes
            // whatever its register held.
            if (!op.wide_set)
            {
                unmodelled_++;
                return;
            }
            const uint32_t at = kRegLong + 2 * (reg - 0x28u);
            track.registers[at] = static_cast<uint16_t>(op.wide >> 16);
            track.registers[at + 1] = static_cast<uint16_t>(op.wide);
            return;
        }

    default:
        break;
    }

    SetRegister(track, reg, static_cast<uint16_t>(value));
    track.registers[kRegValue] = result;
    if (reg == kRegBank)
    {
        ClearOverwrite(track);
    }
    if (reg == kRegBendRange)
    {
        track.update_flags |= kUpdatePitch;
    }
    if (reg == kRegPriority)
    {
        ChannelManager& manager = channels_.Manager(index);
        manager.priority = track.registers[kRegPriority] | 0x10000u;
        manager.release_time = 0;
    }
}

uint16_t Tracks::ReadRegDirect(uint32_t index, uint8_t reg)
{
    // Jam_ReadRegDirect: 0x20 the bank, 0x21 the program, 0x22 registers 0 and 1 as one word, 0x2c the children that
    // play, 0x2d the notes that have stopped, 0x30 the count of the current loop. The result is a 16-bit signed value,
    // and the bank's shift extends its sign.
    const Track& track = tracks_[index];
    int16_t result = 0;
    switch (reg)
    {
    case 0x20:
    case 0x21:
        result = static_cast<int16_t>(track.registers[kRegBank]);
        break;

    case 0x22:
        {
            const uint16_t low = ReadRegDirect(index, 1);
            const uint16_t high = ReadRegDirect(index, 0);
            result = static_cast<int16_t>(high << 8 | low);
            break;
        }

    case 0x2c:
        {
            uint32_t mask = 0;
            for (std::size_t i = 16; i-- > 0;)
            {
                const uint32_t child = track.children[i];
                mask = mask << 1 | (child != Track::kNone && tracks_[child].state != Track::kClosed ? 1 : 0);
            }
            result = static_cast<int16_t>(mask);
            break;
        }

    case 0x2d:
        {
            uint32_t mask = 0;
            for (uint8_t i = 8; i-- > 0;)
            {
                mask = mask << 1 | (CheckNoteStop(index, i) ? 1 : 0);
            }
            result = static_cast<int16_t>(mask);
            break;
        }

    case 0x30:
        result = track.depth == 0 ? 0 : static_cast<int16_t>(track.loop_counts[track.depth - 1]);
        break;

    default:
        result = static_cast<int16_t>(Register(track, reg));
        break;
    }

    switch (reg)
    {
    case 0x00:
    case 0x01:
    case 0x02:
    case 0x21:
        result = static_cast<int16_t>(result & 0xff);
        break;
    case 0x20:
        result = static_cast<int16_t>(result >> 8);
        break;
    default:
        break;
    }

    return static_cast<uint16_t>(result);
}

uint32_t Tracks::ReadReg32(uint32_t index, uint8_t reg)
{
    // Jam_ReadReg32: 40 to 43 the 32-bit registers, 35 registers 4 and 5 as one word.
    const Track& track = tracks_[index];
    if (reg >= 40 && reg <= 43)
    {
        const uint32_t at = kRegLong + 2 * (reg - 40u);
        return static_cast<uint32_t>(track.registers[at]) << 16 | track.registers[at + 1];
    }
    if (reg == 35)
    {
        return static_cast<uint32_t>(ReadRegDirect(index, kRegX)) << 16 | ReadRegDirect(index, kRegY);
    }

    return ReadRegDirect(index, reg);
}

uint32_t Tracks::ExchangeRegisterValue(uint32_t index, uint8_t reg)
{
    // __ExchangeRegisterValue: registers below 64, the ports' values from 64.
    if (reg < 64)
    {
        return ReadReg32(index, reg);
    }

    const uint32_t port = reg - 64u;
    if (port < 16)
    {
        return tracks_[index].ports[port].value;
    }

    unmodelled_++;

    return 0;
}

void Tracks::SetInterrupt(uint32_t index, uint16_t interrupt)
{
    Track& track = tracks_[index];
    const uint32_t bit = 1u << (interrupt & 31);
    if ((track.enabled & bit) != 0)
    {
        track.requests = static_cast<uint8_t>(track.requests | bit);
    }
}

bool Tracks::TryInterrupt(uint32_t index)
{
    // Jam_TryInterrupt: the lowest interrupt that is enabled and requested, unless a handler runs.
    Track& track = tracks_[index];
    if (track.interrupted != 0)
    {
        return false;
    }

    for (uint32_t i = 0; i < 8; i++)
    {
        const uint32_t bit = 1u << i;
        if ((track.enabled & bit) != 0 && (track.requests & bit) != 0)
        {
            track.saved_cursor = track.cursor;
            track.cursor = track.interrupt_targets[i];
            track.interrupted = static_cast<uint8_t>(bit);
            track.saved_wait = track.wait;
            track.wait = 0;
            track.requests = static_cast<uint8_t>(track.requests ^ bit);
            return true;
        }
    }

    return false;
}

bool Tracks::WritePortAppDirect(uint32_t index, uint32_t port, uint16_t value)
{
    if (index == Track::kNone)
    {
        return false;
    }
    if (port >= 16)
    {
        unmodelled_++;
        return true;
    }

    Track& track = tracks_[index];
    track.ports[port].value = value;
    track.ports[port].import_flag = 1;
    if (port == 0)
    {
        SetInterrupt(index, 3);
    }
    if (port == 1)
    {
        SetInterrupt(index, 4);
    }

    return true;
}

bool Tracks::ReadPortAppDirect(uint32_t index, uint32_t port, uint16_t& value)
{
    if (index == Track::kNone)
    {
        return false;
    }
    if (port >= 16)
    {
        unmodelled_++;
        return true;
    }

    Track& track = tracks_[index];
    track.ports[port].export_flag = 0;
    value = track.ports[port].value;

    return true;
}

bool Tracks::CheckPortAppDirect(uint32_t index, uint32_t port, uint16_t value)
{
    // Condition 0: whether the sequence has written the port. Condition 1: whether the game's last write has been
    // read.
    if (index == Track::kNone || port >= 16)
    {
        unmodelled_++;
        return false;
    }

    const Port& p = tracks_[index].ports[port];
    switch (value & 0xff)
    {
    case 0:
        return p.export_flag != 0;
    case 1:
        return p.import_flag != 1;
    default:
        return false;
    }
}

void Tracks::InitRegistTrack()
{
    registrations_ = 0;
    for (Registration& entry : registered_)
    {
        entry.track = Track::kNone;
    }
}

void Tracks::RegistTrack(uint32_t index, uint32_t id)
{
    // Jam_RegistTrack: an ID that the list holds keeps its track. A full list takes the first free entry.
    for (uint32_t i = 0; i < registrations_; i++)
    {
        if (registered_[i].id == id)
        {
            return;
        }
    }
    uint32_t at = registrations_;
    if (registrations_ == kRegistered)
    {
        at = 0;
        while (at < kRegistered && registered_[at].track != Track::kNone)
        {
            at++;
        }
        if (at == kRegistered)
        {
            return;
        }
    }
    else
    {
        registrations_++;
    }
    registered_[at].id = id;
    registered_[at].track = index;
    tracks_[index].registered = true;
}

void Tracks::UnRegistTrack(uint32_t index)
{
    Track& track = tracks_[index];
    if (!track.registered)
    {
        return;
    }

    for (uint32_t i = 0; i < registrations_; i++)
    {
        if (registered_[i].track == index)
        {
            registered_[i].track = Track::kNone;
        }
    }
    track.registered = false;
    while (registrations_ != 0 && registered_[registrations_ - 1].track == Track::kNone)
    {
        registrations_--;
    }
}

uint32_t Tracks::TrackHandle(uint32_t id) const
{
    for (uint32_t i = 0; i < registrations_; i++)
    {
        if (registered_[i].id == id && registered_[i].track != Track::kNone)
        {
            return registered_[i].track;
        }
    }

    return Track::kNone;
}

void Tracks::InitExtBuffer(uint32_t outer)
{
    OuterParam& param = outer_[outer];
    param.switches = 0;
    param.updates = 0;
    param.assigned = false;
    param.references = 0;
}

bool Tracks::AssignExtBuffer(uint32_t index, uint32_t outer)
{
    if (index == Track::kNone || outer == Track::kNone)
    {
        return false;
    }

    tracks_[index].outer = outer;
    outer_[outer].references++;

    return true;
}

uint32_t Tracks::AddOuter()
{
    outer_.emplace_back();
    const auto index = static_cast<uint32_t>(outer_.size() - 1);
    InitExtBuffer(index);

    return index;
}

bool Tracks::AssignExtBufferP(uint32_t index, uint8_t child, uint32_t outer)
{
    if (index == Track::kNone || outer == Track::kNone)
    {
        return false;
    }
    if (child >= 16)
    {
        unmodelled_++;
        return true;
    }

    Track& track = tracks_[index];
    track.child_outer[child] = outer;
    outer_[outer].assigned = true;
    AssignExtBuffer(track.children[child], outer);

    return true;
}

void Tracks::SetExtParamD(uint32_t outer, uint8_t flag, float value)
{
    if (outer == Track::kNone)
    {
        return;
    }

    OuterParam& param = outer_[outer];
    switch (flag)
    {
    case OuterParam::kVolume:
        param.volume = value;
        break;
    case OuterParam::kPitch:
        param.pitch = value;
        break;
    case OuterParam::kFxmix:
        param.fxmix = value;
        break;
    case OuterParam::kDolby:
        param.dolby = value;
        break;
    case OuterParam::kPan:
        param.pan = value;
        break;
    case OuterParam::kTempo:
        param.tempo = value;
        break;
    default:
        return;
    }
    param.updates = static_cast<uint16_t>(param.updates | flag);
}

void Tracks::SetExtParam(uint32_t index, uint8_t flag, float value)
{
    if (index == Track::kNone)
    {
        return;
    }

    SetExtParamD(tracks_[index].outer, flag, value);
}

void Tracks::OnExtSwitch(uint32_t index, uint16_t switches)
{
    if (index == Track::kNone)
    {
        return;
    }

    OnExtSwitchD(tracks_[index].outer, switches);
}

void Tracks::OffExtSwitch(uint32_t index, uint16_t switches)
{
    if (index == Track::kNone)
    {
        return;
    }

    OffExtSwitchD(tracks_[index].outer, switches);
}

void Tracks::SetExtParamP(uint32_t index, uint8_t child, uint8_t flag, float value)
{
    if (index == Track::kNone)
    {
        return;
    }
    if (child >= 16)
    {
        unmodelled_++;
        return;
    }

    SetExtParamD(tracks_[index].child_outer[child], flag, value);
}

void Tracks::OnExtSwitchP(uint32_t index, uint8_t child, uint16_t switches)
{
    if (index == Track::kNone)
    {
        return;
    }
    if (child >= 16)
    {
        unmodelled_++;
        return;
    }

    OnExtSwitchD(tracks_[index].child_outer[child], switches);
}

void Tracks::OnExtSwitchD(uint32_t outer, uint16_t switches)
{
    if (outer == Track::kNone)
    {
        return;
    }

    OuterParam& param = outer_[outer];
    param.switches = static_cast<uint16_t>(param.switches | switches);
    param.updates = static_cast<uint16_t>(param.updates | switches);
}

void Tracks::OffExtSwitchD(uint32_t outer, uint16_t switches)
{
    if (outer == Track::kNone)
    {
        return;
    }

    OuterParam& param = outer_[outer];
    param.switches = static_cast<uint16_t>(param.switches ^ (param.switches & switches));
    param.updates = static_cast<uint16_t>(param.updates | switches);
}

uint32_t Tracks::SequenceOuter(const Sequence* sequence, uint32_t offset)
{
    // Cmd_EXTSet's buffer lies in the sequence's data. The game reads its levels from the data's bytes until the code
    // above the sequences sets them. The model starts them at 0.
    const auto [entry, added] = sequence_outer_.try_emplace({sequence, offset}, 0);
    if (added)
    {
        outer_.emplace_back();
        entry->second = static_cast<uint32_t>(outer_.size() - 1);
    }

    return entry->second;
}

} // namespace ntdlib::jac
