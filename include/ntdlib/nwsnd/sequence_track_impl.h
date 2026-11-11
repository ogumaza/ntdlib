// SPDX-License-Identifier: MIT

// The definitions of SequenceTrack's templates in sequence.h, apart from its commands: the track's channels, its
// notes and its passes for a rip tool's length analysis. sequence_impl.h includes this file.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/envelope.h"
#include "ntdlib/nwsnd/sequence.h"

namespace ntdlib::nwsnd
{

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
void SequenceTrack<Policy>::SetSilence(Engine& engine, bool silence, int frames)
{
    silence_ = silence;
    const uint8_t target = silence ? 0 : 255;
    for (Channel* channel = First(engine); channel; channel = Next(engine, *channel))
    {
        // The ramp starts over from where it stands, as at a note's start.
        if (channel->silence_counter >= channel->silence_duration)
        {
            channel->silence_from = channel->silence_to;
        }
        else
        {
            const int32_t change = (static_cast<int32_t>(channel->silence_to) - channel->silence_from) *
                                   channel->silence_counter / channel->silence_duration;
            channel->silence_from = static_cast<uint8_t>(channel->silence_from + change);
        }
        channel->silence_to = target;
        channel->silence_counter = 0;
        channel->silence_duration = static_cast<uint16_t>(frames > 0 ? frames : 0);
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
void SequenceTrack<Policy>::CountLoop(Player& player, Engine& engine, uint32_t target)
{
    // The pass began when the track last ran `target`. A still pass also left the variables that tracks read with the
    // values that the pass before left. The first pass compares them with 0. A variable that no track reads doesn't
    // count. The game reads some, such as the beats that a timing track marks. The loop's pass is now the track's last
    // pass, and the idle pass of a poll before it no longer counts.
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
    polled_at_ = 0;

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
void SequenceTrack<Policy>::CountPoll(Player& player, uint32_t target)
{
    // After a pass that isn't idle, IsIdle follows the track's loops again.
    const uint64_t begun = target < run_.size() ? run_[target] : 0;
    const bool idle = changed_at_ < begun && set_at_ < begun && drew_at_ < begun;
    polled_at_ = idle ? player.commands_run_ : 0;
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
void SequenceTrack<Policy>::MarkPlayerChange(Player& player)
{
    player.changed_at_ = player.commands_run_;
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

    // Whether the newest note sounds at the volume of this note's velocity, and whether this note has the velocity of
    // the note that started or last continued the newest channel. The second can hold without the first where the
    // channel keeps the region's volume.
    const Channel* newest = First(engine);
    const bool same_volume = newest && newest->velocity_volume == velocity_volume();
    const bool same_velocity = newest && newest->place == note_channel_ && note_velocity_ == scaled;
    const bool same = newest && newest->key == key && (same_volume || same_velocity);
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
        Policy::SetReceiver(info, player.basic_.receiver);

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
    channel->pan_mode = player.basic_.pan_mode;
    channel->pan_curve = player.basic_.pan_curve;
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

    // The newest channel now plays this note.
    note_channel_ = channel->place;
    note_velocity_ = scaled;

    const bool changes = channel != newest || !same || sweep != 0.0f;
    if (!changes && !same_volume)
    {
        MarkPlayerChange(player);
    }

    return changes;
}

template <typename Policy>
void SequenceTrack<Policy>::UpdateChannelParam(Player& player, Engine& engine)
{
    if (!open_ || channel_list_ == kNoChannel)
    {
        return;
    }

    const float volume =
        Policy::TrackVolume(vol_.Get(), volume2_.Get(), player.main_volume_.Get(), volume_, player.basic_.volume);

    const float bend = (static_cast<float>(bend_.Get()) * 0.0078125f) * static_cast<float>(bend_range_);
    const float pitch = player.basic_.pitch * pitch_;
    const float lpf = lpf_ + player.basic_.lpf;
    int biquad_type = biquad_type_;
    float biquad_value = biquad_value_;
    if (player.basic_.biquad_type != -1)
    {
        biquad_type = player.basic_.biquad_type;
        biquad_value = player.basic_.biquad_value;
    }
    const uint32_t output_line = output_line_ != -1 ? static_cast<uint32_t>(output_line_) : player.basic_.output_line;

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
    const auto outputs = Policy::CombineOutputs(player.basic_.outputs, outputs_, pan, span, main_send, fx_send);

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

} // namespace ntdlib::nwsnd
