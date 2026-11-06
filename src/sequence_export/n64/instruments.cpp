// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/n64/instruments.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <numbers>
#include <utility>

#include "seqlib/loop.h"
#include "seqlib/midi/performance_writer.h"
#include "seqlib/sf2/presets.h"
#include "seqlib/sf2/samples.h"
#include "seqlib/sf2/soundfont.h"
#include "seqlib/sf2/units.h"
#include "seqlib/sfz/opcodes.h"
#include "seqlib/sfz/text.h"
#include "seqlib/wav.h"

#include "ntdlib/sequence_export/conversion.h"

namespace ntdlib::sequence_export::n64
{

namespace
{

namespace sf2 = seqlib::sf2;
namespace sfz = seqlib::sfz;
using seqlib::sfz::Number;

// A zone's pan at either side, in tenths of a percent, in SoundFont's pan generator.
constexpr double kPanRange = 500.0;

// The fall in decibels that SoundFont's decay and release times count, and that the documentation of SFZ's count: a
// factor of e^-8.
constexpr double kSoundFontFallDb = 100.0;
constexpr double kSfzFallDb = 8.0 * 20.0 / std::numbers::ln10;

// SoundFont's limits on the times, in timecents.
constexpr int kShortestTimecents = -12000;
constexpr int kLongestTimecents = 8000;
constexpr int kLongestHoldTimecents = 5000;

// The attenuation in centibels from which a sustain counts as silence.
constexpr int kSilentCentibels = 1000;

// A fall in SFZ's time, in seconds.
double SfzFall(double soundfont_ms)
{
    return soundfont_ms / 1000.0 * kSfzFallDb / kSoundFontFallDb;
}

int Timecents(double ms, int longest)
{
    return sf2::ToTimecents(ms, kShortestTimecents, longest);
}

// Adds generator `oper` with `value` to `zone`, before its sample.
void AddGenerator(sf2::Zone& zone, uint16_t oper, int value)
{
    zone.generators.insert(zone.generators.end() - 1, sf2::Generator::Value(oper, value));
}

// The instrument's global zone holds seqlib's kGlobalZoneModulators. The Nintendo 64's engines don't filter by
// velocity. A pitch wheel covers its full range. CC10 pans at the amount of 500 that most synthesizers play. The mod
// wheel moves nothing: SoundFont's default CC1 modulator would add 50 cents of vibrato.
sf2::Zone GlobalZone()
{
    sf2::Zone global;
    global.modulators.assign(sf2::kGlobalZoneModulators.begin(), sf2::kGlobalZoneModulators.end());
    global.modulators.push_back({sf2::kSrcModWheel, sf2::kVibLfoToPitch, 0, 0});

    return global;
}

// The SoundFont's samples: each wave once.
class SoundFontSamples
{
public:
    explicit SoundFontSamples(sf2::SoundFont& sf) : sf_(sf)
    {
    }

    uint16_t Get(const Wave& wave, bool& loops)
    {
        const auto found = ids_.find(wave.name);
        if (found != ids_.end())
        {
            loops = wave.pcm.loop;
            return found->second;
        }

        const std::vector<uint16_t> added =
            sf2::AddSamples(sf_, seqlib::PrepareForSoundFont(wave.pcm), wave.name, wave.rate, 60);
        ids_[wave.name] = added.at(0);
        loops = wave.pcm.loop;

        return added.at(0);
    }

private:
    sf2::SoundFont& sf_;
    std::map<std::string, uint16_t> ids_;
};

sf2::Zone SoundFontZone(const Zone& z, uint16_t sample, bool loop, double quieter)
{
    const int cents = static_cast<int>(std::lround(z.tune_cents));

    sf2::ZoneValues values;
    values.keys = {z.low_key, z.high_key};
    values.velocities = {z.low_velocity, z.high_velocity};
    values.attenuation = Centibels(z.gain) + quieter + (z.tremolo && !z.tremolo->wheel ? z.tremolo->depth * 10.0 : 0.0);
    values.pan = static_cast<int>(std::lround(z.pan * kPanRange));
    if (z.delay_ms > 0.0)
    {
        values.delay = Timecents(z.delay_ms, kLongestHoldTimecents);
    }

    values.attack = Timecents(z.attack_ms, kLongestTimecents);
    values.hold = Timecents(z.hold_ms, kLongestHoldTimecents);
    values.decay = Timecents(z.decay_ms, kLongestTimecents);
    values.sustain = std::clamp(static_cast<int>(std::lround(z.sustain_db * 10.0)), 0, kSilentCentibels);
    values.release = Timecents(z.release_ms, kLongestTimecents);
    values.root_key = z.root_key;
    values.exclusive_class = z.exclusive_class;
    values.coarse_tune = cents / 100;
    values.fine_tune = cents % 100;
    values.loop = loop;
    values.sample = sample;
    sf2::Zone zone = sf2::RegionZone(values);

    if (!z.follows_key)
    {
        AddGenerator(zone, sf2::kScaleTuning, 0);
    }

    if (z.low_pass_hz > 0.0)
    {
        AddGenerator(zone, sf2::kInitialFilterFc,
                     static_cast<int>(std::lround(1200.0 * std::log2(z.low_pass_hz / 8.176))));
    }

    if (z.vibrato)
    {
        if (z.vibrato->delay_ms > 0.0)
        {
            AddGenerator(zone, sf2::kDelayVibLfo, Timecents(z.vibrato->delay_ms, kLongestHoldTimecents));
        }

        AddGenerator(zone, sf2::kFreqVibLfo, sf2::LfoFrequency(z.vibrato->hz));
        const int depth = static_cast<int>(std::lround(z.vibrato->depth));
        if (z.vibrato->wheel)
        {
            zone.modulators.push_back({sf2::kSrcModWheel, sf2::kVibLfoToPitch, static_cast<int16_t>(depth), 0});
        }
        else
        {
            AddGenerator(zone, sf2::kVibLfoToPitch, depth);
        }
    }

    if (z.tremolo)
    {
        if (z.tremolo->delay_ms > 0.0)
        {
            AddGenerator(zone, sf2::kDelayModLfo, Timecents(z.tremolo->delay_ms, kLongestHoldTimecents));
        }

        AddGenerator(zone, sf2::kFreqModLfo, sf2::LfoFrequency(z.tremolo->hz));
        const int depth = static_cast<int>(std::lround(z.tremolo->depth * 10.0));
        if (z.tremolo->wheel)
        {
            zone.modulators.push_back({sf2::kSrcModWheel, sf2::kModLfoToVolume, static_cast<int16_t>(depth), 0});
        }
        else
        {
            AddGenerator(zone, sf2::kModLfoToVolume, depth);
        }
    }

    return zone;
}

// The SFZ files' samples of a sequence: each wave once, as a WAV file.
class SfzSamples
{
public:
    // A wave as a file, with the loop that its opcodes give.
    struct File
    {
        std::string name;
        bool loop = false;
        uint32_t loop_start = 0;
        uint32_t loop_end = 0; // the loop's last sample
    };

    const File& Get(const Wave& wave)
    {
        const auto found = files_.find(wave.name);
        if (found != files_.end())
        {
            return found->second;
        }

        // sfizz's crossfade of a loop spans at least 1 ms. 4 ms of repeated loop data before the loop's start makes it
        // inaudible. A wave without a loop goes out as it is.
        seqlib::Pcm out = wave.pcm;
        if (wave.pcm.loop)
        {
            const seqlib::PreparedWave prepared = seqlib::PrepareForSfz(wave.pcm, (wave.rate * 4 + 999) / 1000);
            out.channels = prepared.channels;
            out.loop = prepared.loop;
            out.loop_start = prepared.loop_start;
            out.loop_end = prepared.loop_end;
        }

        File file;
        file.name = wave.name + ".wav";
        file.loop = out.loop;
        file.loop_start = out.loop_start;
        file.loop_end = out.loop_end > 0 ? out.loop_end - 1 : 0;
        samples_.push_back({file.name, seqlib::WavFile(out, wave.rate)});

        return files_.emplace(wave.name, std::move(file)).first->second;
    }

    std::vector<sfz::Sample> Take()
    {
        return std::move(samples_);
    }

private:
    std::map<std::string, File> files_;
    std::vector<sfz::Sample> samples_;
};

std::string SfzRegion(const Zone& z, const SfzSamples::File& file, double gain_db)
{
    std::string s = "<region> sample=" + file.name + " lokey=" + std::to_string(z.low_key) +
                    " hikey=" + std::to_string(z.high_key) + " pitch_keycenter=" + std::to_string(z.root_key);
    if (z.low_velocity != 0 || z.high_velocity != 127)
    {
        s += " lovel=" + std::to_string(std::max(z.low_velocity, 1)) + " hivel=" + std::to_string(z.high_velocity);
    }

    if (!z.follows_key)
    {
        s += " pitch_keytrack=0";
    }

    if (z.exclusive_class != 0)
    {
        s += " group=" + std::to_string(z.exclusive_class) + " off_by=" + std::to_string(z.exclusive_class);
    }

    if (z.low_pass_hz > 0.0)
    {
        s += " fil_type=lpf_2p cutoff=" + Number(z.low_pass_hz, 0);
    }

    s += sfz::TuneOpcodes(static_cast<int>(std::lround(z.tune_cents)));
    const double volume =
        20.0 * std::log10(std::max(z.gain, 1e-7)) + gain_db - (z.tremolo && !z.tremolo->wheel ? z.tremolo->depth : 0.0);
    if (std::abs(volume) >= 0.005)
    {
        s += " volume=" + Number(volume, 2);
    }

    if (std::abs(z.pan) >= 0.0005)
    {
        s += " pan=" + Number(z.pan * 100.0, 1);
    }

    s += sfz::AmpEnvelopeOpcodes({.delay = z.delay_ms / 1000.0,
                                  .attack = z.attack_ms / 1000.0,
                                  .hold = z.hold_ms / 1000.0,
                                  .decay = SfzFall(z.decay_ms),
                                  .sustain = 100.0 * std::pow(10.0, -z.sustain_db / 20.0),
                                  .release = SfzFall(z.release_ms)});
    s += sfz::LoopOpcodes(file.loop, file.loop_start, file.loop_end);
    if (z.vibrato)
    {
        s += " lfo1_wave=1 lfo1_freq=" + Number(z.vibrato->hz, 6) +
             (z.vibrato->wheel ? " lfo1_pitch_oncc1=" : " lfo1_pitch=") + Number(z.vibrato->depth, 3);
        if (z.vibrato->delay_ms > 0.0)
        {
            s += " lfo1_delay=" + Number(z.vibrato->delay_ms / 1000.0, 6);
        }
    }

    if (z.tremolo)
    {
        s += " lfo2_wave=1 lfo2_freq=" + Number(z.tremolo->hz, 6) +
             (z.tremolo->wheel ? " lfo2_volume_oncc1=" : " lfo2_volume=") + Number(z.tremolo->depth, 3);
        if (z.tremolo->delay_ms > 0.0)
        {
            s += " lfo2_delay=" + Number(z.tremolo->delay_ms / 1000.0, 6);
        }
    }

    return s + "\n";
}

// The SoundFont holds zones below full level. A zone above it that a note plays makes every zone quieter. A zone that
// no note plays doesn't count. SFZ keeps the levels.
double LoudestZone(const Performance& performance)
{
    double loudest = 0.0;
    for (const std::vector<seqlib::midi::TrackEvent>& track : performance.tracks)
    {
        // A part plays preset 0 until it sends another, as a player does.
        std::map<uint8_t, uint32_t> presets;
        for (const seqlib::midi::TrackEvent& event : track)
        {
            if (event.kind == seqlib::midi::EventKind::kPreset)
            {
                presets[event.part] = event.preset;
                continue;
            }

            const auto found = presets.find(event.part);
            const uint32_t preset = found != presets.end() ? found->second : 0;
            if (event.kind != seqlib::midi::EventKind::kNoteOn || preset >= performance.presets.size())
            {
                continue;
            }

            for (const Zone& zone : performance.presets[preset].zones)
            {
                if (event.key >= zone.low_key && event.key <= zone.high_key && event.value >= zone.low_velocity &&
                    event.value <= zone.high_velocity)
                {
                    loudest = std::max(loudest, zone.gain);
                }
            }
        }
    }

    return loudest;
}

sfz::Instruments BuildSfz(const Performance& performance, const seqlib::midi::MidiReport& midi,
                          const std::string& title, const std::vector<std::string>& names, const std::string& program)
{
    sfz::TrackFileOptions options;
    options.title = title;
    options.program = program;
    options.preset_names = names;
    options.presets = performance.presets.size();
    options.level_cut_db = performance.level_cut_db;

    SfzSamples samples;
    auto text = [&](int)
    {
        sfz::TrackText added;
        added.preset = [&](uint32_t preset)
        {
            std::string s = "<group>\n";
            for (const Zone& zone : performance.presets[preset].zones)
            {
                s += SfzRegion(zone, samples.Get(*zone.wave), performance.level_cut_db);
            }

            return s;
        };

        return added;
    };

    sfz::Instruments out;
    out.files = sfz::TrackFiles(performance, midi, options, text);
    out.samples = samples.Take();

    return out;
}

} // namespace

double Centibels(double amplitude)
{
    return amplitude <= 0.0 ? 1440.0 : -200.0 * std::log10(amplitude);
}

Conversion BuildFiles(Performance performance, const std::string& title, const std::string& program)
{
    Conversion c;
    c.performance = std::move(performance);
    const seqlib::midi::MidiReport midi = ntdlib::sequence_export::WriteMidiFile(c, title);

    const double quieter = ntdlib::sequence_export::CutAboveFullLevel(c, LoudestZone(c.performance));
    sf2::SoundFont sf;
    SoundFontSamples samples(sf);
    std::vector<std::string> names;
    for (std::size_t i = 0; i < c.performance.presets.size(); i++)
    {
        const Instrument& instrument = c.performance.presets[i];
        sf2::Instrument out;
        out.name = instrument.name;
        out.zones.push_back(GlobalZone());
        for (const Zone& zone : instrument.zones)
        {
            bool loop = false;
            const uint16_t sample = samples.Get(*zone.wave, loop);
            out.zones.push_back(SoundFontZone(zone, sample, loop, quieter));
        }

        names.push_back(instrument.name);
        sf2::AddInstrumentPreset(sf, static_cast<uint32_t>(i), instrument.name, std::move(out));
    }

    std::vector<std::string> builder_warnings;
    for (const uint32_t preset : midi.channel_10_presets)
    {
        sf2::AddChannel10Copy(sf, preset, builder_warnings);
    }

    ntdlib::sequence_export::WriteSoundFont(c, sf, title, "Converted by " + program + ": " + title, program);
    c.sfz = BuildSfz(c.performance, midi, title, names, program);
    ntdlib::sequence_export::CollectWarnings(c, midi, builder_warnings, {});

    return c;
}

} // namespace ntdlib::sequence_export::n64
