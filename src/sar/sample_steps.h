// SPDX-License-Identifier: MIT

// The steps on samples that the readers and writers of sar and rsar share: a channel of a wave or a stream in the form
// of wave/decode.h, the sizes of a stream's blocks, the decoding of the channels of a stream, and the steps of
// MakeStream from samples to the channels of a stream. The Wii writes its samples big-endian. The later consoles write
// them in the byte order of the file. Each format passes its byte order, its DSP-ADPCM sums and its types.

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "ntdlib/wave/adpcm.h"
#include "ntdlib/wave/decode.h"
#include "ntdlib/wave/encode.h"

namespace ntdlib::sar::detail
{

// Adds a channel of `data` to `encoded`, and returns it. A DSP-ADPCM channel takes the coefficients and the history of
// `adpcm`.
template <typename AdpcmInfo>
wave::EncodedChannel& AddEncodedChannel(wave::EncodedWave& encoded, std::span<const uint8_t> data,
                                        const std::optional<AdpcmInfo>& adpcm)
{
    wave::EncodedChannel& channel = encoded.channels.emplace_back();
    channel.data = data;
    if (adpcm)
    {
        channel.coefs = &adpcm->coefs;
        channel.yn1 = adpcm->yn1;
        channel.yn2 = adpcm->yn2;
    }

    return channel;
}

// The wave decoder's form of `encoding`, a format's 8-bit PCM, 16-bit PCM or DSP-ADPCM. Any other encoding gives
// wave::Encoding::kUnknown.
template <typename WaveEncoding>
wave::Encoding DecoderEncoding(WaveEncoding encoding)
{
    switch (encoding)
    {
    case WaveEncoding::kPcm8:
        return wave::Encoding::kPcm8;
    case WaveEncoding::kPcm16:
        return wave::Encoding::kPcm16;
    case WaveEncoding::kDspAdpcm:
        return wave::Encoding::kDspAdpcm;
    default:
        return wave::Encoding::kUnknown;
    }
}

// The bytes of each channel's piece of a full block of a stream.
inline constexpr uint32_t kStreamBlockBytes = 0x2000;

// The bytes that `samples` samples of a stream in `encoding` take, up to the last sample's byte or nibble. Throws
// std::invalid_argument for an encoding other than PCM and DSP-ADPCM.
template <typename WaveEncoding>
uint32_t StreamBytes(WaveEncoding encoding, uint32_t samples)
{
    switch (encoding)
    {
    case WaveEncoding::kPcm8:
        return samples;
    case WaveEncoding::kPcm16:
        return samples * 2;
    case WaveEncoding::kDspAdpcm:
        return static_cast<uint32_t>(wave::DspAdpcmByteCount(uint64_t{samples}));
    default:
        throw std::invalid_argument("a stream of PCM or DSP-ADPCM");
    }
}

// The samples of each channel in a full block of a stream in `encoding`: the samples that fill kStreamBlockBytes, 14 in
// each 8 bytes of DSP-ADPCM. Throws std::invalid_argument for an encoding other than PCM and DSP-ADPCM.
template <typename WaveEncoding>
uint32_t StreamBlockSamplesOf(WaveEncoding encoding)
{
    switch (encoding)
    {
    case WaveEncoding::kPcm8:
        return kStreamBlockBytes;
    case WaveEncoding::kPcm16:
        return kStreamBlockBytes / 2;
    case WaveEncoding::kDspAdpcm:
        return kStreamBlockBytes / 8 * 14;
    default:
        throw std::invalid_argument("a stream of PCM or DSP-ADPCM");
    }
}

// The blocks that `sample_count` samples fill, `block_samples` to a block, the last one in part. 0 for blocks of no
// samples. The sum is taken in 64 bits.
inline uint32_t StreamBlockCount(uint32_t sample_count, uint32_t block_samples)
{
    return block_samples == 0 ? 0 : static_cast<uint32_t>((uint64_t{sample_count} + block_samples - 1) / block_samples);
}

// The samples of the last of those blocks, or 0 for no blocks.
inline uint32_t StreamLastBlockSamples(uint32_t sample_count, uint32_t block_samples)
{
    const uint32_t blocks = StreamBlockCount(sample_count, block_samples);
    return blocks == 0 ? 0 : sample_count - (blocks - 1) * block_samples;
}

// `size` rounded up to a multiple of 32: the bytes of a channel's piece of a stream's block with its padding. A size
// within 31 bytes of 2^32 gives 0.
inline uint32_t PaddedPieceSize(uint32_t size)
{
    return static_cast<uint32_t>((uint64_t{size} + 31) / 32 * 32);
}

// How the channels of a stream hold their samples: the encoding, the byte order of 16-bit PCM and the sums of
// DSP-ADPCM.
struct StreamCoding
{
    wave::Encoding encoding = wave::Encoding::kDspAdpcm;
    bool big_endian = false;
    wave::DspAdpcmSum sum = wave::DspAdpcmSum::kWide;
};

// Decodes every channel of `stream` from its start, sample_count samples each, its blocks as one run. Each channel
// decodes by itself: the wave decoder decodes only the first two channels. With `missing_info`, a channel without a
// DSP-ADPCM info throws xsflib::FormatError(missing_info) before it decodes. Without it, the wave decoder throws for a
// DSP-ADPCM channel without coefficients.
template <typename Stream>
wave::Pcm DecodeStreamChannels(const Stream& stream, const StreamCoding& coding, const char* missing_info)
{
    wave::Pcm pcm;
    pcm.sample_rate = stream.sample_rate;
    pcm.loop = stream.loop && stream.loop_start < stream.sample_count;
    pcm.loop_start = pcm.loop ? stream.loop_start : 0;
    pcm.loop_end = stream.sample_count;

    for (const typename Stream::Channel& channel : stream.channels)
    {
        if (missing_info && !channel.adpcm)
        {
            throw xsflib::FormatError(missing_info);
        }

        wave::EncodedWave encoded;
        encoded.encoding = coding.encoding;
        encoded.big_endian = coding.big_endian;
        encoded.sum = coding.sum;
        encoded.sample_rate = stream.sample_rate;
        encoded.loop_end = stream.sample_count;
        AddEncodedChannel(encoded, channel.samples, channel.adpcm);
        pcm.channels.push_back(std::move(wave::DecodeWave(encoded).channels.front()));
    }

    return pcm;
}

// The length of `channels`, the samples that MakeStream makes a stream of. Throws std::invalid_argument for no channels
// or more than 255, channels of different lengths, no samples, and a loop start at the end of the samples or past it.
inline std::size_t CheckedLength(std::span<const std::vector<int16_t>> channels, std::optional<uint32_t> loop_start)
{
    if (channels.empty() || channels.size() > std::numeric_limits<uint8_t>::max())
    {
        throw std::invalid_argument("a stream with 1 to 255 channels");
    }
    const std::size_t length = channels.front().size();
    for (const std::vector<int16_t>& channel : channels)
    {
        if (channel.size() != length)
        {
            throw std::invalid_argument("a stream whose channels have different lengths");
        }
    }
    if (length == 0 || (loop_start && *loop_start >= length))
    {
        throw std::invalid_argument("a stream with samples, and a loop that starts before their end");
    }

    return length;
}

// Moves the loop of `samples` from `loop_start` forward to `moved`, the start of a block. The tools move a loop that
// way. The samples from the old start up to the new one follow the end of each channel. Returns the new start. Throws
// std::invalid_argument for a start past 32 bits.
inline uint32_t MoveLoop(std::vector<std::vector<int16_t>>& samples, uint32_t loop_start, uint64_t moved)
{
    for (std::vector<int16_t>& channel : samples)
    {
        for (uint64_t i = loop_start; i < moved; ++i)
        {
            const int16_t sample = channel[i];
            channel.push_back(sample);
        }
    }

    if (moved > std::numeric_limits<uint32_t>::max())
    {
        throw std::invalid_argument("a stream with more samples than its 32-bit sizes hold");
    }

    return static_cast<uint32_t>(moved);
}

// The 32-bit count of the samples in each channel of `samples`. Throws std::invalid_argument for more than the 32-bit
// sizes of the file hold.
inline uint32_t SampleCount(const std::vector<std::vector<int16_t>>& samples)
{
    if (samples.front().size() > std::numeric_limits<uint32_t>::max() / 2)
    {
        throw std::invalid_argument("a stream with more samples than its 32-bit sizes hold");
    }

    return static_cast<uint32_t>(samples.front().size());
}

// How MakeStream lays out a channel of a stream: in DSP-ADPCM with `sum` and the state at `loop_start`, or else in
// 16-bit PCM in a byte order. Zeros pad its samples to `size` bytes. Its history holds the two samples before each of
// `points` points, `interval` samples apart from the first sample.
struct ChannelLayout
{
    bool adpcm = false;
    bool big_endian = false;
    wave::DspAdpcmSum sum = wave::DspAdpcmSum::kWide;
    uint32_t loop_start = 0;
    std::size_t size = 0;
    std::size_t points = 0;
    std::size_t interval = 0;
};

// The channel of a stream that holds `input`, laid out as `layout` gives. A sample before the first counts as 0 in the
// history.
template <typename Channel>
Channel EncodeChannel(std::span<const int16_t> input, const ChannelLayout& layout)
{
    Channel channel;
    if (layout.adpcm)
    {
        wave::DspAdpcmChannel encoded = wave::EncodeDspAdpcm(input, layout.loop_start, layout.sum);
        channel.samples = std::move(encoded.data);
        auto& adpcm = channel.adpcm.emplace();
        adpcm.coefs = encoded.coefs;
        adpcm.pred_scale = encoded.pred_scale;
        adpcm.loop_pred_scale = encoded.loop_pred_scale;
        adpcm.loop_yn1 = encoded.loop_yn1;
        adpcm.loop_yn2 = encoded.loop_yn2;
    }
    else
    {
        channel.samples.resize(input.size() * 2);
        for (std::size_t i = 0; i < input.size(); ++i)
        {
            layout.big_endian ? xsflib::StoreBe16(&channel.samples[i * 2], static_cast<uint16_t>(input[i]))
                              : xsflib::StoreLe16(&channel.samples[i * 2], static_cast<uint16_t>(input[i]));
        }
    }

    channel.samples.resize(layout.size);

    for (std::size_t p = 0; p < layout.points; ++p)
    {
        const std::size_t point = p * layout.interval;
        channel.history.push_back(
            {point >= 1 ? input[point - 1] : int16_t{0}, point >= 2 ? input[point - 2] : int16_t{0}});
    }

    return channel;
}

} // namespace ntdlib::sar::detail
