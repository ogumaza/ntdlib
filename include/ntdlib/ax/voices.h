// SPDX-License-Identifier: MIT

// AX's voice allocation in the Wii's and the Wii U's AX libraries (AXAcquireVoice, AXFreeVoice and AXSetVoicePriority):
// a free stack, and a list for each priority from its newest voice to its oldest. The lists hold voice indices. A copy
// of a Voices is therefore a snapshot of the allocation.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "ntdlib/ax/ax.h"

namespace ntdlib::ax
{

class Voices
{
public:
    // A voice that Take found, and whether it belonged to another owner.
    struct Taken
    {
        uint32_t voice = 0;
        bool stolen = false;
    };

    // `count` voices, all free. They go onto the free stack in index order. The first voice taken is then the last.
    explicit Voices(int count = kVoiceCount);

    // The voice that AXAcquireVoice takes for `priority`: the top of the free stack, or else the oldest voice of the
    // lowest priority below `priority`. The voice leaves its list. The caller then runs the old owner's callback and
    // pushes the voice onto `priority`'s list. Both consoles' code does that too. Returns nothing when every voice has
    // `priority` or higher.
    std::optional<Taken> Take(uint32_t priority);

    // Puts a voice that's in no list at the newest end of `priority`'s list. Priority 0's list is the free stack, and
    // its newest end is its top.
    void Push(uint32_t voice, uint32_t priority);

    // Takes a voice out of its list. A voice that's in no list stays out.
    void Remove(uint32_t voice);

    // The priority of the list that holds `voice`, or of the last list that held it.
    uint32_t Priority(uint32_t voice) const
    {
        return priority_[voice];
    }

    // Calls f(voice) for each voice of `priority`, from the newest, or from the oldest. The free stack's newest voice
    // is its top. `f` mustn't change the lists.
    template <typename F>
    void ForEachNewestFirst(uint32_t priority, const F& f) const
    {
        for (uint32_t voice = head_[priority]; voice != kNone; voice = next_[voice])
        {
            f(voice);
        }
    }

    template <typename F>
    void ForEachOldestFirst(uint32_t priority, const F& f) const
    {
        for (uint32_t voice = tail_[priority]; voice != kNone; voice = prev_[voice])
        {
            f(voice);
        }
    }

private:
    static constexpr uint32_t kNone = 0xffffffff;

    // Each list's newest voice (head) and oldest (tail), and each voice's older neighbour (next) and newer one (prev).
    std::array<uint32_t, kPriorityMax + 1> head_;
    std::array<uint32_t, kPriorityMax + 1> tail_;
    std::vector<uint32_t> next_;
    std::vector<uint32_t> prev_;
    std::vector<uint32_t> priority_;
};

} // namespace ntdlib::ax
