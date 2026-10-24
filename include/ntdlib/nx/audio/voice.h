// SPDX-License-Identifier: MIT

// A voice's data source in the Switch's audio renderer: the wave buffers a voice plays, each channel's processing
// state, and the decoding of a frame's samples from the buffers with the sample-rate conversion. This follows
// nn::audio::DecodeFromWaveBuffers in WarioWare: Get It Together!'s sdk (0x6eda0), the build of the renderer for the
// CPU, to the bit.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/nx/audio/dsp.h"

namespace ntdlib::nx::audio
{

// The renderer has four wave buffer slots for each voice.
inline constexpr int kWaveBufferCount = 4;

// A wave buffer as the renderer keeps it: the samples, the range to play, how it loops, and for DSP-ADPCM the context
// its start has, if the game gave one.
struct WaveBuffer
{
    std::span<const uint8_t> buffer;
    const AdpcmContext* context = nullptr;
    int32_t start = 0;
    int32_t end = 0;
    int32_t loop_start = 0;
    int32_t loop_end = 0;
    int32_t loop_count = 0; // the loops after the first time through, or -1 for ever
    bool loop = false;
    bool end_of_stream = false;
};

// A voice channel's processing state (nn::audio::VoiceState): the samples played, the place in the current wave buffer,
// which buffers are still to play, the converter's history and position, the DSP-ADPCM context, the current buffer's
// loops, the biquad filters' states, and the last sample mixed into each buffer of the destination, for the depop.
struct VoiceState
{
    uint64_t played = 0;
    int32_t offset = 0;
    int32_t index = 0;
    std::array<bool, kWaveBufferCount> valid{};
    int32_t consumed = 0;
    std::array<int16_t, 8> history{};
    int32_t fraction = 0;
    AdpcmContext adpcm;
    int32_t loop_count = 0;
    std::array<BiquadFilterState, 2> biquads;
    std::array<int32_t, 24> last_samples{};
};

// What a data source command gives the decoding (nn::audio::DecodeFromWaveBuffersArgs).
struct DecodeFromWaveBuffersArgs
{
    SampleFormat format = SampleFormat::kPcmInt16;
    int32_t* out = nullptr;
    VoiceState* state = nullptr;
    const std::array<WaveBuffer, kWaveBufferCount>* buffers = nullptr;
    int32_t channel = 0;
    int32_t channel_count = 1;
    SrcQuality quality = SrcQuality::kMedium;
    int32_t pitch = 0x8000; // Q15
    int32_t source_rate = 48000;
    int32_t target_rate = 48000;
    int32_t count = 240;
    const AdpcmCoefficients* coefficients = nullptr;
    bool reset_played_at_loop = false;
};

// Decodes and converts `count` samples of one channel into `out` (nn::audio::DecodeFromWaveBuffers, 0x6eda0). The
// buffers play in turn from the current one. A buffer that ends is consumed unless it loops. A looping buffer plays its
// loop range until the loops are done. When no buffer is left, the rest of the input is zeros, and the frame stops
// there. The state is the channel's processing state. With `out` null, the channel moves on in the same way without
// the conversion.
void DecodeFromWaveBuffers(const DecodeFromWaveBuffersArgs& args);

// The samples before a batch's input that the enhanced decoding keeps for each channel. The interpolator's widest
// kernel takes 128 samples on each side of a position, and a position lies up to 5 samples behind the batch's input.
inline constexpr int kWideHistory = 132;

// What the enhanced decoding takes beside the arguments: the channel's past samples, and room for a batch's input. The
// past samples start at zero for a new channel.
struct EnhancedDecodeBuffers
{
    std::span<float, kWideHistory> history;
    std::vector<int16_t>* decoded = nullptr;
    std::vector<float>* input = nullptr;
};

// The enhanced mixing mode's decoding: `count` samples of one channel into `out`, on the scale of 16-bit samples. The
// channel takes the same steps through its buffers as DecodeFromWaveBuffers takes, with the same ratio and batches.
// It consumes the same buffers, goes round the same loops and leaves the same state. Each output comes from the
// band-limited interpolator (ntdlib/wave/sinc.h) for the step's ratio, between the same two input samples and at the
// same fraction as the console's filter of the voice's quality: 3 samples behind the batch's input at medium quality,
// 5 at high quality and 4 at low quality. The interpolator also takes the samples that follow the batch's input. They
// come from a copy of the state, through the buffers that the channel has in this frame, and zeros past them. The
// next batch decodes them again. Nothing happens for a frame that DecodeFromWaveBuffers leaves alone, and `out` keeps
// what it held. With `out` null, the channel moves on in the same way without the outputs and the samples after them.
void DecodeFromWaveBuffersEnhanced(const DecodeFromWaveBuffersArgs& args, const EnhancedDecodeBuffers& buffers,
                                   float* out);

} // namespace ntdlib::nx::audio
