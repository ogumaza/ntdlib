// SPDX-License-Identifier: MIT

// The performer runs a sequence's bytecode and records its note, controller and preset events for MIDI output (see
// performance.h).
//
// Jumps, calls, variables, conditions and random arguments require execution rather than static translation. The
// performer follows the sequence player of the console that Settings describes: MML parsing, track state, tick
// scheduling, note lengths and note waits.

#pragma once

#include <cstdint>

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

// The preset key of a note of `program` of the bank in slot 0, on a track that keeps the settings that it starts with:
// no envelope overrides, an LFO of depth 0 and the centre as its initial pan.
PresetKey StartingPresetKey(uint32_t program, const Settings& settings);

} // namespace ntdlib::sequence_export
