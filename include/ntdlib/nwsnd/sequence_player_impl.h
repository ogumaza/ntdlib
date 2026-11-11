// SPDX-License-Identifier: MIT

// The definitions of SequenceSoundPlayer's templates in sequence.h. sequence_impl.h includes this file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>

#include "ntdlib/nwsnd/sequence.h"

namespace ntdlib::nwsnd
{

template <typename Policy>
SequenceSoundPlayer<Policy>::SequenceSoundPlayer(int id, const typename Policy::SoundInfo& info,
                                                 std::span<const uint8_t> data, const Receiver* receiver)
    : id_(id),
      data_(data),
      allocate_track_flags_(info.allocate_track_flags),
      start_offset_(info.start_offset),
      basic_{.outputs = {}, .receiver = receiver}
{
    // What the archive player gives a sound from its info (SoundArchivePlayer::SetCommonSoundParam, 0x02295C98, and the
    // sequence's setup): the volume, a division by 127, the pan mode and curve, and the channels' priority.
    basic_.volume = Policy::PlayerVolume(info.volume);
    basic_.pan_mode = info.pan_mode;
    basic_.pan_curve = info.pan_curve;
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
            tracks_[i]->volume_ = start_track_volumes_[static_cast<std::size_t>(i)];
        }
    }

    for (const TrackVariable& write : pending_track_variables_)
    {
        if (TrackType* owner = GetTrack(write.track))
        {
            owner->vars_[static_cast<std::size_t>(write.index)] = write.value;
        }
    }
    pending_track_variables_.clear();

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
