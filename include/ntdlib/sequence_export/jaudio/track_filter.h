// SPDX-License-Identifier: MIT

// A JAudio track's IIR filter, as SoundFont and SFZ zones can play it.
//
// The filter's command takes an entry of a table of 128 coefficient sets (0x80397f70 in the USA release of The Wind
// Waker's main.dol, 0x804a38f8 in Pikmin 2's). Each set holds four 16-bit values. They run linearly from entry 0 to
// entry 127. Entry 127 passes the signal through. The filter is y[n] = (c0 x[n] + c1 x[n-1] + c2 y[n-1] + c3 y[n-2]) /
// 32768 at the game's output rate. GCSF's game mode, which runs the game's code, fits that form within 0.06 dB rms.
//
// The filter falls to a shelf above a few kilohertz. A zone's low-pass goes on falling. A region therefore becomes two
// zones: one unfiltered and one low-passed, with gains that together follow the filter's curve. In a model of
// FluidSynth 2.6.1's low-pass, entry 64 comes within 0.7 dB rms of the curve this way, and within 2.9 dB with the
// low-pass alone. Rendered through FluidSynth 2.6.1 against GCSF's game mode, The Wind Waker's goma_a went from 3.9 dB
// to 1.2 dB in spectrum with the two zones, and to 2.6 dB with the low-pass alone.
//
// The fit takes the low-pass that each format gives a zone at a resonance of 0. FluidSynth and the other SoundFont
// players read 0 cB as a Q of 1/sqrt(2), 3 dB down at the cutoff. sfzformat.com gives SFZ's resonance as the gain at
// the cutoff. 0 dB is a Q of 1.

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace ntdlib::sequence_export::jaudio
{

// The table's entry that passes the signal through.
constexpr uint8_t kOpenTrackFilter = 127;

// A zone that a region becomes under a track's filter. Its sound is the region's times `gain`, through a low-pass at
// `cutoff_cents` absolute cents if there's a cutoff.
struct FilterLayer
{
    double gain = 1.0;
    std::optional<int> cutoff_cents;
};

// Whether entry `entry` moves the sound by at least 0.3 dB in some band up to 16 kHz at the game's output rate `rate`,
// in Hz. A smaller change can't be heard, and the zones leave the filter out.
bool TrackFilterHeard(uint8_t entry, double rate);

// The format whose low-pass the zones use.
enum class FilterFormat
{
    kSoundFont,
    kSfz,
};

// The zones for entry `entry` at the game's output rate `rate`, in Hz, in `format`. The open filter gives one zone,
// unchanged.
std::vector<FilterLayer> TrackFilterLayers(uint8_t entry, double rate, FilterFormat format);

// The frequency of `cents` absolute cents, in Hz.
double CutoffHz(int cents);

} // namespace ntdlib::sequence_export::jaudio
