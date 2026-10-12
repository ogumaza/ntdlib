// SPDX-License-Identifier: MIT

// CWAV and FWAV decoding to 16-bit PCM.

#pragma once

#include <cstdint>
#include <span>

#include "ntdlib/sar/formats.h"
#include "ntdlib/wave/decode.h"

namespace ntdlib::sar
{

// Decoded PCM for the first two wave channels, with loop_end samples per channel.
using Pcm = wave::Pcm;

// Decodes the first two channels of a CWAV or FWAV file the way nw::snd and nn::atk do. Supports DSP-ADPCM, PCM8 and
// PCM16, and IMA-ADPCM on the 3DS. DSP-ADPCM sums in 64 bits on the 3DS, and in 32 bits that wrap on the Wii U and the
// Switch. Throws FormatError on failure.
Pcm DecodeWave(std::span<const uint8_t> file, const Options& options);

} // namespace ntdlib::sar
