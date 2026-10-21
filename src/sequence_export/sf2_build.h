// SPDX-License-Identifier: MIT

// The SoundFont parts that every bank model shares: each preset's global zone, a region's generators and zones, the
// presets' names, the loudest region that a performance plays, and the samples.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/loop.h"
#include "seqlib/sf2/soundfont.h"

#include "ntdlib/sequence_export/envelope.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/resampling.h"
#include "ntdlib/sequence_export/settings.h"
#include "ntdlib/wave/decode.h"
#include "sequence_export/zone_low_pass.h"

namespace ntdlib::sequence_export
{

// Builds the instrument's global zone. The zone starts with seqlib's kGlobalZoneModulators. The engine has no
// velocity-to-filter modulator. The first two of those modulators disable the default one by overriding both the
// SoundFont 2.01 and FluidSynth versions. The fourth, kCc10ToPan, gives CC10 the pan range that most synthesizers give
// it.
//
// CC1 holds LFO depth / 2. The resulting depth is MML depth / 128 times `range` semitones for pitch, or 6 * range dB
// for volume. When the LFO is omitted, the zone disables the default CC1-to-vibrato modulator too.
//
// The default pitch wheel modulator uses 12700 cents times the bend range / 128. This makes a 7-semitone bend set
// through RPN 0 about 5.5 cents flat. Like FluidSynth, the zone replaces it with a 12800-cent modulator targeting
// fineTune. The bend then covers the full range.
//
// The zone sets the track filter (see filter.h). A filter that changes during a note uses a modulator driven by
// kFilterController instead.
seqlib::sf2::Zone GlobalZone(const PresetKey& key, const Settings& settings);

// Generator values shared by a region's zones, after applying the track settings.
struct RegionGenerators
{
    double attenuation = 0.0; // centibels, before a channel's extra attenuation
    Sf2Envelope envelope;
    uint8_t root_key = 60;
    int coarse_tune = 0, fine_tune = 0; // semitones and cents
    bool loop = false;
    uint8_t exclusive_class = 0; // the region's key group, or 0 (always 0 on the Wii)
};

// A region's generators: its envelope `env` after the overrides in preset `key`, its tuning ratio `tune`, its volume
// over 127 with `quieter` more centibels of attenuation (see SoundFontCut), its root key and whether its wave loops.
// `ignore_note_off` keeps an inaudible sustain decaying after the note-off.
RegionGenerators ConvertRegion(const PresetKey& key, const EnvelopeValues& env, bool ignore_note_off, float tune,
                               uint8_t volume, uint8_t original_key, bool loop, double quieter,
                               const Settings& settings);

// Builds a zone for a key range and velocity range, using `sample` and the region's generators, for a channel at `pan`
// (the engine's -1, left, to 1, right) that adds `extra` centibels of attenuation.
seqlib::sf2::Zone RegionZone(const RegionGenerators& g, const std::pair<int, int>& keys,
                             const std::pair<int, int>& velocities, double pan, double extra, uint16_t sample);

// The spans of a region's `keys` for the zones of preset `key`: ResamplingSpans, without a low-pass where the track's
// fixed filter has the lower cutoff. Those keys keep the track's filter. A preset whose filter changes during notes
// (filter_controller) takes no low-pass. A preset whose zones fit their cutoff to the track's one-pole low-pass
// (FitsTrackLowPass) keeps ResamplingSpans' spans. Each of its zones picks one of the two low-passes (AddZoneLowPass).
std::vector<ResamplingSpan> ZoneSpans(std::pair<int, int> keys, const ResamplingRegion& region, const PresetKey& key,
                                      const Settings& settings);

// Adds a cutoff that tracks the key to `zone`: `base` + `slope` * k absolute cents at key k. The generator holds the
// cutoff at key 0, 64 or 128, the first that keeps it within SoundFont's range of 1500 to 13500 cents, or at key 0 when
// none does. A modulator from the note-on key adds the rest: unipolar from key 0, bipolar from key 64, or unipolar and
// negative from key 128. A slope of 0 takes no modulator.
void AddTrackingCutoff(seqlib::sf2::Zone& zone, int base, int slope);

// Adds the low-pass `filter` of a zone that plays `region` in preset `key` to `zone`: a cutoff that rises by 100 cents
// a key (AddTrackingCutoff). Where the track's filter has a resonance, the zone sets a resonance of 0.
void AddResamplingFilter(seqlib::sf2::Zone& zone, const ResamplingFilter& filter, const ResamplingRegion& region,
                         const PresetKey& key);

// Adds the low-pass of a zone that plays `region` in preset `key` to `zone`. The zone's span (ZoneSpans) has the
// resampling low-pass `filter`, or none. `fitted` is the zone's cutoff for the track's one-pole low-pass
// (ZoneLowPass::Fit), or nothing where the zone keeps the preset's cutoff. A zone of a preset that FitsTrackLowPass
// takes the lower of its two low-passes at the region's original key, with that low-pass's key tracking. Other zones
// take the resampling low-pass wherever their span has one.
void AddZoneLowPass(seqlib::sf2::Zone& zone, const ResamplingFilter* filter, const std::optional<FittedCutoff>& fitted,
                    const ResamplingRegion& region, const PresetKey& key, const Settings& settings);

// A region's zone up to its low-pass, for the wave's channel `channel`, the keys `keys` and the velocities
// `velocities` (RegionZone).
using ChannelZoneFunction =
    std::function<seqlib::sf2::Zone(std::size_t channel, std::pair<int, int> keys, std::pair<int, int> velocities)>;

// Adds the zones of a region that plays the key ranges `keys` at the velocity ranges `velocities` to `zones`: one for
// each span of a key range (ZoneSpans), each velocity range and each of the wave's `channels`. `zone` gives each zone
// up to its low-pass. AddZoneLowPass then adds the span's low-pass, with the cutoff that `low_pass` fits to the span
// (ZoneLowPass::Fit). The region is in preset `index`, whose key is `key`. The region plays `wave` as `region`
// describes. `wave_key` names the wave.
void AddRegionZones(std::vector<seqlib::sf2::Zone>& zones, uint32_t index, const PresetKey& key,
                    const std::vector<std::pair<int, int>>& keys, const std::vector<std::pair<int, int>>& velocities,
                    std::size_t channels, const ResamplingRegion& region, std::pair<uint32_t, uint32_t> wave_key,
                    const wave::Pcm& wave, ZoneLowPass& low_pass, const Settings& settings,
                    const ChannelZoneFunction& zone);

// Names presets by bank slot and program ("b0 p12"), or by program alone ("p12") without `bank_slots`. Adds a number
// when different track settings produce several presets from the same instrument (seqlib::sf2::PresetNames).
std::vector<std::string> PresetNames(const std::vector<PresetKey>& keys, bool bank_slots);

// Returns the volume of the region that `key`'s instrument plays at `note` and `velocity`, or 0 for none.
using RegionVolumeFunction = std::function<int(const PresetKey& key, int note, int velocity)>;

// Finds the highest region volume used by `perf`. Like the synth, it matches regions by preset, key and velocity.
int LoudestRegion(const Performance& perf, const RegionVolumeFunction& volume_of);

// The attenuation in centibels that brings a region of volume `loudest` down to 127, or 0 for a region of 127 or less.
double RegionVolumeCut(int loudest);

// The approximation that a SoundFont notes when its zones take `quieter` centibels of attenuation (RegionVolumeCut).
std::string RegionVolumeCutText(double quieter);

// The attenuation in centibels that every zone of a sequence's SoundFont takes. Region volumes can reach 255, but
// SoundFont zones can't amplify their samples. If the loudest region that `perf` plays (LoudestRegion) is above 127,
// every zone takes the same attenuation. That keeps the zones' relative levels, and `perf` notes the approximation. SFZ
// keeps the original volumes.
double SoundFontCut(Performance& perf, const RegionVolumeFunction& volume_of);

// A sequence's SoundFont, the samples that its presets share, and the warnings. A bank model's builder adds each
// preset's instrument from its bank (convert.cpp and convert_rsar.cpp).
class SoundFontParts
{
public:
    // The sample ids of the wave that `key` names, "w0_12" for key {0, 12}. On first use, `pcm` gives the decoded wave,
    // or nullptr for one that can't be read. `adjust`, if given, changes the prepared samples. The samples take root
    // key `root_key`, from the first region that plays the wave. No ids means that the wave can't be read.
    const std::vector<uint16_t>& Samples(std::pair<uint32_t, uint32_t> key, uint8_t root_key,
                                         const std::function<const wave::Pcm*()>& pcm,
                                         const std::function<void(seqlib::PreparedWave&)>& adjust = {});

    // Copies preset `index` to bank 128 for MIDI channel 10, at its program number. Warns if another preset already
    // holds that place.
    void AddChannel10Copy(uint32_t index);

    // The resulting SoundFont.
    seqlib::sf2::SoundFont& Result()
    {
        return sf_;
    }

    // Missing instruments and waves, and presets that MIDI channel 10 can't select.
    const std::vector<std::string>& Warnings() const
    {
        return warnings_;
    }

protected:
    seqlib::sf2::SoundFont sf_;
    std::vector<std::string> warnings_;

private:
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint16_t>> samples_; // wave key -> sample ids
};

} // namespace ntdlib::sequence_export
