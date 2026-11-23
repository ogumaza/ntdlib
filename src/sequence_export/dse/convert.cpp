// SPDX-License-Identifier: MIT

// DSE's conversion (see convert.h). This file builds the SoundFont, and sfz.cpp the SFZ files. seqlib writes the MIDI
// and SoundFont files.
//
// Each zone takes its split's keys, velocities, sample, pan and envelope. A split's keys split into runs that each take
// the tuning at which the voice's timer plays them (TunedZones). A sequence's SoundFont tunes only the keys that the
// sequence plays. A zone for each run of keys of one tuning over every key could pass SoundFont's limit of 65,535
// generators in a large sequence. A bank's SoundFonts and every SFZ file tune every key. A zone's attenuation gives its
// volume and the level at its pan against the level at the centre (linear_pan.h). The level of the channel's pan goes
// in CC11 (the performer). A zone panned off the centre then plays above full level. Every zone of every SoundFont
// takes the cut of a zone at full volume panned fully to one side (PanCut), and the SFZ files keep the level. For a
// mixer that pans linearly (PerformOptions::linear_pans), a zone keeps the pan of its split and plays at its volume,
// with no cut. Presets that MIDI channel 10 plays also get a copy in bank 128. Synthesizers look for drums there.

#include "ntdlib/sequence_export/dse/convert.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/loop.h"
#include "seqlib/midi/performance_writer.h"
#include "seqlib/sf2/presets.h"
#include "seqlib/sf2/samples.h"
#include "seqlib/sf2/soundfont.h"
#include "seqlib/sf2/units.h"
#include "xsflib/bytes.h"

#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/sequence_export/conversion.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/dse/instruments.h"
#include "ntdlib/sequence_export/dse/performance.h"
#include "ntdlib/sequence_export/linear_pan.h"
#include "ntdlib/sequence_export/psg_samples.h"
#include "sequence_export/ntr/bank_programs.h"
#include "sequence_export/sf2_build.h"

namespace ntdlib::sequence_export::dse
{

namespace sf2 = seqlib::sf2;

namespace
{

// The LFOs' outputs (LfoValues::output).
constexpr uint8_t kPitchOutput = 1;
constexpr uint8_t kVolumeOutput = 2;

// The first of `shapes` with `output`, or null.
const LfoShape* FirstOf(const std::vector<LfoShape>& shapes, uint8_t output)
{
    const auto found = std::ranges::find(shapes, output, &LfoShape::output);
    return found != shapes.end() ? &*found : nullptr;
}

// The preset's global zone: seqlib's kGlobalZoneModulators, and the rates and delays of its first pitch LFO and its
// first volume LFO. The pitch LFO's depth goes here. The volume LFO's depth depends on the zone's level, and goes in
// each zone.
sf2::Zone GlobalZone(const std::vector<LfoShape>& shapes)
{
    sf2::Zone global;
    global.modulators.assign(sf2::kGlobalZoneModulators.begin(), sf2::kGlobalZoneModulators.end());
    if (const LfoShape* pitch = FirstOf(shapes, kPitchOutput))
    {
        global.generators.push_back(sf2::Generator::Value(sf2::kFreqVibLfo, sf2::LfoFrequency(pitch->hz)));
        global.generators.push_back(sf2::Generator::Value(
            sf2::kDelayVibLfo,
            sf2::ToTimecents(pitch->delay * 1000.0, sf2::kShortestTimecents, sf2::kLongestHoldTimecents)));
        global.generators.push_back(
            sf2::Generator::Value(sf2::kVibLfoToPitch, std::clamp<int>(sf2::ToAmount(pitch->depth), -12000, 12000)));
    }
    if (const LfoShape* volume = FirstOf(shapes, kVolumeOutput))
    {
        global.generators.push_back(sf2::Generator::Value(sf2::kFreqModLfo, sf2::LfoFrequency(volume->hz)));
        global.generators.push_back(sf2::Generator::Value(
            sf2::kDelayModLfo,
            sf2::ToTimecents(volume->delay * 1000.0, sf2::kShortestTimecents, sf2::kLongestHoldTimecents)));
    }
    return global;
}

// The level of `zone` against full level from its volume alone, as an amplitude: the square of the volume.
double VolumeLevel(const PresetZone& zone)
{
    const double volume = zone.volume / 127.0;
    return volume * volume;
}

// The zone of `tuned` with sample `sample`, `quieter` centibels quieter, for a driver whose ticks take
// `tick_microseconds`. With `linear_pans`, the zone keeps the pan of the split for a mixer that pans linearly. Its
// level is then the square of its volume alone (PerformOptions::linear_pans).
sf2::Zone RegionZone(const TunedZone& tuned, uint16_t sample, bool loop, double quieter,
                     const std::vector<LfoShape>& shapes, int tick_microseconds, bool linear_pans)
{
    const PresetZone& z = tuned.zone;
    const ZoneTuning& tuning = tuned.tuning;
    const Sf2Envelope e = ConvertEnvelope(z.envelope, tick_microseconds);
    const double level = linear_pans ? VolumeLevel(z) : ZoneLevel(z);
    const double position = linear_pans ? LinearPanPosition(z.pan) : PanPosition(z.pan);
    sf2::ZoneValues values;
    values.keys = {z.low_key, z.high_key};
    values.velocities = {z.low_velocity, z.high_velocity};
    values.attenuation = level > 0.0 ? std::clamp<double>(quieter - 200.0 * std::log10(level), 0.0, sf2::kMaxCentibels)
                                     : double{sf2::kMaxCentibels};
    values.pan = std::clamp(static_cast<int>(std::lround(500.0 * position)), -500, 500);
    values.attack = e.attack;
    values.hold = e.hold;
    values.decay = e.decay;
    values.sustain = e.sustain;
    values.release = e.release;
    values.root_key = tuning.root_key;
    values.coarse_tune = tuning.coarse;
    values.fine_tune = tuning.fine;
    values.loop = loop;
    values.exclusive_class = z.exclusive_class;
    values.sample = sample;

    sf2::Zone zone = sf2::RegionZone(values);
    if (const LfoShape* volume = FirstOf(shapes, kVolumeOutput))
    {
        sf2::AddGenerator(zone, sf2::kModLfoToVolume,
                          std::clamp<int>(sf2::ToAmount(10.0 * VolumeLfoDb(volume->depth, z)), -960, 960));
    }
    return zone;
}

// The samples of a sequence's SoundFont: each wave's sample, added once, on first use.
class SoundFontSamples
{
public:
    // The sample of `bank`'s wave, its rate, and whether it loops, or nothing for a wave that can't be read.
    struct Sample
    {
        uint16_t id = 0;
        uint32_t rate = 0;
        bool loop = false;
    };

    explicit SoundFontSamples(sf2::SoundFont& sf) : sf_(sf)
    {
    }

    std::optional<Sample> Get(BankSet& banks, uint16_t bank, uint16_t wave)
    {
        const auto key = std::pair{bank, wave};
        if (const auto it = samples_.find(key); it != samples_.end())
        {
            return it->second;
        }

        std::optional<Sample> sample;
        if (const Pcm* pcm = banks.Wave(bank, wave))
        {
            const std::vector<uint16_t> ids = sf2::AddSamples(sf_, seqlib::PrepareForSoundFont(*pcm),
                                                              BankSet::WaveName(bank, wave), pcm->sample_rate, 60);
            sample = Sample{ids.at(0), pcm->sample_rate, pcm->loop};
        }

        return samples_.emplace(key, sample).first->second;
    }

private:
    sf2::SoundFont& sf_;
    std::map<std::pair<uint16_t, uint16_t>, std::optional<Sample>> samples_;
};

} // namespace

double ZoneLevel(const PresetZone& zone)
{
    return VolumeLevel(zone) * PanGain(zone.pan) / PanGain(64);
}

std::vector<std::string> PresetNames(const std::vector<PresetKey>& keys)
{
    std::vector<std::pair<uint32_t, uint32_t>> programs;
    for (const PresetKey& key : keys)
    {
        programs.emplace_back(key.bank, key.program);
    }

    return sf2::PresetNames(programs, true);
}

Conversion ConvertSequence(const SequenceFiles& files, const std::string& name, const PerformOptions& options,
                           const std::string& program)
{
    return ConvertSequence(files, name, options, program, {});
}

Conversion ConvertSequence(const SequenceFiles& files, const std::string& name, const PerformOptions& options,
                           const std::string& program, const std::map<uint16_t, std::string>& sfz_bank_names)
{
    Conversion c;
    c.performance = Perform(files, options);

    // The driver runs a tick of the sequence in a later tick of its own than MIDI's tempo puts it.
    const seqlib::midi::MidiReport midi =
        ntdlib::sequence_export::WriteMidiFile(c, name, {seqlib::midi::FrameTiming::kFrameStart});

    // SoundFont zones can't amplify their samples. Every zone takes the cut of a zone at full volume panned fully to
    // one side. A zone for a mixer that pans linearly plays at its volume, and takes none.
    BankSet banks(files);
    const double quieter = options.linear_pans ? 0.0 : ntdlib::sequence_export::PanCut();

    sf2::SoundFont sf;
    SoundFontSamples samples(sf);
    std::vector<std::string> warnings;
    const std::vector<std::string> names = PresetNames(c.performance.presets);
    for (std::size_t i = 0; i < c.performance.presets.size(); i++)
    {
        const PresetKey& key = c.performance.presets[i];
        const std::vector<LfoShape> shapes = PresetLfos(key, c.performance.tick_microseconds);
        sf2::Instrument instrument;
        instrument.name = names[i];
        instrument.zones.push_back(GlobalZone(shapes));
        const ntdlib::dse::WaveBank* bank = banks.Bank(key.bank);
        const std::vector<PresetZone> zones = bank != nullptr ? PresetZones(*bank, key) : std::vector<PresetZone>{};
        if (zones.empty())
        {
            warnings.push_back(names[i] + ": bank " + std::to_string(key.bank) +
                               " has no split that plays for program " + std::to_string(key.program));
        }
        const std::set<uint8_t> none;
        const std::set<uint8_t>& keys = i < c.performance.preset_keys.size() ? c.performance.preset_keys[i] : none;
        for (const PresetZone& z : zones)
        {
            if (const auto sample = samples.Get(banks, key.bank, z.wave))
            {
                for (const TunedZone& tuned : TunedZones(z, sample->rate, &keys))
                {
                    instrument.zones.push_back(RegionZone(tuned, sample->id, sample->loop, quieter, shapes,
                                                          c.performance.tick_microseconds, options.linear_pans));
                }
            }
        }

        sf2::AddInstrumentPreset(sf, static_cast<uint32_t>(i), names[i], std::move(instrument));
    }
    for (const uint32_t index : midi.channel_10_presets)
    {
        sf2::AddChannel10Copy(sf, index, warnings);
    }

    ntdlib::sequence_export::WriteSoundFont(
        c, sf, name, "Converted by " + program + (files.effects ? " from sound effect " : " from song ") + name + ".",
        program);

    // SFZ players apply CC10 and CC11 of the MIDI file by the sin/cos law. They can't follow a mixer that pans
    // linearly.
    banks.SetSfzNames(sfz_bank_names);
    if (!options.linear_pans)
    {
        c.sfz = BuildSfz(c.performance, midi, banks, name, names, program);
    }
    ntdlib::sequence_export::CollectWarnings(c, midi, warnings, banks.Warnings());

    return c;
}

ntdlib::sequence_export::BankConversion ConvertBank(const ntdlib::dse::WaveBank& bank,
                                                    const ntdlib::dse::WaveBank* main_bank,
                                                    std::span<const uint32_t> programs, int tick_microseconds,
                                                    const std::string& name, const std::string& program,
                                                    ntdlib::dse::Build build)
{
    const auto id = xsflib::LoadLe16(&bank.header[0x0E]);
    const auto key_of = [&bank, id, build](uint32_t p)
    {
        const auto number = static_cast<uint16_t>(p);
        return PresetKey{
            .bank = id, .program = number, .lfos = ntdlib::dse::ProgramLfos(bank, number, build), .build = build};
    };

    // The programs that play something.
    ntdlib::sequence_export::BankConversion c;
    BankSet banks({.banks = {&bank}, .main_bank = main_bank});
    const auto plays = [&bank, &key_of](uint32_t p)
    {
        return p <= 0x7FFF && !PresetZones(bank, key_of(p)).empty();
    };
    const std::vector<uint32_t> playing = detail::PlayingPrograms(programs, id, plays, c.warnings);

    // SoundFont zones can't amplify their samples. Every zone takes the cut of a zone at full volume panned fully to
    // one side.
    const double quieter = ntdlib::sequence_export::PanCut();

    const auto part = [&](std::span<const uint32_t> run)
    {
        sf2::SoundFont sf;
        SoundFontSamples samples(sf);
        for (const uint32_t p : run)
        {
            const PresetKey key = key_of(p);
            const std::vector<LfoShape> shapes = PresetLfos(key, tick_microseconds);
            const std::string preset = "Program " + std::to_string(p);
            sf2::Instrument instrument;
            instrument.name = preset;
            instrument.zones.push_back(GlobalZone(shapes));
            for (const PresetZone& z : PresetZones(bank, key))
            {
                if (const auto sample = samples.Get(banks, id, z.wave))
                {
                    for (const TunedZone& tuned : TunedZones(z, sample->rate))
                    {
                        instrument.zones.push_back(
                            RegionZone(tuned, sample->id, sample->loop, quieter, shapes, tick_microseconds, false));
                    }
                }
            }
            sf2::AddInstrumentPreset(sf, p, preset, std::move(instrument));
        }

        return sf;
    };
    sf2::SoundFont whole = part(playing);
    c.sound_fonts = ntdlib::sequence_export::BankSoundFonts(whole, playing, part, name, program);
    c.sfz = BuildBankSfz(playing, banks, id, tick_microseconds, name, program, build);
    c.warnings.insert(c.warnings.end(), banks.Warnings().begin(), banks.Warnings().end());

    return c;
}

} // namespace ntdlib::sequence_export::dse
