// SPDX-License-Identifier: MIT

// An encoder of IMA-ADPCM, the 4-bit ADPCM that the 3DS's waves and streams and the DS's waves can hold. adpcm.h holds
// the decoder. Each sample becomes a nibble, two to a byte, the low nibble first. The decoder adds a multiple of its
// step to the last sample, as the nibble gives, and the nibble moves the step up or down a table.
//
// For each sample, the encoder takes the nibble whose decoded sample comes closest to the input. It decodes each of
// the 16 nibbles as the decoder does. A look at the next sample too gained less than 1 dB on test signals, and took ten
// times as long. The first state holds the first sample and the step whose encoding of the first 64 samples comes
// closest to them.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/wave/adpcm.h"

namespace ntdlib::wave
{

// A channel in IMA-ADPCM, and the decoder's states at the start and at the loop's start.
struct ImaAdpcmChannel
{
    std::vector<uint8_t> data; // two samples to a byte, the low nibble first
    ImaAdpcmState start;       // the state before the first sample
    ImaAdpcmState loop;        // the state before the loop's first sample
};

// The bytes of IMA-ADPCM that hold `samples` samples. The high nibble of the last byte of an odd count is 0.
std::size_t ImaAdpcmBytes(std::size_t samples);

// Encodes `samples`. The loop's state is the decoder's state before sample `loop_start` when it plays the channel from
// the start. A decoder that goes back to the loop's start with that state plays the loop's samples again as in the
// first pass. A loop start past the last sample gives the state after the last sample. Throws std::invalid_argument for
// more samples than a wave's 32-bit sizes hold.
ImaAdpcmChannel EncodeImaAdpcm(std::span<const int16_t> samples, uint32_t loop_start);

} // namespace ntdlib::wave
