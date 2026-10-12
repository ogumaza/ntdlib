// SPDX-License-Identifier: MIT

// The decoding of a sound archive's wave to 16-bit PCM. sar/decode.h and rsar/decode.h describe each wave file in the
// form below: its encoding, loop and channels. The decoder decodes the first two channels. The sound libraries play
// only those two.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/wave/adpcm.h"

namespace ntdlib::wave
{

// Decoded PCM for the first two wave channels, with loop_end samples per channel.
struct Pcm
{
    uint32_t Length() const
    {
        return loop_end;
    }

    uint32_t sample_rate = 32000;
    bool loop = false;       // only when loop_start < loop_end
    uint32_t loop_start = 0; // first sample of the loop
    uint32_t loop_end = 0;   // one past the last sample played (the length)
    std::vector<std::vector<int16_t>> channels;
};

// A wave's encoding. kUnknown stands for a value that the console's library doesn't decode.
enum class Encoding : uint8_t
{
    kPcm8,
    kPcm16,
    kDspAdpcm,
    kImaAdpcm,
    kUnknown,
};

// A channel's sample data, and the decoder's state at its start. DSP-ADPCM takes the coefficients and the two samples
// before the first. A channel without coefficients can't be decoded. IMA-ADPCM takes the first sample and the step
// index.
struct EncodedChannel
{
    std::span<const uint8_t> data;
    const std::array<int16_t, 16>* coefs = nullptr;
    int16_t yn1 = 0;
    int16_t yn2 = 0;
    int16_t ima_sample = 0;
    uint8_t ima_step_index = 0;
};

// A wave file's encoding, PCM16's byte order, DSP-ADPCM's sums, the sample rate, the loop and the channels.
struct EncodedWave
{
    Encoding encoding = Encoding::kPcm16;
    bool big_endian = true;
    DspAdpcmSum sum = DspAdpcmSum::kWide;
    uint32_t sample_rate = 32000;
    bool loop = false;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;
    std::vector<EncodedChannel> channels;
};

// Decodes the first two channels of `wave`, loop_end samples each. A loop that doesn't end after its start plays as no
// loop. Throws xsflib::FormatError for an encoding that can't be decoded, a DSP-ADPCM channel without coefficients, a
// channel shorter than loop_end samples, or a wave without channels.
Pcm DecodeWave(const EncodedWave& wave);

} // namespace ntdlib::wave
