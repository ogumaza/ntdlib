// SPDX-License-Identifier: MIT

// The track low-pass of the SoundFont zones, fitted to each zone's wave on the consoles whose track low-pass is AX's
// one-pole filter (Settings::zone_low_pass).
//
// AX's one-pole low-pass falls by 6 dB an octave above its cutoff. FluidSynth's low-pass has two poles and falls by
// 12 dB an octave. The fitted tables (filter.h) give the two-pole filter the cutoff whose response comes closest to the
// one-pole's in shape. The two filters still pass different amounts of a sound. Just above the cutoff, the two-pole
// filter passes more. Far above the cutoff, it passes less. The difference depends on where the sound's power lies. A
// sound whose power lies just above a low cutoff plays several decibels louder through the table's cutoff than in the
// game.
//
// A zone of a preset with the one-pole's cutoff therefore takes the cutoff at which FluidSynth passes as much of the
// zone's wave as AX does. A zone keeps the table's cutoff where that cutoff comes within 1 dB of the game's level at
// every key. The table's cutoff matches the response's shape best. The fit compares the two filters at each key that
// the performance plays in the zone. The key moves the wave's spectrum. The game's cutoff stays where it is. Each key
// has a cutoff that matches the game's level. The zone takes the mean of those cutoffs, weighted by how much each key's
// level moves with the cutoff. Where that cutoff misses the game's level by more than 1 dB at one of the keys, the
// cutoff tracks the key instead: a line fitted through the keys' cutoffs (AddTrackingCutoff).
//
// The level is the power from 20 Hz up. In the game, the power ends at 16 kHz, half of AX's rate of 32 kHz. In
// FluidSynth, it ends at 22.05 kHz, half of FluidSynth's usual rate of 44.1 kHz. FluidSynth gives every voice a gain of
// 1 / sqrt(Q). The fit leaves that gain out. A key plays at its pitch without bend, sweep or LFO.

#pragma once

#include <bitset>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/settings.h"
#include "ntdlib/wave/decode.h"

namespace ntdlib::sequence_export
{

// A wave's power in bands of 1/24 octave.
struct WaveSpectrum
{
    std::vector<double> hz;    // each band's frequency when the wave plays at its sample rate
    std::vector<double> power; // each band's power, summed over the wave's channels
};

// Measures the spectrum of the first 2^15 samples of `wave`, with every sample weighted alike.
WaveSpectrum MeasureSpectrum(const wave::Pcm& wave);

// A zone's cutoff: `base` + `slope` * k absolute cents at key k.
struct FittedCutoff
{
    bool operator==(const FittedCutoff&) const = default;

    int base = 0;
    int slope = 0;
};

// Fits the cutoff of a zone to the game's one-pole low-pass at `game_hz` (SoundFontFilter::low_pass_hz). The zone
// plays a wave with `spectrum` at `keys`. The wave plays at its sample rate times `tune` at `original_key`. Returns
// nothing where the table's cutoff `table_cutoff` comes within 1 dB of the game's level at every key, and where the
// cutoff moves the level of none of the keys.
std::optional<FittedCutoff> FitZoneCutoff(const WaveSpectrum& spectrum, int original_key, double tune, uint32_t game_hz,
                                          int table_cutoff, const std::vector<int>& keys, const Settings& settings);

// The keys that each preset of `perf` plays, by the preset's index.
std::vector<std::bitset<128>> PlayedKeys(const Performance& perf);

// Whether the zones of preset `key` fit their cutoff to the track's one-pole low-pass: on a console with the fit
// (Settings::zone_low_pass), for a preset with the one-pole's cutoff and a filter that stays fixed during notes.
bool FitsTrackLowPass(const PresetKey& key, const Settings& settings);

// Fits the cutoffs of the zones of a sequence's SoundFont (FitZoneCutoff). Measures each wave's spectrum once.
class ZoneLowPass
{
public:
    ZoneLowPass(const Performance& perf, const Settings& settings);

    // The cutoff of the zones that play `keys` of a region in preset `index`, whose key is `key`. The region plays
    // `wave` at `original_key` and `tune`. `wave_key` names the wave. Returns nothing for zones that keep the preset's
    // cutoff: where FitsTrackLowPass is false, for keys that the performance doesn't play, and where FitZoneCutoff
    // returns nothing.
    std::optional<FittedCutoff> Fit(uint32_t index, const PresetKey& key, std::pair<int, int> keys, int original_key,
                                    double tune, std::pair<uint32_t, uint32_t> wave_key, const wave::Pcm& wave);

private:
    std::vector<std::bitset<128>> played_;
    std::map<std::pair<uint32_t, uint32_t>, WaveSpectrum> spectra_;
    const Settings& settings_;
};

} // namespace ntdlib::sequence_export
