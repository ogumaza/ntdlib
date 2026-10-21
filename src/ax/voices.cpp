// SPDX-License-Identifier: MIT

// AX's voice allocation (see voices.h).

#include "ntdlib/ax/voices.h"

#include <cstddef>
#include <cstdint>
#include <optional>

#include "ntdlib/ax/ax.h"

namespace ntdlib::ax
{

Voices::Voices(int count)
    : next_(static_cast<std::size_t>(count), kNone),
      prev_(static_cast<std::size_t>(count), kNone),
      priority_(static_cast<std::size_t>(count), kPriorityFree)
{
    head_.fill(kNone);
    tail_.fill(kNone);

    for (uint32_t voice = 0; voice < static_cast<uint32_t>(count); voice++)
    {
        Push(voice, kPriorityFree);
    }
}

std::optional<Voices::Taken> Voices::Take(uint32_t priority)
{
    if (const uint32_t top = head_[kPriorityFree]; top != kNone)
    {
        Remove(top);
        return Taken{top, false};
    }

    for (uint32_t lower = kPriorityMin; lower < priority; lower++)
    {
        if (const uint32_t oldest = tail_[lower]; oldest != kNone)
        {
            Remove(oldest);
            return Taken{oldest, true};
        }
    }

    return std::nullopt;
}

void Voices::Push(uint32_t voice, uint32_t priority)
{
    next_[voice] = head_[priority];
    prev_[voice] = kNone;

    if (head_[priority] != kNone)
    {
        prev_[head_[priority]] = voice;
    }
    else
    {
        tail_[priority] = voice;
    }

    head_[priority] = voice;
    priority_[voice] = priority;
}

void Voices::Remove(uint32_t voice)
{
    // A voice that's in no list stays out of them.
    const uint32_t priority = priority_[voice];
    if (prev_[voice] == kNone && head_[priority] != voice)
    {
        return;
    }

    if (prev_[voice] != kNone)
    {
        next_[prev_[voice]] = next_[voice];
    }
    else
    {
        head_[priority] = next_[voice];
    }

    if (next_[voice] != kNone)
    {
        prev_[next_[voice]] = prev_[voice];
    }
    else
    {
        tail_[priority] = prev_[voice];
    }

    next_[voice] = kNone;
    prev_[voice] = kNone;
}

} // namespace ntdlib::ax
