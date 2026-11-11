// SPDX-License-Identifier: MIT

// The parser of the tracks' sequences (JASSeqParser): the commands and the channels' callbacks (see track.cpp).

#include "ntdlib/jaudio1/track.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "jaudio1/track_internal.h"
#include "ntdlib/jaudio1/calc.h"
#include "ntdlib/jaudio1/iir_table.h"

namespace ntdlib::jaudio1
{

namespace
{

// A DSP block's mixers, 8 bytes each. A mixer's word at 6 holds its delays.
constexpr std::size_t kMixerBytes = 8;
constexpr std::size_t kMixerDelays = DspChannelBlock::kMixers + 6;

// The tables of JASSeqParser::cmdPanSwSet (0x94b534 and 0x94b53b): the ways that a track's level mixes with the
// outer level and with the parent's level. Index 7 reads the byte after each table, 0.
constexpr std::array<uint8_t, 8> kCalcTypes = {0, 0, 0, 1, 1, 2, 2, 0};
constexpr std::array<uint8_t, 8> kParentCalcTypes = {0, 1, 2, 0, 2, 0, 2, 0};

// JASSeqParser's Arglist (0x94b542): for each command from 0xc0, the count of arguments, and their widths in 2 bits
// each from the low bits: 0 a byte, 1 16 bits, 2 24 bits, 3 a register's value by a byte.
struct ArgSpec
{
    uint16_t count;
    uint16_t types;
};

// clang-format off
constexpr std::array<ArgSpec, 64> kArgs = {{
    {0, 0}, {2, 8}, {2, 8}, {1, 2}, {0, 0}, {0, 0}, {1, 0}, {1, 2}, {0, 0}, {1, 1}, {0, 0}, {2, 0}, {2, 0xc}, {1, 0},
    {1, 0}, {1, 3}, {0, 0}, {2, 0xc}, {2, 0xc}, {0, 0}, {1, 0}, {1, 0}, {1, 0}, {2, 8}, {5, 0x155}, {1, 0}, {1, 0},
    {1, 0}, {1, 1}, {2, 4}, {1, 0}, {2, 8}, {1, 0}, {0, 0}, {0, 0}, {0, 0}, {2, 4}, {1, 0}, {1, 1}, {1, 1}, {0, 0},
    {0, 0}, {1, 2}, {5, 0}, {4, 0x55}, {1, 2}, {1, 2}, {3, 0}, {1, 0}, {1, 0}, {3, 0x28}, {1, 0}, {1, 0}, {0, 0},
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {1, 1}, {0, 0}, {0, 0}, {1, 1}, {1, 1}, {0, 0},
}};
// clang-format on

// The commands from 0xc0 without a handler (sCmdPList's null entries).
constexpr uint64_t kNullCommands = 0x03e00000000900a9ull;

} // namespace

void Tracks::CmdSetParam(uint32_t index, uint32_t flags)
{
    // JASSeqParser::cmdSetParam (0x3e0c54): a timed level, its target as a fraction of 32767 and its time.
    Track& track = tracks_[index];
    const uint32_t param = ReadByte(track);
    uint32_t data = 0;
    switch ((flags >> 2) & 3)
    {
    case 0:
        data = ReadReg16(index, ReadByte(track));
        break;

    case 1:
        data = ReadByte(track);
        break;

    case 2:
        {
            const uint32_t byte = ReadByte(track);
            data = byte << 8 | ((byte & 0x80) == 0 ? byte << 1 : 0);
            break;
        }

    default:
        data = Read16(track);
        break;
    }

    int32_t time = -1;
    switch (flags & 3)
    {
    case 1:
        time = static_cast<int32_t>(ReadReg16(index, ReadByte(track)) & 0xffff);
        break;
    case 2:
        time = ReadByte(track);
        break;
    case 3:
        time = Read16(track);
        break;
    default:
        break;
    }

    const float value = static_cast<float>(static_cast<int16_t>(data)) / kShortMax;
    SetParam(track, param, value, time);
}

void Tracks::CmdCall(uint32_t index)
{
    // JASSeqParser::cmdCall (0x3dfe68).
    Track& track = tracks_[index];
    const uint32_t flag = ReadByte(track);
    uint32_t target = 0;
    if ((flag & 0x80) == 0)
    {
        target = Read24(track);
    }
    else
    {
        target = ReadReg16(index, ReadByte(track)) & 0xffff;
        if ((flag & 0x40) != 0)
        {
            const uint32_t table = (flag & 0x20) != 0 ? ReadReg16(index, ReadByte(track)) & 0xffff : Read24(track);
            target = track.sequence->U24(table + target * 3);
        }
    }

    if (Condition(track, flag))
    {
        if (track.depth >= track.stack.size())
        {
            unmodelled_++;
            return;
        }
        track.stack[track.depth++] = track.cursor;
        track.cursor = target;
    }
}

void Tracks::CmdJmp(uint32_t index)
{
    // JASSeqParser::cmdJmp (0x3e0020). A jump to a 32-bit register's value takes it as an address in memory: the
    // game asserts "not support" and jumps there anyway.
    Track& track = tracks_[index];
    const uint32_t flag = ReadByte(track);
    uint32_t target = 0;
    bool address = false;
    if ((flag & 0x80) == 0)
    {
        target = Read24(track);
    }
    else
    {
        const uint32_t reg = ReadByte(track);
        if ((flag & 0x40) != 0)
        {
            const uint32_t entry = ReadReg16(index, reg) & 0xffff;
            const uint32_t table = (flag & 0x20) != 0 ? ReadReg16(index, ReadByte(track)) & 0xffff : Read24(track);
            target = track.sequence->U24(table + entry * 3);
        }
        else
        {
            address = (reg & 0xfc) == 0x28;
            target = ReadReg32(index, reg);
        }
    }

    if (Condition(track, flag))
    {
        if (address)
        {
            unmodelled_++;
            return;
        }
        JumpBack(index, target);
        track.cursor = target;
    }
}

void Tracks::CmdPrintf(Track& track)
{
    // JASSeqParser::cmdPrintf (0x3e090c): the format, at most 128 characters, then a byte for each conversion. The
    // model prints nothing.
    uint32_t conversions = 0;
    for (uint32_t i = 0; i < 128; i++)
    {
        const uint32_t c = ReadByte(track);
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
        if (ReadByte(track) == 0)
        {
            break;
        }

        conversions++;
    }
    for (uint32_t i = 0; i < conversions; i++)
    {
        ReadByte(track);
    }
}

int32_t Tracks::CmdProcess(uint32_t index, uint32_t command, uint32_t register_mask)
{
    // JASSeqParser::Cmd_Process (0x3e0ae4): the arguments by Arglist's widths, or a register's value where the mask
    // says so, then the command.
    command &= 0xff;
    if (command < 0xc0)
    {
        // The game reads the tables before their start.
        unmodelled_++;
        return 0;
    }
    const ArgSpec& spec = kArgs[command - 0xc0];
    Args args{};
    if (spec.count != 0)
    {
        uint32_t types = spec.types | register_mask;
        const uint32_t count = spec.count;
        Track& track = tracks_[index];
        for (uint32_t i = 0; i < count; i++)
        {
            switch (types & 3)
            {
            case 0:
                args[i] = ReadByte(track);
                break;
            case 1:
                args[i] = Read16(track);
                break;
            case 2:
                args[i] = Read24(track);
                break;
            default:
                args[i] = ExchangeRegisterValue(index, ReadByte(track));
                break;
            }
            types = (types >> 2) & 0x3fff;
        }
    }
    if (((kNullCommands >> (command - 0xc0)) & 1) != 0)
    {
        return 0;
    }

    return Execute(index, command, args);
}

int32_t Tracks::Execute(uint32_t index, uint32_t command, const Args& a)
{
    // The commands by group. A helper runs each group.
    switch (command)
    {
    case 0xc1: // openTrack
    case 0xc2: // openTrackBros
    case 0xc4: // call
    case 0xc6: // ret
    case 0xc8: // jmp
    case 0xc9: // loopS
    case 0xca: // loopE
    case 0xcf: // wait
    case 0xea:
    case 0xda: // closeTrack
    case 0xff: // finish
        return FlowCommand(index, command, a);

    case 0xcb: // readPort
    case 0xcc: // writePort
    case 0xcd: // checkPortImport
    case 0xce: // checkPortExport
    case 0xd1: // parentWritePort
    case 0xd2: // childWritePort
    case 0xe7: // syncCPU
        return PortCommand(index, command, a);

    case 0xd6: // simpleOsc
    case 0xd7: // simpleEnv
    case 0xd8: // simpleADSR
    case 0xe5: // vibDepth
    case 0xe6: // vibDepthMidi
    case 0xf0: // oscRoute
    case 0xf2: // oscFull
    case 0xf4: // vibPitch
        return OscCommand(index, command, a);

    case 0xdb: // outSwitch
    case 0xdc: // updateSync
    case 0xdd: // busConnect
    case 0xeb: // panPowSet
    case 0xec: // IIRSet
    case 0xed: // FIRSet
    case 0xee: // EXTSet
    case 0xef: // panSwSet
    case 0xf1: // IIRCutOff
    case 0xf3: // volumeMode
        return LevelCommand(index, command, a);

    case 0xdf: // setInterrupt
    case 0xe0: // disInterrupt
    case 0xe1: // clrI
    case 0xe2: // setI
    case 0xe3: // retI
    case 0xe4: // intTimer
        return InterruptCommand(index, command, a);

    case 0xd4: // setLastNote
    case 0xd5: // timeRelate
    case 0xd9: // transpose
    case 0xde: // pauseStatus
    case 0xfa: // checkWave
    case 0xfb: // printf
    case 0xfd: // tempo
    case 0xfe: // timeBase
        return StateCommand(index, command, a);

    case 0xe8: // flushAll
    case 0xe9: // flushRelease
        return 0;

    case 0xfc: // nop
        return 0;

    default:
        return 0;
    }
}

int32_t Tracks::FlowCommand(uint32_t index, uint32_t command, const Args& a)
{
    Track& track = tracks_[index];
    switch (command)
    {
    case 0xc1: // openTrack
    case 0xc2: // openTrackBros
        {
            uint32_t parent = index;
            if (command == 0xc2)
            {
                parent = track.parent;
                if (parent == Track::kNone)
                {
                    return 0;
                }
            }
            const uint32_t child = OpenChild(parent, a[0] & 0xf, (a[0] >> 6) & 3);
            if (child == Track::kNone)
            {
                // The game starts the track that it didn't get: a write through a null pointer.
                unmodelled_++;
                return 0;
            }
            Start(child, track.sequence, a[1]);
            return 0;
        }

    case 0xc4: // call
        CmdCall(index);
        return 0;

    case 0xc6: // ret
        if (Condition(track, a[0]))
        {
            if (track.depth == 0)
            {
                return 3;
            }

            track.cursor = track.stack[--track.depth];
        }
        return 0;

    case 0xc8: // jmp
        CmdJmp(index);
        return 0;

    case 0xc9: // loopS
        if (track.depth >= track.stack.size())
        {
            unmodelled_++;
            return 0;
        }
        track.stack[track.depth] = track.cursor;
        track.loop_counts[track.depth] = static_cast<uint16_t>(a[0]);
        track.depth++;
        return 0;

    case 0xca: // loopE (JASSeqCtrl::loopEnd)
        {
            if (track.depth == 0)
            {
                return 0;
            }

            const uint32_t at = track.depth - 1;
            const uint32_t count = track.loop_counts[at];
            if (count != 0 && ((count - 1) & 0xffff) != 0)
            {
                track.loop_counts[at] = static_cast<uint16_t>(count - 1);
                track.cursor = track.stack[at];
            }
            else
            {
                track.depth = at;
            }
            return 0;
        }

    case 0xcf: // wait
    case 0xea:
        track.wait = static_cast<int32_t>(a[0]);
        return a[0] != 0 ? 1 : 0;

    case 0xda: // closeTrack
        {
            const uint32_t slot = a[0] & 0xff;
            if (slot >= 16)
            {
                unmodelled_++;
                return 0;
            }
            const uint32_t child = track.children[slot];
            if (child != Track::kNone)
            {
                Close(child);
                track.children[slot] = Track::kNone;
            }
            return 0;
        }

    case 0xff: // finish
        return 3;

    default:
        return 0;
    }
}

int32_t Tracks::PortCommand(uint32_t index, uint32_t command, const Args& a)
{
    Track& track = tracks_[index];
    switch (command)
    {
    case 0xcb: // readPort
        {
            const uint32_t port = a[0];
            if (port >= 16)
            {
                unmodelled_++;
                return 0;
            }
            track.import_flags[port] = 0;
            WriteRegDirect(track, a[1], track.ports[port]);
            return 0;
        }

    case 0xcc: // writePort
        if (a[0] >= 16)
        {
            unmodelled_++;
            return 0;
        }
        track.export_flags[a[0]] = 1;
        track.ports[a[0]] = static_cast<uint16_t>(a[1]);
        return 0;

    case 0xcd: // checkPortImport
    case 0xce: // checkPortExport
        if (a[0] >= 16)
        {
            unmodelled_++;
            return 0;
        }
        SetRegister(track, kRegResult, command == 0xcd ? track.import_flags[a[0]] : track.export_flags[a[0]]);
        return 0;

    case 0xd1: // parentWritePort
        if (track.parent == Track::kNone)
        {
            unmodelled_++;
            return 0;
        }
        WritePortAppDirect(track.parent, a[0] & 0xf, static_cast<uint16_t>(a[1]));
        return 0;

    case 0xd2: // childWritePort
        {
            const uint32_t child = track.children[(a[0] >> 4) & 0xf];
            if (child == Track::kNone)
            {
                unmodelled_++;
                return 0;
            }
            WritePortAppDirect(child, a[0] & 0xf, static_cast<uint16_t>(a[1]));
            return 0;
        }

    case 0xe7: // syncCPU
        SetRegister(track, kRegResult, sync_ ? sync_(index, a[0]) : 0xffff);
        return 0;

    default:
        return 0;
    }
}

int32_t Tracks::OscCommand(uint32_t index, uint32_t command, const Args& a)
{
    Track& track = tracks_[index];
    switch (command)
    {
    case 0xd6: // simpleOsc
        OscSetupSimple(track, a[0]);
        return 0;

    case 0xd7: // simpleEnv
        OscSetupSimpleEnv(track, a[0], a[1]);
        return 0;

    case 0xd8: // simpleADSR
        track.oscillators[0] = kAdsrDef;
        track.oscillators[0].attack = {track.adsr.data(), track.adsr.size()};
        track.adsr[0].time = static_cast<int16_t>(a[0]);
        track.adsr[1].time = static_cast<int16_t>(a[1]);
        track.adsr[2].time = static_cast<int16_t>(a[2]);
        track.adsr[2].value = static_cast<int16_t>(a[3]);
        track.direct_release = static_cast<uint16_t>(a[4]);
        return 0;

    case 0xe5: // vibDepth
        {
            const uint32_t byte = a[0] & 0xff;
            track.vibrate.depth = static_cast<float>(static_cast<int32_t>(byte << 8 | byte << 1)) / 393204.0f;
            return 0;
        }

    case 0xe6: // vibDepthMidi
        track.vibrate.depth = static_cast<float>(a[0]) / 393204.0f;
        return 0;

    case 0xf0: // oscRoute
        {
            const uint32_t slot = (a[0] >> 4) & 0xf;
            if (slot >= 2)
            {
                unmodelled_++;
                return 0;
            }
            track.osc_routes[slot] = a[0] & 0xf;
            return 0;
        }

    case 0xf2: // oscFull
        OscSetupFull(track, a[0], a[1], a[2]);
        return 0;

    case 0xf4: // vibPitch
        {
            const uint32_t byte = a[0] & 0xff;
            track.vibrate.pitch = static_cast<float>(byte << 8 | byte << 1) / 294903.0f;
            return 0;
        }

    default:
        return 0;
    }
}

int32_t Tracks::LevelCommand(uint32_t index, uint32_t command, const Args& a)
{
    Track& track = tracks_[index];
    switch (command)
    {
    case 0xdb: // outSwitch
        if (track.outer != nullptr)
        {
            track.outer->outer_switch = static_cast<uint16_t>(a[0]);
            track.outer->outer_update = 0xffff;
        }
        return 0;

    case 0xdc: // updateSync
        UpdateTrack(index, a[0]);
        return 0;

    case 0xdd: // busConnect
        if (a[0] >= track.updater.mix_configs.size())
        {
            unmodelled_++;
            return 0;
        }
        track.updater.mix_configs[a[0]] = static_cast<uint16_t>(a[1]);
        return 0;

    case 0xeb: // panPowSet
        track.registers[kRegPanPower] = static_cast<uint16_t>(a[0]);
        track.registers[kRegPanPower + 1] = static_cast<uint16_t>(a[1]);
        track.registers[kRegPanPower + 2] = static_cast<uint16_t>(a[2]);
        track.registers[kRegOuterWeight] = static_cast<uint16_t>(ToInt32(static_cast<float>(a[3]) * 327.67f));
        track.registers[kRegParentWeight] = static_cast<uint16_t>(ToInt32(static_cast<float>(a[4]) * 327.67f));
        return 0;

    case 0xec: // IIRSet
        for (std::size_t i = 0; i < 4; i++)
        {
            MoveParam& param = track.timed[kTimedIir + i];
            const float value = static_cast<float>(static_cast<int16_t>(a[i])) * kShortScale;
            param.value = value;
            param.target = value;
            param.time = 1.0f;
            param.amount = 0.0f;
        }
        return 0;

    case 0xed: // FIRSet: the outer parameters' FIR filter from the sequence, in the host's order
        if (track.outer == nullptr)
        {
            unmodelled_++;
            return 0;
        }
        track.outer->outer_update |= OuterParam::kFir;
        track.outer->outer_switch |= OuterParam::kFir;
        for (uint32_t i = 0; i < 8; i++)
        {
            track.outer->fir[i] = static_cast<int16_t>(track.sequence->Native16(a[0] + 2 * i));
        }
        return 0;

    case 0xee: // EXTSet
        {
            OuterParam* const outer = SequenceOuter(track.sequence, a[0]);
            outer->Init();
            track.outer = outer;
            return 0;
        }

    case 0xef: // panSwSet
        track.pan_calc = kCalcTypes[(a[0] >> 5) & 7];
        track.parent_pan_calc = kParentCalcTypes[(a[0] >> 5) & 7];
        track.updater.pan_calc = static_cast<uint8_t>(a[0] & 0x1f);
        track.fxmix_calc = kCalcTypes[(a[1] >> 5) & 7];
        track.parent_fxmix_calc = kParentCalcTypes[(a[1] >> 5) & 7];
        track.updater.fxmix_calc = static_cast<uint8_t>(a[1] & 0x1f);
        track.dolby_calc = kCalcTypes[(a[2] >> 5) & 7];
        track.parent_dolby_calc = kParentCalcTypes[(a[2] >> 5) & 7];
        track.updater.dolby_calc = static_cast<uint8_t>(a[2] & 0x1f);
        track.update_flags |= kUpdateFxmix | kUpdatePan | kUpdateDolby;
        return 0;

    case 0xf1: // IIRCutOff
        {
            const uint32_t cutoff = a[0] & 0xff;
            if (cutoff >= 128)
            {
                unmodelled_++;
                return 0;
            }
            for (std::size_t i = 0; i < 4; i++)
            {
                MoveParam& param = track.timed[kTimedIir + i];
                const float value = static_cast<float>(kCutoffToIir[cutoff * 4 + i]) / kShortMax;
                param.value = value;
                param.target = value;
                param.time = 1.0f;
                param.amount = 0.0f;
            }
            return 0;
        }

    case 0xf3: // volumeMode
        track.volume_mode = static_cast<uint8_t>(a[0]);
        return 0;

    default:
        return 0;
    }
}

int32_t Tracks::InterruptCommand(uint32_t index, uint32_t command, const Args& a)
{
    Track& track = tracks_[index];
    switch (command)
    {
    case 0xdf: // setInterrupt
        track.enabled = static_cast<uint8_t>(track.enabled | (1u << (a[0] & 31)));
        if (a[0] < 8)
        {
            track.interrupt_targets[a[0]] = a[1];
            track.targets_set = static_cast<uint8_t>(track.targets_set | (1u << a[0]));
        }
        else
        {
            unmodelled_++;
        }
        return 0;

    case 0xe0: // disInterrupt
        track.enabled = static_cast<uint8_t>(track.enabled & ~(1u << (a[0] & 31)));
        return 0;

    case 0xe1: // clrI
        track.interrupted = false;
        track.return_cursor = 0;
        track.interrupts_on = true;
        return 0;

    case 0xe2: // setI
        track.interrupts_on = false;
        return 0;

    case 0xe3: // retI
        track.interrupts_on = true;
        if (track.interrupted)
        {
            track.wait = track.saved_wait;
            track.cursor = track.return_cursor;
            track.interrupted = false;
            track.return_cursor = 0;
        }
        TryInterrupt(track);
        return track.wait > 0 ? 1 : 0;

    case 0xe4: // intTimer
        track.timer_repeats = static_cast<uint8_t>(a[0]);
        track.timer = a[1];
        track.timer_reload = a[1];
        return 0;

    default:
        return 0;
    }
}

int32_t Tracks::StateCommand(uint32_t index, uint32_t command, const Args& a)
{
    Track& track = tracks_[index];
    switch (command)
    {
    case 0xd4: // setLastNote
        track.latest_key = static_cast<uint8_t>(a[0] + static_cast<uint32_t>(Transpose(track)));
        return 0;

    case 0xd5: // timeRelate
        track.time_relate = a[0] != 0;
        return 0;

    case 0xd9: // transpose
        track.transpose = static_cast<int8_t>(a[0]);
        return 0;

    case 0xde: // pauseStatus
        track.pause_status = static_cast<uint8_t>(a[0]);
        return 0;

    case 0xfa: // checkWave
        SetRegister(track, kRegResult, 0);
        return 0;

    case 0xfb: // printf
        CmdPrintf(track);
        return 0;

    case 0xfd: // tempo
        SetTempo(index, a[0]);
        return 0;

    case 0xfe: // timeBase
        SetTimebase(index, a[0]);
        return 0;

    default:
        return 0;
    }
}

int32_t Tracks::ParseSeq(uint32_t index)
{
    // JASSeqParser::parseSeq (0x3e1108): the commands up to a wait (0) or the sequence's end (-1).
    for (;;)
    {
        Track& track = tracks_[index];
        const uint32_t command = ReadByte(track);
        int32_t result = 0;
        if ((command & 0x80) == 0)
        {
            result = CmdNoteOn(index, command);
        }
        else if ((command & ~8u) == 0x80)
        {
            result = CmdWait(index, command);
        }
        else if (command == 0xf9 || (command & 0xf0) == 0x80)
        {
            CmdNoteOff(index, command);
            continue;
        }
        else
        {
            switch (command & 0xf0)
            {
            case 0x90:
                CmdSetParam(index, command & 0xf);
                continue;

            case 0xa0:
                WriteRegParam(index, command & 0xf);
                continue;

            case 0xb0:
                result = CmdRegisters(index, command);
                break;

            default:
                result = CmdProcess(index, command, 0);
                break;
            }
        }

        if (result == 3)
        {
            return -1;
        }
        if (result == 1)
        {
            return 0;
        }
    }
}

int32_t Tracks::CmdWait(uint32_t index, uint32_t command)
{
    // cmdWait: a byte or 16 bits. A wait of 0 goes on to the next command.
    Track& track = tracks_[index];
    uint32_t value = ReadByte(track);
    if (command != 0x80)
    {
        value = value << 8 | ReadByte(track);
    }
    track.wait = static_cast<int32_t>(value);

    return value != 0 ? 1 : 0;
}

int32_t Tracks::CmdRegisters(uint32_t index, uint32_t command)
{
    // RegCmd_Process: the command from the next byte or a register, and a mask of the arguments that come from
    // registers.
    Track& track = tracks_[index];
    uint32_t inner = ReadByte(track);
    const uint32_t mask_count = command & 7;
    uint32_t mask = 0;
    if ((command & 8) != 0)
    {
        inner = ExchangeRegisterValue(index, inner);
    }
    if ((command & 8) == 0 || mask_count != 0)
    {
        const uint32_t bits = ReadByte(track);
        for (uint32_t i = 0; i <= mask_count; i++)
        {
            if (((bits << i) & 0x80) != 0)
            {
                mask |= 3u << (2 * i);
            }
        }
    }

    return CmdProcess(index, inner, mask);
}

void Tracks::OnChannel(uint32_t owner, Channel::Event event, uint32_t channel, DspBlock* block)
{
    // JASTrack::channelUpdateCallback (0x3dfb0c) and JASChannelUpdater's two updates (0x3e15c4 and 0x3e17b4).
    Track& track = tracks_[owner];
    Channel& ch = voices_.ChannelAt(channel);
    const ChannelUpdater& updater = track.updater;
    switch (event)
    {
    case Channel::kEventStart:
        ch.mix_configs = updater.mix_configs;
        if (ch.DolbyMode())
        {
            block->InitAutoMixer();
        }
        else
        {
            block->SetU16(DspChannelBlock::kDelayCount, updater.delay_max);
            for (std::size_t i = 0; i < 6; i++)
            {
                block->SetU16(kMixerDelays + kMixerBytes * i,
                              static_cast<uint32_t>(updater.delays[i]) << 8 | updater.delays[i]);
            }
        }
        ch.pan_calc = updater.pan_calc;
        ch.fxmix_calc = updater.fxmix_calc;
        ch.dolby_calc = updater.dolby_calc;
        [[fallthrough]];

    case Channel::kEventPlay:
        if (event == Channel::kEventPlay && ch.status == Channel::kRelease)
        {
            return;
        }

        ch.volume_channel = updater.volume;
        ch.pitch_channel = updater.pitch;
        ch.pan_channel = updater.pan;
        ch.fxmix_channel = updater.fxmix;
        ch.dolby_channel = updater.dolby;
        for (std::size_t i = 0; i < 6; i++)
        {
            const std::size_t at = kMixerDelays + kMixerBytes * i;
            block->SetU16(at, (block->U16(at) & 0xff) | static_cast<uint32_t>(updater.delays[i]) << 8);
        }
        if ((updater.filter_mode & 0x20) != 0)
        {
            for (std::size_t i = 0; i < 4; i++)
            {
                block->SetU16(DspChannelBlock::kIirCoefficients + 2 * i, static_cast<uint16_t>(updater.iir[i]));
            }
        }
        if ((updater.filter_mode & 0x1f) != 0)
        {
            std::array<uint16_t, 8> fir{};
            for (std::size_t i = 0; i < 8; i++)
            {
                fir[i] = static_cast<uint16_t>(updater.fir[i]);
            }
            block->SetFir8FilterParam(fir);
        }
        block->SetFilterMode(updater.filter_mode);
        block->SetDistFilter(updater.distortion);
        return;

    case Channel::kEventStop:
        {
            for (uint32_t& slot : track.channels)
            {
                if (slot == channel)
                {
                    slot = Track::kNone;
                    break;
                }
            }
            const auto at = std::find(track.channel_list.begin(), track.channel_list.end(), channel);
            if (at != track.channel_list.end())
            {
                track.channel_list.erase(at);
            }
            return;
        }

    case Channel::kEventTimer:
        voices_.Release(channel, 0);
        return;

    default:
        return;
    }
}

} // namespace ntdlib::jaudio1
