// SPDX-License-Identifier: MIT

// The tracks' reads of their sequences, their registers (JASTrack's readReg and writeRegParam), the random numbers,
// the conditions and the timing (see track.cpp).

#include "ntdlib/jaudio1/track.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

#include "jaudio1/track_internal.h"
#include "ntdlib/jaudio1/calc.h"

namespace ntdlib::jaudio1
{

namespace
{

// JASPlayer::extend8to16: a byte's sign extended to 16 bits.
uint32_t Extend8To16(uint32_t value)
{
    const uint32_t byte = value & 0xff;
    return (byte & 0x80) != 0 ? (byte | 0xff00) : byte;
}

// Whether JASTrack::readReg16 reads register `reg` from the registers or from the track's state, and not from the
// memory after the registers.
bool ReadableRegister16(uint32_t reg)
{
    return reg < Track::kRegisters || reg == 0x20 || reg == 0x21 || reg == 0x22 || reg == 0x2c || reg == 0x2d ||
           reg == 0x30;
}

// The 16-bit value `value` shifted by `shift`, a 16-bit signed count: left for a positive count, right otherwise.
// Operation 0x10 shifts the bits as they are, and operation 0x20 shifts the value's sign in from the left.
uint32_t ShiftedLogical(uint32_t value, uint32_t shift)
{
    const int32_t count = static_cast<int16_t>(shift);
    const uint32_t base = value & 0xffff;
    return count < 0 ? base >> ((0u - static_cast<uint32_t>(count)) & 31) : base << (static_cast<uint32_t>(count) & 31);
}

uint32_t ShiftedArithmetic(uint32_t value, uint32_t shift)
{
    const int32_t count = static_cast<int16_t>(shift);
    const int32_t base = static_cast<int16_t>(value);
    return count < 0 ? static_cast<uint32_t>(base >> ((0u - static_cast<uint32_t>(count)) & 31))
                     : static_cast<uint32_t>(base) << (static_cast<uint32_t>(count) & 31);
}

// Whether the result of writeRegParam's operation depends on the target's value.
bool OperationReadsTarget(uint32_t operation)
{
    switch (operation)
    {
    case 1:
    case 2:
    case 3:
    case 0xb:
    case 0x10:
    case 0x20:
    case 0x30:
    case 0x40:
    case 0x50:
    case 0x60:
        return true;
    default:
        return false;
    }
}

} // namespace

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

    // JASTrack's words after the registers on AArch64, low word first: the two oscillators' settings
    // (JASOscillator::Data, 16 words each: the target, the rate, the addresses of the two tables, the width and the
    // vertex), their routes, then the points of a simple ADSR. A table's address is 0 while the table is null.
    // Otherwise it depends on where the game's memory holds the table.
    const auto word = static_cast<uint32_t>(reg - track.registers.size());
    const auto half = [](uint32_t value, uint32_t at)
    {
        return static_cast<uint16_t>(at % 2 == 0 ? value : value >> 16);
    };
    if (word < 32)
    {
        const OscData& data = track.oscillators[word / 16];
        const uint32_t at = word % 16;
        if (at < 2)
        {
            return half(data.target, at);
        }
        if (at < 4)
        {
            return half(std::bit_cast<uint32_t>(data.rate), at);
        }
        if (at >= 12)
        {
            return half(std::bit_cast<uint32_t>(at < 14 ? data.width : data.vertex), at);
        }
        if ((at < 8 ? data.attack : data.release).Null())
        {
            return 0;
        }
    }
    else if (word < 36)
    {
        return half(track.osc_routes[(word - 32) / 2], word);
    }
    else if (word < 48)
    {
        const OscPoint& point = track.adsr[(word - 36) / 3];
        const std::array<int16_t, 3> values = {point.mode, point.time, point.value};
        return static_cast<uint16_t>(values[(word - 36) % 3]);
    }

    unmodelled_++;
    return 0;
}

void Tracks::SetRegister(Track& track, uint32_t reg, uint32_t value)
{
    if (reg < track.registers.size())
    {
        track.registers[reg] = static_cast<uint16_t>(value);
        return;
    }
    unmodelled_++;
}

uint32_t Tracks::ReadReg16(uint32_t index, uint32_t reg)
{
    // JASTrack::readReg16 (0x3debb8).
    const Track& track = tracks_[index];
    switch (reg & 0xff)
    {
    case 0x20:
        return track.registers[kRegBankProgram] >> 8;

    case 0x21:
        return track.registers[kRegBankProgram] & 0xff;

    case 0x22:
        return static_cast<uint32_t>(track.registers[0]) << 8 | track.registers[1];

    case 0x2c:
        {
            uint32_t result = 0;
            for (std::size_t slot = 16; slot-- > 0;)
            {
                const uint32_t child = track.children[slot];
                result = result << 1 | (child != Track::kNone && tracks_[child].status != Track::kClosed ? 1 : 0);
            }
            return result;
        }

    case 0x2d:
        {
            uint32_t result = 0;
            for (std::size_t note = 8; note-- > 0;)
            {
                const uint32_t channel = track.channels[note];
                result = result << 1 |
                         (channel == Track::kNone || voices_.ChannelAt(channel).status == Channel::kStop ? 1 : 0);
            }
            return result;
        }

    case 0x30:
        return track.depth == 0 ? 0 : track.loop_counts[track.depth - 1];

    default:
        return Register(track, reg & 0xff);
    }
}

uint32_t Tracks::ReadReg32(uint32_t index, uint32_t reg)
{
    // JASTrack::readReg32 (0x3deb60).
    reg &= 0xff;
    const Track& track = tracks_[index];
    if (reg >= 0x28 && reg <= 0x2b)
    {
        const uint32_t at = kRegLong + 2 * (reg - 0x28);
        return static_cast<uint32_t>(track.registers[at]) << 16 | track.registers[at + 1];
    }
    if (reg == 0x23)
    {
        return static_cast<uint32_t>(track.registers[kRegProduct]) << 16 | track.registers[kRegProduct + 1];
    }

    return ReadReg16(index, reg) & 0xffff;
}

uint32_t Tracks::ExchangeRegisterValue(uint32_t index, uint32_t reg)
{
    // JASTrack::exchangeRegisterValue (0x3deaec): registers below 0x40, ports from 0x40.
    reg &= 0xff;
    if (reg < 0x40)
    {
        return ReadReg32(index, reg);
    }

    const uint32_t port = reg - 0x40;
    if (port < 16)
    {
        return tracks_[index].ports[port];
    }

    // A higher number reads past the ports, into the fields that follow them in JASTrack. Registers 0x58 to 0x77 read
    // JASIntrMgr's targets, 4 words each, low word first: 0 for a target that the track hasn't set.
    constexpr uint32_t kTargetWords = 24;
    if (port >= kTargetWords && port < kTargetWords + 4 * 8 &&
        (tracks_[index].targets_set & (1u << ((port - kTargetWords) / 4))) == 0)
    {
        return 0;
    }
    unmodelled_++;

    return 0;
}

void Tracks::WriteRegDirect(Track& track, uint32_t reg, uint32_t value)
{
    // JASTrack::writeRegDirect (0x3def8c). Register 3 takes the value too, or the byte's sign extension for registers
    // 0 to 2.
    reg &= 0xff;
    value &= 0xffff;
    if (reg < 3)
    {
        SetRegister(track, reg, value & 0xff);
        SetRegister(track, kRegResult, Extend8To16(value));
        return;
    }
    if (reg == 0x20 || reg == 0x21)
    {
        return;
    }
    if (reg == 0x22)
    {
        SetRegister(track, 0, value >> 8);
        SetRegister(track, kRegResult, Extend8To16(value >> 8));
        SetRegister(track, 1, value & 0xff);
        SetRegister(track, kRegResult, value);
        return;
    }
    SetRegister(track, reg, value);
    SetRegister(track, kRegResult, value);
}

void Tracks::WriteRegParam(uint32_t index, uint32_t command)
{
    // JASTrack::writeRegParam (0x3df01c): an operation on a register. The command's low bits, or a byte after it,
    // give the operation and the kind of operand.
    RegOperation op = ReadRegOperand(index, command);
    if (!ApplyRegOperation(index, op))
    {
        return;
    }
    StoreRegResult(index, op);
}

Tracks::RegOperation Tracks::ReadRegOperand(uint32_t index, uint32_t command)
{
    Track& track = tracks_[index];
    RegOperation op;
    op.operation = command & 0xf;
    if (op.operation == 9)
    {
        const uint32_t byte = ReadByte(track);
        op.operand_kind = (byte & 0xc) == 8 ? 0x10 : byte & 0xc;
        op.operation = byte & 0xf0;
    }
    else if (op.operation == 0xb)
    {
        op.operand_kind = 0;
    }
    else if (op.operation == 0xa)
    {
        const uint32_t byte = ReadByte(track);
        op.operand_kind = byte & 0xc;
        op.table_kind = (byte >> 4) + 4;
    }
    else
    {
        op.operand_kind = command & 0xc;
        op.operation = command & 3;
    }

    op.target = ReadByte(track);
    if (op.operation == 0xa)
    {
        op.table = ReadReg32(index, ReadByte(track));
    }

    switch (op.operand_kind >> 2)
    {
    case 0:
        op.operand = ReadReg16(index, ReadByte(track));
        break;

    case 1:
        op.operand = ReadByte(track);
        break;

    case 2:
        {
            const uint32_t byte = ReadByte(track);
            op.operand = byte << 8 | ((byte & 0x80) == 0 ? byte << 1 : 0);
            break;
        }

    case 3:
        op.operand = Read16(track);
        break;

    default:
        op.operand = 0xffff;
        break;
    }

    return op;
}

bool Tracks::ApplyRegOperation(uint32_t index, RegOperation& op)
{
    // The game reads the target whatever the operation. A target past the registers reads the memory after them, and
    // a stored value that depends on it is unmodelled. `last` is the value that the game's code holds in its first
    // register when it reaches the store: the 32-bit value that a 32-bit register takes.
    Track& track = tracks_[index];
    const bool known = ReadableRegister16(op.target);
    const uint32_t value = known ? ReadReg16(index, op.target) : 0;
    const bool result_reads_target = OperationReadsTarget(op.operation);
    if (!known && (op.operation == 2 || op.operation == 3))
    {
        unmodelled_++;
    }
    uint32_t last = value;
    bool last_reads_target = true;
    uint32_t result = op.operand;
    // A byte operand (kind 4) is sign-extended. The game's code then holds the extended operand.
    const auto extend_byte = [&]
    {
        if (op.operand_kind == 4)
        {
            result = Extend8To16(result);
            last = result;
            last_reads_target = false;
        }
    };
    switch (op.operation)
    {
    case 0:
        break;

    case 1:
        extend_byte();
        result = result + value;
        break;

    case 2:
        {
            const uint32_t product =
                static_cast<uint32_t>(static_cast<int16_t>(value) * static_cast<int16_t>(op.operand));
            SetRegister(track, kRegProduct, product >> 16);
            SetRegister(track, kRegProduct + 1, product);
            SetRegister(track, kRegResult, product);
            return false;
        }

    case 3:
        SetRegister(track, kRegResult, value - op.operand);
        return false;

    case 0xa:
        result = RegTableEntry(track, op);
        last = result;
        last_reads_target = false;
        break;

    case 0xb:
        result = value - op.operand;
        break;

    case 0x10:
        extend_byte();
        result = ShiftedLogical(value, result);
        break;

    case 0x20:
        extend_byte();
        result = ShiftedArithmetic(value, result);
        break;

    case 0x30:
        result = value & result;
        break;

    case 0x40:
        result = value | result;
        break;

    case 0x50:
        result = value ^ result;
        break;

    case 0x60:
        result = 0u - value;
        break;

    case 0x90:
        {
            last = static_cast<uint32_t>(GetRandomS32());
            random_draws_++;
            last_reads_target = false;
            const uint32_t divisor = result & 0xffff;
            result = divisor == 0 ? last : last % divisor;
            break;
        }

    default:
        break;
    }

    const bool long_target = op.target >= 0x28 && op.target <= 0x2b;
    if (!known && (long_target ? last_reads_target : result_reads_target))
    {
        unmodelled_++;
    }

    op.result = result;
    op.last = last;
    return true;
}

uint32_t Tracks::RegTableEntry(const Track& track, const RegOperation& op) const
{
    const int32_t at = static_cast<int16_t>(op.operand);
    switch (op.table_kind)
    {
    case 4:
        return track.sequence->U8(op.table + static_cast<uint32_t>(at));
    case 5:
        return track.sequence->U16(op.table + static_cast<uint32_t>(at * 2));
    case 6:
        return track.sequence->U24(static_cast<uint32_t>(at * 3) + op.table);
    case 7:
        return track.sequence->U32(op.table + static_cast<uint32_t>(at * 4));
    case 8:
        return track.sequence->U32(op.table + static_cast<uint32_t>(at));
    default:
        return op.operand;
    }
}

void Tracks::StoreRegResult(uint32_t index, const RegOperation& op)
{
    Track& track = tracks_[index];
    switch (op.target)
    {
    case 0:
    case 1:
    case 2:
        SetRegister(track, op.target, op.result & 0xff);
        SetRegister(track, kRegResult, Extend8To16(op.result));
        return;

    case 0x20:
        SetRegister(track, kRegBankProgram, (op.result << 8) | (track.registers[kRegBankProgram] & 0xff));
        SetRegister(track, kRegResult, 0);
        track.osc_routes = {0xf, 0xf};
        return;

    case 0x21:
        SetRegister(track, kRegBankProgram, (track.registers[kRegBankProgram] & 0xff00) | (op.result & 0xff));
        SetRegister(track, kRegResult, 0);
        track.osc_routes = {0xf, 0xf};
        return;

    case 0x22:
        SetRegister(track, 0, (op.result >> 8) & 0xff);
        SetRegister(track, kRegResult, Extend8To16(op.result >> 8));
        SetRegister(track, 1, op.result & 0xff);
        SetRegister(track, kRegResult, op.result & 0xff);
        return;

    case 0x28:
    case 0x29:
    case 0x2a:
    case 0x2b:
        {
            const uint32_t at = kRegLong + 2 * (op.target - 0x28);
            track.registers[at] = static_cast<uint16_t>(op.last >> 16);
            track.registers[at + 1] = static_cast<uint16_t>(op.last);
            return;
        }

    case 0x2e:
        SetRegister(track, kRegPriority, (track.registers[kRegPriority] & 0xff00) | (op.result & 0xff));
        SetRegister(track, kRegResult, 0);
        return;

    case 0x2f:
        SetRegister(track, kRegPriority, (op.result << 8) | (track.registers[kRegPriority] & 0xff));
        SetRegister(track, kRegResult, 0);
        return;

    default:
        break;
    }

    SetRegister(track, op.target, op.result);
    SetRegister(track, kRegResult, op.result);
    if (op.target == kRegBendRange)
    {
        track.update_flags |= kUpdatePitch;
    }
    else if (op.target == kRegBankProgram)
    {
        track.osc_routes = {0xf, 0xf};
    }
}

int32_t Tracks::GetRandomS32()
{
    // JASPlayer::getRandomS32 (0x3e1430).
    const int32_t result = NextRandomS32(seq_random_[0], seq_random_[1]);
    seq_random_[0] = seq_random_[1];
    seq_random_[1] = result;

    return result;
}

bool Tracks::Condition(const Track& track, uint32_t flag) const
{
    // JASSeqParser::conditionCheck on register 3.
    return ConditionHolds(track.registers[kRegResult], flag);
}

int32_t Tracks::Transpose(const Track& track) const
{
    // JASTrack::getTranspose: the sum of the transposes up the tree.
    int32_t sum = track.transpose;
    for (uint32_t at = track.parent; at != Track::kNone; at = tracks_[at].parent)
    {
        sum += tracks_[at].transpose;
    }

    return sum;
}

int32_t Tracks::SeqTimeToDspTime(const Track& track, int32_t time, uint32_t percent) const
{
    // JASTrack::seqTimeToDspTime (0x3de2b4).
    float value = static_cast<float>(time) * static_cast<float>(percent & 0xff);
    value = value / 100.0f;
    if (track.time_relate)
    {
        return ToInt32(value / track.tempo_rate);
    }

    value = value * (120.0f / static_cast<float>(track.timebase));
    return ToInt32(value * (static_cast<float>(Voices::kSubframes) / 10.0f));
}

void Tracks::SetParam(Track& track, uint32_t param, float value, int32_t time)
{
    // JASTrack::setParam (0x3de338).
    if (param >= track.timed.size())
    {
        unmodelled_++;
        return;
    }
    MoveParam& move = track.timed[param];
    move.target = value;
    if (time <= 0)
    {
        move.value = value;
        move.time = 1.0f;
        move.amount = 0.0f;
        return;
    }
    move.time = static_cast<float>(time);
    move.amount = (value - move.value) / move.time;
}

void Tracks::SetTempo(uint32_t index, uint32_t tempo)
{
    // JASTrack::setTempo.
    Track& track = tracks_[index];
    track.tempo = static_cast<uint16_t>(tempo);
    if (track.parent == Track::kNone)
    {
        UpdateTempo(index);
    }
    else
    {
        track.tempo_child = true;
    }
}

void Tracks::SetTimebase(uint32_t index, uint32_t timebase)
{
    // JASTrack::setTimebase.
    Track& track = tracks_[index];
    track.timebase = static_cast<uint16_t>(timebase);
    if (track.parent == Track::kNone)
    {
        UpdateTempo(index);
    }
}

OscTable Tracks::SequenceTable(const Track& track, uint32_t offset) const
{
    const std::span<const OscPoint> points = track.sequence->Table(offset);

    return {points.data(), points.size()};
}

OuterParam* Tracks::SequenceOuter(const Sequence* sequence, uint32_t offset)
{
    return &sequence_outer_[{sequence, offset}];
}

} // namespace ntdlib::jaudio1
