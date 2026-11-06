// SPDX-License-Identifier: MIT

// The DS's conversion (see convert.h). This file builds the SoundFonts, and sfz.cpp the SFZ files. seqlib writes the
// MIDI and SoundFont files.
//
// Each zone takes its region's keys, sample, root key, pan and envelope. The DS pans linearly, and its level falls off
// the sides. A zone's attenuation gives the level at its pan against the level at the centre (PanGain). The track's
// pan's level goes in CC11 (the performer). A zone panned off the centre then plays above full level. The SoundFont's
// zones all take the cut that the loudest requires, and the SFZ files keep the level. Presets that MIDI channel 10
// plays also get a copy in bank 128. Synthesizers look for drums there.

#include "ntdlib/sequence_export/ntr/convert.h"

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
#include "xsflib/binary.h"

#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sequence_export/conversion.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/ntr/instruments.h"
#include "ntdlib/sequence_export/ntr/performance.h"
#include "sequence_export/sf2_build.h"

namespace ntdlib::sequence_export::ntr
{

namespace sf2 = seqlib::sf2;

namespace
{

namespace snd = ntdlib::ntr::snd;

// A frame of the driver in milliseconds.
constexpr double kFrameMs = 1000.0 * snd::kFrameClocks / ntdlib::ntr::kSystemClockHz;

// The LFO's targets (snd::LfoParameters).
constexpr uint8_t kPitchTarget = 0;
constexpr uint8_t kVolumeTarget = 1;

// The preset's global zone. It starts with seqlib's kGlobalZoneModulators. CC1 gives the LFO its depth. A modulator
// from CC1 to the vibrato's pitch replaces SoundFont's default one, whose amount is 50 cents.
sf2::Zone GlobalZone(const PresetKey& key)
{
    sf2::Zone global;
    global.modulators.assign(sf2::kGlobalZoneModulators.begin(), sf2::kGlobalZoneModulators.end());

    const double hz = LfoHz(key.lfo_speed);
    const bool pitch = hz > 0.0 && key.lfo_target == kPitchTarget;
    const bool volume = hz > 0.0 && key.lfo_target == kVolumeTarget;
    const auto amount = [](double value)
    {
        return static_cast<int16_t>(std::clamp(std::lround(value), -32768L, 32767L));
    };
    global.modulators.push_back({sf2::kSrcModWheel, sf2::kVibLfoToPitch,
                                 amount(pitch ? kPitchLfoCents * 128.0 / 127.0 * key.lfo_range : 0.0), 0});
    const int delay = sf2::ToTimecents(key.lfo_delay * kFrameMs, -12000, 5000);
    if (pitch)
    {
        global.generators.push_back(sf2::Generator::Value(sf2::kFreqVibLfo, sf2::LfoFrequency(hz)));
        global.generators.push_back(sf2::Generator::Value(sf2::kDelayVibLfo, delay));
    }
    if (volume)
    {
        global.modulators.push_back(
            {sf2::kSrcModWheel, sf2::kModLfoToVolume, amount(kVolumeLfoDb * 10.0 * 128.0 / 127.0 * key.lfo_range), 0});
        global.generators.push_back(sf2::Generator::Value(sf2::kFreqModLfo, sf2::LfoFrequency(hz)));
        global.generators.push_back(sf2::Generator::Value(sf2::kDelayModLfo, delay));
    }

    return global;
}

// The zone of `z` with sample `sample`, `quieter` centibels quieter.
sf2::Zone RegionZone(const PresetZone& z, uint16_t sample, bool loop, double quieter, const snd::Build& build)
{
    const Sf2Envelope e = ConvertEnvelope(z.envelope, build);
    sf2::ZoneValues values;
    values.keys = {z.low_key, z.high_key};
    values.attenuation = quieter - 200.0 * std::log10(ZoneGain(z));
    values.pan = std::clamp(static_cast<int>(std::lround(500.0 * PanPosition(z.region.pan))), -500, 500);
    values.attack = e.attack;
    values.hold = e.hold;
    values.decay = e.decay;
    values.sustain = e.sustain;
    values.release = e.release;
    values.root_key = std::min<int>(z.region.root_key, 127);
    values.loop = loop;
    values.sample = sample;

    return sf2::RegionZone(values);
}

// The samples of a sequence's SoundFont: each wave's sample, added once, on first use.
class SoundFontSamples
{
public:
    explicit SoundFontSamples(sf2::SoundFont& sf) : sf_(sf)
    {
    }

    // The sample of `region`'s wave, and whether it loops, or nothing for a wave that can't be read.
    std::optional<std::pair<uint16_t, bool>> Get(BankSet& banks, const sdat::Region& region)
    {
        const std::string name = banks.WaveName(region);
        if (const auto it = samples_.find(name); it != samples_.end())
        {
            return it->second;
        }

        std::optional<std::pair<uint16_t, bool>> sample;
        if (const Pcm* pcm = banks.Wave(region))
        {
            const std::vector<uint16_t> ids =
                sf2::AddSamples(sf_, seqlib::PrepareForSoundFont(*pcm), name, pcm->sample_rate, 60);
            sample = std::pair{ids.at(0), pcm->loop};
        }

        return samples_.emplace(name, sample).first->second;
    }

private:
    sf2::SoundFont& sf_;
    std::map<std::string, std::optional<std::pair<uint16_t, bool>>> samples_;
};

// The instrument of preset `key`: its global zone, and a zone for each of its regions whose wave reads, `quieter`
// centibels quieter.
sf2::Instrument PresetInstrument(BankSet& banks, SoundFontSamples& samples, const PresetKey& key,
                                 const std::string& name, double quieter, const snd::Build& build)
{
    sf2::Instrument instrument;
    instrument.name = name;
    instrument.zones.push_back(GlobalZone(key));
    if (banks.Bank() != nullptr)
    {
        for (const PresetZone& z : PresetZones(*banks.Bank(), key))
        {
            if (const auto sample = samples.Get(banks, z.region))
            {
                instrument.zones.push_back(RegionZone(z, sample->first, sample->second, quieter, build));
            }
        }
    }

    return instrument;
}

} // namespace

double ZoneGain(const PresetZone& zone)
{
    return PanGain(zone.region.pan) / PanGain(64);
}

double LfoHz(uint8_t speed)
{
    return speed * 1000.0 / (512.0 * kFrameMs);
}

std::vector<std::string> PresetNames(const std::vector<PresetKey>& keys)
{
    std::vector<std::pair<uint32_t, uint32_t>> programs;
    for (const PresetKey& key : keys)
    {
        programs.emplace_back(0, key.program);
    }

    return sf2::PresetNames(programs, false);
}

Conversion ConvertSequence(const sdat::Archive& archive, const ntdlib::ntr::SequenceRef& sequence,
                           const std::string& name, const PerformOptions& options, const std::string& program)
{
    Conversion c;
    c.performance = Perform(archive, sequence, options);

    // The driver runs a tick at the start of a later frame than MIDI's tempo puts it.
    const seqlib::midi::MidiReport midi =
        ntdlib::sequence_export::WriteMidiFile(c, name, {seqlib::midi::FrameTiming::kFrameStart});

    // SoundFont zones can't amplify their samples. A zone above full level makes every zone quieter.
    BankSet banks(archive, c.performance.bank);
    double loudest = 0.0;
    for (const PresetKey& key : c.performance.presets)
    {
        if (banks.Bank() != nullptr)
        {
            for (const PresetZone& z : PresetZones(*banks.Bank(), key))
            {
                loudest = std::max(loudest, ZoneGain(z));
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
        if (banks.Bank() != nullptr && PresetZones(*banks.Bank(), key).empty())
        {
            warnings.push_back(names[i] + ": bank " + std::to_string(c.performance.bank) +
                               " has no instrument that plays for program " + std::to_string(key.program));
        }
        sf2::AddInstrumentPreset(sf, static_cast<uint32_t>(i), names[i],
                                 PresetInstrument(banks, samples, key, names[i], quieter, options.build));
    }
    for (const uint32_t index : midi.channel_10_presets)
    {
        sf2::AddChannel10Copy(sf, index, warnings);
    }

    ntdlib::sequence_export::WriteSoundFont(c, sf, name, "Converted by " + program + " from sequence " + name + ".",
                                            program);
    c.sfz = BuildSfz(c.performance, midi, banks, options.build, name, names, program);
    ntdlib::sequence_export::CollectWarnings(c, midi, warnings, banks.Warnings());

    return c;
}

ntdlib::sequence_export::BankConversion ConvertBank(const sdat::Archive& archive, uint16_t bank,
                                                    std::span<const uint32_t> programs, const snd::Build& build,
                                                    const std::string& program)
{
    if (bank >= archive.banks.size() || !archive.banks[bank])
    {
        throw xsflib::FormatError("the archive has no bank " + std::to_string(bank));
    }

    // The programs that play something, and the loudest of their zones. A track starts with the regions' envelopes,
    // at the centre, and with its LFO at a depth of 0.
    ntdlib::sequence_export::BankConversion c;
    BankSet banks(archive, bank);
    std::vector<uint32_t> playing;
    double loudest = 0.0;
    for (const uint32_t p : programs)
    {
        const std::vector<PresetZone> zones =
            banks.Bank() != nullptr && p <= 0xFFFF
                ? PresetZones(*banks.Bank(), PresetKey{.program = static_cast<uint16_t>(p)})
                : std::vector<PresetZone>();
        if (zones.empty())
        {
            c.warnings.push_back("program " + std::to_string(p) + " of bank " + std::to_string(bank) +
                                 " plays nothing");
            continue;
        }

        playing.push_back(p);
        for (const PresetZone& z : zones)
        {
            loudest = std::max(loudest, ZoneGain(z));
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
            const std::string name = "Program " + std::to_string(p);
            sf2::AddInstrumentPreset(
                sf, p, name,
                PresetInstrument(banks, samples, PresetKey{.program = static_cast<uint16_t>(p)}, name, quieter, build));
        }

        return sf;
    };
    sf2::SoundFont whole = part(playing);
    const std::string name =
        archive.banks[bank]->name.empty() ? "BANK_" + std::to_string(bank) : archive.banks[bank]->name;
    c.sound_fonts = ntdlib::sequence_export::BankSoundFonts(whole, playing, part, name, program);
    c.sfz = BuildBankSfz(playing, banks, build, name, program);
    c.warnings.insert(c.warnings.end(), banks.Warnings().begin(), banks.Warnings().end());

    return c;
}

} // namespace ntdlib::sequence_export::ntr
