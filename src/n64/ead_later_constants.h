// SPDX-License-Identifier: MIT

// The constants that the source files of the later EAD model share. ead_later.cpp contains the sound files and the
// players, ead_later_scripts.cpp the scripts of the players and channels, ead_later_layers.cpp the scripts of the
// layers, ead_later_voices.cpp the voices and ead_later_synthesis.cpp the audio lists.

#pragma once

#include <cstdint>

namespace ntdlib::n64::ead_later_constants
{

// The bytes of an update's buffer of one channel (13 frames of 16 samples) and of two channels.
inline constexpr uint32_t kOneChannel = 0x1A0;
inline constexpr uint32_t kTwoChannels = 0x340;

inline constexpr uint32_t kSurroundStateSize = 0x100;

// The samples of each of the four harmonics that a synthetic wave holds (ead_later_tables.h).
inline constexpr int kWaveSamples = 64;

// The bits of a channel's mute behaviour: what the channel does while its player is muted.
inline constexpr uint8_t kMuteStopVoices = 1U << 3;
inline constexpr uint8_t kMuteStopLayerNotes = 1U << 4;
inline constexpr uint8_t kMuteSoften = 1U << 5;
inline constexpr uint8_t kMuteStopNotes = 1U << 6;
inline constexpr uint8_t kMuteStopScript = 1U << 7;

// The value by which the flow commands report a script's end.
inline constexpr int kScriptEnd = -1;

inline constexpr int kTicksPerBeat = 48;

} // namespace ntdlib::n64::ead_later_constants
