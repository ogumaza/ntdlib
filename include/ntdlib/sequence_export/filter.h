// SPDX-License-Identifier: MIT

// Track filters and their SoundFont/SFZ equivalents.
//
// A track has a low-pass filter (0xd8) and a biquad filter (0xb4 type, 0xb5 value). The engine updates both on sounding
// voices every frame, and the renderer runs them on each voice. The low-pass has one pole: 23 cutoffs from 80 to
// 12800 Hz on the 3DS, the Wii U and the Switch. The Wii's builds of NW4R SND before November 2008 have 58 from 31 to
// 14993 Hz. Later builds take the Wii U's cutoffs (RevolutionFilters). The biquad selects coefficients
// from preset tables: one low-pass, one high-pass and three band-passes, all Robert Bristow-Johnson cookbook filters.
// The Switch has ten: types 1 to 5 are the Wii U's tables for its 32 kHz renderer, and types 6 to 10 are the same
// filters made for 48 kHz. NW4R SND has the biquad from its builds of June 2008.
//
// SoundFont provides one resonant two-pole low-pass. It matches the biquad low-pass and approximates the one-pole
// filter with fitted cutoffs. High-pass and band-pass are omitted. When both low-passes are active, the conversion uses
// the lower cutoff.
//
// SFZ supports all these filters: sfizz's lpf_1p matches the one-pole; lpf_2p, hpf_2p and bpf_2p are cookbook filters.
// The track settings are sent on two MIDI controllers.
//
// Each console's fitted tables are a FilterTables. The script tools/sequence_export/filter_tables.py writes them from
// the game's code.

#pragma once

#include <cstdint>
#include <span>

#include "ntdlib/sequence_export/settings.h"
#include "ntdlib/sound_build.h"
#include "seqlib/midi/performance.h"

namespace ntdlib::sequence_export
{

// Open SoundFont filter cutoff (initialFilterFc, absolute cents).
constexpr uint16_t kFilterOpen = 13500;

// SoundFont cutoff controller for filters that change during a note. A modulator subtracts kFilterControllerRange *
// value / 128 cents from kFilterOpen: 75 cents per step. Zero leaves the filter open.
constexpr uint8_t kFilterController = 102;
constexpr int kFilterControllerRange = 9600;

// Returns the value of kFilterController that comes closest to `cutoff`.
uint8_t FilterControllerValue(uint16_t cutoff);

// SFZ filter controllers. Low-pass uses 64 minus the 0xd8 argument (6 or less means off). Biquad uses the 0xb5
// argument, or 0 for types without a preset. The default controller value of 0 disables both.
constexpr uint8_t kSfzLowPassController = 103;
constexpr uint8_t kSfzBiquadController = 104;

// The events that set the three controllers above.
constexpr seqlib::midi::EventKind kFilterEvent = seqlib::midi::ControlKind(kFilterController);
constexpr seqlib::midi::EventKind kSfzLowPassEvent = seqlib::midi::ControlKind(kSfzLowPassController);
constexpr seqlib::midi::EventKind kSfzBiquadEvent = seqlib::midi::ControlKind(kSfzBiquadController);

// A SoundFont filter setting.
struct SoundFontFilter
{
    bool operator==(const SoundFontFilter&) const = default;

    uint16_t cutoff = kFilterOpen; // initialFilterFc, in absolute cents
    uint8_t resonance = 0;         // initialFilterQ, in centibels
    uint16_t low_pass_hz = 0;      // the one-pole's cutoff in Hz that `cutoff` stands for, or 0 for a biquad's or none
};

// An SFZ biquad setting: cutoff/centre frequency in cents relative to kSfzBiquadBase, plus resonance (Q in dB) and gain
// in 0.01 dB units.
struct SfzBiquad
{
    int16_t cutoff = 0;
    int16_t resonance = 0;
    int16_t gain = 0;
};

// A console's fitted filter tables.
struct FilterTables
{
    std::span<const uint16_t> low_pass_cutoff;            // SoundFont cutoff for each low-pass step
    std::span<const int16_t> sfz_low_pass_cutoff;         // SFZ cutoff for each low-pass step (see SfzLowPassCutoff)
    std::span<const uint16_t> low_pass_hz;                // the Wii's cutoff in Hz for each step, or empty
    std::span<const SoundFontFilter> biquad_low_pass;     // SoundFont form of biquad type 1's steps
    std::span<const SoundFontFilter> biquad_low_pass_48k; // SoundFont form of biquad type 6's steps, or empty
    std::span<const SfzBiquad> sfz_biquads[10];           // SFZ form of each biquad type's steps, or empty
};

// The tables of each console. kRevolutionFilters are the tables of the Wii's builds before November 2008.
extern const FilterTables kCtrFilters;
extern const FilterTables kRevolutionFilters;
extern const FilterTables kCafeFilters;
extern const FilterTables kSwitchFilters;

// The Wii's tables for `build` of NW4R SND. Builds before November 2008 take kRevolutionFilters. Later builds take the
// low-pass's cutoffs from kLpfFreq, as the Wii U does (SndBuild::SinceNovember2008). Their tables give each 0xd8
// argument that leaves the low-pass on the Wii U's cutoffs for the argument.
const FilterTables& RevolutionFilters(const SndBuild& build);

// Returns the value of kSfzLowPassController for the 0xd8 argument `low_pass`.
uint8_t SfzLowPassValue(int low_pass);

// Returns the value of kSfzBiquadController for the 0xb4 and 0xb5 arguments `biquad_type` and `biquad_value`.
uint8_t SfzBiquadValue(int biquad_type, int biquad_value, const Settings& settings);

// Returns the kind of filter that biquad preset `biquad_type` is: 1 low-pass, 2 high-pass, or 3 to 5 band-pass, for
// types 1 to 5 and, on the Switch, 6 to 10 alike. A type with no preset gives 0.
int BiquadShape(int biquad_type, const Settings& settings);

// The Wii's low-pass setting for the 0xd8 argument `low_pass`: 0 to 57, from a cutoff of 31 Hz to 14993 Hz, or -1 for
// off.
int LowPassIndex(int low_pass);

// AX's cutoff in Hz on the Wii for low-pass setting `index` (0-57), in `build` of NW4R SND.
int LowPassHz(int index, const SndBuild& build);

// Active filters derived from a track's settings.
struct GameFilters
{
    int low_pass = -1;       // the low-pass step, or -1 for off
    uint8_t biquad_type = 0; // 1-10 (see BiquadShape), 0 off
    int biquad_step = 0;     // index into the preset's coefficient table
};

// Decodes 0xd8 (`low_pass`), 0xb4 (`biquad_type`) and 0xb5 (`biquad_value`) as the engine does.
GameFilters TrackFilters(int low_pass, int biquad_type, int biquad_value, const Settings& settings);

// Converts `filters` to a SoundFont filter approximation.
SoundFontFilter ToSoundFont(const GameFilters& filters, const Settings& settings);

// Open SFZ low-pass cutoff, lowered by kSfzLowPassController. This is sfizz's maximum for lpf_1p.
constexpr int kSfzLowPassOpen = 20000; // Hz

// Returns the SFZ lpf_1p cutoff of low-pass step `index`, in cents relative to kSfzLowPassOpen.
int SfzLowPassCutoff(int index, const Settings& settings);

// Reference frequency for SFZ biquad cutoffs.
constexpr int kSfzBiquadBase = 1000; // Hz

// Returns biquad preset `type` at `step` as an SFZ filter.
SfzBiquad SfzBiquadStep(int type, int step, const Settings& settings);

} // namespace ntdlib::sequence_export
