// SPDX-License-Identifier: MIT

// The constants that the source files of libultra's synthesizer share. al_synth.cpp contains the voices and their
// parameters. al_synth_standard.cpp builds the audio lists for the standard microcode, and al_synth_compact.cpp the
// lists for the compact microcode.

#pragma once

#include <cstdint>

namespace ntdlib::n64::al_synth_constants
{

inline constexpr int kAdpcmFrameBytes = 9;
inline constexpr int kAdpcmFrameSamples = 16;
inline constexpr int kAdpcmFrameShift = 4;
inline constexpr double kMaxRatio = 1.99996;
inline constexpr int32_t kUnityPitch = 0x8000;
inline constexpr uint32_t kForever = 0xFFFFFFFF;

} // namespace ntdlib::n64::al_synth_constants
