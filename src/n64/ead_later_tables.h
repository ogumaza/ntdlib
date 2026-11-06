// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstdint>

namespace ntdlib::n64::ead_later_tables
{

// The synthetic waves, 8 of 256 samples: each holds 64, 32, 16 and 8 samples to the period, its harmonics.
extern const std::array<int16_t, 2048> kWaves;

// The frequency scale of a bend across two octaves, with the value 128 at 1.
extern const std::array<float, 256> kBendOctave;

// The frequency scale of a fine bend, across four semitones.
extern const std::array<float, 256> kBendFine;

// The frequency scale of each note.
extern const std::array<float, 128> kPitchFrequencies;

// The velocities that a short note's command can name by default.
extern const std::array<uint8_t, 16> kShortVelocity;

// The gate times that a short note's command can name by default.
extern const std::array<uint8_t, 16> kShortGate;

// The delay in bytes of a side at each halved pan, for a channel with stereo effects in headset mode.
extern const std::array<uint16_t, 64> kHaasDelays;

// A book that a channel's book offset can choose.
extern const std::array<int16_t, 32> kNoteBook;

// The volume of a side at each pan, for a channel with stereo effects in headset mode.
extern const std::array<float, 128> kHeadsetPanVolume;

// The volume of a side at each pan, for a channel with stereo effects in stereo mode.
extern const std::array<float, 128> kStereoPanVolume;

// The volume of a side at each pan, in the default mode.
extern const std::array<float, 128> kDefaultPanVolume;

// The low-pass filters' taps, 8 for each cutoff. The first is the identity.
extern const std::array<int16_t, 128> kLowPass;

// The high-pass filters' taps, 8 for each cutoff from 1.
extern const std::array<int16_t, 120> kHighPass;

} // namespace ntdlib::n64::ead_later_tables
