// SPDX-License-Identifier: MIT

// The constants that the source files of the early EAD model share. ead_early.cpp contains the sound files and the
// players, ead_early_scripts.cpp the scripts, ead_early_voices.cpp the voices and ead_early_synthesis.cpp the audio
// lists.

#pragma once

#include <cstdint>

namespace ntdlib::n64::ead_early_constants
{

// The bits of a channel's mute behaviour: what the channel does while its player is muted.
inline constexpr uint8_t kMuteStopScript = 0x80;
inline constexpr uint8_t kMuteStopNotes = 0x40;
inline constexpr uint8_t kMuteSoften = 0x20;

// A voice's priorities.
inline constexpr uint8_t kPriorityDisabled = 0;
inline constexpr uint8_t kPriorityStopping = 1;
inline constexpr uint8_t kPriorityMinimum = 2;
inline constexpr uint8_t kPriorityDefault = 3;

// A player's fade states.
inline constexpr uint8_t kStateFadeOut = 1;
inline constexpr uint8_t kStateFadeIn = 2;
inline constexpr uint8_t kStateFadeTo = 3;
inline constexpr uint8_t kStateFadeStay = 4;

// The pool of voices that every player and channel can take.
inline constexpr int kGlobalPool = 0;

// The tatums in a beat. A player's tempo counts tatums a minute.
inline constexpr int kTatumsPerBeat = 48;

} // namespace ntdlib::n64::ead_early_constants
