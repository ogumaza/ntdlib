// SPDX-License-Identifier: MIT

#include "ntdlib/jaudio/game_lengths.h"

#include <algorithm>
#include <cmath>
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
        if (!first_sample_ && (block[2 * i] != 0 || block[2 * i + 1] != 0))
        {
            first_sample_ = frames_ + i;
        }

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
        return Looped(event.frame, finder_->PassFrames());
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

    // The sound ends once `ring_ms` has passed without a sound after the release, or 10 s after the release.
    if (!held && finder_ && !released_)
    {
        released_ = frames_;
    }

    if (released_ &&
        (Ms(frames_ - std::max(*released_, last_sound_)) >= rules_.ring_ms || Ms(frames_ - *released_) >= kStillMs))
    {
        return Ended();
    }

    // A sound that holds loops where it holds, if it still sounds.
    if (first_sound_ && finder_ && finder_->Holds(frames_))
    {
        if (Ms(frames_ - last_sound_) <= rules_.holding_ms)
        {
            return Heard({.length_ms = Ms(frames_), .fade_ms = rules_.loop_fade_ms});
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

    return Heard({.length_ms = rules_.limit_length_ms, .fade_ms = rules_.loop_fade_ms, .at_limit = true});
}

long long SoundLength::Ms(uint64_t frames) const
{
    return FramesToMs(frames, rules_.sample_rate);
}

xsflib::rip::Timing SoundLength::Ended() const
{
    if (!first_sound_)
    {
        return {.silent = true};
    }

    return Heard({.length_ms = Ms(last_sound_ + 1)});
}

xsflib::rip::Timing SoundLength::Looped(uint64_t frame, uint64_t pass) const
{
    constexpr auto kPasses = static_cast<uint64_t>(xsflib::rip::kLengthLoops);
    xsflib::rip::Timing timing = Heard({.length_ms = Ms(frame), .fade_ms = rules_.loop_fade_ms});

    // The frames by which two passes would start before the analysis: a first pass that is shorter than the second.
    const uint64_t early = kPasses * pass > frame ? kPasses * pass - frame : 0;
    if (!xsflib::rip::KeepsPass(pass, static_cast<double>(Ms(early))))
    {
        return timing;
    }

    // Passes that would start before the analysis start at 0, and the pass keeps its length.
    const long long pass_ms = Ms(pass);
    const long long start_ms = std::max(timing.length_ms - xsflib::rip::kLengthLoops * pass_ms, 0LL);
    timing.loop = xsflib::rip::LoopPass{.start_ms = start_ms, .length_ms = pass_ms};

    return timing;
}

xsflib::rip::Timing SoundLength::Heard(xsflib::rip::Timing timing) const
{
    const auto rounded_ms = [this](uint64_t frame)
    {
        return std::llround(static_cast<double>(frame) * 1000.0 / static_cast<double>(rules_.sample_rate));
    };

    if (first_sample_)
    {
        timing.first_sample_ms = rounded_ms(*first_sample_);
    }

    if (first_sound_)
    {
        timing.first_sound_ms = rounded_ms(*first_sound_);
    }

    return timing;
}

} // namespace ntdlib::jaudio
