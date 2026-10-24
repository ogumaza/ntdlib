// SPDX-License-Identifier: MIT

// The definitions of sequence.h's templates. The code that uses the model includes this file in one source file, with
// the policy's types complete. That file builds the templates for the policy:
//
//     template class ntdlib::nwsnd::SequenceTrack<Policy>;
//     template class ntdlib::nwsnd::SequenceSoundPlayer<Policy>;

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/nwsnd/envelope.h"
#include "ntdlib/nwsnd/sequence.h"
#include "ntdlib/nwsnd/util.h"

namespace ntdlib::nwsnd
{

// ----------------------------------------------------------------------------------------------------- SequenceTrack

// Whether `engine` follows the sequence's loops now, for a rip tool's length analysis: its FollowsLoops(), or false for
// an engine without one.
template <typename Engine>
bool FollowsLoops(const Engine& engine)
{
    if constexpr (requires { engine.FollowsLoops(); })
    {
        return engine.FollowsLoops();
    }
    else
    {
        return false;
    }
}

template <typename Policy>
SequenceTrack<Policy>::SequenceTrack(int index) : index_(index)
{
    InitParam();
}

template <typename Policy>
typename SequenceTrack<Policy>::Channel* SequenceTrack<Policy>::First(Engine& engine) const
{
    return channel_list_ == kNoChannel ? nullptr : &engine.Channels().At(channel_list_);
}

template <typename Policy>
const typename SequenceTrack<Policy>::Channel* SequenceTrack<Policy>::First(const Engine& engine) const
{
    return channel_list_ == kNoChannel ? nullptr : &engine.Channels().At(channel_list_);
}

template <typename Policy>
typename SequenceTrack<Policy>::Channel* SequenceTrack<Policy>::Next(Engine& engine, const Channel& channel) const
{
    return channel.next_in_track == kNoChannel ? nullptr : &engine.Channels().At(channel.next_in_track);
}

template <typename Policy>
const typename SequenceTrack<Policy>::Channel* SequenceTrack<Policy>::Next(const Engine& engine,
                                                                           const Channel& channel) const
{
    return channel.next_in_track == kNoChannel ? nullptr : &engine.Channels().At(channel.next_in_track);
}

template <typename Policy>
std::vector<const typename SequenceTrack<Policy>::Channel*> SequenceTrack<Policy>::Channels(const Engine& engine) const
{
    std::vector<const Channel*> channels;
    for (const Channel* channel = First(engine); channel; channel = Next(engine, *channel))
    {
        channels.push_back(channel);
    }

    return channels;
}

template <typename Policy>
void SequenceTrack<Policy>::InitParam()
{
    volume_ = 1.0f;
    pitch_ = 1.0f;
    pan_range_ = 1.0f;
    outputs_ = {};
    mute_ = false;
    bank_index_ = 0;
    note_finish_wait_ = false;
    note_wait_ = true;
    wait_ = 0;
    cmp_flag_ = true;
    data_ = {};
    has_data_ = false;
    porta_ = false;
    silence_ = false;
    mono_ = false;
    prg_no_ = 0;
    damper_ = false;
    tie_ = false;
    call_depth_ = 0;
    current_ = 0;
    lfo_params_ = {};
    lfo_types_ = {};
    vol_.Init(127);
    volume2_.Init(127);
    pan_.Init(0);
    span_.Init(0);
    bend_.Init(0);
    porta_key_ = 60;
    velocity_range_ = 127;
    bend_range_ = 2;
    hold_ = 0xFF;
    attack_ = decay_ = sustain_ = release_ = 0xFF;
    main_send_ = 127;
    init_pan_ = 0;
    transpose_ = 0;
    sweep_pitch_ = 0.0f;
    porta_time_ = 0;
    priority_ = 64;
    fx_send_ = {};
    lpf_ = 0.0f;
    biquad_value_ = 0.0f;
    biquad_type_ = 0;
    output_line_ = -1;
    vars_.fill(-1);
}

template <typename Policy>
void SequenceTrack<Policy>::SetSeqData(std::span<const uint8_t> data, uint32_t offset)
{
    data_ = data;
    current_ = offset;
    has_data_ = true;
}

template <typename Policy>
void SequenceTrack<Policy>::Open()
{
    open_ = true;
    if constexpr (Policy::kOpenClearsState)
    {
        wait_ = 0;
        call_depth_ = 0;
        note_finish_wait_ = false;
    }
}

template <typename Policy>
void SequenceTrack<Policy>::Close(Player& player, Engine& engine)
{
    if constexpr (requires { engine.OnTrackClosing(player, index_); })
    {
        engine.OnTrackClosing(player, index_);
    }

    ReleaseAllChannel(player, engine, -1);
    FreeAllChannel(player, engine);
    open_ = false;

    if constexpr (requires { engine.OnTrackClosed(player, index_); })
    {
        engine.OnTrackClosed(player, index_);
    }
}

template <typename Policy>
void SequenceTrack<Policy>::ReleaseAllChannel(Player& player, Engine& engine, int release)
{
    if constexpr (requires { engine.OnReleaseAll(player, index_); })
    {
        engine.OnReleaseAll(player, index_);
    }

    UpdateChannelParam(player, engine);

    for (Channel* channel = First(engine); channel; channel = Next(engine, *channel))
    {
        if (channel->active)
        {
            if (release >= 0)
            {
                channel->envelope.SetRelease(static_cast<uint8_t>(release));
            }

            ReleaseChannel(player, engine, *channel);
        }
    }
}

template <typename Policy>
void SequenceTrack<Policy>::FreeAllChannel(Player& player, Engine& engine)
{
    if constexpr (requires { engine.OnDetachAll(player, index_); })
    {
        engine.OnDetachAll(player, index_);
    }

    for (Channel* channel = First(engine); channel; channel = Next(engine, *channel))
    {
        channel->Detach();
    }

    channel_list_ = kNoChannel;
}

template <typename Policy>
void SequenceTrack<Policy>::StopAllChannel(Player& player, Engine& engine)
{
    for (Channel* channel = First(engine); channel;)
    {
        Channel* next = Next(engine, *channel);
        StopChannel(player, engine, *channel);
        channel = next;
    }

    channel_list_ = kNoChannel;
}

template <typename Policy>
void SequenceTrack<Policy>::StopChannel(Player& player, Engine& engine, Channel& channel)
{
    // The callback takes the channel out of the track's list (OnChannelEvent) before the channel is freed.
    channel.Stop(engine);
    if constexpr (requires { engine.OnStop(player, index_, channel); })
    {
        engine.OnStop(player, index_, channel);
    }
    channel.CallChannelCallback(engine, Channel::CallbackStatus::kStopped);
    engine.Channels().Free(channel);
}

template <typename Policy>
void SequenceTrack<Policy>::ReleaseChannel(Player& player, Engine& engine, Channel& channel)
{
    channel.Release(engine);
    if constexpr (requires { engine.OnRelease(player, index_, channel); })
    {
        engine.OnRelease(player, index_, channel);
    }
}

template <typename Policy>
void SequenceTrack<Policy>::SetMute(Player& player, Engine& engine, int mode)
{
    switch (mode)
    {
    case 0:
        mute_ = false;
        break;

    case 1:
        mute_ = true;
        break;

    case 2:
        ReleaseAllChannel(player, engine, -1);
        FreeAllChannel(player, engine);
        mute_ = true;
        break;

    case 3:
        if constexpr (requires { engine.OnStopAll(player, index_); })
        {
            engine.OnStopAll(player, index_);
        }

        StopAllChannel(player, engine);
        mute_ = true;
        break;

    default:
        break;
    }
}

template <typename Policy>
void SequenceTrack<Policy>::UpdateChannelLength(Player& player, Engine& engine)
{
    if (!open_)
    {
        return;
    }

    for (Channel* channel = First(engine); channel; channel = Next(engine, *channel))
    {
        if (channel->length > 0)
        {
            --channel->length;
        }

        // UpdateChannelRelease (0x022B6E3C).
        if (channel->length == 0 && channel->envelope.GetStatus() != AdshrStatus::kRelease && !damper_)
        {
            channel->NoteOff(engine);
            if constexpr (requires { engine.OnRelease(player, index_, *channel); })
            {
                engine.OnRelease(player, index_, *channel);
            }
        }

        if (!channel->auto_sweep)
        {
            channel->UpdateSweep(1);
        }

        if constexpr (requires { engine.OnLengthUpdated(player, index_, *channel); })
        {
            engine.OnLengthUpdated(player, index_, *channel);
        }
    }

    if constexpr (requires { engine.OnLengthsCounted(player, index_); })
    {
        engine.OnLengthsCounted(player, index_);
    }
}

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
    // conditional command isn't a loop.
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
        VariableCommand(player, engine, op, *variable, arg2);
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

template <typename Policy>
void SequenceTrack<Policy>::CountLoop(Player& player, Engine& engine, uint32_t target)
{
    // The pass began when the track last ran `target`. A still pass also left the variables that tracks read with the
    // values that the pass before left. The first pass compares them with 0. A variable that no track reads doesn't
    // count. The game reads some, such as the beats that a timing track marks.
    const uint64_t begun = target < run_.size() ? run_[target] : 0;
    std::array<int16_t, 48> variables{};
    for (int i = 0; i < static_cast<int>(variables.size()); ++i)
    {
        const int16_t* variable = IsRead(player, i) ? Variable(player, engine, i) : nullptr;
        variables[static_cast<std::size_t>(i)] = variable ? *variable : 0;
    }
    idle_ = changed_at_ < begun;
    still_ = idle_ && drew_at_ < begun && variables == loop_variables_;
    loop_variables_ = variables;
    looped_at_ = player.commands_run_;

    // The pass played a note if the track has played one since the pass began. A loop inside the pass doesn't hide the
    // notes before that loop.
    const bool noted = noted_at_ != 0 && noted_at_ >= begun;
    if constexpr (!Policy::kLengthRules.quiet_after_report)
    {
        quiet_ = !noted;
    }

    const bool alone = HoldsSustainedNote(engine) && player.OthersRest(engine, index_);
    if (FollowsLoops(engine) && !idle_ && (index_ == 0 || (noted && player.IsMainTrack(engine, index_)) || alone))
    {
        if constexpr (requires { engine.OnLoop(player, index_, target); })
        {
            engine.OnLoop(player, index_, target);
        }
    }

    quiet_ = !noted;
    if constexpr (requires { engine.OnTrackLoop(player, index_, target); })
    {
        if (FollowsLoops(engine))
        {
            engine.OnTrackLoop(player, index_, target);
        }
    }
}

template <typename Policy>
bool SequenceTrack<Policy>::LoopsForEver(Player& player, Engine& engine, uint32_t address) const
{
    if constexpr (requires { engine.LoopsForEver(player, index_, address); })
    {
        return engine.LoopsForEver(player, index_, address);
    }
    else
    {
        return false;
    }
}

template <typename Policy>
void SequenceTrack<Policy>::MarkChange(Player& player)
{
    changed_at_ = player.commands_run_;
    player.changed_at_ = changed_at_;
}

template <typename Policy>
uint32_t SequenceTrack<Policy>::DrawRandom(Player& player, Engine& engine)
{
    const uint32_t random = engine.Rng().Next();
    drew_at_ = player.commands_run_;
    if constexpr (requires { engine.OnRandomDraw(player, index_); })
    {
        engine.OnRandomDraw(player, index_);
    }

    return random;
}

template <typename Policy>
void SequenceTrack<Policy>::MarkRead(Player& player, int index)
{
    if (index >= 0 && index < 32)
    {
        player.variables_read_ |= 1u << index;
    }
    else if (index >= 32 && index < 48)
    {
        variables_read_ = static_cast<uint16_t>(variables_read_ | 1u << (index - 32));
    }
}

template <typename Policy>
bool SequenceTrack<Policy>::IsRead(const Player& player, int index) const
{
    if (index >= 0 && index < 32)
    {
        return (player.variables_read_ >> index & 1) != 0;
    }
    if (index >= 32 && index < 48)
    {
        return (variables_read_ >> (index - 32) & 1) != 0;
    }

    return false;
}

template <typename Policy>
bool SequenceTrack<Policy>::WaitsOnEndlessNote(const Engine& engine) const
{
    if (!note_finish_wait_)
    {
        return false;
    }

    for (const Channel* channel = First(engine); channel; channel = Next(engine, *channel))
    {
        if (channel->length < 0 && channel->WaveLoops())
        {
            return true;
        }
    }

    return false;
}

template <typename Policy>
bool SequenceTrack<Policy>::HasEndingNote(const Engine& engine) const
{
    if (holds_ || (Policy::kLengthRules.damper_holds_notes && damper_))
    {
        return false;
    }

    for (const Channel* channel = First(engine); channel; channel = Next(engine, *channel))
    {
        if (channel->length > 0)
        {
            return true;
        }
    }

    return false;
}

template <typename Policy>
bool SequenceTrack<Policy>::HoldsSustainedNote(const Engine& engine) const
{
    for (const Channel* channel = First(engine); channel; channel = Next(engine, *channel))
    {
        const bool endless = holds_ || channel->length < 0;
        if (endless && channel->envelope.GetStatus() == AdshrStatus::kSustain && channel->WaveLoops())
        {
            return true;
        }
    }

    return false;
}

template <typename Policy>
void SequenceTrack<Policy>::OnChannelEvent(Engine& engine, Channel& channel)
{
    // SequenceSoundPlayer::ChannelCallback (0x022CE614) does nothing. The channel leaves the track's list.
    if (channel_list_ == channel.place)
    {
        channel_list_ = channel.next_in_track;
        return;
    }

    for (Channel* previous = First(engine); previous; previous = Next(engine, *previous))
    {
        if (previous->next_in_track == channel.place)
        {
            previous->next_in_track = channel.next_in_track;
            return;
        }
    }
}

template <typename Policy>
bool SequenceTrack<Policy>::NoteOn(Player& player, Engine& engine, int key, int velocity, int32_t length, bool tie)
{
    if constexpr (requires { engine.OnNoteStart(player, index_); })
    {
        engine.OnNoteStart(player, index_);
    }

    const int scaled = velocity * velocity_range_ / 127;
    const auto velocity_volume = [scaled]
    {
        // Bank::CalcChannelVelocityVolume (0x022C49D4).
        return Policy::VelocityVolume(scaled);
    };

    const Channel* newest = First(engine);
    const bool same = newest && newest->key == key && newest->velocity_volume == velocity_volume();
    Channel* channel = nullptr;
    if (tie)
    {
        channel = First(engine);
        if (channel)
        {
            channel->key = static_cast<uint8_t>(key);
            channel->velocity_volume = velocity_volume();
        }
    }

    if (mono_)
    {
        channel = First(engine);
        if (channel)
        {
            if (channel->envelope.GetStatus() == AdshrStatus::kRelease)
            {
                StopChannel(player, engine, *channel);
                channel = nullptr;
            }
            else
            {
                channel->key = static_cast<uint8_t>(key);
                channel->velocity_volume = velocity_volume();
                channel->length = length;
            }
        }
    }

    const bool starts = !channel;
    if (!channel)
    {
        typename Policy::NoteOnInfo info;
        info.prg_no = prg_no_;
        info.key = key;
        info.velocity = scaled;
        info.length = tie ? -1 : length;
        info.init_pan = init_pan_;
        info.priority = static_cast<decltype(info.priority)>(player.channel_priority_) + priority_;
        info.owner_player = player.id_;
        info.owner_track = index_;
        Policy::SetReceiver(info, player.receiver_);

        channel = engine.NoteOn(player.bank_items_, bank_index_, info);
        if (!channel)
        {
            return false;
        }

        if (channel->key_group != 0)
        {
            for (Channel* other = First(engine); other; other = Next(engine, *other))
            {
                if (other->key_group == channel->key_group)
                {
                    if constexpr (Policy::kKeyGroupFastRelease)
                    {
                        other->envelope.SetRelease(0x7E);
                    }

                    ReleaseChannel(player, engine, *other);
                }
            }
        }

        // AddChannel (0x022C9A3C).
        channel->next_in_track = channel_list_;
        channel_list_ = channel->place;
    }

    if (Policy::ReplacesEnvelope(player.build_, attack_))
    {
        channel->envelope.SetAttack(attack_);
    }
    if (Policy::ReplacesEnvelope(player.build_, decay_))
    {
        channel->envelope.SetDecay(decay_);
    }
    if (Policy::ReplacesEnvelope(player.build_, sustain_))
    {
        channel->envelope.SetSustain(sustain_);
    }
    if (Policy::ReplacesEnvelope(player.build_, release_))
    {
        channel->envelope.SetRelease(release_);
    }
    if (Policy::ReplacesEnvelope(player.build_, hold_))
    {
        channel->envelope.SetHold(hold_);
    }

    float sweep = sweep_pitch_;
    if (porta_)
    {
        sweep = sweep + static_cast<float>(static_cast<int>(porta_key_) - key);
    }

    if (porta_time_ == 0)
    {
        channel->SetSweepParam(sweep, length, false);
    }
    else
    {
        const int squared = porta_time_ * porta_time_;
        const float magnitude = sweep >= 0.0f ? sweep : -sweep;
        const int time = Policy::ToInt32(static_cast<float>(squared) * magnitude) >> 5;
        channel->SetSweepParam(sweep, time * 5, true);
    }

    porta_key_ = static_cast<uint8_t>(key);

    // The silence ramp starts over from where it stands. The new target is the track's silence.
    const uint8_t silence = silence_ ? 0 : 255;
    if (channel->silence_counter >= channel->silence_duration)
    {
        channel->silence_duration = 0;
        channel->silence_from = channel->silence_to;
        channel->silence_to = silence;
        channel->silence_counter = 0;
    }
    else
    {
        const int32_t change = (static_cast<int32_t>(channel->silence_to) - channel->silence_from) *
                               channel->silence_counter / channel->silence_duration;
        channel->silence_duration = 0;
        channel->silence_counter = 0;
        channel->silence_from = static_cast<uint8_t>(channel->silence_from + change);
        channel->silence_to = silence;
    }

    channel->release_priority_fix = player.release_priority_fix_;
    channel->pan_mode = player.pan_mode_;
    channel->pan_curve = player.pan_curve_;
    Policy::SetFrontBypass(engine, *channel, front_bypass_);

    if (starts)
    {
        if constexpr (requires { engine.OnNoteOn(player, index_, *channel); })
        {
            engine.OnNoteOn(player, index_, *channel);
        }
    }
    else
    {
        if constexpr (requires { engine.OnLegato(player, index_, *channel); })
        {
            engine.OnLegato(player, index_, *channel);
        }
    }

    return channel != newest || !same || sweep != 0.0f;
}

template <typename Policy>
void SequenceTrack<Policy>::UpdateChannelParam(Player& player, Engine& engine)
{
    if (!open_ || channel_list_ == kNoChannel)
    {
        return;
    }

    const float volume =
        Policy::TrackVolume(vol_.Get(), volume2_.Get(), player.main_volume_.Get(), volume_, player.volume_);

    const float bend = (static_cast<float>(bend_.Get()) * 0.0078125f) * static_cast<float>(bend_range_);
    const float pitch = player.pitch_ * pitch_;
    const float lpf = lpf_ + player.lpf_;
    int biquad_type = biquad_type_;
    float biquad_value = biquad_value_;
    if (player.biquad_type_ != -1)
    {
        biquad_type = player.biquad_type_;
        biquad_value = player.biquad_value_;
    }
    const uint32_t output_line = output_line_ != -1 ? static_cast<uint32_t>(output_line_) : player.output_line_;

    float pan = Policy::PanOfValue(pan_.Get());
    pan = pan * pan_range_;
    pan = pan * player.pan_range_;
    const float span = Policy::SpanOfValue(span_.Get());
    const float main_send = Policy::SendOfValue(main_send_) - 1.0f;
    std::array<float, 3> fx_send{};
    for (int bus = 0; bus < Policy::kFxSendCount; ++bus)
    {
        fx_send[bus] = Policy::SendOfValue(fx_send_[bus]);
    }

    // The output parameters combine the player's parameters, the track's parameters, and the bytecode's pan, surround
    // pan and sends.
    const auto outputs = Policy::CombineOutputs(player.outputs_, outputs_, pan, span, main_send, fx_send);

    for (Channel* channel = First(engine); channel; channel = Next(engine, *channel))
    {
        channel->bend = bend;
        channel->track_volume = volume;
        channel->user_pitch = pitch;
        channel->lpf = lpf;
        channel->biquad_type = static_cast<uint8_t>(biquad_type);
        channel->biquad_value = biquad_value;
        channel->output_line = output_line;
        for (int i = 0; i < Policy::kLfoCount; ++i)
        {
            channel->lfos[i].param = lfo_params_[i];
            channel->lfo_types[i] = lfo_types_[i];
        }
        Policy::SetOutputs(*channel, outputs);
    }
}

// ----------------------------------------------------------------------------------------------- SequenceSoundPlayer

template <typename Policy>
SequenceSoundPlayer<Policy>::SequenceSoundPlayer(int id, const typename Policy::SoundInfo& info,
                                                 std::span<const uint8_t> data, const Receiver* receiver)
    : id_(id),
      data_(data),
      allocate_track_flags_(info.allocate_track_flags),
      start_offset_(info.start_offset),
      receiver_(receiver)
{
    // What the archive player gives a sound from its info (SoundArchivePlayer::SetCommonSoundParam, 0x02295C98, and the
    // sequence's setup): the volume, a division by 127, the pan mode and curve, and the channels' priority.
    volume_ = Policy::PlayerVolume(info.volume);
    pan_mode_ = info.pan_mode;
    pan_curve_ = info.pan_curve;
    channel_priority_ = info.channel_priority;
    release_priority_fix_ = info.release_priority_fix;
    bank_items_ = info.banks;
    main_volume_.Init(127);
    local_vars_.fill(-1);
}

template <typename Policy>
int16_t* SequenceSoundPlayer<Policy>::Variable(Engine& engine, int index)
{
    if (index < 0)
    {
        return nullptr;
    }
    if (index < 16)
    {
        return &local_vars_[index];
    }
    if (index < 32)
    {
        return &engine.GlobalVariables()[index - 16];
    }

    return nullptr;
}

template <typename Policy>
void SequenceSoundPlayer<Policy>::Start()
{
    for (int i = 0; i < kTrackCount; ++i)
    {
        if (allocate_track_flags_ & (1u << i))
        {
            tracks_[i].emplace(i);
        }
    }

    if (tracks_[0])
    {
        tracks_[0]->SetSeqData(data_, start_offset_);
        tracks_[0]->Open();
    }

    started_ = true;
}

template <typename Policy>
void SequenceSoundPlayer<Policy>::Update(Engine& engine, int frames)
{
    if (!started_ || finished_)
    {
        return;
    }

    if constexpr (Policy::kFloatClock)
    {
        for (int frame = 0; frame < frames && !finished_; ++frame)
        {
            UpdateTick(engine, 1);
        }
    }
    else
    {
        UpdateTick(engine, frames);
    }

    if (finished_)
    {
        return;
    }

    UpdateTrackParams(engine);
}

template <typename Policy>
void SequenceSoundPlayer<Policy>::BeginTick()
{
    main_volume_.Update();
}

template <typename Policy>
bool SequenceSoundPlayer<Policy>::RunTrackTick(Engine& engine, int index)
{
    TrackType* track = GetTrack(index);
    if (!track)
    {
        return false;
    }

    track->UpdateChannelLength(*this, engine);
    if (track->ParseNextTick(*this, engine) < 0)
    {
        CloseTrack(engine, index);
        return false; // the track was freed, and reads as closed
    }

    return track->IsOpen();
}

template <typename Policy>
bool SequenceSoundPlayer<Policy>::ContinueTrackTick(Engine& engine, int index)
{
    TrackType* track = GetTrack(index);
    if (!track || !track->IsOpen())
    {
        return false;
    }

    if (track->ParseCommands(*this, engine) < 0)
    {
        CloseTrack(engine, index);
        return false;
    }

    return track->IsOpen();
}

template <typename Policy>
bool SequenceSoundPlayer<Policy>::EndTick(Engine& engine, bool any_open)
{
    if constexpr (requires { engine.OnTickEnd(*this); })
    {
        engine.OnTickEnd(*this);
    }

    if (!any_open)
    {
        FinishPlayer(engine);
        return true;
    }

    ++tick_count_;

    return false;
}

template <typename Policy>
void SequenceSoundPlayer<Policy>::UpdateTrackParams(Engine& engine)
{
    for (int i = 0; i < kTrackCount; ++i)
    {
        if (TrackType* track = GetTrack(i))
        {
            track->UpdateChannelParam(*this, engine);
        }
    }
}

template <typename Policy>
bool SequenceSoundPlayer<Policy>::IsSettled(const Engine& engine) const
{
    const auto settled = [&engine](const TrackType& track)
    {
        return track.WaitsForNotes(engine) || (track.IsStill() && !track.HasEndingNote(engine));
    };

    return tempo_ == 0 || (Policy::kLengthRules.timebase_zero_stops && timebase_ == 0) || EveryOpenTrack(settled);
}

template <typename Policy>
bool SequenceSoundPlayer<Policy>::IsStuck(const Engine& engine) const
{
    const auto stuck = [&engine](const TrackType& track)
    {
        return track.WaitsForEver(engine) || (track.IsStill() && track.IsQuiet());
    };

    return tempo_ == 0 || (Policy::kLengthRules.timebase_zero_stops && timebase_ == 0) || EveryOpenTrack(stuck);
}

template <typename Policy>
bool SequenceSoundPlayer<Policy>::IsMoving() const
{
    const auto still = [](const TrackType& track)
    {
        return !track.IsMoving();
    };

    return main_volume_.Moving() || !EveryOpenTrack(still);
}

template <typename Policy>
bool SequenceSoundPlayer<Policy>::IsMainTrack(const Engine& engine, int index) const
{
    for (int i = 0; i < index; ++i)
    {
        const TrackType* track = Track(i);
        if (track && track->IsOpen() && !track->IsIdle() && !track->WaitsForEver(engine) &&
            (i == 0 || !track->IsQuiet()))
        {
            return false;
        }
    }

    return true;
}

template <typename Policy>
bool SequenceSoundPlayer<Policy>::OthersRest(const Engine& engine, int index) const
{
    for (int i = 0; i < kTrackCount; ++i)
    {
        const TrackType* track = Track(i);
        if (i != index && track && track->IsOpen() && !track->IsIdle() && !track->WaitsForEver(engine))
        {
            return false;
        }
    }

    return true;
}

template <typename Policy>
bool SequenceSoundPlayer<Policy>::EveryOpenTrack(const std::function<bool(const TrackType& track)>& test) const
{
    bool open = false;
    for (const std::optional<TrackType>& track : tracks_)
    {
        if (track && track->IsOpen())
        {
            if (!test(*track))
            {
                return false;
            }

            open = true;
        }
    }

    return open || Policy::kLengthRules.no_track_passes;
}

template <typename Policy>
bool SequenceSoundPlayer<Policy>::RunTick(Engine& engine)
{
    BeginTick();

    bool any_open = false;
    for (int i = 0; i < kTrackCount; ++i)
    {
        if (RunTrackTick(engine, i))
        {
            any_open = true;
        }
    }

    return EndTick(engine, any_open);
}

template <typename Policy>
void SequenceSoundPlayer<Policy>::StartClock(int frames)
    requires(!Policy::kFloatClock)
{
    clock_left_ = static_cast<uint64_t>(frames) * Policy::kFrameUnits;
    clock_ticked_ = false;
}

template <typename Policy>
std::optional<uint64_t> SequenceSoundPlayer<Policy>::NextTickTime()
    requires(!Policy::kFloatClock)
{
    // The rate comes from the tempo before the frames' first tick, and from the tempo after the last tick later on. A
    // rate of 0 ends the frames and keeps the fraction.
    const float rate = Policy::TicksPerMs(timebase_, tempo_, tempo_ratio_);
    if (rate == 0.0f)
    {
        return std::nullopt;
    }

    // The first tick comes once the fraction of a tick left from the frame before has passed. Each later tick comes a
    // whole tick after the one before. A tick that the frames don't reach leaves its fraction to the next frame.
    const uint64_t next =
        clock_ticked_ ? ToUint64(Policy::kUnitsPerMs / rate) : ToUint64((Policy::kUnitsPerMs * tick_fraction_) / rate);
    if (next >= clock_left_)
    {
        tick_fraction_ = (static_cast<float>(next - clock_left_) * rate) * Policy::kMsPerUnit;
        return std::nullopt;
    }

    clock_left_ -= next;
    clock_ticked_ = true;

    return clock_left_;
}

template <typename Policy>
void SequenceSoundPlayer<Policy>::UpdateTick(Engine& engine, int frames)
{
    StartClock(frames);

    while (const std::optional<uint64_t> left = NextTickTime())
    {
        if constexpr (requires { engine.OnTickStart(*this, *left); })
        {
            engine.OnTickStart(*this, *left);
        }

        if (RunTick(engine))
        {
            return;
        }
    }
}

template <typename Policy>
void SequenceSoundPlayer<Policy>::StartClock([[maybe_unused]] int frames)
    requires Policy::kFloatClock
{
    // NW4R SND's clock counts each frame's milliseconds in floats (SeqPlayer::UpdateTick, 0x8003eec4 in Wii Sports'
    // main.dol). The clock of its build of October 2006 counts in 416ths of a tick (0x800e6588 in WarioWare: Smooth
    // Moves' main.dol). Each frame takes out the whole ticks first, then adds the tempo times the tempo ratio, then
    // runs the ticks that it took out. The product is a float, truncated to an integer (0x800e6618 to 0x800e664c). The
    // timebase plays no part.
    clock_rest_ms_ = Policy::kFrameMs;
    clock_ticked_ = false;

    if (Policy::CountsTickParts(build_))
    {
        clock_ticks_due_ = 0;
        while (tick_parts_ >= 416)
        {
            ++clock_ticks_due_;
            tick_parts_ -= 416;
        }

        const int32_t parts = Policy::ToInt32(static_cast<float>(tempo_) * tempo_ratio_);
        tick_parts_ = static_cast<uint16_t>(tick_parts_ + parts);
    }
}

template <typename Policy>
std::optional<uint64_t> SequenceSoundPlayer<Policy>::NextTickTime()
    requires Policy::kFloatClock
{
    // The ticks of the clock in 416ths all start with the frame.
    if (Policy::CountsTickParts(build_))
    {
        if (clock_ticks_due_ == 0)
        {
            return std::nullopt;
        }

        --clock_ticks_due_;
        return Policy::kFrameUnits;
    }

    // The first tick comes once the fraction of a tick left from the frame before has passed. Each later tick comes a
    // whole tick after the one before, at the tempo after it. The rest of a tick passes to the next frame. A rate of 0
    // ends the frame and keeps the fraction.
    const float rate = Policy::TicksPerMs(timebase_, tempo_, tempo_ratio_);
    if (rate == 0.0f)
    {
        return std::nullopt;
    }

    const float next = clock_ticked_ ? 1.0f / rate : tick_fraction_ / rate;
    if (!(next < clock_rest_ms_))
    {
        tick_fraction_ = (next - clock_rest_ms_) * rate;
        return std::nullopt;
    }

    clock_rest_ms_ -= next;
    clock_ticked_ = true;

    return ToUint64(clock_rest_ms_ * Policy::kUnitsPerMs);
}

template <typename Policy>
void SequenceSoundPlayer<Policy>::FinishPlayer(Engine& engine)
{
    started_ = false;

    for (int i = 0; i < kTrackCount; ++i)
    {
        CloseTrack(engine, i);
    }

    finished_ = true;
}

template <typename Policy>
void SequenceSoundPlayer<Policy>::CloseTrack(Engine& engine, int index)
{
    if (TrackType* track = GetTrack(index))
    {
        track->Close(*this, engine);
        tracks_[index].reset();
    }
}

} // namespace ntdlib::nwsnd
