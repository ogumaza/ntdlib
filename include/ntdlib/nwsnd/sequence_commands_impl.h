// SPDX-License-Identifier: MIT

// The definitions of SequenceTrack's commands in sequence.h: the parse of each tick, the arguments and variables, the
// commands, the jumps, calls and loops, and the LFOs' settings. sequence_impl.h includes this file.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/nwsnd/envelope.h"
#include "ntdlib/nwsnd/sequence.h"
#include "ntdlib/nwsnd/sequence_track_impl.h"

namespace ntdlib::nwsnd
{

template <typename Policy>
int SequenceTrack<Policy>::ParseNextTick(Player& player, Engine& engine)
{
    if (!open_)
    {
        return 0;
    }

    vol_.Update();
    volume2_.Update();
    pan_.Update();
    span_.Update();
    bend_.Update();

    if (note_finish_wait_)
    {
        if (channel_list_ != kNoChannel)
        {
            return 1;
        }

        note_finish_wait_ = false;
    }

    if (wait_ > 0)
    {
        --wait_;
        if (wait_ > 0)
        {
            return 1;
        }
    }

    return ParseCommands(player, engine);
}

template <typename Policy>
int SequenceTrack<Policy>::ParseCommands(Player& player, Engine& engine)
{
    if (!has_data_)
    {
        return 1;
    }

    int count = 0;
    while (wait_ == 0 && !note_finish_wait_)
    {
        if constexpr (requires { engine.StopsTick(player, index_); })
        {
            if (engine.StopsTick(player, index_))
            {
                break;
            }
        }
        if (++count > Policy::kParseLimit)
        {
            if constexpr (Policy::kParseLimitEndsTrack)
            {
                if constexpr (requires { engine.OnParseLimitEnd(player, index_); })
                {
                    engine.OnParseLimitEnd(player, index_);
                }
                return -1;
            }
            else
            {
                if constexpr (requires { engine.OnParseLimit(player, index_); })
                {
                    if (engine.OnParseLimit(player, index_))
                    {
                        return -1;
                    }
                }
                break;
            }
        }

        if (RunCommand(player, engine) == 1)
        {
            return -1;
        }
    }

    return 1;
}

template <typename Policy>
int16_t* SequenceTrack<Policy>::Variable(Player& player, Engine& engine, int index)
{
    if (index < 0x20)
    {
        return player.Variable(engine, index);
    }
    if (index < 0x30)
    {
        return &vars_[index - 0x20];
    }

    return nullptr;
}

template <typename Policy>
int32_t SequenceTrack<Policy>::ArgumentValue(Player& player, Engine& engine, const commands::Argument& argument)
{
    switch (argument.source)
    {
    case commands::Source::kValue:
        return static_cast<int32_t>(argument.value);

    case commands::Source::kRandom:
        {
            const auto low = static_cast<int32_t>(argument.value);
            const auto high = static_cast<int32_t>(argument.high);
            const uint32_t random = DrawRandom(player, engine);
            const int32_t product = static_cast<int32_t>(static_cast<uint32_t>((high - low) + 1) * random);
            return low + (product >> 16);
        }

    case commands::Source::kVariable:
        {
            const auto index = static_cast<int>(argument.value);
            MarkRead(player, index);
            if constexpr (requires { engine.OnVariableRead(player, index_, index); })
            {
                engine.OnVariableRead(player, index_, index);
            }

            const int16_t* variable = Variable(player, engine, index);
            return variable ? *variable : 0;
        }
    }

    return 0;
}

template <typename Policy>
typename SequenceTrack<Policy>::SoundState SequenceTrack<Policy>::State(const Player& player) const
{
    SoundState state{};
    state.prg_no = prg_no_;
    state.bank_index = bank_index_;
    state.transpose = transpose_;
    state.velocity_range = velocity_range_;
    state.bend_range = bend_range_;
    state.priority = priority_;
    state.porta_key = porta_key_;
    state.porta_time = porta_time_;
    state.main_send = main_send_;
    state.init_pan = init_pan_;
    state.biquad_type = biquad_type_;
    state.envelope = {attack_, decay_, sustain_, release_};
    state.hold = hold_;
    state.fx_send = fx_send_;
    state.lpf = lpf_;
    state.biquad_value = biquad_value_;
    state.sweep_pitch = sweep_pitch_;
    state.output_line = output_line_;
    state.lfo_params = lfo_params_;
    state.lfo_types = lfo_types_;
    state.mute = mute_;
    state.silence = silence_;
    state.tie = tie_;
    state.mono = mono_;
    state.porta = porta_;
    state.damper = damper_;
    state.front_bypass = front_bypass_;
    state.note_wait = note_wait_;

    std::size_t at = 0;
    const auto ramp = [&state, &at](const auto& value)
    {
        state.ramps[at++] = value.Get();
        state.ramps[at++] = value.target;
        state.ramps[at++] = value.duration - value.counter;
    };
    ramp(vol_);
    ramp(volume2_);
    ramp(pan_);
    ramp(span_);
    ramp(bend_);
    ramp(player.main_volume_);

    state.newest = channel_list_;
    state.timebase = player.timebase_;
    state.tempo = player.tempo_;

    return state;
}

template <typename Policy>
int SequenceTrack<Policy>::RunCommand(Player& player, Engine& engine)
{
    if (!FollowsLoops(engine))
    {
        return Parse(player, engine);
    }

    const uint64_t now = ++player.commands_run_;
    run_.resize(data_.size() + 1);
    run_[std::min<std::size_t>(current_, data_.size())] = now;

    const SoundState before = State(player);
    const int result = Parse(player, engine);
    if (State(player) != before)
    {
        MarkChange(player);
    }

    return result;
}

template <typename Policy>
int SequenceTrack<Policy>::Parse(Player& player, Engine& engine)
{
    // Returns 1 at the bytecode's end (FF).
    held_wait_ = false;
    const uint32_t start = current_;
    const PlayedCommand played = ReadPlayedCommand(data_, current_, player.grammar_);
    const commands::Command& read = played.command;
    current_ = played.next;
    conditional_ = read.conditional;
    bool condition = true;
    if (conditional_) // if
    {
        conditional_at_ = player.commands_run_;
        condition = cmp_flag_;
    }

    // The command after its prefixes: a note's key, an opcode, or 0xF0 before an extended command's byte.
    const uint32_t code = played.code;
    const uint32_t command = code > 0xFF ? 0xF0 : code;
    if constexpr (requires { engine.OnCommandStart(player, index_, start, command, conditional_); })
    {
        engine.OnCommandStart(player, index_, start, command, conditional_);
    }

    if (command < 0x80)
    {
        // A note: the command is the key, and the velocity and the length follow.
        const auto velocity = static_cast<uint8_t>(read.velocity);
        const int32_t length = ArgumentValue(player, engine, read.value);
        if (!condition)
        {
            return 0;
        }

        const int key = std::clamp(static_cast<int>(command) + static_cast<int>(transpose_), 0, 127);
        noted_at_ = player.commands_run_;
        holds_ = length >= kHeldTicks;
        if constexpr (requires { engine.OnNoteCommand(player, index_, key, int{velocity}, length); })
        {
            engine.OnNoteCommand(player, index_, key, int{velocity}, length);
        }

        if (!mute_ && NoteOn(player, engine, key, velocity, length > 0 ? length : -1, tie_))
        {
            MarkChange(player);
        }

        if (note_wait_)
        {
            wait_ = length;
            held_wait_ = holds_;
            if (length == 0)
            {
                note_finish_wait_ = true;
            }
        }

        return 0;
    }

    switch (command & 0xF0)
    {
    case 0x80:
        switch (command)
        {
        case 0x80: // wait
            {
                const int32_t wait = ArgumentValue(player, engine, read.value);
                if (condition)
                {
                    wait_ = wait;
                }

                return 0;
            }

        case 0x81: // program
            {
                const int32_t program = ArgumentValue(player, engine, read.value);
                if (condition)
                {
                    CommandProc(player, engine, command, program, 0);
                }

                return 0;
            }

        case 0x88: // open a track
            if (condition)
            {
                CommandProc(player, engine, command, read.index, static_cast<int32_t>(read.target));
            }

            return 0;

        case 0x89: // jump
        case 0x8A: // call
            if (condition)
            {
                CommandProc(player, engine, command, static_cast<int32_t>(read.target), 0);
            }

            return 0;

        default:
            return 0;
        }

    case 0x90:
        if (condition)
        {
            CommandProc(player, engine, command, 0, 0);
        }

        return 0;

    case 0xB0:
    case 0xC0:
    case 0xD0:
        {
            // A time prefix gives a time after the value.
            const uint32_t value = static_cast<uint32_t>(ArgumentValue(player, engine, read.value)) & 0xFF;
            const int32_t time = read.time ? ArgumentValue(player, engine, *read.time) : 0;
            if (!condition)
            {
                return 0;
            }

            const int32_t arg =
                command == 0xC3 || command == 0xC4 ? static_cast<int8_t>(value) : static_cast<int32_t>(value);
            CommandProc(player, engine, command, arg, time);
            return 0;
        }

    case 0xE0:
        {
            const int32_t value = static_cast<int16_t>(ArgumentValue(player, engine, read.value));
            if (condition)
            {
                CommandProc(player, engine, command, value, 0);
            }

            return 0;
        }

    case 0xF0:
        if (command == 0xF0)
        {
            RunExtended(player, engine, played, condition);
            return 0;
        }

        if (command == 0xFE) // the tracks to allocate: the sound's setup reads them
        {
            return 0;
        }

        if (command == 0xFF) // the end, after a false condition too where the end isn't conditional
        {
            return (condition || !Policy::kEndIsConditional) ? 1 : 0;
        }

        if (condition)
        {
            CommandProc(player, engine, command, 0, 0);
        }

        return 0;

    default:
        return 0;
    }
}

template <typename Policy>
void SequenceTrack<Policy>::RunExtended(Player& player, Engine& engine, const PlayedCommand& played, bool condition)
{
    // A random or variable prefix gives the argument's type, as in Parse. Without one, the commands that take an
    // argument take a 16-bit one.
    const commands::Command& read = played.command;
    const uint32_t command = played.code;
    switch (command & 0xF0)
    {
    case 0x80:
    case 0x90:
        {
            const int32_t value = static_cast<int16_t>(ArgumentValue(player, engine, read.value));
            if (condition)
            {
                CommandProc(player, engine, command, read.index, value);
            }

            return;
        }

    case 0xA0:
    case 0xB0:
        {
            // The extra LFOs' settings: a plain byte. No prefix changes it. A console without them reads no byte.
            if (!Policy::HasCommand(player.build_, command))
            {
                return;
            }

            if (condition)
            {
                CommandProc(player, engine, command, static_cast<int32_t>(read.value.value), 0);
            }

            return;
        }

    case 0xE0:
        {
            const int32_t value = static_cast<uint16_t>(ArgumentValue(player, engine, read.value));
            if (condition)
            {
                CommandProc(player, engine, command, value, 0);
            }

            return;
        }

    default:
        return;
    }
}

template <typename Policy>
void SequenceTrack<Policy>::CommandProc(Player& player, Engine& engine, uint32_t command, int32_t arg1, int32_t arg2)
{
    command_end_ = current_;

    // A command the console's library doesn't have does nothing.
    if (!Policy::HasCommand(player.build_, command))
    {
        return;
    }

    if (command > 0xFF)
    {
        ExtendedCommandProc(player, engine, command, arg1, arg2);
        return;
    }

    const uint8_t byte = static_cast<uint8_t>(arg1);
    switch (command)
    {
    case 0x81: // program
        if (arg1 < 0x10000)
        {
            prg_no_ = static_cast<uint16_t>(arg1);
        }
        break;

    case 0x88: // open a track
        OpenTrack(player, engine, arg1, static_cast<uint32_t>(arg2));
        break;

    case 0x89: // jump
        Jump(player, engine, static_cast<uint32_t>(arg1));
        break;

    case 0x8A: // call
        Call(static_cast<uint32_t>(arg1));
        break;

    case 0xB0:
        player.timebase_ = byte;
        break;

    case 0xB1:
        hold_ = byte;
        break;

    case 0xB2: // monophonic
        mono_ = arg1 != 0;
        if (mono_)
        {
            ReleaseAllChannel(player, engine, -1);
            FreeAllChannel(player, engine);
        }
        break;

    case 0xB3:
        velocity_range_ = byte;
        break;

    case 0xB4:
        biquad_type_ = static_cast<int8_t>(byte);
        break;

    case 0xB5:
        biquad_value_ = Policy::BiquadValueOf(arg1);
        break;

    case 0xB6:
        bank_index_ = byte;
        break;

    case 0xBD:
        SetLfo(0, LfoSetting::kPhase, arg1);
        break;

    case 0xBE:
        SetLfo(0, LfoSetting::kCurve, arg1);
        break;

    case 0xBF: // front bypass
        front_bypass_ = arg1 != 0;
        break;

    case 0xC0:
        pan_.SetTarget(static_cast<int8_t>(Policy::PanArgument(player.build_, arg1) - 0x40),
                       static_cast<int16_t>(arg2));
        break;

    case 0xC1:
        vol_.SetTarget(byte, static_cast<int16_t>(arg2));
        break;

    case 0xC2:
        if constexpr (Policy::kMainVolumeRamps)
        {
            player.main_volume_.SetTarget(byte, static_cast<int16_t>(arg2));
        }
        else
        {
            player.main_volume_.Init(byte);
        }
        break;

    case 0xC3:
        transpose_ = static_cast<int8_t>(arg1);
        break;

    case 0xC4:
        if (Policy::BendRamps(player.build_))
        {
            bend_.SetTarget(static_cast<int8_t>(arg1), static_cast<int16_t>(arg2));
        }
        else
        {
            bend_.Init(static_cast<int8_t>(arg1));
        }
        break;

    case 0xC5:
        bend_range_ = byte;
        break;

    case 0xC6:
        priority_ = byte;
        break;

    case 0xC7:
        note_wait_ = arg1 != 0;
        break;

    case 0xC8: // tie
        tie_ = arg1 != 0;
        ReleaseAllChannel(player, engine, -1);
        FreeAllChannel(player, engine);
        break;

    case 0xC9: // portamento from a key
        porta_ = true;
        porta_key_ = static_cast<uint8_t>(arg1 + transpose_);
        break;

    case 0xCA:
        SetLfo(0, LfoSetting::kDepth, arg1);
        break;

    case 0xCB:
        SetLfo(0, LfoSetting::kSpeed, arg1);
        break;

    case 0xCC:
        SetLfo(0, LfoSetting::kType, arg1);
        break;

    case 0xCD:
        SetLfo(0, LfoSetting::kRange, arg1);
        break;

    case 0xCE:
        porta_ = arg1 != 0;
        break;

    case 0xCF:
        porta_time_ = byte;
        break;

    case 0xD0:
        attack_ = byte;
        break;

    case 0xD1:
        decay_ = byte;
        break;

    case 0xD2:
        sustain_ = byte;
        break;

    case 0xD3:
        release_ = byte;
        break;

    case 0xD4: // loop start
        LoopStart(byte);
        break;

    case 0xD5:
        if constexpr (Policy::kVolume2Ramps)
        {
            volume2_.SetTarget(byte, static_cast<int16_t>(arg2));
        }
        else
        {
            volume2_.Init(byte);
        }
        break;

    case 0xD6: // print a variable: only a debug flag in the game shows it
        break;

    case 0xD7:
        span_.SetTarget(static_cast<int8_t>(Policy::PanArgument(player.build_, arg1)), static_cast<int16_t>(arg2));
        break;

    case 0xD8:
        lpf_ = static_cast<float>(arg1 - 0x40) * 0.015625f;
        break;

    case 0xD9:
        fx_send_[0] = byte;
        break;

    case 0xDA:
        fx_send_[1] = byte;
        break;

    case 0xDB:
        main_send_ = byte;
        break;

    case 0xDC:
        init_pan_ = static_cast<int8_t>(Policy::PanArgument(player.build_, arg1) - 0x40);
        break;

    case 0xDD:
        SetMute(player, engine, byte);
        break;

    case 0xDE:
        fx_send_[2] = byte;
        break;

    case 0xDF:
        damper_ = byte >= 0x40;
        break;

    case 0xE0:
        SetLfo(0, LfoSetting::kDelay, arg1);
        break;

    case 0xE1:
        player.tempo_ = Policy::TempoOf(player.build_, arg1);
        break;

    case 0xE3:
        sweep_pitch_ = Policy::SweepPitchOf(player.build_, arg1);
        break;

    case 0xE4:
        SetLfo(0, LfoSetting::kPeriod, arg1);
        break;

    case 0xFB: // the bank's envelope again
        attack_ = decay_ = sustain_ = release_ = 0xFF;
        hold_ = 0xFF;
        break;

    case 0xFC: // loop end
        LoopEnd(player, engine);
        break;

    case 0xFD: // return
        Return();
        break;

    default:
        break;
    }

    if constexpr (requires { engine.OnCommand(player, index_, command, arg1, arg2); })
    {
        engine.OnCommand(player, index_, command, arg1, arg2);
    }
}

template <typename Policy>
void SequenceTrack<Policy>::OpenTrack(Player& player, Engine& engine, int32_t index, uint32_t offset)
{
    SequenceTrack* track = player.GetTrack(index);
    if (!track || track == this)
    {
        return;
    }

    track->Close(player, engine);
    track->SetSeqData(data_, offset);
    track->Open();
    MarkChange(player); // the new track's sound is this pass's change
}

template <typename Policy>
void SequenceTrack<Policy>::Jump(Player& player, Engine& engine, uint32_t target)
{
    // A jump back to a command that the track has run is a loop, outside a subroutine. Inside a subroutine, such as one
    // that draws a random value again until it differs from the last, the jump isn't a loop. An unconditional jump
    // inside a subroutine is still a loop when the track holds a note that sounds until the game stops it. A
    // subroutine that bends a held note's pitch for ever is an example. Under the policy's length rules
    // (jump_loops_without_condition), it's a loop too when the track has run no conditional command since the jump's
    // target. Nothing can then leave the loop. A melody that steps through its notes this way and leaves through a
    // conditional command isn't a loop. A pass of such a jump can still be idle (CountPoll).
    if (target < current_ && target < run_.size() && run_[target] != 0)
    {
        const auto call = [](const CallStackEntry& entry)
        {
            return !entry.is_loop;
        };
        const bool in_call = std::any_of(call_stack_.begin(), call_stack_.begin() + call_depth_, call);
        const bool unconditioned = Policy::kLengthRules.jump_loops_without_condition && conditional_at_ < run_[target];
        if (!in_call || (!conditional_ && (HoldsSustainedNote(engine) || unconditioned)))
        {
            CountLoop(player, engine, target);
        }
        else
        {
            CountPoll(player, target);
        }
    }

    current_ = target;
}

template <typename Policy>
void SequenceTrack<Policy>::Call(uint32_t target)
{
    if (call_depth_ >= kCallStackDepth)
    {
        return;
    }

    // The entry keeps the count of the last loop at its level (see LoopEnd).
    call_stack_[call_depth_].address = current_;
    call_stack_[call_depth_].is_loop = false;
    ++call_depth_;
    current_ = target;
}

template <typename Policy>
void SequenceTrack<Policy>::LoopStart(uint8_t count)
{
    if (call_depth_ >= kCallStackDepth)
    {
        return;
    }

    call_stack_[call_depth_].address = current_;
    call_stack_[call_depth_].is_loop = true;
    call_stack_[call_depth_].loop_count = count;
    ++call_depth_;
}

template <typename Policy>
void SequenceTrack<Policy>::LoopEnd(Player& player, Engine& engine)
{
    if (call_depth_ == 0)
    {
        return;
    }

    // A call's entry keeps the last loop count at its level. On a console whose loop end takes it, that's the count.
    CallStackEntry& entry = call_stack_[call_depth_ - 1];
    if (Policy::kLoopEndIgnoresCalls && !entry.is_loop)
    {
        return;
    }

    uint8_t count = entry.loop_count;
    if (count != 0 && !LoopsForEver(player, engine, entry.address))
    {
        --count;
        if (count == 0)
        {
            --call_depth_;
            return;
        }
    }
    else
    {
        // A count of 0 loops for ever. On the main track, that's a loop of the sequence. So is a counted loop that the
        // engine takes as endless.
        CountLoop(player, engine, entry.address);
    }

    entry.loop_count = count;
    current_ = entry.address;
}

template <typename Policy>
void SequenceTrack<Policy>::Return()
{
    if constexpr (Policy::kReturnSkipsLoops)
    {
        // The return leaves the loops above the call too.
        while (call_depth_ != 0)
        {
            --call_depth_;
            if (!call_stack_[call_depth_].is_loop)
            {
                current_ = call_stack_[call_depth_].address;
                return;
            }
        }
    }
    else if (call_depth_ != 0)
    {
        --call_depth_;
        current_ = call_stack_[call_depth_].address;
    }
}

template <typename Policy>
void SequenceTrack<Policy>::ExtendedCommandProc(Player& player, Engine& engine, uint32_t command, int32_t arg1,
                                                int32_t arg2)
{
    const uint32_t op = command & 0xFF;
    if (op >= 0xA0 && op <= 0xB1)
    {
        // LFOs 1 to 3 have six commands each: the curve, phase, depth, speed, type and range.
        constexpr std::array kSettings = {LfoSetting::kCurve, LfoSetting::kPhase, LfoSetting::kDepth,
                                          LfoSetting::kSpeed, LfoSetting::kType,  LfoSetting::kRange};
        SetLfo(static_cast<int>(1 + (op - 0xA0) / 6), kSettings[(op - 0xA0) % 6], arg1);
    }
    else if (op >= 0xE1 && op <= 0xE6)
    {
        // Their delays and periods have two commands each. 0xE0 calls the game's procedure. Archive mode has none.
        const LfoSetting setting = (op - 0xE1) % 2 == 0 ? LfoSetting::kDelay : LfoSetting::kPeriod;
        SetLfo(static_cast<int>(1 + (op - 0xE1) / 2), setting, arg1);
    }
    else if ((op & 0xF0) == 0x80 || (op & 0xF0) == 0x90)
    {
        // A command on variable arg1, or a comparison with it. Where the variable doesn't exist, the command does
        // nothing. The engine's OnExtendedCommand isn't called then either.
        int16_t* variable = Variable(player, engine, arg1);
        if (!variable)
        {
            return;
        }

        if ((op & 0xF0) == 0x90)
        {
            MarkRead(player, arg1);
        }
        const int16_t before = *variable;
        VariableCommand(player, engine, op, *variable, arg2);
        if (*variable != before)
        {
            set_at_ = player.commands_run_;
        }
    }

    if constexpr (requires { engine.OnExtendedCommand(player, index_, command, arg1, arg2); })
    {
        engine.OnExtendedCommand(player, index_, command, arg1, arg2);
    }
}

template <typename Policy>
void SequenceTrack<Policy>::VariableCommand(Player& player, Engine& engine, uint32_t op, int16_t& variable,
                                            int32_t value)
{
    const int32_t current = variable;
    const auto set = [&variable](int32_t result)
    {
        variable = static_cast<int16_t>(result);
    };

    switch (op)
    {
    case 0x80: // set
        set(value);
        break;

    case 0x81: // add
        set(current + value);
        break;

    case 0x82: // subtract
        set(current - value);
        break;

    case 0x83: // multiply
        set(static_cast<int32_t>(static_cast<uint32_t>(current) * static_cast<uint32_t>(value)));
        break;

    case 0x84: // divide
        if (value != 0)
        {
            set(current / static_cast<int16_t>(value));
        }
        break;

    case 0x85: // shift (see the policy's Shift)
        set(Policy::Shift(current, value));
        break;

    case 0x86: // random
        {
            int32_t range = value;
            const bool negative = range < 0;
            if (negative)
            {
                range = static_cast<int16_t>(-range);
            }

            const uint32_t random = DrawRandom(player, engine);
            int32_t result = static_cast<int32_t>(random * static_cast<uint32_t>(range + 1)) >> 16;
            if (negative)
            {
                result = -result;
            }

            set(result);
            break;
        }

    case 0x87: // and
        set(current & value);
        break;

    case 0x88: // or
        set(current | value);
        break;

    case 0x89: // exclusive or
        set(current ^ value);
        break;

    case 0x8A: // not
        set(~value);
        break;

    case 0x8B: // modulo
        if (value != 0)
        {
            set(current - (current / value) * value);
        }
        break;

    case 0x90:
        cmp_flag_ = current == value;
        break;

    case 0x91:
        cmp_flag_ = current >= value;
        break;

    case 0x92:
        cmp_flag_ = current > value;
        break;

    case 0x93:
        cmp_flag_ = current <= value;
        break;

    case 0x94:
        cmp_flag_ = current < value;
        break;

    case 0x95:
        cmp_flag_ = current != value;
        break;

    default:
        break;
    }
}

template <typename Policy>
void SequenceTrack<Policy>::SetLfo(int lfo, LfoSetting setting, int32_t arg)
{
    CurveLfoParam& param = lfo_params_[static_cast<std::size_t>(lfo)];
    const auto byte = static_cast<uint8_t>(arg);
    switch (setting)
    {
    case LfoSetting::kCurve:
        param.curve = byte;
        break;

    case LfoSetting::kPhase:
        param.phase = byte;
        break;

    case LfoSetting::kDepth:
        param.depth = static_cast<float>(byte) * 0.0078125f;
        break;

    case LfoSetting::kSpeed:
        param.speed = static_cast<float>(byte) * 0.390625f;
        break;

    case LfoSetting::kType:
        lfo_types_[static_cast<std::size_t>(lfo)] = byte;
        break;

    case LfoSetting::kRange:
        param.range = byte;
        break;

    case LfoSetting::kDelay: // in steps of 5 ms
        param.delay = static_cast<uint32_t>(arg * 5);
        break;

    case LfoSetting::kPeriod: // the speed, from a period in 10 ms
        {
            const auto period = static_cast<int16_t>(arg);
            param.speed = period == 0 ? 0.0f : 100.0f / static_cast<float>(period);
            break;
        }
    }
}

} // namespace ntdlib::nwsnd
