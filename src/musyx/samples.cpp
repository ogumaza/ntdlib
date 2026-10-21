// SPDX-License-Identifier: MIT

#include "ntdlib/musyx/samples.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "xsflib/binary.h"

#include "ntdlib/wave/adpcm.h"
#include "ntdlib/wave/pcm.h"

namespace ntdlib::musyx
{
namespace
{

// A directory entry's size, and the ID that ends the directory.
constexpr std::size_t kEntrySize = 32;
constexpr uint16_t kEndOfDirectory = 0xffff;

// The size of a DSP-ADPCM sample's settings, from the coefficients' count to the end of the coefficients.
constexpr std::size_t kAdpcmInfoSize = 40;

// DSP-ADPCM frames: a byte of predictor and scale, then 14 samples in 7 bytes.
constexpr uint32_t kFrameSamples = 14;
constexpr std::size_t kFrameBytes = 8;

// The GameCube's DSP decodes DSP-ADPCM in its accelerator.
constexpr wave::DspAdpcmSum kSum = wave::DspAdpcmSum::kAccelerator;

// The bytes that `info`'s samples take.
std::size_t DataSize(const SampleInfo& info)
{
    switch (info.format)
    {
    case SampleFormat::kAdpcm:
    case SampleFormat::kAdpcmPlus:
        return (std::size_t{info.length} + kFrameSamples - 1) / kFrameSamples * kFrameBytes;
    case SampleFormat::kPcm16:
        return std::size_t{info.length} * 2;
    case SampleFormat::kPcm8:
        break;
    }

    return info.length;
}

// Decodes DSP-ADPCM from sample `start` to sample `end` of `data` after a jump to `start`. The DSP then takes
// `pred_scale` as the predictor and scale of the frame that holds `start`, and `hist1` and `hist2` as the samples
// before it. Later frames take theirs from their first byte.
std::vector<int16_t> DecodeFrom(std::span<const uint8_t> data, uint32_t start, uint32_t end,
                                const std::array<int16_t, 16>& coefficients, uint8_t pred_scale, int16_t hist1,
                                int16_t hist2)
{
    std::vector<int16_t> out;
    int32_t h1 = hist1, h2 = hist2;
    uint8_t ps = pred_scale;
    for (uint32_t s = start; s < end; s++)
    {
        const std::size_t frame = std::size_t{s / kFrameSamples} * kFrameBytes;
        const uint32_t within = s % kFrameSamples;
        if (within == 0 && s != start)
        {
            ps = data[frame];
        }

        const uint8_t byte = data[frame + 1 + within / 2];
        const int32_t nibble = (within & 1) != 0 ? (byte & 0x0f) : (byte >> 4);
        const std::size_t predictor = (ps >> 4) & 7;
        const int16_t sample = wave::DspAdpcmSample(wave::SignedNibble(nibble), ps & 0x0f, coefficients[predictor * 2],
                                                    coefficients[predictor * 2 + 1], h1, h2, kSum);
        h2 = h1;
        h1 = sample;
        out.push_back(sample);
    }

    return out;
}

} // namespace

std::vector<SampleInfo> ReadSampleDirectory(std::span<const uint8_t> file)
{
    const xsflib::Reader r(file, true);
    std::vector<SampleInfo> samples;
    for (std::size_t at = 0; r.U16(at) != kEndOfDirectory; at += kEntrySize)
    {
        // An entry holds the ID, the offset in the samples file, the key in the top byte of the rate's word, the format
        // in the top byte of the length's word, the loop, and the offset of the DSP-ADPCM settings.
        r.Sub(at, kEntrySize);
        SampleInfo& info = samples.emplace_back();
        info.id = r.U16(at);
        info.offset = r.U32(at + 4);
        info.key = r.U8(at + 12);
        info.rate = r.U32(at + 12) & 0xffffff;
        const uint8_t format = r.U8(at + 16);
        if (format > static_cast<uint8_t>(SampleFormat::kPcm8))
        {
            throw xsflib::FormatError("sample directory has a sample of unknown format " + std::to_string(format));
        }

        info.format = static_cast<SampleFormat>(format);
        info.length = r.U32(at + 16) & 0xffffff;
        info.loop_start = r.U32(at + 20);
        info.loop_length = r.U32(at + 24);
        if (info.format != SampleFormat::kAdpcm && info.format != SampleFormat::kAdpcmPlus)
        {
            continue;
        }

        // The settings start with the coefficients' count, then the first frame's and the loop's predictor and scale,
        // the loop's history and the coefficients.
        const uint32_t settings = r.U32(at + 28);
        r.Sub(settings, kAdpcmInfoSize);
        AdpcmInfo& adpcm = info.adpcm.emplace();
        adpcm.pred_scale = r.U8(settings + 2);
        adpcm.loop_pred_scale = r.U8(settings + 3);
        adpcm.loop_hist2 = r.S16(settings + 4);
        adpcm.loop_hist1 = r.S16(settings + 6);
        for (std::size_t c = 0; c < adpcm.coefficients.size(); c++)
        {
            adpcm.coefficients[c] = r.S16(settings + 8 + 2 * c);
        }
    }

    return samples;
}

DecodedSample DecodeSample(const SampleInfo& info, std::span<const uint8_t> samples)
{
    const std::size_t size = DataSize(info);
    if (info.offset > samples.size() || size > samples.size() - info.offset)
    {
        throw xsflib::FormatError("sample " + std::to_string(info.id) + " runs past the end of the samples file");
    }

    // A sample that loops plays to its loop end, and one that doesn't to its last sample.
    DecodedSample out;
    const std::span<const uint8_t> data = samples.subspan(info.offset, size);
    out.loop = info.loop_length != 0 && info.loop_start < info.length;
    out.loop_start = out.loop ? info.loop_start : 0;
    out.loop_end = out.loop ? std::min(info.length, info.loop_start + info.loop_length) : 0;
    const uint32_t length = out.loop ? out.loop_end : info.length;
    switch (info.format)
    {
    case SampleFormat::kAdpcm:
    case SampleFormat::kAdpcmPlus:
        if (!info.adpcm)
        {
            throw xsflib::FormatError("sample " + std::to_string(info.id) + " has no DSP-ADPCM settings");
        }

        out.pcm = wave::DecodeDspAdpcm(data, length, info.adpcm->coefficients, 0, 0, kSum);
        break;

    case SampleFormat::kPcm16:
        out.pcm = wave::DecodePcm16(data, length, true);
        break;

    case SampleFormat::kPcm8:
        out.pcm = wave::DecodePcm8(data, length);
        break;
    }

    if (!out.loop || !info.adpcm)
    {
        return out;
    }

    // Each repeat of a DSP-ADPCM loop decodes from the loop's settings. Where that differs from the first pass, keep
    // the first pass and loop on a repeat.
    const AdpcmInfo& adpcm = *info.adpcm;
    const std::vector<int16_t> repeat = DecodeFrom(data, out.loop_start, out.loop_end, adpcm.coefficients,
                                                   adpcm.loop_pred_scale, adpcm.loop_hist1, adpcm.loop_hist2);
    if (std::equal(repeat.begin(), repeat.end(), out.pcm.begin() + out.loop_start))
    {
        return out;
    }

    out.pcm.insert(out.pcm.end(), repeat.begin(), repeat.end());
    const uint32_t loop_length = out.loop_end - out.loop_start;
    out.loop_start = out.loop_end;
    out.loop_end += loop_length;

    return out;
}

} // namespace ntdlib::musyx
