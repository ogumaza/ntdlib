// SPDX-License-Identifier: MIT

// The DS's SFZ files (see convert.h).
//
// Each track's file has a <master> for each preset, selected by loprog and hiprog, and above 128 presets by bank select
// (CC0). The regions match the SoundFont's zones, with their tunings (TunedZones). The DS's channels play their waves
// without interpolation. The regions therefore take sample_quality=0. The LFO is a sine with its depth from CC1. A
// bank's file of one program has the frame of a track's file, with the bend range that a track starts with, then the
// program's LFO and regions.

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

#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sequence_export/ntr/convert.h"
#include "ntdlib/sequence_export/ntr/instruments.h"
#include "ntdlib/sequence_export/ntr/performance.h"
#include "ntdlib/sequence_export/sfz_bank_file.h"
#include "ntdlib/sequence_export/sfz_files.h"

namespace ntdlib::sequence_export::ntr
{

namespace
{

namespace snd = ntdlib::ntr::snd;
using seqlib::sfz::Number;

// A frame of the driver in seconds.
constexpr double kFrameSeconds = static_cast<double>(snd::kFrameClocks) / ntdlib::ntr::kSystemClockHz;

// The bend range that a track starts with, in semitones.
constexpr int kStartingBendRange = 2;

// The samples of a sequence's SFZ files, by the waves' names (BankSet::WaveName).
using SfzSamples = ntdlib::sequence_export::SfzSamples<Pcm>;

// A region's opcodes: its sample, keys, root key, tuning, level, pan, envelope and loop. `gain_db` adds to its volume.
// The tuning's whole semitones go in transpose, and the rest in tune (seqlib::sfz::TuneOpcodes).
std::string RegionLine(const PresetZone& z, const SfzSamples::Wave& wave, double gain_db, const snd::Build& build)
{
    std::string s = "<region> sample=" + wave.name + " lokey=" + std::to_string(z.low_key) +
                    " hikey=" + std::to_string(z.high_key) +
                    " pitch_keycenter=" + std::to_string(std::min<int>(z.region.root_key, 127));
    s += seqlib::sfz::TuneOpcodes(z.tuning);
    s += " sample_quality=0";

    const double volume = 20.0 * std::log10(ZoneGain(z)) + gain_db;
    s += seqlib::sfz::VolumeOpcode(volume);

    const double pan = std::clamp(100.0 * PanPosition(z.region.pan), -100.0, 100.0);
    if (std::abs(pan) >= 0.05)
    {
        s += " pan=" + Number(pan, 1);
    }

    const SfzEnvelope e = ConvertEnvelopeForSfz(z.envelope, build);
    s += seqlib::sfz::AmpEnvelopeOpcodes(
        {.attack = e.attack, .hold = e.hold, .decay = e.decay, .sustain = e.sustain, .release = e.release});
    s += seqlib::sfz::LoopOpcodes(wave.loop, wave.loop_start, wave.loop_end);

    return s + "\n";
}

// The regions of preset `key`'s zones whose waves read, `gain_db` louder.
std::string PresetRegions(BankSet& banks, SfzSamples& samples, const PresetKey& key, double gain_db,
                          const snd::Build& build)
{
    std::string s;
    if (banks.Bank() == nullptr)
    {
        return s;
    }
    for (const PresetZone& z : PresetZones(*banks.Bank(), key))
    {
        const SfzSamples::Wave& wave = samples.Get(banks.WaveName(z.region), [&] { return banks.Wave(z.region); });
        if (!wave.name.empty())
        {
            for (const PresetZone& tuned : TunedZones(z, *banks.Wave(z.region)))
            {
                s += RegionLine(tuned, wave, gain_db, build);
            }
        }
    }

    return s;
}

// Preset `key`'s LFO as SFZ's LFO 1: a sine at its speed, after its delay, with its depth from CC1. sfizz divides a
// controller's value by 127.
std::string LfoOpcodes(const PresetKey& key)
{
    const double hz = LfoHz(key.lfo_speed);
    if (hz <= 0.0)
    {
        return "";
    }

    std::string s = "lfo1_wave=1 lfo1_freq=" + Number(hz, 6);
    if (key.lfo_delay != 0)
    {
        s += " lfo1_delay=" + Number(key.lfo_delay * kFrameSeconds, 6);
    }
    switch (key.lfo_target)
    {
    case 0:
        s += " lfo1_pitch_oncc1=" + Number(kPitchLfoCents * key.lfo_range, 6);
        break;
    case 1:
        s += " lfo1_volume_oncc1=" + Number(kVolumeLfoDb * key.lfo_range, 6);
        break;
    case 2:
        s += " lfo1_pan_oncc1=" + Number(kPanLfoPercent * key.lfo_range, 6);
        break;
    default:
        return "";
    }

    return s + "\n";
}

} // namespace

seqlib::sfz::Instruments BuildSfz(const Performance& perf, const seqlib::midi::MidiReport& midi, BankSet& banks,
                                  const snd::Build& build, const std::string& title,
                                  const std::vector<std::string>& preset_names, const std::string& program)
{
    // Each preset adds its LFO and a group of its zones.
    SfzSamples samples;
    const auto preset = [&](uint32_t p)
    {
        const PresetKey& key = perf.presets[p];
        return LfoOpcodes(key) + "<group>\n" + PresetRegions(banks, samples, key, perf.level_cut_db, build);
    };
    seqlib::sfz::Instruments out;
    out.files = ntdlib::sequence_export::SfzPresetFiles(perf, midi, title, preset_names, program, preset);
    out.samples = samples.Take();

    return out;
}

seqlib::sfz::Instruments BuildBankSfz(std::span<const uint32_t> programs, BankSet& banks, const snd::Build& build,
                                      const std::string& title, const std::string& program)
{
    SfzSamples samples;
    seqlib::sfz::Instruments out;
    for (const uint32_t p : programs)
    {
        std::string s = SfzBankFileStart("Program " + std::to_string(p), title, program, kStartingBendRange);
        const PresetKey key{.program = static_cast<uint16_t>(p)};
        s += "\n" + LfoOpcodes(key) + "<group>\n" + PresetRegions(banks, samples, key, 0.0, build);

        out.files.push_back({SfzBankFileName(title, p, programs.back()), std::move(s)});
    }
    out.samples = samples.Take();

    return out;
}

} // namespace ntdlib::sequence_export::ntr
