// SPDX-License-Identifier: MIT

// An encoder of DSP-ADPCM, the 4-bit ADPCM of the GameCube, the Wii, the Wii U, the 3DS and the Switch. adpcm.h holds
// the decoder. A channel's samples become frames of 8 bytes, each of 14 samples. A frame's first byte is its header:
// the index of one of the channel's 8 pairs of coefficients in the high nibble, and a scale as a power of 2 in the low
// nibble. 14 signed nibbles follow, the high nibble of each byte first. The decoder predicts each sample from the two
// before it with the frame's pair, and adds the nibble times the scale.
//
// The encoder works out the channel's 8 pairs first. A pair's error on a frame is the squared error of its prediction
// of the frame's samples, relative to the frame's energy. Quiet frames then count as much as loud ones. Rounds of the
// k-means method group the frames around 8 pairs. In each round, each frame goes to the pair with the least error on
// it. Each pair then becomes the pair with the least error on its group. The pairs stay in the range where the
// decoder's prediction is stable. Then each frame takes the pair and the scale whose decoded samples come closest to
// the input. The encoder decodes each candidate as the decoder does, from the samples that the decoder gives before
// the frame.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/wave/adpcm.h"

namespace ntdlib::wave
{

// A channel in DSP-ADPCM, and the decoder's state at the loop's start.
struct DspAdpcmChannel
{
    std::vector<uint8_t> data;       // the frames, the last one whole
    std::array<int16_t, 16> coefs{}; // 8 pairs, in units of 1/2048
    uint8_t pred_scale = 0;          // the first frame's header
    uint8_t loop_pred_scale = 0;     // the header of the frame that holds the loop's start
    int16_t loop_yn1 = 0;            // the decoded sample before the loop's start
    int16_t loop_yn2 = 0;            // the decoded sample before that one
};

// The bytes of DSP-ADPCM that hold `samples` samples: 8 for each frame of 14, the last frame whole.
std::size_t DspAdpcmBytes(std::size_t samples);

// Encodes `samples` for a decoder that starts from two samples of 0 and adds up as `sum` gives. The loop's state is
// the decoder's state at sample `loop_start` when it plays the channel from the start. A decoder that goes back to the
// loop's start with that state plays the loop's samples again as in the first pass. A loop start past the last sample
// gives the state after the last sample. Throws std::invalid_argument for more samples than a wave's 32-bit sizes
// hold.
DspAdpcmChannel EncodeDspAdpcm(std::span<const int16_t> samples, uint32_t loop_start, DspAdpcmSum sum);

// Moves the loop's start forward to the start of a frame of 14 samples. Each of `channels` holds the wave's samples up
// to the end of its loop. The samples from `loop_start` up to the new start repeat after the loop's end, and the loop
// then plays the same samples. A decoder that goes back to the loop's start then starts at a frame's header. Returns
// the new start. A start on a frame stays where it is. Throws std::invalid_argument for a loop start past the end of a
// channel.
uint32_t AlignDspAdpcmLoop(std::vector<std::vector<int16_t>>& channels, uint32_t loop_start);

} // namespace ntdlib::wave
