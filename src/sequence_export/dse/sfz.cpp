// SPDX-License-Identifier: MIT

// DSE's SFZ files (see convert.h).
//
// Each track's file has a <master> for each preset, selected by loprog and hiprog, and above 128 presets by bank select
// (CC0). The regions match the SoundFont's zones, each with the preset's LFOs. The sound unit plays the waves without
// interpolation. The regions therefore take sample_quality=0.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/loop.h"
#include "seqlib/sfz/opcodes.h"
#include "seqlib/sfz/text.h"
#include "seqlib/sfz/track_file.h"
#include "seqlib/wav.h"

#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/sequence_export/dse/convert.h"
#include "ntdlib/sequence_export/dse/instruments.h"
#include "ntdlib/sequence_export/dse/performance.h"
#include "ntdlib/sequence_export/linear_pan.h"
#include "ntdlib/sequence_export/sfz_bank_file.h"

namespace ntdlib::sequence_export::dse
{
namespace
{

using seqlib::sfz::Number;

// The bend range of a bank's SFZ files, in semitones: SoundFont's and MIDI's default.
constexpr int kStartingBendRange = 2;

// The samples of a sequence's SFZ files: a WAV file for each wave, made once, on first use.
class SfzSamples
{
public:
    // A sample's file name, rate and loop. SFZ's loop end is the loop's last sample. A wave that can't be read has no
    // name.
    struct Wave
    {
        std::string name;
        uint32_t rate = 0;
        bool loop = false;
        uint32_t loop_start = 0;
        uint32_t loop_end = 0;
    };

    const Wave& Get(BankSet& banks, uint16_t bank, uint16_t wave)
    {
        const auto key = std::pair{bank, wave};
        if (const auto it = waves_.find(key); it != waves_.end())
        {
            return it->second;
        }

        Wave out;
        if (const Pcm* pcm = banks.Wave(bank, wave))
        {
            const uint32_t crossfade = (pcm->sample_rate * 4 + 999) / 1000; // 4 ms
            const seqlib::PreparedWave prepared = seqlib::PrepareForSfz(*pcm, crossfade);
            out.name = BankSet::WaveName(bank, wave) + ".wav";
            out.rate = pcm->sample_rate;
            files_.push_back({out.name, seqlib::WavFile(prepared.channels[0], pcm->sample_rate)});
            out.loop = prepared.loop;
            if (prepared.loop)
            {
                out.loop_start = prepared.loop_start;
                out.loop_end = prepared.loop_end - 1;
            }
        }

        return waves_.emplace(key, std::move(out)).first->second;
    }

    // Takes the WAV files made. The collection is left empty.
    std::vector<seqlib::sfz::Sample> Take()
    {
        return std::move(files_);
    }

private:
    std::map<std::pair<uint16_t, uint16_t>, Wave> waves_;
    std::vector<seqlib::sfz::Sample> files_;
};

// SFZ's waveform for the driver's: a triangle for the triangles and the random waveforms, a square for the squares,
// and a saw up or down for the saws (sfzformat.com's lfoN_wave).
int SfzWave(uint8_t waveform)
{
    switch (waveform)
    {
    case 0:
    case 1:
        return 3;
    case 4:
        return 6;
    case 5:
        return 7;
    default:
        return 0;
    }
}

// The zone's LFOs as SFZ's LFOs 1 to 4. A pan LFO swings by its steps over the 64 of each side.
std::string LfoOpcodes(const PresetZone& z, const std::vector<LfoShape>& shapes)
{
    std::string s;
    int index = 1;
    for (const LfoShape& shape : shapes)
    {
        const std::string lfo = " lfo" + std::to_string(index);
        std::string target;
        switch (shape.output)
        {
        case 1:
            target = lfo + "_pitch=" + Number(shape.depth, 3);
            break;
        case 2:
            target = lfo + "_volume=" + Number(VolumeLfoDb(shape.depth, z), 3);
            break;
        default:
            target = lfo + "_pan=" + Number(std::clamp(shape.depth / 64.0 * 100.0, -100.0, 100.0), 3);
            break;
        }

        s += lfo + "_wave=" + std::to_string(SfzWave(shape.waveform)) + lfo + "_freq=" + Number(shape.hz, 6);
        if (shape.delay > 0.0)
        {
            s += lfo + "_delay=" + Number(shape.delay, 6);
        }
        if (shape.fade > 0.0)
        {
            s += lfo + "_fade=" + Number(shape.fade, 6);
        }
        s += target;
        index++;
    }

    return s;
}

// A region's opcodes: its sample, keys, velocities, tuning, level, pan, envelope, loop and LFOs. `gain_db` adds to its
// volume. A range of velocities from 0 starts at lovel=1: a velocity of 0 is a note-off.
std::string RegionLine(const PresetZone& z, const SfzSamples::Wave& wave, double gain_db,
                       const std::vector<LfoShape>& shapes)
{
    const ZoneTuning tuning = TuningOf(z, wave.rate);
    std::string s =
        "<region> sample=" + wave.name + " lokey=" + std::to_string(z.low_key) + " hikey=" + std::to_string(z.high_key);
    if (z.low_velocity > 1 || z.high_velocity < 127)
    {
        s += " lovel=" + std::to_string(std::max<int>(z.low_velocity, 1)) + " hivel=" + std::to_string(z.high_velocity);
    }

    s += " pitch_keycenter=" + std::to_string(tuning.root_key);
    if (tuning.coarse != 0)
    {
        s += " transpose=" + std::to_string(tuning.coarse);
    }
    if (tuning.fine != 0)
    {
        s += " tune=" + std::to_string(tuning.fine);
    }

    s += " sample_quality=0";

    const double level = ZoneLevel(z);
    const double volume = level > 0.0 ? 20.0 * std::log10(level) + gain_db : -144.0;
    if (std::abs(volume) >= 0.005)
    {
        s += " volume=" + Number(volume, 2);
    }

    const double pan = std::clamp(100.0 * PanPosition(z.pan), -100.0, 100.0);
    if (std::abs(pan) >= 0.05)
    {
        s += " pan=" + Number(pan, 1);
    }

    const SfzEnvelope e = ConvertEnvelopeForSfz(z.envelope);
    s += seqlib::sfz::AmpEnvelopeOpcodes(
        {.attack = e.attack, .hold = e.hold, .decay = e.decay, .sustain = e.sustain, .release = e.release});
    s += seqlib::sfz::LoopOpcodes(wave.loop, wave.loop_start, wave.loop_end);
    s += LfoOpcodes(z, shapes);

    // A note of the group cuts the group's other notes at once, as sfizz's default off_mode (fast) does.
    if (z.exclusive_class != 0)
    {
        s += " group=" + std::to_string(z.exclusive_class) + " off_by=" + std::to_string(z.exclusive_class);
    }

    return s + "\n";
}

} // namespace

seqlib::sfz::Instruments BuildSfz(const Performance& perf, const seqlib::midi::MidiReport& midi, BankSet& banks,
                                  const std::string& title, const std::vector<std::string>& preset_names,
                                  const std::string& program)
{
    seqlib::sfz::TrackFileOptions options;
    options.title = title;
    options.program = program;
    options.preset_names = preset_names;
    options.presets = perf.presets.size();
    options.level_cut_db = perf.level_cut_db;
    options.track_names = true;

    // Each preset adds a group of its zones.
    SfzSamples samples;
    seqlib::sfz::Instruments out;
    const auto text = [&](int)
    {
        seqlib::sfz::TrackText added;
        added.preset = [&](uint32_t p)
        {
            const PresetKey& key = perf.presets[p];
            std::string s = "<group>\n";
            const ntdlib::dse::WaveBank* bank = banks.Bank(key.bank);
            if (bank == nullptr)
            {
                return s;
            }

            const std::vector<LfoShape> shapes = PresetLfos(key);
            for (const PresetZone& z : PresetZones(*bank, key))
            {
                const SfzSamples::Wave& wave = samples.Get(banks, key.bank, z.wave);
                if (!wave.name.empty())
                {
                    s += RegionLine(z, wave, perf.level_cut_db, shapes);
                }
            }

            return s;
        };

        return added;
    };
    out.files = seqlib::sfz::TrackFiles(perf, midi, options, text);
    out.samples = samples.Take();

    return out;
}

seqlib::sfz::Instruments BuildBankSfz(std::span<const uint32_t> programs, BankSet& banks, uint16_t id,
                                      const std::string& title, const std::string& program)
{
    SfzSamples samples;
    seqlib::sfz::Instruments out;
    const ntdlib::dse::WaveBank* bank = banks.Bank(id);
    for (const uint32_t p : programs)
    {
        std::string s = SfzBankFileStart("Program " + std::to_string(p), title, program, kStartingBendRange);
        s += "\n<group>\n";
        const auto number = static_cast<uint16_t>(p);
        const PresetKey key{.bank = id, .program = number, .lfos = ntdlib::dse::ProgramLfos(*bank, number)};
        const std::vector<LfoShape> shapes = PresetLfos(key);
        for (const PresetZone& z : PresetZones(*bank, key))
        {
            const SfzSamples::Wave& wave = samples.Get(banks, id, z.wave);
            if (!wave.name.empty())
            {
                s += RegionLine(z, wave, 0.0, shapes);
            }
        }

        out.files.push_back({SfzBankFileName(title, p, programs.back()), std::move(s)});
    }

    out.samples = samples.Take();

    return out;
}

} // namespace ntdlib::sequence_export::dse
