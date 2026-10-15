// SPDX-License-Identifier: MIT

// Sequence conversion for the CSAR and FSAR formats (see convert.h). This file builds the SoundFont. seqlib writes the
// MIDI file, and sfz.cpp the SFZ files.
//
// Each performance preset combines a bank instrument with its track settings. Each such preset becomes one SoundFont
// instrument and preset, with a zone for each region. Zones contain the region's key and velocity ranges, sample, root
// key, tuning, volume, pan, envelope, key group and loop. Track overrides apply to the envelope. Presets used by MIDI
// channel 10 also get a copy in bank 128. Synths take drum instruments from that bank.

#include "ntdlib/sequence_export/convert.h"

#include <optional>
#include <utility>

#include "ntdlib/sequence_export/conversion.h"
#include "ntdlib/sequence_export/instruments.h"
#include "ntdlib/sequence_export/performer.h"
#include "seqlib/midi/performance_writer.h"
#include "seqlib/sf2/presets.h"
#include "sequence_export/sf2_build.h"
#include "sequence_export/zone_low_pass.h"

namespace ntdlib::sequence_export
{

namespace sf2 = seqlib::sf2;

namespace
{

// Builds a SoundFont instrument and preset for each performance preset. Adds each wave's samples only once.
class SoundFontBuilder : public SoundFontParts
{
public:
    // Adds `quieter` centibels of attenuation to every region (see SoundFontCut). The zones fit their cutoff for the
    // track's low-pass to the keys that `perf` plays (ZoneLowPass).
    SoundFontBuilder(BankSet& banks, double quieter, const Performance& perf, const Settings& settings)
        : banks_(banks), quieter_(quieter), settings_(settings), low_pass_(perf, settings)
    {
    }

    // Adds preset `index` at bank index / 128, program index % 128. The preset uses the bank instrument specified by
    // `key`.
    void Add(uint32_t index, const PresetKey& key, const std::string& name);

private:
    // The samples of wave `wave_id_index` of the bank in `slot` (two for stereo), made on first use. Returns an empty
    // list if the wave cannot be read.
    const std::vector<uint16_t>& WaveSamples(int slot, uint32_t wave_id_index, uint8_t root_key);

    BankSet& banks_;
    double quieter_;
    const Settings& settings_;
    ZoneLowPass low_pass_;
};

const std::vector<uint16_t>& SoundFontBuilder::WaveSamples(int slot, uint32_t wave_id_index, uint8_t root_key)
{
    static const std::vector<uint16_t> kNone;
    const auto key = banks_.WaveKey(slot, wave_id_index);
    if (!key)
    {
        return kNone;
    }

    return Samples(*key, root_key, [&] { return banks_.GetWave(slot, wave_id_index); });
}

void SoundFontBuilder::Add(uint32_t index, const PresetKey& key, const std::string& name)
{
    sf2::Instrument inst;
    inst.name = name;
    inst.zones.push_back(GlobalZone(key, settings_));

    const sar::Instrument* bank_inst = banks_.GetInstrument(key.bank_slot, key.program);
    if (!bank_inst)
    {
        warnings_.push_back(name + ": bank " + banks_.BankName(key.bank_slot) + " has no instrument " +
                            std::to_string(key.program));
    }

    for (const PlayedRanges& played : bank_inst ? RegionRanges(*bank_inst) : std::vector<PlayedRanges>())
    {
        const sar::VelocityRegion& r = *played.region;
        // A no-wave entry is silent in the game too.
        const std::vector<uint16_t>& ids = WaveSamples(key.bank_slot, r.wave_id_index, r.original_key);
        if (ids.empty() && !banks_.NoWave(key.bank_slot, r.wave_id_index))
        {
            warnings_.push_back(name + ": no wave for keys " + std::to_string(played.key_region->lo) + "-" +
                                std::to_string(played.key_region->hi));
        }
        if (ids.empty())
        {
            continue;
        }

        // The engine's pan: (region pan + track initial pan - 64) / 63. Stereo channels get an additional -1 or +1.
        const double pan = (static_cast<int>(r.pan) + key.init_pan - 64) / 63.0;
        const wave::Pcm& wave = *banks_.GetWave(key.bank_slot, r.wave_id_index);
        RegionGenerators g =
            ConvertRegion(key, {r.adshr.attack, r.adshr.decay, r.adshr.sustain, r.adshr.hold, r.adshr.release},
                          r.ignore_note_off, r.pitch, r.volume, r.original_key, wave.loop, quieter_, settings_);
        g.exclusive_class = r.key_group;
        const ResamplingRegion resampling = {r.original_key, r.pitch, wave.sample_rate, r.interpolation};
        const std::pair<uint32_t, uint32_t> wave_key = *banks_.WaveKey(key.bank_slot, r.wave_id_index);
        for (const std::pair<int, int>& keys : played.keys)
        {
            for (const ResamplingSpan& span : ZoneSpans(keys, resampling, key, settings_))
            {
                const std::optional<FittedCutoff> fitted =
                    low_pass_.Fit(index, key, span.keys, r.original_key, r.pitch, wave_key, wave);
                for (const std::pair<int, int>& velocities : played.velocities)
                {
                    for (std::size_t c = 0; c < ids.size(); c++)
                    {
                        double p = pan;
                        if (ids.size() == 2)
                        {
                            p += c == 0 ? -1.0 : 1.0;
                        }

                        sf2::Zone zone = RegionZone(g, span.keys, velocities, p, 0.0, ids[c]);
                        AddZoneLowPass(zone, span.filter, fitted, resampling, key, settings_);
                        inst.zones.push_back(std::move(zone));
                    }
                }
            }
        }
    }

    sf2::AddInstrumentPreset(sf_, index, name, std::move(inst));
}

} // namespace

Conversion ConvertSequence(const sar::SoundArchive& archive, uint32_t sound_index, const PerformOptions& options,
                           const Settings& settings, const std::string& program)
{
    const sar::SoundInfo& sound = archive.Sounds()[sound_index];
    BankSet banks(archive, sound, settings);
    Conversion c;
    c.performance = Perform(archive, sound_index, banks, options, settings);

    const seqlib::midi::MidiReport midi = WriteMidiFile(c, sound.name);

    // The attenuation that keeps the loudest region that the performance plays at full level (SoundFontCut).
    auto volume_of = [&banks](const PresetKey& key, int note, int velocity)
    {
        const sar::VelocityRegion* r = banks.FindRegion(key.bank_slot, key.program, note, velocity);
        return r ? static_cast<int>(r->volume) : 0;
    };
    const double quieter = SoundFontCut(c.performance, volume_of);

    SoundFontBuilder builder(banks, quieter, c.performance, settings);
    const std::vector<std::string> names = PresetNames(c.performance.presets, true);
    for (std::size_t i = 0; i < c.performance.presets.size(); i++)
    {
        builder.Add(static_cast<uint32_t>(i), c.performance.presets[i], names[i]);
    }
    for (uint32_t index : midi.channel_10_presets)
    {
        builder.AddChannel10Copy(index);
    }

    sf2::SoundFont& sf = builder.Result();
    std::string comment = "Converted by " + program + " from sequence " + sound.name + ". Banks:";
    for (int slot = 0; slot < 4; slot++)
    {
        if (banks.GetBank(slot))
        {
            comment += " b" + std::to_string(slot) + " = " + banks.BankName(slot) + ";";
        }
    }

    WriteSoundFont(c, sf, sound.name, comment, program);
    c.sfz = BuildSfz(c.performance, midi, banks, sound.name, names, settings, program);

    CollectWarnings(c, midi, builder.Warnings(), banks.Errors());

    return c;
}

} // namespace ntdlib::sequence_export
