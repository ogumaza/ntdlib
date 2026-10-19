// SPDX-License-Identifier: MIT

// The part of a performance that every performer records: seqlib's Performance, with the results that the command-line
// frame reports (PrintConversion). A performer's Performance derives from PerformanceBase and adds its presets.

#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "seqlib/midi/performance.h"

namespace ntdlib::sequence_export
{

// A recorded sequence: seqlib's Performance, with what the performer found while playing it.
struct PerformanceBase : seqlib::midi::Performance
{
    // The tick of the last MIDI event or tempo change, or 0 without either.
    uint32_t LastEventTick() const
    {
        uint32_t last = 0;
        for (const std::vector<seqlib::midi::TrackEvent>& events : tracks)
        {
            if (!events.empty())
            {
                last = std::max(last, events.back().tick);
            }
        }
        if (!tempo.empty())
        {
            last = std::max(last, tempo.back().tick);
        }

        return last;
    }

    // Removes the main loop: the performance then has no loop markers, and `loops` is 0.
    void DropLoop()
    {
        loop.reset();
        loops = 0;
    }

    // MIDI attenuation in dB. CC11 stops at 127. All CC11 values scale equally to fit. The tracks keep their balance.
    // SFZ adds this gain back.
    double level_cut_db = 0.0;

    double seconds = 0;     // length, release tails included
    int loops = 0;          // times the main loop was played
    bool truncated = false; // stopped at the time limit
    bool holds = false;     // holds until stopped by the game; subject to the hold limit
    std::vector<std::string> warnings;

    // Counts of features approximated or omitted in MIDI/SoundFont output.
    std::map<std::string, uint32_t> approximations;
};

} // namespace ntdlib::sequence_export
