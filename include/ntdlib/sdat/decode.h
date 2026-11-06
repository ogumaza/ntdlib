// SPDX-License-Identifier: MIT

// The decoding of the DS's waves and streams to 16-bit PCM. The DS's sound channels decode them the same way. GBATEK
// describes the channels' formats: signed 8-bit PCM, 16-bit PCM and IMA-ADPCM. A channel plays an 8-bit sample as the
// high byte of a 16-bit sample. A block of IMA-ADPCM starts with a word of the decoder's state. ntr::AdpcmStep decodes
// each nibble.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/sdat/formats.h"

namespace ntdlib::sdat
{

// Decodes up to `samples` samples of a block of IMA-ADPCM that starts with its state's word, the low nibble of each
// byte first.
std::vector<int16_t> DecodeAdpcm(std::span<const uint8_t> data, uint32_t samples);

// Decodes a wave's samples, from its start to the end of its loop. Throws xsflib::FormatError for an unknown encoding
// or for data shorter than the loop's end.
std::vector<int16_t> DecodeWave(const Wave& wave);

// Decodes a stream's channel, from its start to its last sample.
std::vector<int16_t> DecodeStream(const Stream& stream, uint32_t channel);

} // namespace ntdlib::sdat
