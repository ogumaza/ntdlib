// SPDX-License-Identifier: MIT

// The performance of one sequence (Perform in performer.h): reading the sequence from a sound archive, and the
// performer's passes over it.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <utility>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/settings.h"
#include "sequence_export/performer_records.h"

namespace ntdlib::sequence_export
{

// What the sequences of a piece share in their passes (PerformTogether): the least CC11 peak that each sequence's
// levels scale for, and the parts that each of its tracks may have. Without part limits, the passes fit the parts into
// MIDI's channels themselves (FitParts).
struct SharedPasses
{
    double peak = 0.0;
    std::optional<std::array<uint8_t, kTrackCount>> part_limits;
};

// A sequence's performance, with the CC11 peak that its levels were scaled for (LevelScale).
struct Passes
{
    Performance performance;
    double peak = 0.0;
};

// A sound's fields and its sequence's bytecode, in sar's form, for the performer: from a CSAR or FSAR archive
// (ReadSarSequence) or from a Wii archive (ReadWiiSequence).
struct SarSequence
{
    SoundInfo sound;
    Sequence seq;
};

// Reads sequence `sound_index` of a CSAR or FSAR archive, with the archive options of `settings`. Throws FormatError
// for a sequence that the archive and its group files don't hold.
SarSequence ReadSarSequence(const SoundArchive& archive, uint32_t sound_index, const Settings& settings);

// Reads sequence `sound_index` of the Wii's `archive`. The Wii's arguments are big-endian. Throws FormatError for a
// sequence that the archive doesn't hold.
SarSequence ReadWiiSequence(const rsar::SoundArchive& archive, uint32_t sound_index);

// Runs as many passes of a sequence's performance as it takes. The passes take what `shared` gives. The counted loops
// in `endless_loops` loop for ever. A pass stops at `stop_ms`, when it has one (Performer::SetStop).
Passes PerformPasses(const SoundInfo& sound, const Sequence& seq, Regions& banks, const PerformOptions& options,
                     const Settings& settings, const SharedPasses& shared = {},
                     const std::set<std::pair<int, uint32_t>>& endless_loops = {},
                     std::optional<double> stop_ms = std::nullopt);

// Plays a sequence once, as the first pass of PerformPasses does, and stops at `stop_ms` when there's one. What a pass
// finds for the next changes only the MIDI events, not what the sequence's commands do. One pass therefore gives the
// changes of the global variables and the variables read (PerformBeside).
Performance PlayOnce(const SarSequence& source, Regions& regions, const PerformOptions& options,
                     const Settings& settings, std::optional<double> stop_ms);

} // namespace ntdlib::sequence_export
