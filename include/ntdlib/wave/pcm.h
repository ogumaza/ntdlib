// SPDX-License-Identifier: MIT

// Decoders for PCM wave data. Each decoder stops where its data ends. A caller that requires every sample checks the
// length of the result. EncodePcm8 encodes one sample of 16-bit PCM as signed 8-bit PCM.

#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace ntdlib::wave
{

// The byte of signed 8-bit PCM for a sample of 16-bit PCM: its high byte, rounded to the nearest. A sample that rounds
// past 127 gives 127. DecodePcm8 scales the byte back by 256.
inline constexpr uint8_t EncodePcm8(int16_t sample)
{
    return static_cast<uint8_t>(std::clamp((sample + 0x80) >> 8, -128, 127));
}

// Decodes up to `samples` samples of signed 8-bit PCM, scaled to 16 bits.
std::vector<int16_t> DecodePcm8(std::span<const uint8_t> data, uint32_t samples);

// Decodes up to `samples` samples of 16-bit PCM in either byte order. An odd last byte is left out.
std::vector<int16_t> DecodePcm16(std::span<const uint8_t> data, uint32_t samples, bool big_endian);

} // namespace ntdlib::wave
