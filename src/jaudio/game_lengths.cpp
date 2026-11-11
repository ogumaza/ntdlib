// SPDX-License-Identifier: MIT

#include "ntdlib/jaudio/game_lengths.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>

#include "xsflib/rip/length.h"

#include "ntdlib/jaudio/loop_finder.h"

namespace ntdlib::jaudio
{

SoundLength::SoundLength(const GameLengthRules& rules) : rules_(rules)
{
}

bool SoundLength::Running() const
{
    return Ms(frames_) < rules_.limit_ms;
}

uint64_t SoundLength::Frames() const
{
    return frames_;
}

bool SoundLength::Following() const
{
    return finder_.has_value();
}

void SoundLength::Follow(uint32_t root, bool (*changes_sound)(uint8_t command), bool idle_in_calls,
                         bool restarts_as_passes)
{
    finder_.emplace(root, rules_.sample_rate, changes_sound, idle_in_calls, restarts_as_passes);
}

void SoundLength::MarkSound()
{
    last_sound_ = frames_;
    first_sound_ = first_sound_.value_or(frames_);
}

void SoundLength::Hear(std::span<const int16_t> block)
{
    for (std::size_t i = 0; i < block.size() / 2; i++)
    {
        if (xsflib::rip::Audible(block.subspan(2 * i, 2)))
        {
            last_sound_ = frames_ + i;
            first_sound_ = first_sound_.value_or(last_sound_);
        }
    }
}

std::optional<xsflib::rip::Timing> SoundLength::Take(const TrackEvent& event)
{
    if (!finder_)
    {
        return std::nullopt;
    }

    switch (finder_->Take(event, {first_sound_.has_value(), last_sound_}))
    {
    case LoopFinder::Verdict::kLoops:
        return xsflib::rip::Timing{Ms(event.frame), rules_.loop_fade_ms, false};
    case LoopFinder::Verdict::kEnded:
        return Ended();
    case LoopFinder::Verdict::kNone:
        break;
    }

    return std::nullopt;
}

std::optional<xsflib::rip::Timing> SoundLength::EndBlock(bool held,
                                                         const std::function<TrackWait(uint32_t track)>& read_wait)
{
    frames_ += rules_.block_frames;
    if (finder_)
    {
        finder_->TakeWaits(frames_, read_wait);
    }

    // The sound ends once `tail_ms` has passed without a sound after the release, or 10 s after the release.
    if (!held && finder_ && !released_)
    {
        released_ = frames_;
    }

    if (released_ &&
        (Ms(frames_ - std::max(*released_, last_sound_)) >= rules_.tail_ms || Ms(frames_ - *released_) >= kStillMs))
    {
        return Ended();
    }

    // A sound that holds loops where it holds, if it still sounds.
    if (first_sound_ && finder_ && finder_->Holds(frames_))
    {
        if (Ms(frames_ - last_sound_) <= rules_.holding_ms)
        {
            return xsflib::rip::Timing{Ms(frames_), rules_.loop_fade_ms, false};
        }

        return Ended();
    }

    if (first_sound_ && Ms(frames_ - last_sound_) >= rules_.silence_ms)
    {
        return Ended();
    }

    return std::nullopt;
}

xsflib::rip::Timing SoundLength::AtLimit() const
{
    if (!first_sound_)
    {
        return Ended();
    }

    return {rules_.limit_ms, rules_.loop_fade_ms, false};
}

long long SoundLength::Ms(uint64_t frames) const
{
    return FramesToMs(frames, rules_.sample_rate);
}

xsflib::rip::Timing SoundLength::Ended() const
{
    if (!first_sound_)
    {
        return {rules_.tail_ms, 0, true};
    }

    return {Ms(last_sound_ + 1) + rules_.tail_ms, 0, false};
}

} // namespace ntdlib::jaudio
