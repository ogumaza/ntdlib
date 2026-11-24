// SPDX-License-Identifier: MIT

// DSE's SFZ files (see convert.h).
//
// Each track's file has a <master> for each preset, selected by loprog and hiprog, and above 128 presets by bank select
// (CC0). The regions match the SoundFont's zones, each with the preset's LFOs. Every key takes the tuning at which the
// voice's timer plays it (TunedZones). The sound unit plays the waves without interpolation. The regions therefore
// take sample_quality=0.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/sfz/opcodes.h"
#include "seqlib/sfz/text.h"
#include "seqlib/sfz/track_file.h"

#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/sequence_export/dse/convert.h"
#include "ntdlib/sequence_export/dse/instruments.h"
#include "ntdlib/sequence_export/dse/performance.h"
#include "ntdlib/sequence_export/linear_pan.h"
#include "ntdlib/sequence_export/sfz_bank_file.h"
#include "ntdlib/sequence_export/sfz_files.h"

namespace ntdlib::sequence_export::dse
{
namespace
{

using seqlib::sfz::Number;

// The bend range of a bank's SFZ files, in semitones: SoundFont's and MIDI's default.
constexpr int kStartingBendRange = 2;

// The samples of a sequence's SFZ files, by the waves' names in the SFZ files (BankSet::SfzWaveName).
using SfzSamples = ntdlib::sequence_export::SfzSamples<Pcm>;

// The sample of wave `wave` of bank `bank`.
const SfzSamples::Wave& SampleOf(SfzSamples& samples, BankSet& banks, uint16_t bank, uint16_t wave)
{
    return samples.Get(banks.SfzWaveName(bank, wave), [&] { return banks.Wave(bank, wave); });
}

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
        case kPitchOutput:
            target = lfo + "_pitch=" + Number(shape.depth, 3);
            break;
        case kVolumeOutput:
            target = lfo + "_volume=" + Number(VolumeSwingOf(shape, z).swing, 3);
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
// volume. A range of velocities from 0 starts at lovel=1: a velocity of 0 is a note-off. The driver's ticks take
// `tick_microseconds`.
std::string RegionLine(const TunedZone& tuned, const SfzSamples::Wave& wave, double gain_db,
                       const std::vector<LfoShape>& shapes, int tick_microseconds)
{
    const PresetZone& z = tuned.zone;
    const ZoneTuning& tuning = tuned.tuning;
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
    const double volume = level > 0.0 ? 20.0 * std::log10(level) + VolumeShiftDb(shapes, z) + gain_db : -144.0;
    s += seqlib::sfz::VolumeOpcode(volume);

    const double pan = std::clamp(100.0 * PanPosition(z.pan), -100.0, 100.0);
    if (std::abs(pan) >= 0.05)
    {
        s += " pan=" + Number(pan, 1);
    }

    const SfzEnvelope e = ConvertEnvelopeForSfz(z.envelope, tick_microseconds);
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
    // Each preset adds a group of its zones.
    SfzSamples samples;
    const auto preset = [&](uint32_t p)
    {
        const PresetKey& key = perf.presets[p];
        std::string s = "<group>\n";
        const ntdlib::dse::WaveBank* bank = banks.Bank(key.bank);
        if (bank == nullptr)
        {
            return s;
        }

        const std::vector<LfoShape> shapes = PresetLfos(key, perf.tick_microseconds);
        for (const PresetZone& z : PresetZones(*bank, key))
        {
            const SfzSamples::Wave& wave = SampleOf(samples, banks, key.bank, z.wave);
            if (!wave.name.empty())
            {
                for (const TunedZone& tuned : TunedZones(z, wave.rate))
                {
                    s += RegionLine(tuned, wave, perf.level_cut_db, shapes, perf.tick_microseconds);
                }
            }
        }

        return s;
    };
    seqlib::sfz::Instruments out;
    out.files = ntdlib::sequence_export::SfzPresetFiles(perf, midi, title, preset_names, program, preset);
    out.samples = samples.Take();

    return out;
}

seqlib::sfz::Instruments BuildBankSfz(std::span<const uint32_t> programs, BankSet& banks, uint16_t id,
                                      int tick_microseconds, const std::string& title, const std::string& program,
                                      ntdlib::dse::Build build)
{
    SfzSamples samples;
    seqlib::sfz::Instruments out;
    const ntdlib::dse::WaveBank* bank = banks.Bank(id);
    for (const uint32_t p : programs)
    {
        std::string s = SfzBankFileStart("Program " + std::to_string(p), title, program, kStartingBendRange);
        s += "\n<group>\n";
        const auto number = static_cast<uint16_t>(p);
        const PresetKey key{
            .bank = id, .program = number, .lfos = ntdlib::dse::ProgramLfos(*bank, number, build), .build = build};
        const std::vector<LfoShape> shapes = PresetLfos(key, tick_microseconds);
        for (const PresetZone& z : PresetZones(*bank, key))
        {
            const SfzSamples::Wave& wave = SampleOf(samples, banks, id, z.wave);
            if (!wave.name.empty())
            {
                for (const TunedZone& tuned : TunedZones(z, wave.rate))
                {
                    s += RegionLine(tuned, wave, 0.0, shapes, tick_microseconds);
                }
            }
        }

        out.files.push_back({SfzBankFileName(title, p, programs.back()), std::move(s)});
    }

    out.samples = samples.Take();

    return out;
}

} // namespace ntdlib::sequence_export::dse
