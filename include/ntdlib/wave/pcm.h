// SPDX-License-Identifier: MIT

// Decoders for PCM wave data. Each decoder stops where its data ends. A caller that requires every sample checks the
// length of the result.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace ntdlib::wave
{

// Decodes up to `samples` samples of signed 8-bit PCM, scaled to 16 bits.
std::vector<int16_t> DecodePcm8(std::span<const uint8_t> data, uint32_t samples);

// Decodes up to `samples` samples of 16-bit PCM in either byte order. An odd last byte is left out.
std::vector<int16_t> DecodePcm16(std::span<const uint8_t> data, uint32_t samples, bool big_endian);

} // namespace ntdlib::wave
