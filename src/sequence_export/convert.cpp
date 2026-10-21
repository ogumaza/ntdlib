// SPDX-License-Identifier: MIT

// Sequence conversion for the CSAR and FSAR formats (see convert.h). This file builds the SoundFont. seqlib writes the
// MIDI file, and sfz.cpp the SFZ files.
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
        RegionGenerators g = ConvertRegion(key, RegionEnvelope(r), r.ignore_note_off, r.pitch, r.volume, r.original_key,
                                           wave.loop, quieter_, settings_);
        g.exclusive_class = r.key_group;
        const ResamplingRegion resampling = {r.original_key, r.pitch, wave.sample_rate, r.interpolation};
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
                       *banks_.WaveKey(key.bank_slot, r.wave_id_index), wave, low_pass_, settings_, zone);
    }

    sf2::AddInstrumentPreset(sf_, index, name, std::move(inst));
}

// The SoundFont format numbers the zones, generators and modulators of its instruments and of its presets with 16
// bits. A file holds at most 65,535 of each.
constexpr std::size_t kSoundFontLimit = 0xffff;

// The zones of some of a SoundFont's instruments or presets, with their generators and modulators.
struct ZoneCount
{
    std::size_t zones = 0;
    std::size_t generators = 0;
    std::size_t modulators = 0;
};

// Adds `zones` to `count`. Returns whether the count stays within the format's limits.
bool AddZones(ZoneCount& count, const std::vector<sf2::Zone>& zones)
{
    count.zones += zones.size();
    for (const sf2::Zone& zone : zones)
    {
        count.generators += zone.generators.size();
        count.modulators += zone.modulators.size();
    }

    return count.zones <= kSoundFontLimit && count.generators <= kSoundFontLimit && count.modulators <= kSoundFontLimit;
}

// Splits the presets of `sf` into runs that each fit in a SoundFont. Preset k plays instrument k. Returns the index
// after each run's last preset. A preset whose instrument exceeds the limits by itself takes a run of its own. The
// SoundFont's writer then refuses it.
std::vector<std::size_t> SoundFontRuns(const sf2::SoundFont& sf)
{
    std::vector<std::size_t> ends;
    std::size_t start = 0;
    ZoneCount instruments;
    ZoneCount presets;
    for (std::size_t k = 0; k < sf.presets.size(); k++)
    {
        ZoneCount more_instruments = instruments;
        ZoneCount more_presets = presets;
        const bool instruments_fit = AddZones(more_instruments, sf.instruments[k].zones);
        const bool presets_fit = AddZones(more_presets, sf.presets[k].zones);
        if ((!instruments_fit || !presets_fit) && k > start)
        {
            ends.push_back(k);
            start = k;
            more_instruments = {};
            more_presets = {};
            AddZones(more_instruments, sf.instruments[k].zones);
            AddZones(more_presets, sf.presets[k].zones);
        }
        instruments = more_instruments;
        presets = more_presets;
    }
    ends.push_back(sf.presets.size());

    return ends;
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
    for (int slot = 0; slot < sar::kBankSlots; slot++)
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
    const auto build = [&](std::span<const uint32_t> run)
    {
        SoundFontBuilder builder(banks, quieter, kNone, settings);
        for (const uint32_t p : run)
        {
            builder.Add(p, StartingPresetKey(p, settings), "Program " + std::to_string(p));
        }

        return builder;
    };
    SoundFontBuilder whole = build(programs);

    // Instruments past the format's limits go into a SoundFont for each run of programs that fits. Each SoundFont's
    // name gives its programs.
    const std::string& name = banks.BankName(0);
    const std::vector<std::size_t> ends = SoundFontRuns(whole.Result());
    std::size_t begin = 0;
    for (const std::size_t end : ends)
    {
        BankSoundFont& sound_font = c.sound_fonts.emplace_back();
        std::string title = name;
        if (end > begin)
        {
            sound_font.first = programs[begin];
            sound_font.last = programs[end - 1];
        }
        if (ends.size() > 1)
        {
            title += ", programs " + std::to_string(sound_font.first) + " to " + std::to_string(sound_font.last);
        }

        const std::string comment = "Converted by " + program + " from bank " + title + ".";
        if (ends.size() == 1)
        {
            WriteSoundFont(sound_font, whole.Result(), title, comment, program);
        }
        else
        {
            SoundFontBuilder part = build(std::span(programs).subspan(begin, end - begin));
            WriteSoundFont(sound_font, part.Result(), title, comment, program);
        }
        begin = end;
    }

    c.sfz = BuildBankSfz(programs, banks, name, settings, program);
    c.warnings.insert(c.warnings.end(), whole.Warnings().begin(), whole.Warnings().end());
    c.warnings.insert(c.warnings.end(), banks.Errors().begin(), banks.Errors().end());

    return c;
}

} // namespace ntdlib::sequence_export
