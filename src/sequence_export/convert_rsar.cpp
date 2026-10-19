// SPDX-License-Identifier: MIT

// Sequence conversion for the Wii's RSAR format (see convert.h). This file builds the SoundFont from NW4R SND's banks.
// seqlib writes the MIDI file, and sfz_rsar.cpp the SFZ files.
//
// Each performance preset combines a bank instrument with its track settings. Each such preset becomes one SoundFont
// instrument and preset, with a zone for each region. Zones contain the region's key and velocity ranges, sample, root
// key, tuning, volume, pan, envelope and loop. Track overrides apply to the envelope. Presets used by MIDI channel 10
// also get a copy in bank 128. Synths take drum instruments from there.

#include <cmath>
#include <utility>

#include "ntdlib/sequence_export/conversion.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/performer.h"
#include "ntdlib/sequence_export/rsar_instruments.h"
#include "seqlib/loop.h"
#include "seqlib/midi/performance_writer.h"
#include "seqlib/sf2/presets.h"
#include "sequence_export/sf2_build.h"
#include "sequence_export/wii_pan.h"
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
    // Adds `quieter` centibels of attenuation to every region (see SoundFontCut). `balance` plays stereo waves in
    // balance pan mode, and `since_march_2008` applies regions' holds and note-off types. The zones fit their cutoff
    // for the track's low-pass to the keys that `perf` plays (ZoneLowPass).
    SoundFontBuilder(RsarBankSet& banks, double quieter, bool balance, bool since_march_2008, const Performance& perf,
                     const Settings& settings)
        : banks_(banks),
          quieter_(quieter),
          balance_(balance),
          since_march_2008_(since_march_2008),
          settings_(settings),
          low_pass_(perf, settings)
    {
    }

    // Adds preset `index` at bank index / 128, program index % 128. The preset uses the bank instrument specified by
    // `key`.
    void Add(uint32_t index, const PresetKey& key, const std::string& name);

private:
    // The samples of the bank's wave `wave_index` (two for stereo), made on first use. Returns an empty list if the
    // wave cannot be read.
    const std::vector<uint16_t>& WaveSamples(uint32_t wave_index, uint8_t root_key);

    RsarBankSet& banks_;
    double quieter_;
    bool balance_;
    bool since_march_2008_;
    const Settings& settings_;
    ZoneLowPass low_pass_;
};

const std::vector<uint16_t>& SoundFontBuilder::WaveSamples(uint32_t wave_index, uint8_t root_key)
{
    // With balance, each channel of a stereo pair plays at kBalanceScale. The pair takes the first two channels.
    const auto balance = [this](seqlib::PreparedWave& w)
    {
        if (w.channels.size() >= 2 && balance_)
        {
            for (std::size_t c = 0; c < 2; c++)
            {
                for (int16_t& v : w.channels[c])
                {
                    v = static_cast<int16_t>(std::lround(v * kBalanceScale));
                }
            }
        }
    };

    return Samples(banks_.WaveKey(wave_index), root_key, [&] { return banks_.GetWave(wave_index); }, balance);
}

void SoundFontBuilder::Add(uint32_t index, const PresetKey& key, const std::string& name)
{
    sf2::Instrument inst;
    inst.name = name;
    inst.zones.push_back(GlobalZone(key, settings_));

    const rsar::Bank* bank = banks_.GetBank();
    if (bank && !banks_.HasInstrument(key.program))
    {
        warnings_.push_back(name + ": bank " + banks_.BankName() + " has no instrument " + std::to_string(key.program));
    }

    const bool any = bank && banks_.HasInstrument(key.program);
    for (const auto& played : any ? RegionRanges(*bank, key.program) : decltype(RegionRanges(*bank, 0))())
    {
        const rsar::Region& r = *played.region;
        const auto wave_index = static_cast<uint32_t>(r.wave_index);
        const std::vector<uint16_t>& ids = WaveSamples(wave_index, r.original_key);
        if (ids.empty())
        {
            warnings_.push_back(name + ": no wave for keys " + std::to_string(played.keys.front().first) + "-" +
                                std::to_string(played.keys.back().second));
            continue;
        }

        // NW4R SND pan: (region pan + track initial pan - 64) / 63, for each channel of a stereo wave as its pan mode
        // places it. Balance mode's samples are already 3 dB down.
        const double pan = (static_cast<int>(r.pan) + key.init_pan - 64) / 63.0;
        const rsar::Pcm& wave = *banks_.GetWave(wave_index);
        const RegionGenerators g =
            ConvertRegion(key, RegionEnvelope(r, since_march_2008_), since_march_2008_ && r.note_off_type == 1, r.tune,
                          r.volume, r.original_key, wave.loop, quieter_, settings_);
        const ResamplingRegion resampling = {r.original_key, r.tune, wave.sample_rate, 0};
        const auto zone = [&](std::size_t c, std::pair<int, int> keys, std::pair<int, int> velocities)
        {
            const ChannelPan place = WaveChannelPan(pan, c, ids.size(), balance_);
            const double gain_db = ids.size() == 2 && balance_ ? place.gain_db - kBalanceDb : place.gain_db;
            const double extra = gain_db <= -144.0 ? 1440.0 : -10.0 * gain_db;

            return RegionZone(g, keys, velocities, place.pan, extra, ids[c]);
        };
        AddRegionZones(inst.zones, index, key, played.keys, played.velocities, ids.size(), resampling,
                       banks_.WaveKey(wave_index), wave, low_pass_, settings_, zone);
    }

    sf2::AddInstrumentPreset(sf_, index, name, std::move(inst));
}

} // namespace

Conversion ConvertSequence(const rsar::SoundArchive& archive, uint32_t sound_index, const PerformOptions& options,
                           const Settings& settings, const std::string& program)
{
    const rsar::SoundInfo& sound = archive.Sounds()[sound_index];
    RsarBankSet banks(archive, sound, settings);
    Conversion c;
    c.performance = Perform(archive, sound_index, banks, options, settings);

    // NW4R SND runs a tick at its frame's start, a little after MIDI's tempo puts the tick.
    const seqlib::midi::MidiReport midi = WriteMidiFile(c, sound.name, {seqlib::midi::FrameTiming::kFrameStart});

    // The attenuation that keeps the loudest region that the performance plays at full level (SoundFontCut).
    auto volume_of = [&banks](const PresetKey& key, int note, int velocity)
    {
        const rsar::Region* r = banks.FindRegion(key.program, note, velocity);
        return r ? static_cast<int>(r->volume) : 0;
    };
    const double quieter = SoundFontCut(c.performance, volume_of);

    const bool balance = c.performance.balance;
    SoundFontBuilder builder(banks, quieter, balance, c.performance.since_march_2008, c.performance, settings);
    const std::vector<std::string> names = PresetNames(c.performance.presets, false);
    for (std::size_t i = 0; i < c.performance.presets.size(); i++)
    {
        builder.Add(static_cast<uint32_t>(i), c.performance.presets[i], names[i]);
    }
    for (uint32_t index : midi.channel_10_presets)
    {
        builder.AddChannel10Copy(index);
    }

    sf2::SoundFont& sf = builder.Result();
    std::string comment = "Converted by " + program + " from sequence " + sound.name + ".";
    if (banks.GetBank())
    {
        comment += " Bank: " + banks.BankName() + ".";
    }

    WriteSoundFont(c, sf, sound.name, comment, program);
    c.sfz = BuildSfz(c.performance, midi, banks, sound.name, names, balance, settings, program);

    CollectWarnings(c, midi, builder.Warnings(), banks.Errors());

    return c;
}

} // namespace ntdlib::sequence_export
