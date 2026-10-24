// SPDX-License-Identifier: MIT

// SFZ output for the Wii's RSAR format (see sfz.h). sfz_track.cpp writes each track's file. This file writes the
// regions of NW4R SND's banks: their samples, envelopes, loops, and each stereo channel's place as its pan mode gives
// it. The regions have no key groups. From 2008, NW4R SND releases the other notes of a key group at their release
// rates. The MIDI note-offs give that release. The regions select no interpolation.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/midi/performance_writer.h"

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/resampling.h"
#include "ntdlib/sequence_export/rsar_instruments.h"
#include "ntdlib/sequence_export/settings.h"
#include "ntdlib/sequence_export/sfz.h"
#include "sequence_export/sfz_track.h"
#include "sequence_export/wii_pan.h"

namespace ntdlib::sequence_export
{
namespace
{

// Writes one line per region in preset `key`. Adds `gain_db` to each volume. A region's low-pass takes filter slot
// `resampling_slot`, or none with 0 (PresetRegionsFunction). `balance` plays stereo waves in balance pan mode, and
// `since_march_2008` applies the region's hold and note-off type.
std::string PresetRegions(const PresetKey& key, RsarBankSet& banks, SfzSampleSet& samples, double gain_db,
                          int resampling_slot, bool balance, bool since_march_2008, const Settings& settings)
{
    std::string s;
    const rsar::Bank* bank = banks.GetBank();
    if (!bank || !banks.HasInstrument(key.program))
    {
        return s; // already reported by the SoundFont writer
    }

    for (const auto& played : RegionRanges(*bank, key.program))
    {
        const rsar::Region& r = *played.region;
        const auto wave_index = static_cast<uint32_t>(r.wave_index);
        const SfzWave& wave = samples.Get(banks.WaveKey(wave_index), [&] { return banks.GetWave(wave_index); });
        if (wave.names.empty())
        {
            continue; // already reported by the SoundFont writer
        }

        // NW4R SND pan: (region pan + track initial pan - 64) / 63, for each channel of a stereo wave as its pan mode
        // places it.
        const double pan = (static_cast<int>(r.pan) + key.init_pan - 64) / 63.0;
        const std::string opcodes = SfzPitchOpcodes(r.original_key, r.tune) +
                                    SfzEnvelopeOpcodes(key, RegionEnvelope(r, since_march_2008),
                                                       since_march_2008 && r.note_off_type == 1, wave, settings);
        const ResamplingRegion resampling = {r.original_key, r.tune, wave.sample_rate, 0};
        const auto line = [&](std::size_t c, std::pair<int, int> keys, std::pair<int, int> velocities)
        {
            const ChannelPan place = WaveChannelPan(pan, c, wave.names.size(), balance);
            // A note whose volume LFO the voice holds at full level plays lower (PresetKey::lfo_cut).
            return SfzRegionStart(wave.names[c], keys, velocities, place.pan) +
                   SfzVolumeOpcode(r.volume, gain_db + place.gain_db - key.lfo_cut / 10.0, place.gain_db <= -144.0,
                                   key.lift) +
                   opcodes;
        };
        s +=
            RegionLines(played.keys, played.velocities, wave.names.size(), resampling, resampling_slot, settings, line);
    }

    return s;
}

} // namespace

SfzInstruments BuildSfz(const Performance& perf, const seqlib::midi::MidiReport& midi, RsarBankSet& banks,
                        const std::string& title, const std::vector<std::string>& preset_names, bool balance,
                        const Settings& settings, const std::string& program)
{
    SfzSampleSet samples;
    auto regions = [&](const PresetKey& key, double gain_db, int resampling_slot)
    {
        return PresetRegions(key, banks, samples, gain_db, resampling_slot, balance, perf.since_march_2008, settings);
    };

    SfzInstruments out = BuildTrackFiles(perf, midi, title, preset_names, settings, program, regions);
    out.samples = samples.Take();

    return out;
}

SfzInstruments BuildBankSfz(const std::vector<uint32_t>& programs, RsarBankSet& banks, const std::string& title,
                            bool balance, const Settings& settings, const std::string& program)
{
    SfzSampleSet samples;
    const bool since_march_2008 = settings.wii_build.SinceMarch2008();
    auto regions = [&](const PresetKey& key, double gain_db, int resampling_slot)
    {
        return PresetRegions(key, banks, samples, gain_db, resampling_slot, balance, since_march_2008, settings);
    };

    SfzInstruments out;
    out.files = BankFiles(programs, title, settings, program, regions);
    out.samples = samples.Take();

    return out;
}

} // namespace ntdlib::sequence_export
