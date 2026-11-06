// SPDX-License-Identifier: MIT

// Wave decoding (see wave.h).

#include "ntdlib/sequence_export/jaudio/wave.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/sequence_export/jaudio/sound_data.h"
#include "ntdlib/wave/adpcm.h"
#include "ntdlib/wave/pcm.h"

namespace ntdlib::sequence_export::jaudio
{
namespace
{

// Samples in each frame of ADPCM: 9 bytes in the 4-bit form, 5 in the 2-bit form.
constexpr uint32_t kFrameSamples = 16;
constexpr std::size_t kFrameBytes4 = 9;
constexpr std::size_t kFrameBytes2 = 5;

// Decodes ADPCM of either form.
std::vector<int16_t> DecodeAdpcm(WaveFormat format, std::span<const uint8_t> data, uint32_t samples, int16_t hist1,
                                 int16_t hist2)
{
    return format == WaveFormat::kAdpcm2 ? ntdlib::wave::DecodeAfc2(data, samples, hist1, hist2)
                                         : ntdlib::wave::DecodeAfc(data, samples, hist1, hist2);
}

} // namespace

Pcm DecodeWave(const WaveInfo& info, std::span<const uint8_t> aw)
{
    if (info.offset > aw.size() || info.size > aw.size() - info.offset)
    {
        throw FormatError("wave runs past the end of " + info.archive);
    }

    // A looping wave plays to its loop end, a one-shot wave to its last sample (JASDsp's setWaveInfo).
    Pcm pcm;
    std::vector<int16_t>& samples = pcm.channels.emplace_back();
    pcm.sample_rate = info.sample_rate;
    pcm.loop = info.loop && info.loop_start < info.loop_end;
    const uint32_t length = pcm.loop ? info.loop_end : info.sample_count;
    const std::span<const uint8_t> data = aw.subspan(info.offset, info.size);
    switch (info.format)
    {
    case WaveFormat::kAdpcm4:
    case WaveFormat::kAdpcm2:
        samples = DecodeAdpcm(info.format, data, length, 0, 0);
        break;
    case WaveFormat::kPcm8:
        samples = ntdlib::wave::DecodePcm8(data, length);
        break;
    case WaveFormat::kPcm16:
        samples = ntdlib::wave::DecodePcm16(data, length, true);
        break;
    }
    if (samples.size() < length)
    {
        throw FormatError("wave data is shorter than its sample count");
    }
    if (!pcm.loop)
    {
        return pcm;
    }

    pcm.loop_start = info.loop_start;
    pcm.loop_end = info.loop_end;
    if (info.format != WaveFormat::kAdpcm4 && info.format != WaveFormat::kAdpcm2)
    {
        return pcm;
    }

    // Each repeat of an ADPCM loop decodes the frame that holds the loop start from the history that the wave system
    // gives: the two samples before that frame. Where that history leads to other samples than the first pass, play the
    // first pass once and loop on a repeat.
    const uint32_t skip = info.loop_start % kFrameSamples;
    const std::size_t frame_bytes = info.format == WaveFormat::kAdpcm2 ? kFrameBytes2 : kFrameBytes4;
    const std::size_t loop_frame = std::size_t{info.loop_start} / kFrameSamples * frame_bytes;
    if (loop_frame > data.size())
    {
        return pcm;
    }

    const uint32_t loop_length = info.loop_end - info.loop_start;
    std::vector<int16_t> repeat =
        DecodeAdpcm(info.format, data.subspan(loop_frame), skip + loop_length, info.loop_last, info.loop_penult);
    if (repeat.size() < skip + loop_length)
    {
        return pcm;
    }

    repeat.erase(repeat.begin(), repeat.begin() + skip);
    if (std::equal(repeat.begin(), repeat.end(), samples.begin() + info.loop_start))
    {
        return pcm;
    }

    samples.insert(samples.end(), repeat.begin(), repeat.end());
    pcm.loop_start = info.loop_end;
    pcm.loop_end = info.loop_end + loop_length;

    return pcm;
}

} // namespace ntdlib::sequence_export::jaudio
