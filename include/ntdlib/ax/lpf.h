// SPDX-License-Identifier: MIT

// The coefficients of AX's one-pole low-pass filter for a cutoff frequency: AXGetLpfCoefs on the Wii, and
// AXComputeLpfCoefs on the Wii U. Both consoles' code computes k = 2 - cos(2 pi f / rate) and b0 = (k - sqrt(k k - 1))
// 32768, with a0 + b0 = 32767. Their arithmetic differs. The settings reproduce each console's arithmetic exactly.

#pragma once

#include <cstdint>

namespace ntdlib::ax
{

struct LpfSettings
{
    // The renderer's rate in Hz.
    float rate = 32000.0f;

    // cos and sqrt in double precision, each result rounded to float. The Wii's library computes them that way (MSL's
    // cos and sqrt). Otherwise in float. The Wii U's library computes them that way.
    bool double_precision = false;

    // k k - 1 in one fused multiply-add. sndcore2 computes it that way.
    bool fused = false;
};

struct LpfCoefs
{
    uint16_t a0 = 0;
    uint16_t b0 = 0;
};

LpfCoefs ComputeLpfCoefs(uint32_t frequency, const LpfSettings& settings);

} // namespace ntdlib::ax
