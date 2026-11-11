// SPDX-License-Identifier: MIT

// The MIDI status bytes that the sources of libultra's sequence players share. al_player.cpp contains the players, and
// al_player_sequences.cpp the readers of their sequences.

#pragma once

#include <cstdint>

namespace ntdlib::n64::al_player_constants
{

inline constexpr uint8_t kStatusNoteOff = 0x80;
inline constexpr uint8_t kStatusNoteOn = 0x90;
inline constexpr uint8_t kStatusPolyPressure = 0xA0;
inline constexpr uint8_t kStatusController = 0xB0;
inline constexpr uint8_t kStatusProgram = 0xC0;
inline constexpr uint8_t kStatusChannelPressure = 0xD0;
inline constexpr uint8_t kStatusPitchBend = 0xE0;

} // namespace ntdlib::n64::al_player_constants
