// SPDX-License-Identifier: MIT

// SFZ output for the CSAR and FSAR formats of the 3DS, the Wii U and the Switch (see sfz.h).
//
// Each track file has a <master> per preset, selected by loprog/hiprog and, above 128 presets, bank select (CC0).
// Regions match the SoundFont: the engine's key/velocity precedence, sample, root key, tuning, volume, pan, overridden
// envelope, key groups (group/off_by) and loops. LFOs use a sine with depth from CC1.
//
// Controller curves map kSfzLowPassController to lpf_1p cutoff (filter 1), and kSfzBiquadController to biquad cutoff,
// resonance and gain. SFZ cannot bypass a biquad. Instead, hicc/locc select filtered or unfiltered regions at note-on.
//
// The low-pass must support being enabled during a note. It therefore stays active at sfizz's maximum 20 kHz cutoff
// when nominally off. This attenuates 10 kHz by about 0.5 dB and 16 kHz by 1 dB.

#include "ntdlib/sequence_export/sfz.h"

#include <cstddef>
#include <string>
#include <utility>

#include "ntdlib/sequence_export/instruments.h"
#include "sequence_export/sfz_track.h"

namespace ntdlib::sequence_export
{
namespace
{

// Writes velocity region opcodes using track settings `key`, except for sample, key/velocity ranges and pan. Adds
// `gain_db` to the region volume.
std::string RegionOpcodes(const PresetKey& key, const sar::VelocityRegion& r, const SfzWave& wave, double gain_db,
                          const Settings& settings)
{
    std::string s = SfzPitchOpcodes(r.original_key, r.pitch);
    s += SfzVolumeOpcode(r.volume, gain_db, false);
    s += SfzEnvelopeOpcodes(key, {r.adshr.attack, r.adshr.decay, r.adshr.sustain, r.adshr.hold, r.adshr.release},
                            r.ignore_note_off, wave, settings);

    if (r.key_group != 0)
    {
        s += " group=" + std::to_string(r.key_group) + " off_by=" + std::to_string(r.key_group);
    }

    // A region can select linear interpolation (1) on the Wii U's AX and on the 3DS's DSP, and none (2) on the DSP.
    // sfizz's sample_quality=1 is its linear interpolation. sample_quality=0 plays the nearest sample, and the DSP does
    // the same with none. Other regions keep sfizz's default, a cubic interpolation. Cubic interpolation is the closest
    // sfizz has to the Wii U's 4-tap filters and the DSP's polyphase filter. The Switch's regions keep the default.
    if (settings.engine != Engine::kSwitch && r.interpolation == 1)
    {
        s += " sample_quality=1";
    }
    else if (settings.engine == Engine::kCtr && r.interpolation == 2)
    {
        s += " sample_quality=0";
    }

    return s;
}

// Writes one line per region in preset `key`. Adds `gain_db` to each volume.
std::string PresetRegions(const PresetKey& key, BankSet& banks, SfzSampleSet& samples, double gain_db,
                          const Settings& settings)
{
    std::string s;
    const sar::Instrument* inst = banks.GetInstrument(key.bank_slot, key.program);
    if (!inst)
    {
        return s; // already reported by the SoundFont writer
    }

    static const SfzWave kNone;
    for (const PlayedRanges& played : RegionRanges(*inst))
    {
        const sar::VelocityRegion& r = *played.region;
        const auto wave_key = banks.WaveKey(key.bank_slot, r.wave_id_index);
        const SfzWave& wave =
            wave_key ? samples.Get(*wave_key, [&] { return banks.GetWave(key.bank_slot, r.wave_id_index); }) : kNone;
        if (wave.names.empty())
        {
            continue; // already reported by the SoundFont writer
        }

        // The engine's pan: (region pan + track initial pan - 64) / 63. Stereo channels get an additional -1 or +1.
        const double pan = (static_cast<int>(r.pan) + key.init_pan - 64) / 63.0;
        const std::string opcodes = RegionOpcodes(key, r, wave, gain_db, settings);
        for (const std::pair<int, int>& keys : played.keys)
        {
            for (const std::pair<int, int>& velocities : played.velocities)
            {
                for (std::size_t c = 0; c < wave.names.size(); c++)
                {
                    double p = pan;
                    if (wave.names.size() == 2)
                    {
                        p += c == 0 ? -1.0 : 1.0;
                    }

                    s += SfzRegionStart(wave.names[c], keys, velocities, p) + opcodes + "\n";
                }
            }
        }
    }

    return s;
}

} // namespace

SfzInstruments BuildSfz(const Performance& perf, const seqlib::midi::MidiReport& midi, BankSet& banks,
                        const std::string& title, const std::vector<std::string>& preset_names,
                        const Settings& settings, const std::string& program)
{
    SfzSampleSet samples;
    auto regions = [&](const PresetKey& key, double gain_db)
    {
        return PresetRegions(key, banks, samples, gain_db, settings);
    };

    SfzInstruments out = BuildTrackFiles(perf, midi, title, preset_names, settings, program, regions);
    out.samples = samples.Take();

    return out;
}

} // namespace ntdlib::sequence_export
