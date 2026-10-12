// SPDX-License-Identifier: MIT

// The fields of a GameCube or Wii disc's header (sys/boot.bin) that more than one module reads.

#pragma once

#include <cstdint>

namespace ntdlib::disc
{

// The header's size.
constexpr uint32_t kHeaderSize = 0x440;

// The magic words that mark a Wii disc and a GameCube disc, and their offsets.
constexpr uint32_t kWiiMagicOffset = 0x18;
constexpr uint32_t kWiiMagic = 0x5D1C9EA3;
constexpr uint32_t kGameCubeMagicOffset = 0x1C;
constexpr uint32_t kGameCubeMagic = 0xC2339F3D;

// The file system table's offset and size: in bytes on a GameCube disc, and in units of 4 bytes in a Wii partition.
constexpr uint32_t kHeaderFstOffset = 0x424;
constexpr uint32_t kHeaderFstSize = 0x428;

// The size of the largest table among the game's discs, in the same units. A game on one disc gives its table's size.
constexpr uint32_t kHeaderFstMaxSize = 0x42C;

} // namespace ntdlib::disc
