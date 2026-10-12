// SPDX-License-Identifier: MIT

// Wave decoding to 16-bit PCM.

#pragma once

#include "ntdlib/rsar/formats.h"
#include "ntdlib/wave/decode.h"

namespace ntdlib::rsar
{

// Decoded PCM for the first two wave channels, with loop_end samples per channel.
using Pcm = wave::Pcm;

// Decodes the first two channels of `wave`. NW4R SND plays only those. Supports DSP-ADPCM, PCM8 and PCM16. Throws
// FormatError on failure.
Pcm DecodeWave(const Wave& wave);

} // namespace ntdlib::rsar
