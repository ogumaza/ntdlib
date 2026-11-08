// SPDX-License-Identifier: MIT

// The bytes of the entries of the driver's tables in ROM, and of a song's record.

#pragma once

#include <cstdint>

namespace ntdlib::rd1
{

// The bytes of an entry of the song table: the song's record and the player that plays it.
inline constexpr uint32_t kSongEntryBytes = 8;

// The bytes of a song's record.
inline constexpr uint32_t kSongRecordBytes = 20;

// The bytes of a player's entry in the player table: its record in RAM.
inline constexpr uint32_t kPlayerEntryBytes = 12;

// The bytes of a player's entry in the players' setup: what the init sets the player up from.
inline constexpr uint32_t kSetupBytes = 20;

} // namespace ntdlib::rd1
