// SPDX-License-Identifier: MIT

// A SoundFont preset and an SFZ region that play one sample over every key, for the DS drivers whose instruments are
// single samples.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/loop.h"
#include "seqlib/pcm.h"
#include "seqlib/sf2/presets.h"
#include "seqlib/sf2/samples.h"
#include "seqlib/sf2/soundfont.h"
#include "seqlib/sfz/opcodes.h"
#include "seqlib/sfz/track_file.h"
#include "seqlib/wav.h"

namespace ntdlib::sequence_export::detail
{

// Adds `pcm` to `sf` as the sample `sample_name`, at `rate` Hz with the root key `root_key`, and a preset at `index`
// named `name` whose instrument plays it: a global zone with seqlib's kGlobalZoneModulators, and one zone over every
// key.
inline void AddSamplePreset(seqlib::sf2::SoundFont& sf, uint32_t index, const std::string& name,
                            const std::string& sample_name, const seqlib::Pcm& pcm, uint32_t rate, uint8_t root_key)
{
    namespace sf2 = seqlib::sf2;

    const std::vector<uint16_t> ids =
        sf2::AddSamples(sf, seqlib::PrepareForSoundFont(pcm), sample_name, rate, root_key);

    sf2::Instrument instrument;
    instrument.name = name;
    sf2::Zone global;
    global.modulators.assign(sf2::kGlobalZoneModulators.begin(), sf2::kGlobalZoneModulators.end());
    instrument.zones.push_back(std::move(global));
    sf2::ZoneValues values;
    values.loop = pcm.loop;
    values.root_key = root_key;
    values.sample = ids.at(0);
    instrument.zones.push_back(sf2::RegionZone(values));

    sf2::AddInstrumentPreset(sf, index, name, std::move(instrument));
}

// Adds `pcm` to `samples` as the WAV file `file` at `rate` Hz, and gives the SFZ region that plays it over every key
// with the root key `root_key`. The sound unit plays samples without interpolation. The region therefore takes
// sample_quality=0.
inline std::string AddSampleRegion(std::vector<seqlib::sfz::Sample>& samples, const std::string& file,
                                   const seqlib::Pcm& pcm, uint32_t rate, int root_key)
{
    const seqlib::PreparedWave prepared = seqlib::PrepareForSfz(pcm, seqlib::SfzLoopCrossfade(rate));
    samples.push_back({file, seqlib::WavFile(prepared.channels[0], rate)});

    return "<region> sample=" + file + " lokey=0 hikey=127 pitch_keycenter=" + std::to_string(root_key) +
           " sample_quality=0" +
           seqlib::sfz::LoopOpcodes(prepared.loop, prepared.loop_start, prepared.loop ? prepared.loop_end - 1 : 0) +
           "\n";
}

} // namespace ntdlib::sequence_export::detail
