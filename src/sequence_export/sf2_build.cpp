// SPDX-License-Identifier: MIT

// The SoundFont parts that every bank model shares (see sf2_build.h).

#include "sequence_export/sf2_build.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/loop.h"
#include "seqlib/midi/performance.h"
#include "seqlib/sf2/presets.h"
#include "seqlib/sf2/samples.h"
#include "seqlib/sf2/soundfont.h"
#include "seqlib/sf2/units.h"

#include "ntdlib/sequence_export/conversion.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/envelope.h"
#include "ntdlib/sequence_export/filter.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/resampling.h"
#include "ntdlib/sequence_export/settings.h"
#include "ntdlib/wave/decode.h"
#include "sequence_export/pcm.h"
#include "sequence_export/zone_low_pass.h"

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

// The modulator source of the note-on key: linear, unipolar, positive. With kBipolar, it runs from -1 at key 0 through
// 0 at key 64. With kNegative, it runs from 1 at key 0 to 0 at key 128.
constexpr uint16_t kSrcKeyNumber = sf2::kNoteOnKeyNumber;

// The cutoff of the resampling low-passes rises by 100 cents a key, as the pitch does.
constexpr int kResamplingSlope = 100;

// The SoundFont cutoff of the low-pass `filter` for a note at key 0 on `region`, in absolute cents.
int CutoffAtKey0(const ResamplingFilter& filter, const ResamplingRegion& region)
{
    const double cents = 1200.0 * std::log2(ResamplingCutoffHz(filter.cutoff, 0, region) / 8.176);

    return static_cast<int>(std::lround(std::clamp(cents, -32768.0, 32767.0)));
}

} // namespace

sf2::Zone GlobalZone(const PresetKey& key, const Settings& settings)
{
    sf2::Zone global;
    global.modulators.assign(sf2::kGlobalZoneModulators.begin(), sf2::kGlobalZoneModulators.end());
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

    const int freq = sf2::LfoFrequency(std::max(hz, 0.001));
    const int delay = ToTimecents(LfoDelayMs(key, settings), -12000, 5000);
    if (key.lfo_type == 1 && lfo)
    {
        // A note whose volume LFO the voice holds at full level swings less (PresetKey::lfo_swing).
        const int16_t db = amount(60.0 * key.lfo_range * kDepthScale * key.lfo_swing / 16.0);
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

std::vector<ResamplingSpan> ZoneSpans(std::pair<int, int> keys, const ResamplingRegion& region, const PresetKey& key,
                                      const Settings& settings)
{
    if (key.filter_controller)
    {
        return {{keys, nullptr}};
    }

    std::vector<ResamplingSpan> spans;
    const auto add = [&spans](std::pair<int, int> span_keys, const ResamplingFilter* filter)
    {
        if (!spans.empty() && spans.back().filter == filter)
        {
            spans.back().keys.second = span_keys.second;
        }
        else
        {
            spans.push_back({span_keys, filter});
        }
    };

    for (const ResamplingSpan& span : ResamplingSpans(keys, region, settings))
    {
        // From key `split` on, the track's fixed filter has the lower cutoff.
        int split = span.keys.first;
        if (span.filter && key.filter.cutoff != kFilterOpen && !FitsTrackLowPass(key, settings))
        {
            const int base = CutoffAtKey0(*span.filter, region);
            while (split <= span.keys.second && base + kResamplingSlope * split < key.filter.cutoff)
            {
                split++;
            }
        }
        else
        {
            split = span.keys.second + 1;
        }

        if (split > span.keys.first)
        {
            add({span.keys.first, split - 1}, span.filter);
        }
        if (split <= span.keys.second)
        {
            add({split, span.keys.second}, nullptr);
        }
    }

    return spans;
}

void AddTrackingCutoff(sf2::Zone& zone, int base, int slope)
{
    // The key at which the generator holds the cutoff, and the modulator that adds the rest.
    const auto in_range = [](int cents)
    {
        return cents >= 1500 && cents <= 13500;
    };
    int key = 0;
    uint16_t source = kSrcKeyNumber;
    int amount = 128 * slope;
    if (!in_range(base) && in_range(base + 64 * slope))
    {
        key = 64;
        source = kSrcKeyNumber | sf2::kBipolar;
        amount = 64 * slope;
    }
    else if (!in_range(base) && in_range(base + 128 * slope))
    {
        key = 128;
        source = kSrcKeyNumber | sf2::kNegative;
        amount = -128 * slope;
    }

    // The sample's generator stays last.
    const int cutoff = std::clamp(base + key * slope, -32768, 32767);
    zone.generators.insert(zone.generators.empty() ? zone.generators.end() : zone.generators.end() - 1,
                           sf2::Generator::Value(sf2::kInitialFilterFc, cutoff));
    if (slope != 0)
    {
        zone.modulators.push_back(
            {source, sf2::kInitialFilterFc, static_cast<int16_t>(std::clamp(amount, -32768, 32767)), 0});
    }
}

void AddResamplingFilter(sf2::Zone& zone, const ResamplingFilter& filter, const ResamplingRegion& region,
                         const PresetKey& key)
{
    AddTrackingCutoff(zone, CutoffAtKey0(filter, region), kResamplingSlope);

    // The sample's generator stays last.
    if (key.filter.resonance != 0)
    {
        zone.generators.insert(zone.generators.end() - 1, sf2::Generator::Value(sf2::kInitialFilterQ, 0));
    }
}

void AddZoneLowPass(sf2::Zone& zone, const ResamplingFilter* filter, const std::optional<FittedCutoff>& fitted,
                    const ResamplingRegion& region, const PresetKey& key, const Settings& settings)
{
    // The resampling low-pass, where it has the lower cutoff at the original key or ZoneSpans gave the span to it.
    if (filter != nullptr)
    {
        const int root = region.original_key;
        const int track = fitted ? fitted->base + fitted->slope * root : key.filter.cutoff;
        if (!FitsTrackLowPass(key, settings) || CutoffAtKey0(*filter, region) + kResamplingSlope * root < track)
        {
            AddResamplingFilter(zone, *filter, region, key);
            return;
        }
    }

    // Otherwise the fitted cutoff, or the preset's.
    if (fitted)
    {
        AddTrackingCutoff(zone, fitted->base, fitted->slope);
    }
}

void AddRegionZones(std::vector<sf2::Zone>& zones, uint32_t index, const PresetKey& key,
                    const std::vector<std::pair<int, int>>& keys, const std::vector<std::pair<int, int>>& velocities,
                    std::size_t channels, const ResamplingRegion& region, std::pair<uint32_t, uint32_t> wave_key,
                    const wave::Pcm& wave, ZoneLowPass& low_pass, const Settings& settings,
                    const ChannelZoneFunction& zone)
{
    for (const std::pair<int, int>& range : keys)
    {
        for (const ResamplingSpan& span : ZoneSpans(range, region, key, settings))
        {
            const std::optional<FittedCutoff> fitted =
                low_pass.Fit(index, key, span.keys, region.original_key, region.tune, wave_key, wave);
            for (const std::pair<int, int>& v : velocities)
            {
                for (std::size_t c = 0; c < channels; c++)
                {
                    sf2::Zone channel_zone = zone(c, span.keys, v);
                    AddZoneLowPass(channel_zone, span.filter, fitted, region, key, settings);
                    zones.push_back(std::move(channel_zone));
                }
            }
        }
    }
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

    // A lift raises the zone up to full level and no further (PresetKey::lift). A note whose volume LFO the voice holds
    // at full level plays lower (PresetKey::lfo_cut).
    const double below = volume != 0 ? -200.0 * std::log10(volume / 127.0) : 0.0;
    g.attenuation =
        volume != 0 ? quieter + (key.lift > 0 ? std::max(0.0, below - key.lift) : below) + key.lfo_cut : 1440.0;
    g.root_key = original_key;
    g.loop = loop;

    return g;
}

double RegionVolumeCut(int loudest)
{
    return loudest > 127 ? 200.0 * std::log10(loudest / 127.0) : 0.0;
}

std::string RegionVolumeCutText(double quieter)
{
    char what[160];
    std::snprintf(what, sizeof(what),
                  "region volume exceeds 127 (SoundFont attenuated by %.1f dB; SFZ keeps the original volume)",
                  quieter / 10.0);

    return what;
}

double SoundFontCut(Performance& perf, const RegionVolumeFunction& volume_of)
{
    const double quieter = RegionVolumeCut(LoudestRegion(perf, volume_of));
    if (quieter > 0.0)
    {
        perf.approximations[RegionVolumeCutText(quieter)] = seqlib::midi::NoteCount(perf);
    }

    return quieter;
}

namespace
{

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

std::vector<BankSoundFont> BankSoundFonts(sf2::SoundFont& whole, std::span<const uint32_t> programs,
                                          const std::function<sf2::SoundFont(std::span<const uint32_t>)>& part,
                                          const std::string& name, const std::string& program)
{
    std::vector<BankSoundFont> sound_fonts;
    const std::vector<std::size_t> ends = SoundFontRuns(whole);
    std::size_t begin = 0;
    for (const std::size_t end : ends)
    {
        BankSoundFont& sound_font = sound_fonts.emplace_back();
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
            WriteSoundFont(sound_font, whole, title, comment, program);
        }
        else
        {
            sf2::SoundFont run = part(programs.subspan(begin, end - begin));
            WriteSoundFont(sound_font, run, title, comment, program);
        }
        begin = end;
    }

    return sound_fonts;
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
