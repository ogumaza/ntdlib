// SPDX-License-Identifier: MIT

// The constants that the source files of the SBN engine model share. sbn_player.cpp contains the players, the voices
// and the envelopes. sbn_player_synthesis.cpp contains the synthesis driver and the audio lists.

#pragma once

#include <cstdint>

#include "ntdlib/n64/sbn_player.h"

namespace ntdlib::n64::sbn_player_constants
{

inline constexpr int kChunk = SbnPlayer::kChunkSamples;
inline constexpr int kChunkBytes = kChunk * 2;
inline constexpr int kFxLength = 14 * kChunk;

// The channel that the delay of a bus acts on.
inline constexpr uint8_t kDelayNone = 0;
inline constexpr uint8_t kDelayLeft = 1;
inline constexpr uint8_t kDelayRight = 2;

} // namespace ntdlib::n64::sbn_player_constants
