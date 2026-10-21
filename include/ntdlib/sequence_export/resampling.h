// SPDX-License-Identifier: MIT

// The low-passes that stand in for a console's resampling filters in the SoundFonts and the SFZ files.
//
// A console converts each voice's wave to its output rate through a filter. It picks the filter by the voice's rate:
// the wave's sample rate times the voice's pitch ratio, over the output rate. AX on the Wii and the Wii U has three
// sets of 4-tap filters, which the SDK names 8, 12 and 16 kHz. AX takes the 8 kHz set above a rate of 4/3, the 12 kHz
// set above 1 and the 16 kHz set otherwise. The Switch's audio renderer holds the same three sets, and picks them by
// the same thresholds of its step in Q15. The 3DS's DSP has three polyphase kernels whose responses come within 0.1 dB
// of the sets'. It picks them by the same thresholds. A rate of exactly 1 plays unfiltered there. Each filter acts on
// the wave's samples. Its response is therefore fixed in cycles per wave sample, and its cutoff in hertz follows the
// voice's pitch.
//
// The consoles pick at different times. NW4R SND on the Wii picks when it sets the voice up, before the channel gives
// the voice its pitch, while the voice's pitch is still 1 (Voice::Setup, Wii Sports' main.dol 0x8004c454). The pick
// then depends on the wave's sample rate alone. nw::snd on the Wii U picks when the voice starts, at the pitch then,
// and keeps the pick. The 3DS picks again whenever the pitch changes. The Switch's renderer picks in every frame. On
// the Wii U and the 3DS, only regions with the default interpolation (0) play through the filters. Linear
// interpolation (1) and none (2) leave them out.
//
// A synth's interpolation loses highs too. FluidSynth and sfizz interpolate with a Catmull-Rom spline by default. A
// SoundFont zone or an SFZ region whose notes play above a rate of 1 takes a low-pass for the rest: the game's loss
// beyond the spline's. The 16 kHz set and kernel 2 lose about as much as the spline. A rate of 1 or below therefore
// takes no low-pass. The low-pass's cutoff is a fraction of the rate at which the note plays its wave: the wave's
// sample rate times the note's pitch ratio. The cutoff therefore tracks the key at 100 cents a key. The script
// tools/sequence_export/resampling_filters.py fits the fractions to the synths' digital filters at their default rates,
// over the top two octaves of the console's output. The fractions therefore differ with the console's mixing rate.
//
// A zone or a region picks by its notes' rate at the key, without bend, sweep or LFO. That follows the Wii exactly. On
// the other consoles, a bend, a sweep or an LFO can move a note's rate across a threshold. The game then plays the note
// through another filter for a time. A zone's low-pass stays fixed and can't follow that. The performer doesn't report
// such notes.

#pragma once

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "ntdlib/sequence_export/settings.h"

namespace ntdlib::sequence_export
{

// The low-pass for the notes whose rate is above `above`. Each cutoff is a fraction of the rate at which a note plays
// its wave: the wave's sample rate times the note's pitch ratio.
struct ResamplingFilter
{
    double above = 1.0;      // the rate above which the console plays through the filter
    double cutoff = 1.0;     // SoundFont: FluidSynth's two-pole low-pass at a resonance of 0
    double sfz_cutoff = 1.0; // SFZ: the cutoff of sfizz's one-pole lpf_1p
};

// A console's resampling filters: the rate in hertz at which the console mixes its voices, and the low-passes for its
// filters from the highest rate down.
struct ResamplingRule
{
    double output_rate = 32000.0;
    std::span<const ResamplingFilter> filters;
};

// The rule of each console.
extern const ResamplingRule kCtrResampling;
extern const ResamplingRule kRevolutionResampling;
extern const ResamplingRule kCafeResampling;
extern const ResamplingRule kSwitchResampling;

// What decides the low-pass of a region's notes: the region's original key, tuning ratio and interpolation, and its
// wave's sample rate.
struct ResamplingRegion
{
    uint8_t original_key = 60;
    float tune = 1.0f;
    uint32_t sample_rate = 32000;
    uint8_t interpolation = 0; // 0 the default, 1 linear, 2 none
};

// The rate of a note at `key` on `region`, without bend, sweep or LFO: the wave's sample rate times the note's pitch
// ratio, over the console's output rate. On the Wii, the pitch ratio counts as 1.
double ResamplingRate(int key, const ResamplingRegion& region, const Settings& settings);

// The low-pass for a note at `key` on `region` (ResamplingRate), or nullptr for none. A note gets none at a rate of 1
// or below, on a region whose interpolation leaves the filters out, or on a console without a rule.
const ResamplingFilter* ResamplingFilterOf(int key, const ResamplingRegion& region, const Settings& settings);

// The cutoff in hertz of `fraction` for a note at `key` on `region`: the fraction of the wave's sample rate times the
// note's pitch ratio.
double ResamplingCutoffHz(double fraction, int key, const ResamplingRegion& region);

// A span of a region's keys whose notes take the same low-pass, or none.
struct ResamplingSpan
{
    std::pair<int, int> keys;
    const ResamplingFilter* filter = nullptr;
};

// Splits a region's `keys` into spans by their low-pass (ResamplingFilterOf). The rate rises with the key. Each
// low-pass therefore takes one span at most.
std::vector<ResamplingSpan> ResamplingSpans(std::pair<int, int> keys, const ResamplingRegion& region,
                                            const Settings& settings);

} // namespace ntdlib::sequence_export
