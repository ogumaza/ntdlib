// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstdint>

namespace ntdlib::n64::ead_tables
{

// The frequency scale of each note.
extern const std::array<float, 128> kNoteFrequencies;

// The frequency scale of a bend, from -1 to 1 octave in 255 steps.
extern const std::array<float, 255> kBendScale;

// The volume of a side at each pan, in the default mode.
extern const std::array<float, 128> kDefaultPanVolume;

// The volume of a side at each pan, for a channel with stereo effects in stereo mode.
extern const std::array<float, 128> kStereoPanVolume;

// The volume of a side at each pan, for a channel with stereo effects in headset mode.
extern const std::array<float, 128> kHeadsetPanVolume;

// A ramp of 136 samples: the factor of the target volume.
extern const std::array<float, 128> kRampTarget136;

// A ramp of 136 samples: the factor of the source volume.
extern const std::array<float, 128> kRampSource136;

// A ramp of 144 samples: the factor of the target volume.
extern const std::array<float, 128> kRampTarget144;

// A ramp of 144 samples: the factor of the source volume.
extern const std::array<float, 128> kRampSource144;

// A ramp of 128 samples: the factor of the target volume.
extern const std::array<float, 128> kRampTarget128;

// A ramp of 128 samples: the factor of the source volume.
extern const std::array<float, 128> kRampSource128;

// A quarter of the vibrato's wave.
extern const std::array<int8_t, 16> kVibratoCurve;

// The delay in samples of the far side in headset mode, at each of ten pans.
extern const std::array<uint16_t, 10> kHeadsetPanDelay;

// The velocities that a short note's command can name by default.
extern const std::array<uint8_t, 16> kShortVelocity;

// The gate times that a short note's command can name by default.
extern const std::array<uint8_t, 16> kShortGate;

// The synthetic waves: sawtooth, triangle, sine and square.
extern const std::array<std::array<int16_t, 64>, 4> kWaves;

} // namespace ntdlib::n64::ead_tables
