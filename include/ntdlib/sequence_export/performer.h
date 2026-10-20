// SPDX-License-Identifier: MIT

// The performer runs a sequence's bytecode and records its note, controller and preset events for MIDI output (see
// performance.h).
//
// Jumps, calls, variables, conditions and random arguments require execution rather than static translation. The
// performer follows the sequence player of the console that Settings describes: MML parsing, track state, tick
// scheduling, note lengths and note waits.

#pragma once

#include <cstdint>
#include <span>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/instruments.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/rsar_instruments.h"
#include "ntdlib/sequence_export/settings.h"

namespace ntdlib::sequence_export
{

// Performs sequence `sound_index` from `archive` with the banks in `banks`. The performance follows the console that
// `settings` describes. Throws FormatError for corrupt data.
Performance Perform(const sar::SoundArchive& archive, uint32_t sound_index, BankSet& banks,
                    const PerformOptions& options, const Settings& settings);

// Performs sequence `sound_index` from the Wii's `archive` with its bank in `banks`. The performance follows the build
// of NW4R SND in `settings`. Throws FormatError for corrupt data.
Performance Perform(const rsar::SoundArchive& archive, uint32_t sound_index, RsarBankSet& banks,
                    const PerformOptions& options, const Settings& settings);

// Performs sequences `sound_indices` from the Wii's `archive` as one piece, with their bank in `banks`. A game can
// start several sequences in one sound frame and play them as one piece. One example is a sequence for each channel of
// the sound chip that the music was written for. The sequences share their bank and their pan mode. Each plays as it
// does alone, with `options`, from the frame where the piece starts.
//
// The piece holds each sequence's tracks with notes, in the order of `sound_indices`, each named after its sequence
// and its track ("<label> Track <n>"). It takes the tempo of the first sequence with notes. Another sequence's events
// move to the places where that tempo puts their times. The piece's levels share one scale, and its tracks share
// MIDI's channels (FitParts). A sequence that loops marks its loop on the first pass that the game's next pass
// repeats. The piece marks the latest of those passes. Each looping sequence then plays to the piece's end. Where the
// sequences' loops don't fit together, the piece has no loop, with a warning. The performance holds no records for an
// editor (Performance's `stretches`, `note_commands` and `mix`). Throws FormatError for corrupt data, or when the
// tracks with notes are more than 16. Throws std::invalid_argument when `sound_indices` is empty or the sequences don't
// share their bank and pan mode.
Performance PerformTogether(const rsar::SoundArchive& archive, std::span<const uint32_t> sound_indices,
                            RsarBankSet& banks, const PerformOptions& options, const Settings& settings);

// The preset key of a note of `program` of the bank in slot 0, on a track that keeps the settings that it starts with:
// no envelope overrides, an LFO of depth 0 and the centre as its initial pan.
PresetKey StartingPresetKey(uint32_t program, const Settings& settings);

} // namespace ntdlib::sequence_export
