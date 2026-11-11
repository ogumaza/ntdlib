// SPDX-License-Identifier: MIT

// Sequence conversion for the CSAR and FSAR formats (see convert.h): of a sequence, or of sequences that play together
// as one piece. This file builds the SoundFont. seqlib writes the MIDI file, and sfz.cpp the SFZ files.
//
// Each performance preset combines a bank instrument with its track settings. Each such preset becomes one SoundFont
// instrument and preset, with a zone for each region. Zones contain the region's key and velocity ranges, sample, root
// key, tuning, volume, pan, envelope, key group and loop. Track overrides apply to the envelope. Presets used by MIDI
// channel 10 also get a copy in bank 128. Synths take drum instruments from that bank.

#include "ntdlib/sequence_export/convert.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/midi/performance_writer.h"
#include "seqlib/sf2/presets.h"
#include "seqlib/sf2/soundfont.h"

#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/conversion.h"
#include "ntdlib/sequence_export/instruments.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/performer.h"
#include "ntdlib/sequence_export/resampling.h"
#include "ntdlib/sequence_export/settings.h"
#include "ntdlib/sequence_export/sfz.h"
#include "ntdlib/wave/decode.h"
#include "sequence_export/sf2_build.h"
#include "sequence_export/zone_low_pass.h"

namespace ntdlib::sequence_export
{

namespace sf2 = seqlib::sf2;

namespace
{

// Whether `keep` takes a zone of any of `played`'s key ranges and velocity ranges.
bool KeepsAny(const PlayedRanges& played, const ZoneFilter& keep)
{
    for (const std::pair<int, int>& keys : played.keys)
    {
        for (const std::pair<int, int>& velocities : played.velocities)
        {
            if (keep(keys, velocities))
            {
                return true;
            }
        }
    }

    return false;
}

// Builds a SoundFont instrument and preset for each performance preset. Adds each wave's samples only once.
class SoundFontBuilder : public SoundFontParts
{
public:
    // Adds `quieter` centibels of attenuation to every region (see SoundFontCut). The zones fit their cutoff for the
    // track's low-pass to the keys that `perf` plays (ZoneLowPass). `played`, if set, leaves out each zone that no note
    // picks, and each wave that only such zones play.
    SoundFontBuilder(BankSet& banks, double quieter, const Performance& perf, const Settings& settings,
                     const PlayedNotes* played = nullptr)
        : banks_(banks), quieter_(quieter), settings_(settings), low_pass_(perf, settings), played_(played)
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
    const PlayedNotes* played_;
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

    ZoneFilter keep;
    if (played_ != nullptr)
    {
        keep = [this, index](std::pair<int, int> keys, std::pair<int, int> velocities)
        {
            return played_->Picks(index, keys, velocities);
        };
    }

    for (const PlayedRanges& played : bank_inst ? RegionRanges(*bank_inst) : std::vector<PlayedRanges>())
    {
        if (keep && !KeepsAny(played, keep))
        {
            continue;
        }

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
        RegionGenerators g = ConvertRegion(key, RegionEnvelope(r), r.ignore_note_off, r.pitch, r.volume, r.original_key,
                                           wave.loop, quieter_, settings_);
        g.exclusive_class = r.key_group;
        const ResamplingRegion resampling = {r.original_key, r.pitch, wave.sample_rate, r.interpolation,
                                             key.at_pitch_limits};
        const auto zone = [&](std::size_t c, std::pair<int, int> keys, std::pair<int, int> velocities)
        {
            double p = pan;
            if (ids.size() == 2)
            {
                p += c == 0 ? -1.0 : 1.0;
            }

            return RegionZone(g, keys, velocities, p, 0.0, ids[c]);
        };
        AddRegionZones(inst.zones, index, key, played.keys, played.velocities, ids.size(), resampling,
                       *banks_.WaveKey(key.bank_slot, r.wave_id_index), wave, low_pass_, settings_, zone, keep);
    }

    sf2::AddInstrumentPreset(sf_, index, name, std::move(inst));
}

// Builds the MIDI file, the SoundFont and the SFZ files of `performance`, which plays the banks in `banks`. `title`
// names the files. `source` says in the SoundFont's comment what the performance plays, such as "sequence <label>".
Conversion ConvertPerformance(Performance performance, BankSet& banks, const std::string& title,
                              const std::string& source, const Settings& settings, const std::string& program)
{
    Conversion c;
    c.performance = std::move(performance);

    const seqlib::midi::MidiReport midi = WriteMidiFile(c, title);

    // The attenuation that keeps the loudest region that the performance plays at full level (SoundFontCut).
    auto volume_of = [&banks](const PresetKey& key, int note, int velocity)
    {
        const sar::VelocityRegion* r = banks.FindRegion(key.bank_slot, key.program, note, velocity);
        return r ? static_cast<int>(r->volume) : 0;
    };
    const double quieter = SoundFontCut(c.performance, volume_of);

    const std::vector<std::string> names = PresetNames(c.performance.presets, true);
    std::optional<SoundFontBuilder> builder;
    const auto build = [&](const PlayedNotes* played)
    {
        builder.emplace(banks, quieter, c.performance, settings, played);
        for (std::size_t i = 0; i < c.performance.presets.size(); i++)
        {
            builder->Add(static_cast<uint32_t>(i), c.performance.presets[i], names[i]);
        }
        for (uint32_t index : midi.channel_10_presets)
        {
            builder->AddChannel10Copy(index);
        }
    };
    build(nullptr);

    // A SoundFont holds each preset's whole instrument. Many presets of large instruments can pass the format's limits.
    // A sequence plays few of their keys. The SoundFont then holds only the zones that the notes pick. A synth plays
    // the same from it.
    const PlayedNotes played(c.performance);
    std::vector<std::string> builder_warnings;
    if (!FitsSoundFont(builder->Result()))
    {
        build(&played);
        builder_warnings.push_back(
            "the SoundFont holds only the zones that the sequence plays: with every zone, it would pass the format's "
            "limits");
    }

    sf2::SoundFont& sf = builder->Result();
    std::string comment = "Converted by " + program + " from " + source + ". Banks:";
    for (int slot = 0; slot < sar::kBankSlots; slot++)
    {
        if (banks.GetBank(slot))
        {
            comment += " b" + std::to_string(slot) + " = " + banks.BankName(slot) + ";";
        }
    }
    for (int slot = 0; banks.Companion() && slot < sar::kBankSlots; slot++)
    {
        if (banks.GetBank(BankSet::kCompanionSlot + slot))
        {
            comment += " companion b" + std::to_string(slot) + " = " + banks.Companion()->BankName(slot) + ";";
        }
    }

    WriteSoundFont(c, sf, title, comment, program);
    c.sfz = BuildSfz(c.performance, midi, banks, title, names, settings, program);

    builder_warnings.insert(builder_warnings.end(), builder->Warnings().begin(), builder->Warnings().end());
    std::vector<std::string> bank_errors = banks.Errors();
    if (banks.Companion())
    {
        bank_errors.insert(bank_errors.end(), banks.Companion()->Errors().begin(), banks.Companion()->Errors().end());
    }
    CollectWarnings(c, midi, builder_warnings, bank_errors);

    return c;
}

} // namespace

Conversion ConvertSequence(const sar::SoundArchive& archive, uint32_t sound_index, const PerformOptions& options,
                           const Settings& settings, const std::string& program)
{
    // A game's code can give the sequence other banks as it starts it (PerformOptions::banks).
    sar::SoundInfo sound = archive.Sounds()[sound_index];
    if (options.banks)
    {
        sound.banks.assign(options.banks->begin(), options.banks->end());
    }

    BankSet banks(archive, sound, settings);
    Performance performance = Perform(archive, sound_index, banks, options, settings);

    return ConvertPerformance(std::move(performance), banks, sound.name, "sequence " + sound.name, settings, program);
}

Conversion ConvertTogether(const sar::SoundArchive& archive, std::span<const uint32_t> sound_indices,
                           const std::string& name, const PerformOptions& options, const Settings& settings,
                           const std::string& program)
{
    if (sound_indices.empty())
    {
        throw std::invalid_argument("no sequences to convert together");
    }

    BankSet banks(archive, archive.Sounds()[sound_indices[0]], settings);
    Performance performance = PerformTogether(archive, sound_indices, banks, options, settings);

    return ConvertPerformance(std::move(performance), banks, name, SequencesText(archive, sound_indices), settings,
                              program);
}

Conversion ConvertBeside(const sar::SoundArchive& archive, uint32_t sound_index,
                         const sar::SoundArchive& companion_archive, uint32_t companion_index,
                         const CompanionScript& script, const PerformOptions& options, const Settings& settings,
                         const std::string& program)
{
    const sar::SoundInfo& sound = archive.Sounds()[sound_index];
    const sar::SoundInfo& companion = companion_archive.Sounds()[companion_index];
    BankSet banks(archive, sound, settings);
    BankSet companion_banks(companion_archive, companion, settings);
    Performance performance = PerformBeside(archive, sound_index, banks, companion_archive, companion_index,
                                            companion_banks, script, options, settings);

    return ConvertPerformance(std::move(performance), banks, sound.name,
                              "sequence " + sound.name + " beside " + companion.name, settings, program);
}

BankConversion ConvertBank(const sar::SoundArchive& archive, std::size_t bank, const Settings& settings,
                           const std::string& program)
{
    // The programs that hold an instrument, and the loudest of their regions.
    BankSet banks(archive, bank, settings);
    std::vector<uint32_t> programs;
    int loudest = 0;
    if (const sar::Bank* parsed = banks.GetBank(0))
    {
        for (std::size_t p = 0; p < parsed->instruments.size(); p++)
        {
            if (!parsed->instruments[p])
            {
                continue;
            }

            programs.push_back(static_cast<uint32_t>(p));
            for (const PlayedRanges& played : RegionRanges(*parsed->instruments[p]))
            {
                loudest = std::max(loudest, static_cast<int>(played.region->volume));
            }
        }
    }

    // Region volumes can reach 255, but SoundFont zones can't amplify their samples. Above 127, every zone takes the
    // attenuation that keeps the loudest region at full level, as in a sequence's SoundFont (SoundFontCut).
    BankConversion c;
    const double quieter = RegionVolumeCut(loudest);
    if (quieter > 0.0)
    {
        c.warnings.push_back(RegionVolumeCutText(quieter));
    }

    // Each instrument becomes a preset at its program, with a track's starting settings. Those set no filter. The
    // presets therefore need no performance for the fit of their low-passes (ZoneLowPass).
    const Performance kNone;
    const auto make = [&]
    {
        return SoundFontBuilder(banks, quieter, kNone, settings);
    };
    const std::string& name = banks.BankName(0);
    c.sound_fonts = ProgramSoundFonts(programs, make, settings, name, program, c.warnings);
    c.sfz = BuildBankSfz(programs, banks, name, settings, program);
    c.warnings.insert(c.warnings.end(), banks.Errors().begin(), banks.Errors().end());

    return c;
}

} // namespace ntdlib::sequence_export
