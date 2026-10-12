// SPDX-License-Identifier: MIT

// The part of a performance that every performer records: seqlib's Performance, with the results that the command-line
// frame reports (PrintConversion). A performer's Performance derives from PerformanceBase and adds its presets.

#pragma once

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
