// SPDX-License-Identifier: MIT

// The parser of the tracks' sequences: the calls, the commands and the main step of a track (see track.cpp).

#include "ntdlib/jac/track.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "jac/track_internal.h"
#include "ntdlib/jaudio1/iir_table.h"

namespace ntdlib::jac
{

namespace
{

// The bytes from osc_table (0x5bf2c4): the update flags of a track oscillator's targets, then Cmd_PanSwSet's two
// tables (0x5bf2c9 and 0x5bf2d0) and the byte after them. A target past the first table's 5 entries reads on into the
// next ones, as the game does.
constexpr std::array<uint8_t, 20> kOscFlags = {0x01, 0x02, 0x08, 0x04, 0x10, 0, 0, 0, 1, 1,
                                               2,    2,    0,    1,    2,    0, 2, 0, 2, 0};
constexpr std::size_t kCalcTypes = 5;
constexpr std::size_t kParentCalcTypes = 12;

// The command table's argument lists (Arglist, 0x5bf1c4): each command's count of arguments, and their types, two
// bits each from the low bits: 0 a byte, 1 a 16-bit word, 2 a 24-bit word, 3 a register's value.
struct ArgSpec
{
    uint8_t count = 0;
    uint16_t types = 0;
};

constexpr std::array<ArgSpec, 64> kArgs = {{
    {0, 0x000}, {2, 0x008}, {2, 0x008}, {1, 0x002}, {0, 0x000}, {0, 0x000}, {1, 0x000}, {1, 0x002},
    {0, 0x000}, {1, 0x001}, {0, 0x000}, {2, 0x000}, {2, 0x00c}, {1, 0x000}, {1, 0x000}, {1, 0x003},
    {2, 0x005}, {2, 0x00c}, {2, 0x00c}, {2, 0x00f}, {1, 0x000}, {1, 0x000}, {1, 0x000}, {2, 0x008},
    {5, 0x155}, {1, 0x000}, {1, 0x000}, {1, 0x000}, {1, 0x001}, {2, 0x004}, {1, 0x000}, {2, 0x008},
    {1, 0x000}, {0, 0x000}, {0, 0x000}, {0, 0x000}, {2, 0x004}, {0, 0x000}, {0, 0x000}, {1, 0x001},
    {0, 0x000}, {0, 0x000}, {1, 0x002}, {5, 0x000}, {4, 0x055}, {1, 0x002}, {1, 0x002}, {3, 0x000},
    {1, 0x000}, {1, 0x000}, {3, 0x028}, {0, 0x000}, {0, 0x000}, {0, 0x000}, {0, 0x000}, {0, 0x000},
    {0, 0x000}, {0, 0x000}, {1, 0x001}, {0, 0x000}, {0, 0x000}, {1, 0x001}, {1, 0x001}, {0, 0x000},
}};

// The results of a command for the parser: go on, end the tick, try the interrupts again, or end the track.
constexpr int32_t kContinue = 0;
constexpr int32_t kEndTick = 1;
constexpr int32_t kRetry = 2;
constexpr int32_t kEndTrack = 3;

// Two more results, both of them kContinue to the parser: a command with no parent track for it (OpenTrackBros on a
// root), and a command whose argument is out of range (a loop's end outside a loop, a child's slot past 15).
constexpr int32_t kNoParent = 0x40;
constexpr int32_t kOutOfRange = 0x80;

} // namespace

int32_t Tracks::CallF(uint32_t index)
{
    // Cmd_CallF and Cmd_JmpF: a condition in the low nibble of a flag byte, then the target: a 24-bit offset, a
    // register (bit 7), or an entry of a table of 24-bit offsets by a register (bit 6), the table at a 24-bit offset
    // or a register's (bit 5). Only the call pushes the return.
    Track& track = tracks_[index];
    const uint8_t flags = ReadByte(track);
    uint32_t target = 0;
    if ((flags & 0x80) != 0)
    {
        target = ReadRegDirect(index, ReadByte(track));
        if ((flags & 0x40) != 0)
        {
            uint32_t table = 0;
            if ((flags & 0x20) != 0)
            {
                table = ReadRegDirect(index, ReadByte(track));
            }
            else
            {
                table = Read24(track);
            }
            target = track.sequence->U24(table + target * 3);
        }
    }
    else
    {
        target = Read24(track);
    }

    if (Condition(index, flags & 0x0f))
    {
        if (command_ == 0xc0 + kCallF)
        {
            if (track.depth >= track.stack.size())
            {
                unmodelled_++;
                return kContinue;
            }
            track.stack[track.depth++] = track.cursor;
        }
        else
        {
            JumpBack(index, target);
        }
        track.cursor = target;
    }

    return kContinue;
}

void Tracks::Printf(uint32_t index)
{
    // Cmd_Printf: the format, at most 128 characters, then a byte for each conversion. %r and %R read a register. The
    // model prints nothing.
    Track& track = tracks_[index];
    std::vector<uint8_t> kinds;
    for (uint32_t i = 0; i < 128; i++)
    {
        const uint8_t c = ReadByte(track);
        if (c == 0)
        {
            break;
        }
        if (c == '\\')
        {
            if (ReadByte(track) == 0)
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
        const uint8_t conversion = ReadByte(track);
        if (conversion == 0)
        {
            break;
        }
        kinds.push_back(conversion);
    }
    for (const uint8_t kind : kinds)
    {
        const uint8_t value = ReadByte(track);
        if (kind == 'r' || kind == 'R')
        {
            ExchangeRegisterValue(index, value);
        }
    }
}

int32_t Tracks::CommandProcess(uint32_t index, uint8_t command, uint16_t mask)
{
    // Cmd_Process: the command's arguments by its list, each type ORed with `mask`, then the command.
    Track& track = tracks_[index];
    if (command < 0xc0)
    {
        unmodelled_++;
        return kContinue;
    }

    const ArgSpec& spec = kArgs[command - 0xc0];
    uint32_t types = spec.types | mask;
    for (uint32_t i = 0; i < spec.count; i++)
    {
        uint32_t arg = 0;
        switch (types & 3)
        {
        case 0:
            arg = ReadByte(track);
            break;
        case 1:
            arg = Read16(track);
            break;
        case 2:
            arg = Read24(track);
            break;
        default:
            arg = ExchangeRegisterValue(index, ReadByte(track));
            break;
        }
        types >>= 2;
        args_[i] = arg;
    }
    command_ = command;

    return Command(index, static_cast<uint8_t>(command - 0xc0));
}

int32_t Tracks::RegCommandProcess(uint32_t index, bool from_register, uint32_t count)
{
    // RegCmd_Process (commands 0xb0 to 0xbf): a command, from a register with bit 3, and a byte whose top bits turn
    // its first arguments into registers.
    Track& track = tracks_[index];
    uint8_t command = ReadByte(track);
    if (from_register)
    {
        command = static_cast<uint8_t>(ExchangeRegisterValue(index, command));
    }
    uint16_t mask = 0;
    if (!from_register || count != 0)
    {
        uint8_t bits = ReadByte(track);
        uint16_t pair = 3;
        for (uint32_t i = 0; i < count + 1; i++)
        {
            if ((bits & 0x80) != 0)
            {
                mask = static_cast<uint16_t>(mask | pair);
            }
            bits = static_cast<uint8_t>(bits << 1);
            pair = static_cast<uint16_t>(pair << 2);
        }
    }

    return CommandProcess(index, command, mask);
}

int32_t Tracks::Command(uint32_t index, uint8_t command)
{
    // The commands by group. A helper runs each group.
    switch (command)
    {
    case kOpenTrack:
    case kOpenTrackBros:
    case kCall:
    case kCallF:
    case kRet:
    case kRetF:
    case kJmp:
    case kJmpF:
    case kLoopS:
    case kLoopE:
    case kWaitReg:
    case kCloseTrack:
    case kWait3:
    case kFinish:
        return FlowCommand(index, command);

    case kReadPort:
    case kWritePort:
    case kCheckPortImport:
    case kCheckPortExport:
    case kConnectName:
    case kParentWritePort:
    case kChildWritePort:
    case kConnectOpen:
    case kConnectClose:
    case kSyncCpu:
        return PortCommand(index, command);

    case kSimpleOsc:
    case kSimpleEnv:
    case kSimpleAdsr:
    case kOscRoute:
    case kOscFull:
        return OscCommand(index, command);

    case kOutSwitch:
    case kUpdateSync:
    case kBusConnect:
    case kPanPowSet:
    case kIirSet:
    case kFirSet:
    case kExtSet:
    case kPanSwSet:
    case kIirCutOff:
        return LevelCommand(index, command);

    case kSetInterrupt:
    case kDisInterrupt:
    case kClrI:
    case kSetI:
    case kRetI:
    case kIntTimer:
        return InterruptCommand(index, command);

    case kSetLastNote:
    case kTimeRelate:
    case kTranspose:
    case kPauseStatus:
    case kFlushAll:
    case kFlushRelease:
    case kCheckWave:
    case kPrintf:
    case kTempo:
    case kTimeBase:
        return StateCommand(index, command);

    case kNop:
        return kContinue;

    default:
        // The table's empty places: the arguments are read, and nothing happens.
        return kContinue;
    }
}

int32_t Tracks::FlowCommand(uint32_t index, uint8_t command)
{
    Track& track = tracks_[index];
    const std::array<uint32_t, 8>& a = args_;
    switch (command)
    {
    case kOpenTrack:
        OpenTrack(index, a[0], a[1]);
        return kContinue;

    case kOpenTrackBros:
        if (track.parent == Track::kNone)
        {
            return kNoParent;
        }
        OpenTrack(track.parent, a[0], a[1]);
        return kContinue;

    case kCall:
        if (track.depth >= track.stack.size())
        {
            unmodelled_++;
            return kContinue;
        }
        track.stack[track.depth++] = track.cursor;
        track.cursor = a[0];
        return kContinue;

    case kCallF:
    case kJmpF:
        return CallF(index);

    case kRet:
        if (track.depth == 0)
        {
            unmodelled_++;
            return kContinue;
        }
        track.cursor = track.stack[--track.depth];
        return kContinue;

    case kRetF:
        if (Condition(index, a[0] & 0x0f))
        {
            if (track.depth == 0)
            {
                unmodelled_++;
                return kContinue;
            }
            track.cursor = track.stack[--track.depth];
        }
        return kContinue;

    case kJmp:
        // The game jumps to the second argument, which the last command with two or more left. Its one argument goes
        // unused.
        JumpBack(index, a[1]);
        track.cursor = a[1];
        return kContinue;

    case kLoopS:
        if (track.depth >= track.stack.size())
        {
            unmodelled_++;
            return kContinue;
        }
        track.stack[track.depth] = track.cursor;
        track.loop_counts[track.depth++] = static_cast<uint16_t>(a[0]);
        return kContinue;

    case kLoopE:
        {
            if (track.depth == 0)
            {
                return kOutOfRange;
            }
            uint16_t count = track.loop_counts[track.depth - 1];
            if (count != 0)
            {
                count--;
            }
            if (count == 0)
            {
                track.depth--;
                return kContinue;
            }
            track.loop_counts[track.depth - 1] = count;
            track.cursor = track.stack[track.depth - 1];
            return kContinue;
        }

    case kWaitReg:
    case kWait3:
        track.wait = static_cast<int32_t>(a[0]);
        return a[0] != 0 ? kEndTick : kContinue;

    case kCloseTrack:
        {
            const auto slot = static_cast<uint8_t>(a[0]);
            if (slot >= 16)
            {
                return kOutOfRange;
            }
            CloseTrack(track.children[slot]);
            track.children[slot] = Track::kNone;
            return kContinue;
        }

    case kFinish:
        SeqUpdate(index, StepTimed(index));
        return kEndTrack;

    default:
        return kContinue;
    }
}

int32_t Tracks::PortCommand(uint32_t index, uint8_t command)
{
    Track& track = tracks_[index];
    const std::array<uint32_t, 8>& a = args_;
    switch (command)
    {
    case kReadPort:
        {
            if (a[0] >= 16)
            {
                unmodelled_++;
                return kContinue;
            }
            const uint16_t value = track.ports[a[0]].value;
            track.ports[a[0]].import_flag = 0;
            WriteRegDirect(index, static_cast<uint8_t>(a[1]), value);
            return kContinue;
        }

    case kWritePort:
        if (a[0] >= 16)
        {
            unmodelled_++;
            return kContinue;
        }
        track.ports[a[0]].value = static_cast<uint16_t>(a[1]);
        track.ports[a[0]].export_flag = 1;
        return kContinue;

    case kCheckPortImport:
    case kCheckPortExport:
        if (a[0] >= 16)
        {
            unmodelled_++;
            return kContinue;
        }
        WriteRegDirect(index, kRegValue,
                       command == kCheckPortImport ? track.ports[a[0]].import_flag : track.ports[a[0]].export_flag);
        return kContinue;

    case kConnectName:
        track.connection_id = a[0] << 16 | a[1];
        return kContinue;

    case kParentWritePort:
        WritePortAppDirect(track.parent, a[0] & 0x0f, static_cast<uint16_t>(a[1]));
        return kContinue;

    case kChildWritePort:
        {
            const uint32_t child = track.children[(a[0] >> 4) & 0x0f];
            WritePortAppDirect(child, a[0] & 0x0f, static_cast<uint16_t>(a[1]));
            return kContinue;
        }

    case kConnectOpen:
        RegistTrack(index, track.connection_id);
        return kContinue;

    case kConnectClose:
        UnRegistTrack(index);
        return kContinue;

    case kSyncCpu:
        {
            const uint16_t result = callback_ ? callback_(index, static_cast<uint16_t>(a[0])) : 0xffff;
            WriteRegDirect(index, kRegValue, result);
            return kContinue;
        }

    default:
        return kContinue;
    }
}

int32_t Tracks::OscCommand(uint32_t index, uint8_t command)
{
    Track& track = tracks_[index];
    const std::array<uint32_t, 8>& a = args_;
    switch (command)
    {
    case kSimpleOsc:
        SetupSimple(track, static_cast<uint8_t>(a[0]));
        return kContinue;

    case kSimpleEnv:
        SetupSimpleEnv(track, static_cast<uint8_t>(a[0]), a[1]);
        return kContinue;

    case kSimpleAdsr:
        SetupAdsr(track, {static_cast<int16_t>(a[0]), static_cast<int16_t>(a[1]), static_cast<int16_t>(a[2]),
                          static_cast<int16_t>(a[3]), static_cast<int16_t>(a[4])});
        return kContinue;

    case kOscRoute:
        {
            const uint8_t route = a[0] & 0x0f;
            const uint32_t oscillator = (a[0] >> 4) & 0x0f;
            if (oscillator >= 2)
            {
                unmodelled_++;
                return kContinue;
            }
            track.osc_routes[oscillator] = route;
            if (route == 14)
            {
                track.osc_states[oscillator].state = OscState::kStart;
            }
            return kContinue;
        }

    case kOscFull:
        SetupFull(track, static_cast<uint8_t>(a[0]), a[1], a[2]);
        return kContinue;

    default:
        return kContinue;
    }
}

int32_t Tracks::LevelCommand(uint32_t index, uint8_t command)
{
    Track& track = tracks_[index];
    const std::array<uint32_t, 8>& a = args_;
    switch (command)
    {
    case kOutSwitch:
        if (track.outer != Track::kNone)
        {
            outer_[track.outer].switches = static_cast<uint16_t>(a[0]);
            outer_[track.outer].updates = 0xffff;
        }
        return kContinue;

    case kUpdateSync:
        UpdateTrack(index, a[0]);
        return kContinue;

    case kBusConnect:
        if (a[0] < 6)
        {
            channels_.Manager(index).mix_configs[a[0]] = static_cast<uint16_t>(a[1]);
        }
        return kContinue;

    case kPanPowSet:
        for (std::size_t i = 0; i < 3; i++)
        {
            track.registers[kRegArguments + i] = static_cast<uint16_t>(a[i]);
        }
        for (std::size_t i = 3; i < 5; i++)
        {
            const float value = static_cast<float>(a[i]) * 327.67f;
            track.registers[kRegArguments + i] = static_cast<uint16_t>(static_cast<uint32_t>(ToInt32(value)));
        }
        return kContinue;

    case kIirSet:
        // The release's code divides each coefficient by 32,767.
        for (std::size_t i = 0; i < 4; i++)
        {
            MoveParam& iir = track.timed[kTimedIir + i];
            iir.target = static_cast<float>(static_cast<int16_t>(a[i])) / kShortMax;
            TakeTarget(iir);
        }
        return kContinue;

    case kFirSet:
        if (track.outer != Track::kNone)
        {
            // Jam_SetExtFirFilterD: 8 coefficients from the sequence's data, in the host's order.
            OuterParam& outer = outer_[track.outer];
            outer.updates = static_cast<uint16_t>(outer.updates | OuterParam::kFir);
            outer.switches = static_cast<uint16_t>(outer.switches | OuterParam::kFir);
            for (std::size_t i = 0; i < 8; i++)
            {
                outer.fir[i] = static_cast<int16_t>(track.sequence->Native16(a[0] + 2 * static_cast<uint32_t>(i)));
            }
        }
        return kContinue;

    case kExtSet:
        {
            const uint32_t outer = SequenceOuter(track.sequence, a[0]);
            InitExtBuffer(outer);
            AssignExtBuffer(index, outer);
            return kContinue;
        }

    case kPanSwSet:
        for (std::size_t i = 0; i < 3; i++)
        {
            const uint32_t type = (a[i] >> 5) & 7;
            track.pan_calcs[i] = kOscFlags[kCalcTypes + type];
            track.parent_pan_calcs[i] = kOscFlags[kParentCalcTypes + type];
            channels_.Manager(index).pan_calcs[i] = static_cast<uint8_t>(a[i] & 0x1f);
            track.update_flags |= kUpdatePan;
        }
        return kContinue;

    case kIirCutOff:
        {
            const uint32_t filter = a[0] & 0xff;
            if (filter >= jaudio1::kCutoffToIir.size() / 4)
            {
                unmodelled_++;
                return kContinue;
            }

            // CUTOFF_TO_IIR_TABLE, at 0x6afe1c in main's data: the table of Pikmin 2's JASPlayer.
            for (std::size_t i = 0; i < 4; i++)
            {
                MoveParam& iir = track.timed[kTimedIir + i];
                iir.target = static_cast<float>(jaudio1::kCutoffToIir[filter * 4 + i]) / kShortMax;
                TakeTarget(iir);
            }
            return kContinue;
        }

    default:
        return kContinue;
    }
}

int32_t Tracks::InterruptCommand(uint32_t index, uint8_t command)
{
    Track& track = tracks_[index];
    const std::array<uint32_t, 8>& a = args_;
    switch (command)
    {
    case kSetInterrupt:
        track.enabled = static_cast<uint8_t>(track.enabled | 1u << (a[0] & 31));
        if (a[0] >= 8)
        {
            unmodelled_++;
            return kContinue;
        }
        track.interrupt_targets[a[0]] = a[1];
        return kContinue;

    case kDisInterrupt:
        track.enabled = static_cast<uint8_t>(track.enabled & ~static_cast<uint8_t>(1u << (a[0] & 31)));
        return kContinue;

    case kClrI:
        track.interrupted = 0;
        return kContinue;

    case kSetI:
        track.interrupted = 1;
        return kContinue;

    case kRetI:
        track.wait = track.saved_wait;
        track.interrupted = 0;
        track.cursor = track.saved_cursor;
        return kRetry;

    case kIntTimer:
        track.timer_count = static_cast<uint8_t>(a[0]);
        track.timer = a[1];
        track.timer_reload = a[1];
        return kContinue;

    default:
        return kContinue;
    }
}

int32_t Tracks::StateCommand(uint32_t index, uint8_t command)
{
    Track& track = tracks_[index];
    const std::array<uint32_t, 8>& a = args_;
    switch (command)
    {
    case kSetLastNote:
        track.last_note = static_cast<uint8_t>(a[0] + track.final_transpose);
        return kContinue;

    case kTimeRelate:
        track.time_relate = static_cast<uint8_t>(a[0]);
        return kContinue;

    case kTranspose:
        // The child adds its parent's transpose, not the parent's sum.
        track.transpose = static_cast<int8_t>(a[0]);
        track.final_transpose = static_cast<uint8_t>(
            track.parent != Track::kNone ? tracks_[track.parent].transpose + track.transpose : track.transpose);
        return kContinue;

    case kPauseStatus:
        track.pause_status = static_cast<uint8_t>(a[0]);
        return kContinue;

    case kFlushAll:
        channels_.AllStop(index);
        channels_.FlushRelease(index);
        return kContinue;

    case kFlushRelease:
        channels_.FlushRelease(index);
        return kContinue;

    case kTimeBase:
        track.timebase = static_cast<uint16_t>(a[0]);
        if (track.parent == Track::kNone)
        {
            UpdateTempo(index);
        }
        return kContinue;

    case kTempo:
        track.tempo = static_cast<uint16_t>(a[0]);
        if (track.parent == Track::kNone)
        {
            UpdateTempo(index);
        }
        else
        {
            track.tempo_sync = true;
        }
        return kContinue;

    case kCheckWave:
        {
            // One_CheckInstWave with the bank's number as register 6 holds it.
            const uint16_t bank_program = ReadRegDirect(index, kRegBank);
            const uint32_t result =
                channels_.CheckInstWave(bank_program >> 8, bank_program & 0xffu, (a[0] >> 8) & 0xffu, a[0] & 0xffu);
            WriteRegDirect(index, kRegValue, static_cast<uint8_t>(result));
            return kContinue;
        }

    case kPrintf:
        Printf(index);
        return kContinue;

    default:
        return kContinue;
    }
}

uint32_t Tracks::StepTimed(uint32_t index)
{
    // A tick of the timed parameters. Those of the oscillators, 6 to 10, set their settings at once. The others give
    // update flags. Parameter 11, the second oscillator's vertex, gives a flag too.
    Track& track = tracks_[index];
    uint32_t flags = 0;
    for (std::size_t i = 0; i < track.timed.size(); i++)
    {
        MoveParam& move = track.timed[i];
        if (move.time > 0.0f)
        {
            move.value += move.step;
            move.time -= 1.0f;
            if (i <= 5 || i >= 11)
            {
                flags |= 1u << i;
            }
            else
            {
                UpdateOscParam(track, static_cast<uint32_t>(i), move.value);
            }
        }
    }

    return flags;
}

bool Tracks::SyncTempo(uint32_t index)
{
    Track& track = tracks_[index];
    if (track.parent != Track::kNone && track.tempo_sync)
    {
        float ratio = static_cast<float>(track.tempo) / static_cast<float>(tracks_[track.parent].tempo);
        if (ratio > 1.0f)
        {
            ratio = 1.0f;
        }
        track.tempo_counter += ratio;
        if (track.tempo_counter < 1.0f)
        {
            return false;
        }
        track.tempo_counter -= 1.0f;
    }

    return true;
}

void Tracks::TickTimer(uint32_t index)
{
    Track& track = tracks_[index];
    if (track.timer != 0 && --track.timer == 0)
    {
        SetInterrupt(index, 6);
        if (track.timer_count != 0)
        {
            if (--track.timer_count != 0)
            {
                track.timer = track.timer_reload;
            }
        }
        else
        {
            track.timer = track.timer_reload;
        }
    }
}

bool Tracks::CountDownWait(uint32_t index)
{
    Track& track = tracks_[index];
    if (track.wait == -1)
    {
        if (!CheckNoteStop(index, 0))
        {
            return false;
        }
        track.wait = 0;
    }
    if (track.wait > 0)
    {
        if (--track.wait != 0)
        {
            return false;
        }
        if (track.note_time != 0xffffffff && track.note_flags == 0)
        {
            for (uint32_t i = 0; i < track.note_played && i < 8; i++)
            {
                track.notes[i] = kNoNote;
                track.channels[i] = Track::kNone;
            }
        }
    }

    return true;
}

int32_t Tracks::ParseCommands(uint32_t index)
{
    Track& track = tracks_[index];
    for (;;)
    {
        const uint8_t command = ReadByte(track);
        if ((command & 0x80) == 0)
        {
            if (ParseNote(index, command))
            {
                return kEndTick;
            }
            continue;
        }

        if ((command & 0xf0) == 0x80 || command == 0xf9)
        {
            if (ParseNoteEnd(index, command))
            {
                return kEndTick;
            }
            continue;
        }

        int32_t result = kContinue;
        if ((command & 0xf0) == 0x90)
        {
            WriteTimeParam(index, command & 0x0f);
        }
        else if ((command & 0xf0) == 0xa0)
        {
            WriteRegParam(index, command & 0x0f);
        }
        else if ((command & 0xf0) == 0xb0)
        {
            result = RegCommandProcess(index, (command & 8) != 0, command & 7);
        }
        else
        {
            result = CommandProcess(index, command, 0);
        }

        if (result == kEndTick || result == kRetry || result == kEndTrack)
        {
            return result;
        }
    }
}

void Tracks::TickChildren(uint32_t index)
{
    Track& track = tracks_[index];
    for (std::size_t i = 0; i < track.children.size(); i++)
    {
        const uint32_t child = track.children[i];
        if (child == Track::kNone || tracks_[child].state == Track::kClosed)
        {
            continue;
        }

        const bool child_muted = track.muted || ((track.child_mutes >> i) & 1) != 0;
        if (SeqMain(child, child_muted) == -1)
        {
            CloseTrack(child);
            track.children[i] = Track::kNone;
        }
    }
}

int32_t Tracks::SeqMain(uint32_t index, bool muted)
{
    // Jam_SeqmainNote: a tick of the track, then of its children.
    Track& track = tracks_[index];
    if (!SyncTempo(index))
    {
        return 0;
    }

    if (track.parent != Track::kNone)
    {
        track.muted = muted;

        // A child gives one of its free channels back to its parent at each tick.
        if (channels_.Manager(index).count != 0)
        {
            const uint32_t channel = channels_.ListGet(index, ChannelManager::kFree);
            if (channel != kNone)
            {
                channels_.ListAdd(track.parent, ChannelManager::kFree, channel);
                channels_.At(channel).manager = track.parent;
                channels_.Manager(index).count--;
                channels_.Manager(track.parent).count++;
            }
        }

        track.final_transpose = static_cast<uint8_t>(track.transpose + tracks_[track.parent].final_transpose);
    }
    else
    {
        track.final_transpose = static_cast<uint8_t>(track.transpose);
    }

    SetInterrupt(index, 7);
    TickTimer(index);

    uint32_t flags = 0;
    bool timed = true;
    for (bool retry = true; retry;)
    {
        retry = false;
        TryInterrupt(index);
        if (track.paused && (track.pause_status & Track::kPauseParser) != 0)
        {
            timed = false;
            break;
        }
        track.ticks++;

        if (!CountDownWait(index))
        {
            break;
        }

        const int32_t result = ParseCommands(index);
        if (result == kRetry)
        {
            retry = true;
        }
        if (result == kEndTrack)
        {
            return -1;
        }
    }

    if (timed)
    {
        flags = StepTimed(index);
        for (std::size_t i = 0; i < 2; i++)
        {
            if (track.osc_routes[i] == 0x0e)
            {
                flags |= kOscFlags[std::min<std::size_t>(track.oscillators[i].target, kOscFlags.size() - 1)];
            }
        }
    }

    track.update_flags |= flags;
    TickChildren(index);

    return 0;
}

void Tracks::SeqUpdate(uint32_t index, uint32_t flags)
{
    // SeqUpdate: the track's update flags, with those of its outer parameters, then each child's with the parent's.
    Track& track = tracks_[index];
    uint32_t all = flags | track.update_flags;
    if (track.outer != Track::kNone)
    {
        all |= outer_[track.outer].updates;
        outer_[track.outer].updates = 0;
    }
    track.update_flags = 0;
    if (all != 0)
    {
        UpdateTrack(index, all);
    }
    for (const uint32_t child : track.children)
    {
        if (child != Track::kNone && tracks_[child].state != Track::kClosed)
        {
            SeqUpdate(child, all);
        }
    }
}

} // namespace ntdlib::jac
