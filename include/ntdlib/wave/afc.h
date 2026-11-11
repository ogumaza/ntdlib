// SPDX-License-Identifier: MIT

// The predictor pairs of the GameCube's AFC format. The decoders in adpcm.h and the tables of JAudio's DSP program both
// read them.

#pragma once

#include <array>
#include <cstdint>

namespace ntdlib::wave
{

// The 16 predictor pairs that JAudio gives the GameCube's DSP program for AFC, in 2048ths. A frame's header picks one.
inline constexpr std::array<std::array<int32_t, 2>, 16> kAfcCoefficients{{
    {0, 0},
    {2048, 0},
    {0, 2048},
    {1024, 1024},
    {4096, -2048},
    {3584, -1536},
    {3072, -1024},
    {4608, -2560},
    {4200, -2248},
    {4800, -2300},
    {5120, -3072},
    {2048, -2048},
    {1024, -1024},
    {-1024, 1024},
    {-1024, 0},
    {-2048, 0},
}};

} // namespace ntdlib::wave
