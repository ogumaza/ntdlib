// SPDX-License-Identifier: MIT

// DSE's conversion (see convert.h). This file builds the SoundFont, and sfz.cpp the SFZ files. seqlib writes the MIDI
// and SoundFont files.
//
// Each zone takes its split's keys, velocities, sample, tuning, pan and envelope. Its attenuation gives its volume and
// the level at its pan against the level at the centre (linear_pan.h). The channel's pan's level goes in CC11 (the
// performer). A zone panned off the centre then plays above full level. The SoundFont's zones all take the cut that the
// loudest requires, and the SFZ files keep the level. Presets that MIDI channel 10 plays also get a copy in bank 128.
// Synthesizers look for drums there.

#include "ntdlib/sequence_export/dse/convert.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
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

#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/sequence_export/conversion.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/dse/instruments.h"
#include "ntdlib/sequence_export/dse/performance.h"
#include "ntdlib/sequence_export/linear_pan.h"
#include "ntdlib/sequence_export/psg_samples.h"
#include "sequence_export/sf2_build.h"

namespace ntdlib::sequence_export::dse
{

namespace sf2 = seqlib::sf2;

namespace
{

// The rate of the sound unit's timers: half the system clock.
constexpr double kTimerHz = ntdlib::ntr::kSystemClockHz / 2.0;

// The LFOs' outputs (LfoValues::output).
constexpr uint8_t kPitchOutput = 1;
constexpr uint8_t kVolumeOutput = 2;

int16_t Amount(double value)
{
    return static_cast<int16_t>(std::clamp(std::lround(value), -32768L, 32767L));
}

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
        global.generators.push_back(
            sf2::Generator::Value(sf2::kDelayVibLfo, sf2::ToTimecents(pitch->delay * 1000.0, -12000, 5000)));
        global.generators.push_back(
            sf2::Generator::Value(sf2::kVibLfoToPitch, std::clamp<int>(Amount(pitch->depth), -12000, 12000)));
    }
    if (const LfoShape* volume = FirstOf(shapes, kVolumeOutput))
    {
        global.generators.push_back(sf2::Generator::Value(sf2::kFreqModLfo, sf2::LfoFrequency(volume->hz)));
        global.generators.push_back(
            sf2::Generator::Value(sf2::kDelayModLfo, sf2::ToTimecents(volume->delay * 1000.0, -12000, 5000)));
    }
    return global;
}

// The zone of `z` with sample `sample` of `sample_rate`, `quieter` centibels quieter.
sf2::Zone RegionZone(const PresetZone& z, uint16_t sample, uint32_t sample_rate, bool loop, double quieter,
                     const std::vector<LfoShape>& shapes)
{
    const Sf2Envelope e = ConvertEnvelope(z.envelope);
    const ZoneTuning tuning = TuningOf(z, sample_rate);
    const double level = ZoneLevel(z);
    sf2::ZoneValues values;
    values.keys = {z.low_key, z.high_key};
    values.velocities = {z.low_velocity, z.high_velocity};
    values.attenuation = level > 0.0 ? std::clamp(quieter - 200.0 * std::log10(level), 0.0, 1440.0) : 1440.0;
    values.pan = std::clamp(static_cast<int>(std::lround(500.0 * PanPosition(z.pan))), -500, 500);
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
        // The generator goes before the sample's, the last.
        const auto at = zone.generators.end() - 1;
        zone.generators.insert(
            at, sf2::Generator::Value(sf2::kModLfoToVolume,
                                      std::clamp<int>(Amount(10.0 * VolumeLfoDb(volume->depth, z)), -960, 960)));
    }
    return zone;
}

// The samples of a sequence's SoundFont: each wave's sample, added once, on first use.
class SoundFontSamples
{
public:
    explicit SoundFontSamples(sf2::SoundFont& sf) : sf_(sf)
    {
    }

    // The sample of `bank`'s wave, its rate, and whether it loops, or nothing for a wave that can't be read.
    struct Sample
    {
        uint16_t id = 0;
        uint32_t rate = 0;
        bool loop = false;
    };

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

ZoneTuning TuningOf(const PresetZone& zone, uint32_t sample_rate)
{
    ZoneTuning tuning;
    tuning.root_key = std::clamp(60, static_cast<int>(zone.low_key), static_cast<int>(zone.high_key));
    const auto pitch = static_cast<uint16_t>(zone.tuning + (tuning.root_key << 8));
    const int32_t timer = ntdlib::dse::VoiceTimer(pitch, zone.format);
    if (timer <= 0 || sample_rate == 0)
    {
        return tuning;
    }

    // The PSG's and the noise's timers step them once a period, kPsgStepSamples samples of their waves.
    const double rate = kTimerHz / timer * (zone.format >= 3 ? ntdlib::sequence_export::kPsgStepSamples : 1);
    const double cents = 1200.0 * std::log2(rate / sample_rate);
    tuning.coarse = static_cast<int>(std::lround(cents / 100.0));
    tuning.fine = static_cast<int>(std::lround(cents - 100.0 * tuning.coarse));
    return tuning;
}

double ZoneLevel(const PresetZone& zone)
{
    const double volume = zone.volume / 127.0;
    return volume * volume * PanGain(zone.pan) / PanGain(64);
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
    Conversion c;
    c.performance = Perform(files, options);

    // The driver runs a tick of the sequence in a later tick of its own than MIDI's tempo puts it.
    const seqlib::midi::MidiReport midi =
        ntdlib::sequence_export::WriteMidiFile(c, name, {seqlib::midi::FrameTiming::kFrameStart});

    // SoundFont zones can't amplify their samples. A zone above full level makes every zone quieter.
    BankSet banks(files);
    double loudest = 0.0;
    for (const PresetKey& key : c.performance.presets)
    {
        if (const ntdlib::dse::WaveBank* bank = banks.Bank(key.bank))
        {
            for (const PresetZone& z : PresetZones(*bank, key))
            {
                loudest = std::max(loudest, ZoneLevel(z));
            }
        }
    }
    const double quieter = ntdlib::sequence_export::CutAboveFullLevel(c, loudest);

    sf2::SoundFont sf;
    SoundFontSamples samples(sf);
    std::vector<std::string> warnings;
    const std::vector<std::string> names = PresetNames(c.performance.presets);
    for (std::size_t i = 0; i < c.performance.presets.size(); i++)
    {
        const PresetKey& key = c.performance.presets[i];
        const std::vector<LfoShape> shapes = PresetLfos(key);
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
        for (const PresetZone& z : zones)
        {
            if (const auto sample = samples.Get(banks, key.bank, z.wave))
            {
                instrument.zones.push_back(RegionZone(z, sample->id, sample->rate, sample->loop, quieter, shapes));
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
    c.sfz = BuildSfz(c.performance, midi, banks, name, names, program);
    ntdlib::sequence_export::CollectWarnings(c, midi, warnings, banks.Warnings());

    return c;
}

ntdlib::sequence_export::BankConversion ConvertBank(const ntdlib::dse::WaveBank& bank,
                                                    const ntdlib::dse::WaveBank* main_bank,
                                                    std::span<const uint32_t> programs, const std::string& name,
                                                    const std::string& program)
{
    const auto id = static_cast<uint16_t>(bank.header[0x0E] | bank.header[0x0F] << 8);
    const auto key_of = [&bank, id](uint32_t p)
    {
        const auto number = static_cast<uint16_t>(p);
        return PresetKey{.bank = id, .program = number, .lfos = ntdlib::dse::ProgramLfos(bank, number)};
    };

    // The programs that play something, and the loudest of their zones.
    ntdlib::sequence_export::BankConversion c;
    BankSet banks({.banks = {&bank}, .main_bank = main_bank});
    std::vector<uint32_t> playing;
    double loudest = 0.0;
    for (const uint32_t p : programs)
    {
        const std::vector<PresetZone> zones = p <= 0x7FFF ? PresetZones(bank, key_of(p)) : std::vector<PresetZone>();
        if (zones.empty())
        {
            c.warnings.push_back("program " + std::to_string(p) + " of bank " + std::to_string(id) + " plays nothing");
            continue;
        }

        playing.push_back(p);
        for (const PresetZone& z : zones)
        {
            loudest = std::max(loudest, ZoneLevel(z));
        }
    }

    // The SoundFonts' runs and the SFZ files' names take the programs in ascending order, each once.
    std::ranges::sort(playing);
    playing.erase(std::ranges::unique(playing).begin(), playing.end());

    // SoundFont zones can't amplify their samples. A zone above full level makes every zone quieter.
    const double quieter = loudest > 1.0 ? 200.0 * std::log10(loudest) : 0.0;
    if (quieter >= 0.5)
    {
        char what[160];
        std::snprintf(what, sizeof(what),
                      "instrument volume above full level (SoundFont attenuated by %.1f dB; SFZ keeps the level)",
                      quieter / 10.0);
        c.warnings.push_back(what);
    }

    const auto part = [&](std::span<const uint32_t> run)
    {
        sf2::SoundFont sf;
        SoundFontSamples samples(sf);
        for (const uint32_t p : run)
        {
            const PresetKey key = key_of(p);
            const std::vector<LfoShape> shapes = PresetLfos(key);
            const std::string preset = "Program " + std::to_string(p);
            sf2::Instrument instrument;
            instrument.name = preset;
            instrument.zones.push_back(GlobalZone(shapes));
            for (const PresetZone& z : PresetZones(bank, key))
            {
                if (const auto sample = samples.Get(banks, id, z.wave))
                {
                    instrument.zones.push_back(RegionZone(z, sample->id, sample->rate, sample->loop, quieter, shapes));
                }
            }
            sf2::AddInstrumentPreset(sf, p, preset, std::move(instrument));
        }

        return sf;
    };
    sf2::SoundFont whole = part(playing);
    c.sound_fonts = ntdlib::sequence_export::BankSoundFonts(whole, playing, part, name, program);
    c.sfz = BuildBankSfz(playing, banks, id, name, program);
    c.warnings.insert(c.warnings.end(), banks.Warnings().begin(), banks.Warnings().end());

    return c;
}

} // namespace ntdlib::sequence_export::dse
