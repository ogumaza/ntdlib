// SPDX-License-Identifier: MIT

// The SFZ text that NintendoWare's bank models share: what each track's file adds to seqlib's track file
// (seqlib/sfz/track_file.h). That's the filters' curves and opcodes, each preset's LFO, and the biquad's groups. The
// bank model writes each preset's regions, from the samples and the parts of a region's line below.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/sequence_export/envelope.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/resampling.h"
#include "ntdlib/sequence_export/settings.h"
#include "ntdlib/sequence_export/sfz.h"
#include "ntdlib/wave/decode.h"
#include "seqlib/midi/performance_writer.h"

namespace ntdlib::sequence_export
{

// A wave's samples in the SFZ files: one WAV file for each of its first two channels, and its loop. No names means that
// the wave can't be read.
struct SfzWave
{
    std::vector<std::string> names;
    uint32_t sample_rate = 0;
    bool loop = false;
    uint32_t loop_start = 0, loop_end = 0; // inclusive: SFZ requires that
};

// The samples that a sequence's SFZ files share, made on first use. A wave's key names its samples: "w0_12.wav", or
// "w0_12L.wav" and "w0_12R.wav" for stereo.
class SfzSampleSet
{
public:
    // The samples of the wave that `key` names. On first use, `pcm` gives the decoded wave, or nullptr for one that
    // can't be read.
    const SfzWave& Get(std::pair<uint32_t, uint32_t> key, const std::function<const wave::Pcm*()>& pcm);

    // Takes the samples made so far. The collection is left empty.
    std::vector<SfzSample> Take()
    {
        return std::exchange(samples_, {});
    }

private:
    std::map<std::pair<uint32_t, uint32_t>, SfzWave> waves_;
    std::vector<SfzSample> samples_;
};

// The start of a region's line: its sample, keys and velocities, and its pan from -1 (left) to 1 (right).
std::string SfzRegionStart(const std::string& sample, std::pair<int, int> keys, std::pair<int, int> velocities,
                           double pan);

// A region's pitch: its root key and its tuning ratio.
std::string SfzPitchOpcodes(uint8_t original_key, float tune);

// A region's volume, from its volume over 127, raised by `gain_db`. `silent` gives the lowest volume. No opcode means
// 0 dB. `lift` raises the volume over 127 by that many centibels, up to full level (PresetKey::lift).
std::string SfzVolumeOpcode(uint8_t volume, double gain_db, bool silent, int16_t lift = 0);

// A region's amplitude envelope, from its values `env` after the overrides in preset `key`, and the loop of `wave`. A
// region that ignores note-off keeps its decay after the note-off, as in the SoundFont.
std::string SfzEnvelopeOpcodes(const PresetKey& key, const EnvelopeValues& env, bool ignore_note_off,
                               const SfzWave& wave, const Settings& settings);

// The opcodes of the low-pass `filter` in filter slot `slot` (1 or 2) of a region that plays `region`: the cutoff at
// the region's original key, which tracks the key at 100 cents a key.
std::string SfzResamplingOpcodes(const ResamplingFilter& filter, const ResamplingRegion& region, int slot);

// Writes the regions of preset `key`, one line each. `gain_db` raises each volume. `resampling_slot` is the filter slot
// (1 or 2) that the regions' low-passes take (resampling.h), or 0 where the track's filters take both slots. The
// regions then have no low-pass.
using PresetRegionsFunction = std::function<std::string(const PresetKey& key, double gain_db, int resampling_slot)>;

// Returns the SFZ file of each track that has a MIDI channel in `midi`. `regions` writes each preset's regions. The
// caller adds the samples.
SfzInstruments BuildTrackFiles(const Performance& perf, const seqlib::midi::MidiReport& midi, const std::string& title,
                               const std::vector<std::string>& preset_names, const Settings& settings,
                               const std::string& program, const PresetRegionsFunction& regions);

} // namespace ntdlib::sequence_export
