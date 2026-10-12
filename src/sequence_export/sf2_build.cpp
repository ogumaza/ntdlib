// SPDX-License-Identifier: MIT

// The SoundFont parts that every bank model shares (see sf2_build.h).

#include "sequence_export/sf2_build.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

#include "ntdlib/sequence_export/filter.h"
#include "seqlib/midi/performance.h"
#include "seqlib/sf2/presets.h"
#include "seqlib/sf2/samples.h"
#include "seqlib/sf2/units.h"
#include "sequence_export/pcm.h"

namespace ntdlib::sequence_export
{

using seqlib::sf2::SoundFontPan;
using seqlib::sf2::ToTimecents;

namespace sf2 = seqlib::sf2;

namespace
{

// The modulator source of kFilterController: linear, unipolar, positive. seqlib/sf2/soundfont.h has the others, with
// the bits' layout.
constexpr uint16_t kSrcFilterController = 0x0080 | kFilterController;

} // namespace

sf2::Zone GlobalZone(const PresetKey& key, const Settings& settings)
{
    sf2::Zone global;
    global.modulators.assign(sf2::kNoVelocityFilterFullBend.begin(), sf2::kNoVelocityFilterFullBend.end());
    if (key.filter_controller)
    {
        global.modulators.push_back({kSrcFilterController, sf2::kInitialFilterFc, -kFilterControllerRange, 0});
    }
    if (key.filter.cutoff != kFilterOpen)
    {
        global.generators.push_back(sf2::Generator::Value(sf2::kInitialFilterFc, key.filter.cutoff));
    }
    if (key.filter.resonance != 0)
    {
        global.generators.push_back(sf2::Generator::Value(sf2::kInitialFilterQ, key.filter.resonance));
    }

    auto amount = [](double v)
    {
        return static_cast<int16_t>(std::clamp<int64_t>(std::llround(v), 0, 32767));
    };

    constexpr double kDepthScale = 2.0; // CC1 = depth / 2; both use a divisor of 128
    const double hz = LfoHz(key, settings);
    const bool lfo = hz > 0.0;
    const double pitch_amount = key.lfo_type == 0 && lfo ? 100.0 * key.lfo_range * kDepthScale : 0.0;
    global.modulators.push_back({sf2::kSrcModWheel, sf2::kVibLfoToPitch, amount(pitch_amount), 0});

    const int freq =
        std::clamp(static_cast<int>(std::lround(1200.0 * std::log2(std::max(hz, 0.001) / 8.176))), -16000, 4500);
    const int delay = ToTimecents(LfoDelayMs(key, settings), -12000, 5000);
    if (key.lfo_type == 1 && lfo)
    {
        const int16_t db = amount(60.0 * key.lfo_range * kDepthScale);
        global.modulators.push_back({sf2::kSrcModWheel, sf2::kModLfoToVolume, db, 0});
        global.generators.push_back(sf2::Generator::Value(sf2::kFreqModLfo, freq));
        global.generators.push_back(sf2::Generator::Value(sf2::kDelayModLfo, delay));
    }
    else if (key.lfo_type == 0 && lfo)
    {
        global.generators.push_back(sf2::Generator::Value(sf2::kFreqVibLfo, freq));
        global.generators.push_back(sf2::Generator::Value(sf2::kDelayVibLfo, delay));
    }

    return global;
}

sf2::Zone RegionZone(const RegionGenerators& g, const std::pair<int, int>& keys, const std::pair<int, int>& velocities,
                     double pan, double extra, uint16_t sample)
{
    sf2::ZoneValues values;
    values.keys = keys;
    values.velocities = velocities;
    values.attenuation = g.attenuation + extra;
    values.pan = static_cast<int>(std::lround(SoundFontPan(pan) * 500.0));
    values.attack = g.envelope.attack;
    values.hold = g.envelope.hold;
    values.decay = g.envelope.decay;
    values.sustain = g.envelope.sustain;
    values.release = g.envelope.release;
    values.root_key = g.root_key;
    values.coarse_tune = g.coarse_tune;
    values.fine_tune = g.fine_tune;
    values.loop = g.loop;
    values.exclusive_class = g.exclusive_class;
    values.sample = sample;

    return sf2::RegionZone(values);
}

std::vector<std::string> PresetNames(const std::vector<PresetKey>& keys, bool bank_slots)
{
    std::vector<std::pair<uint32_t, uint32_t>> banks_and_programs;
    for (const PresetKey& k : keys)
    {
        banks_and_programs.emplace_back(k.bank_slot, k.program);
    }

    return sf2::PresetNames(banks_and_programs, bank_slots);
}

int LoudestRegion(const Performance& perf, const RegionVolumeFunction& volume_of)
{
    int loudest = 0;
    for (const std::vector<TrackEvent>& events : perf.tracks)
    {
        std::array<const PresetKey*, 256> preset{}; // per part
        for (const TrackEvent& e : events)
        {
            if (e.kind == EventKind::kPreset)
            {
                preset[e.part] = &perf.presets[e.preset];
            }
            else if (e.kind == EventKind::kNoteOn && preset[e.part])
            {
                loudest = std::max(loudest, volume_of(*preset[e.part], e.key, e.value));
            }
        }
    }

    return loudest;
}

RegionGenerators ConvertRegion(const PresetKey& key, const EnvelopeValues& env, bool ignore_note_off, float tune,
                               uint8_t volume, uint8_t original_key, bool loop, double quieter,
                               const Settings& settings)
{
    RegionGenerators g;
    g.envelope = ConvertEnvelope(OverrideEnvelope(env, key), settings);
    if (key.attack_timecents)
    {
        g.envelope.attack = *key.attack_timecents;
    }

    // Regions that ignore note-off get a MIDI note-off when the sound library releases the channel, when they become
    // inaudible, or when the key is struck again (see performer.cpp). For the latter two, keep the decay going by using
    // the faster of the decay and release rates.
    if (ignore_note_off && g.envelope.sustain >= kInaudibleSustain)
    {
        g.envelope.release = std::min(g.envelope.release, g.envelope.decay);
    }

    // Clamp tuning to SoundFont's +/-10 octaves. This also prevents invalid pitches, such as infinity, from rounding
    // differently across platforms.
    const double cents = tune > 0 ? std::clamp(1200.0 * std::log2(tune), -12000.0, 12000.0) : 0.0;
    const int total = static_cast<int>(std::lround(cents));
    g.coarse_tune = total / 100;
    g.fine_tune = total - g.coarse_tune * 100;

    // A lift raises the zone up to full level and no further (PresetKey::lift).
    const double below = volume != 0 ? -200.0 * std::log10(volume / 127.0) : 0.0;
    g.attenuation = volume != 0 ? quieter + (key.lift > 0 ? std::max(0.0, below - key.lift) : below) : 1440.0;
    g.root_key = original_key;
    g.loop = loop;

    return g;
}

double SoundFontCut(Performance& perf, const RegionVolumeFunction& volume_of)
{
    const int loudest = LoudestRegion(perf, volume_of);
    const double quieter = loudest > 127 ? 200.0 * std::log10(loudest / 127.0) : 0.0;
    if (loudest > 127)
    {
        char what[160];
        std::snprintf(what, sizeof(what),
                      "region volume exceeds 127 (SoundFont attenuated by %.1f dB; SFZ keeps the original volume)",
                      quieter / 10.0);
        perf.approximations[what] = seqlib::midi::NoteCount(perf);
    }

    return quieter;
}

const std::vector<uint16_t>& SoundFontParts::Samples(std::pair<uint32_t, uint32_t> key, uint8_t root_key,
                                                     const std::function<const wave::Pcm*()>& pcm,
                                                     const std::function<void(seqlib::PreparedWave&)>& adjust)
{
    if (auto it = samples_.find(key); it != samples_.end())
    {
        return it->second;
    }

    std::vector<uint16_t> ids;
    if (const wave::Pcm* decoded = pcm())
    {
        seqlib::PreparedWave w = seqlib::PrepareForSoundFont(ToSeqlib(*decoded));
        if (adjust)
        {
            adjust(w);
        }
        ids = sf2::AddSamples(sf_, std::move(w), "w" + std::to_string(key.first) + "_" + std::to_string(key.second),
                              decoded->sample_rate, root_key);
    }

    return samples_.emplace(key, std::move(ids)).first->second;
}

void SoundFontParts::AddChannel10Copy(uint32_t index)
{
    sf2::AddChannel10Copy(sf_, index, warnings_);
}

} // namespace ntdlib::sequence_export
