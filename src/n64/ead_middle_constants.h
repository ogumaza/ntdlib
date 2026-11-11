// SPDX-License-Identifier: MIT

// The constants that the source files of the middle EAD model share. ead_middle.cpp contains the players,
// ead_middle_scripts.cpp the scripts, ead_middle_voices.cpp the voices and ead_middle_synthesis.cpp the audio lists.

#pragma once

#include <cstdint>

namespace ntdlib::n64::ead_middle_constants
{

// The bytes of an update's buffer of one channel and of two channels, in the lists for EAD's microcode and for the
// standard one.
inline constexpr uint32_t kOneChannel = 0x180;
inline constexpr uint32_t kTwoChannels = 0x300;
inline constexpr uint32_t kStdOneChannel = 0x140;
inline constexpr uint32_t kStdTwoChannels = 0x280;

// The ticks of a beat in a sequence.
inline constexpr int kTicksPerBeat = 48;

// The samples of each of the four harmonics that a synthetic wave holds (ead_later_tables.h).
inline constexpr int kWaveSamples = 64;

// The bits of a channel's mute behaviour: what the channel does while its player is muted.
inline constexpr uint8_t kMuteSoften = 1U << 5;
inline constexpr uint8_t kMuteStopNotes = 1U << 6;
inline constexpr uint8_t kMuteStopScript = 1U << 7;

// A voice's priorities.
inline constexpr uint8_t kPriorityDisabled = 0;
inline constexpr uint8_t kPriorityStopping = 1;
inline constexpr uint8_t kPriorityMin = 2;
inline constexpr uint8_t kPriorityDefault = 3;

} // namespace ntdlib::n64::ead_middle_constants
