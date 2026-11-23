// SPDX-License-Identifier: MIT

// Definitions that the two parts of the performance of a piece share (perform_pieces.cpp and perform_beside.cpp): a
// sequence of a piece, the timing of its events, and the runs of a piece's sequences. The two files build as one
// translation unit. The definitions therefore keep internal linkage. Only those files include this header.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/instruments.h"
#include "ntdlib/sequence_export/passes.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/performer.h"
#include "ntdlib/sequence_export/rsar_instruments.h"
#include "ntdlib/sequence_export/settings.h"
#include "sequence_export/lengths.h"
#include "sequence_export/perform_sequence.h"
#include "sequence_export/performer_records.h"

namespace ntdlib::sequence_export
{

namespace
{

// How a sequence of a piece plays (PerformTogether): the passes of its main loop to play at least (PerformOptions'
// `loops`), and what it shares with the piece's other sequences.
struct PieceRun
{
    bool operator==(const PieceRun& other) const
    {
        return loops == other.loops && shared.peak == other.shared.peak &&
               shared.part_limits == other.shared.part_limits;
    }

    int loops = 1;
    SharedPasses shared;
};

// A sequence of a piece, how it plays and its performance.
struct PieceSequence
{
    SarSequence source;
    PieceRun run;
    Passes played;
};

// Performs a sequence of a piece as `run` gives.
Passes PlayPieceSequence(const SarSequence& source, const PieceRun& run, Regions& banks, const PerformOptions& options,
                         const Settings& settings)
{
    PerformOptions played = options;
    played.loops = run.loops;

    return PerformPasses(source.sound, source.seq, banks, played, settings, run.shared);
}

// Whether a track's `events` play a note.
bool TrackPlays(const std::vector<TrackEvent>& events)
{
    const auto note_on = [](const TrackEvent& e)
    {
        return e.kind == EventKind::kNoteOn;
    };

    return std::ranges::any_of(events, note_on);
}

// Whether `perf` plays a note.
bool PlaysNotes(const Performance& perf)
{
    return std::ranges::any_of(perf.tracks, TrackPlays);
}

// Whether `perf` has a main loop that the MIDI writer marks: one that ends after it starts.
bool MarksLoop(const Performance& perf)
{
    return perf.loop && perf.loop->end > perf.loop->start;
}

// The time in milliseconds at which the MIDI tempo of `perf` places fractional tick `tick`.
double TempoMs(const Performance& perf, double tick)
{
    const double timebase = std::max<uint32_t>(perf.timebase, 1);
    double ms = 0.0;
    double from = 0.0;
    double bpm = 120.0;
    for (const Performance::Tempo& change : perf.tempo)
    {
        if (change.tick >= tick)
        {
            break;
        }

        ms += (change.tick - from) * 60000.0 / (bpm * timebase);
        from = change.tick;
        bpm = change.bpm;
    }

    return ms + (tick - from) * 60000.0 / (bpm * timebase);
}

// The fractional tick at which the MIDI tempo of `perf` places the time `ms`.
double TempoTick(const Performance& perf, double ms)
{
    const double timebase = std::max<uint32_t>(perf.timebase, 1);
    double at_ms = 0.0;
    double from = 0.0;
    double bpm = 120.0;
    for (const Performance::Tempo& change : perf.tempo)
    {
        const double change_ms = at_ms + (change.tick - from) * 60000.0 / (bpm * timebase);
        if (change_ms >= ms)
        {
            break;
        }

        at_ms = change_ms;
        from = change.tick;
        bpm = change.bpm;
    }

    return from + (ms - at_ms) * bpm * timebase / 60000.0;
}

// Where the MIDI writer places event `e` of `perf`, in fractional ticks: at the start of its frame, even after its tick
// (seqlib::midi::FrameTiming::kFrameStart, the timing of the Wii's conversion).
double EventPlace(const Performance& perf, const TrackEvent& e)
{
    if (e.after > 0)
    {
        return e.tick + e.after;
    }

    return e.tick < perf.frame_ticks.size() ? perf.frame_ticks[e.tick] : static_cast<double>(e.tick);
}

// The timing of a piece's MIDI file: the sequence whose tempo it takes, which sequences place their ticks at that
// sequence's times, the frames of those ticks, and the piece's main loop and end in those ticks.
struct PieceTiming
{
    std::size_t lead = 0;
    std::vector<bool> timed;
    std::vector<double> frame_ticks;
    std::optional<Performance::Loop> loop;
    int loops = 0;
    uint32_t end_tick = 0;
    std::vector<std::string> warnings;
};

// The fractional place `place` in the ticks of a piece as an event's tick and the fraction after it (TrackEvent). The
// fraction is above 0 for any place above 0. The MIDI writer then puts the event at that place, not at its tick's
// frame.
std::pair<uint32_t, double> TickAndAfter(double place)
{
    if (place <= 0.0)
    {
        return {0, 0.0};
    }

    const double tick = std::ceil(place) - 1.0;

    return {static_cast<uint32_t>(tick), place - tick};
}

// How each of `sequences` has to play for its piece, `piece`, with `timing` (PerformTogether): every looping sequence
// to the piece's end, every sequence's levels at the scale of the loudest, and each track within the parts that the
// piece's MIDI channels leave it.
std::vector<PieceRun> PieceRuns(const std::vector<PieceSequence>& sequences, const PieceTiming& timing,
                                const Performance& piece);

} // namespace

} // namespace ntdlib::sequence_export
