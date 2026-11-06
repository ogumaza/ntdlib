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
//
// Two consoles limit a voice's pitch. nw::snd on the Wii U gives AX a starting rate of 8 or less (AxVoice::UpdateSrc,
// amiibo Festival's bbq.rpx 0x022A3954). AX holds later rates to 8 (AXSetVoiceSrcRatio). A note whose pitch asks for
// more plays at 8. nn::atk on the Switch holds a voice's pitch ratio between 1/16 and 16 (LowLevelVoice::UpdateParam,
// WarioWare: Get It Together!'s main 0xc270b0). The rule's limits give both. A note whose key starts past a limit and
// whose pitch holds still gets a preset of its own (PresetKey::at_pitch_limits). The preset's keys past a limit take a
// separate zone or region. That plays every key at the limit: a scale tuning of 0 in the SoundFont and pitch_keytrack=0
// in the SFZ files. At the Wii U's limit, the filters' cutoffs lie above 45 kHz. At the Switch's floor of 1/16, the
// rate stays below 1. At its ceiling of 16, the cutoffs lie above 24 kHz for every wave of 9 kHz or more. A span at a
// limit therefore takes no low-pass. A zone at a limit takes the
// preset's track low-pass without a fit (zone_low_pass.h): the fit follows each key's pitch. A note whose pitch moves,
// by a sweep, an LFO in its part's bends or a bend, keeps every key at its pitch. A zone at the limit would move it
// away from the limit. In the game, the note stays at the limit while its pitch would pass it, and follows its pitch
// inside the limits. In the files, it plays past the limit. Only a pitch held to the limit in each frame of the part's
// bends could follow the game.

#pragma once

#include <cstdint>
#include <optional>
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
// filters from the highest rate down. Then the limits on a voice's pitch, each 0 for none: the highest rate, and the
// lowest and the highest pitch ratio.
struct ResamplingRule
{
    double output_rate = 32000.0;
    std::span<const ResamplingFilter> filters;
    double rate_limit = 0.0;
    double pitch_floor = 0.0;
    double pitch_ceiling = 0.0;
};

// The rule of each console.
extern const ResamplingRule kCtrResampling;
extern const ResamplingRule kRevolutionResampling;
extern const ResamplingRule kCafeResampling;
extern const ResamplingRule kSwitchResampling;

// What decides the low-pass of a region's notes: the region's original key, tuning ratio and interpolation, and its
// wave's sample rate. `at_limits`: the preset's notes start past one of the console's pitch limits and hold their
// pitch (PresetKey::at_pitch_limits). Only then do the keys past a limit play at the limit (PitchLimitSpans).
struct ResamplingRegion
{
    uint8_t original_key = 60;
    float tune = 1.0f;
    uint32_t sample_rate = 32000;
    uint8_t interpolation = 0; // 0 the default, 1 linear, 2 none
    bool at_limits = false;
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

// A span of a region's keys whose notes take the same low-pass, or none. The notes of a span with `limit_cents` start
// past one of the console's pitch limits. Every key of the span plays at the limit: that many cents above the wave's
// rate.
struct ResamplingSpan
{
    std::pair<int, int> keys;
    const ResamplingFilter* filter = nullptr;
    std::optional<int> limit_cents = std::nullopt;
};

// The lowest and the highest pitch ratio at which the console plays a note on `region`. The Wii U's highest is its rate
// limit times the output rate, over the wave's sample rate. The Switch's are 1/16 and 16. A console without limits
// gives 0 and infinity.
std::pair<double, double> PitchLimits(const ResamplingRegion& region, const Settings& settings);

// True if a note at `key` on `region` starts past one of the console's pitch limits (PitchLimits), without bend, sweep
// or LFO.
bool PastPitchLimit(int key, const ResamplingRegion& region, const Settings& settings);

// Splits a region's `keys` at the console's pitch limits (PitchLimits), without low-passes. The pitch rises with the
// key. The keys below the lowest pitch therefore take one span at that pitch. The keys above the highest pitch take one
// at that pitch. The keys between them take the span between. Any of the spans can be missing. A region whose notes
// don't hold their pitch (ResamplingRegion::at_limits) keeps one span.
std::vector<ResamplingSpan> PitchLimitSpans(std::pair<int, int> keys, const ResamplingRegion& region,
                                            const Settings& settings);

// Splits a region's `keys` into spans by their low-pass (ResamplingFilterOf), between the pitch limits
// (PitchLimitSpans). Each low-pass therefore takes one span at most. A span at a limit takes no low-pass.
std::vector<ResamplingSpan> ResamplingSpans(std::pair<int, int> keys, const ResamplingRegion& region,
                                            const Settings& settings);

} // namespace ntdlib::sequence_export
