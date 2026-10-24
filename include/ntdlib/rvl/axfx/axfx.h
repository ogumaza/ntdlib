// SPDX-License-Identifier: MIT

// The RVL SDK's effects for AX's aux buses (AXFX), modelled on the games' code. A game registers an effect's callback
// for a bus. In each AX frame, the callback takes the bus's samples as 32-bit integers: 96 in each of the three
// channels of AX's stereo mode (left, right and surround). It replaces them with the effect's output. The DSP mixes the
// output back into the main mix two frames later.
//
// Each model follows the game's functions operation for operation, in single-precision floats. It then gives the same
// samples as the game's code on Broadway. That requires the compiler to keep each multiply and add apart:
// xsflib_project turns off their contraction into fused multiply-adds for every target, with clang-cl too.

#pragma once

#include <array>
#include <cstdint>

namespace ntdlib::rvl::axfx
{

// The samples in each channel of an AX frame.
constexpr int kSamplesPerFrame = 96;

// The channels of an aux bus in AX's stereo mode, in the order that the callback gets them.
enum Channel
{
    kLeft,
    kRight,
    kSurround,
    kChannelCount,
};

// One frame of an aux bus: 96 samples of each channel.
struct AuxFrame
{
    std::array<std::array<int32_t, kSamplesPerFrame>, kChannelCount> channels{};
};

// The bits of an effect's `active` field in the game. While the field isn't 0, the effect's callback leaves frames as
// they are. kStopped: the effect is off. kSkipFrame: the next callback leaves its frame alone and clears the bit. The
// comment on each class's `active_` lists the functions that set and clear the bits.
constexpr uint32_t kStopped = 1;
constexpr uint32_t kSkipFrame = 2;

// The builds of the AXFX library that the models follow, by the games that link them: October 2006 (WarioWare: Smooth
// Moves), December 2006 (Wii Sports and Wii Play) and September 2008 (Animal Crossing: City Folk).
enum class Build
{
    kOctober2006,
    kDecember2006,
    kSeptember2008,
};

} // namespace ntdlib::rvl::axfx
