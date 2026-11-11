// SPDX-License-Identifier: MIT

#include "ntdlib/wave/adpcm.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/wave/afc.h"

namespace ntdlib::wave
{
namespace
{

// DSP-ADPCM: 14 samples in each 8-byte frame, after a byte that holds the predictor and the scale.
constexpr std::size_t kDspAdpcmFrameBytes = 8;
constexpr std::size_t kDspAdpcmFrameSamples = 14;

// AFC: 16 samples in each 9-byte frame, after a byte that holds the scale and the predictor. The 2-bit form packs the
// 16 samples in 4 bytes, so its frames are 5 bytes.
constexpr std::size_t kAfcFrameBytes = 9;
constexpr std::size_t kAfc2FrameBytes = 5;
constexpr uint32_t kAfcFrameSamples = 16;

// A 2-bit sample stands for the top two bits of a 4-bit one. Its left shift before the frame's scale is therefore 13,
// where a 4-bit sample's is 11.
constexpr int kAfcSampleShift = 11;
constexpr int kAfc2SampleShift = 13;

// Decodes the AFC frames of `frame_bytes` bytes in `data`, up to `samples` samples. Each frame starts with a byte whose
// high nibble gives the scale (a shift) and whose low nibble picks the predictor pair. `sample(bytes, i)` gives the
// frame's signed sample `i` from the bytes after that one. `sample_shift` is the sample's left shift before the scale.
template <typename Sample>
std::vector<int16_t> DecodeAfcFrames(std::span<const uint8_t> data, uint32_t samples, int16_t hist1, int16_t hist2,
                                     std::size_t frame_bytes, int sample_shift, Sample sample)
{
    std::vector<int16_t> out;
    out.reserve(samples);
    int32_t h1 = hist1, h2 = hist2;
    for (std::size_t frame = 0; out.size() < samples; frame++)
    {
        const std::size_t base = frame * frame_bytes;
        if (base + frame_bytes > data.size())
        {
            break;
        }

        const int32_t shift = data[base] >> 4;
        const auto& [c1, c2] = kAfcCoefficients[data[base] & 0x0f];
        const std::span<const uint8_t> bytes = data.subspan(base + 1, frame_bytes - 1);
        for (uint32_t i = 0; i < kAfcFrameSamples && out.size() < samples; i++)
        {
            const int32_t value = sample(bytes, i);
            const int64_t v = (int64_t{value} << shift << sample_shift) + int64_t{c1} * h1 + int64_t{c2} * h2;
            const auto s = static_cast<int16_t>(std::clamp<int64_t>(v >> 11, -32768, 32767));
            h2 = h1;
            h1 = s;
            out.push_back(s);
        }
    }

    return out;
}

} // namespace

std::vector<int16_t> DecodeDspAdpcm(std::span<const uint8_t> data, uint32_t samples, std::span<const int16_t, 16> coefs,
                                    int16_t hist1, int16_t hist2, DspAdpcmSum sum)
{
    std::vector<int16_t> out;
    out.reserve(std::min<std::size_t>(
        samples, (data.size() + kDspAdpcmFrameBytes - 1) / kDspAdpcmFrameBytes * kDspAdpcmFrameSamples));
    int32_t h1 = hist1, h2 = hist2;
    for (std::size_t frame = 0; out.size() < samples; frame++)
    {
        const std::size_t base = frame * kDspAdpcmFrameBytes;
        if (base >= data.size())
        {
            break;
        }

        const uint8_t ps = data[base];
        const std::size_t predictor = (ps >> 4) & 7;
        const int32_t shift = ps & 0x0f;
        const int32_t c1 = coefs[predictor * 2], c2 = coefs[predictor * 2 + 1];
        for (std::size_t i = 0; i < kDspAdpcmFrameSamples && out.size() < samples; i++)
        {
            const std::size_t byte = base + 1 + i / 2;
            if (byte >= data.size())
            {
                break;
            }

            const int32_t nibble = (i & 1) ? (data[byte] & 0x0f) : (data[byte] >> 4); // high nibble first
            const int16_t s = DspAdpcmSample(SignedNibble(nibble), shift, c1, c2, h1, h2, sum);
            h2 = h1;
            h1 = s;
            out.push_back(s);
        }
    }

    return out;
}

std::vector<int16_t> DecodeImaAdpcm(std::span<const uint8_t> data, uint32_t samples, int16_t sample, uint8_t index)
{
    std::vector<int16_t> out;
    out.reserve(std::min<std::size_t>(samples, data.size() * 2));
    ImaAdpcmState state{sample, index};
    for (uint32_t i = 0; i < samples; i++)
    {
        const std::size_t byte = i / 2;
        if (byte >= data.size())
        {
            break;
        }

        const uint8_t nibble = (i & 1) ? (data[byte] >> 4) : (data[byte] & 0x0f); // low nibble first
        state = ImaAdpcmNext(state, nibble);
        out.push_back(state.sample);
    }

    return out;
}

ImaAdpcmState ImaAdpcmNext(ImaAdpcmState state, uint8_t nibble)
{
    const int index = std::min<int>(state.index, 88);
    const int step = kImaStep[index];
    int diff = step >> 3;
    if (nibble & 1)
    {
        diff += step >> 2;
    }
    if (nibble & 2)
    {
        diff += step >> 1;
    }
    if (nibble & 4)
    {
        diff += step;
    }
    if (nibble & 8)
    {
        diff = -diff;
    }

    return {static_cast<int16_t>(std::clamp(state.sample + diff, -32768, 32767)),
            static_cast<uint8_t>(std::clamp(index + kImaIndex[nibble & 0x0f], 0, 88))};
}

std::vector<int16_t> DecodeAfc(std::span<const uint8_t> data, uint32_t samples, int16_t hist1, int16_t hist2)
{
    // Sixteen signed nibbles follow the frame's first byte. The high nibble of each byte comes first.
    const auto nibble = [](std::span<const uint8_t> bytes, uint32_t i)
    {
        const uint8_t byte = bytes[i / 2];
        return SignedNibble((i & 1) ? (byte & 0x0f) : (byte >> 4));
    };

    return DecodeAfcFrames(data, samples, hist1, hist2, kAfcFrameBytes, kAfcSampleShift, nibble);
}

std::vector<int16_t> DecodeAfc2(std::span<const uint8_t> data, uint32_t samples, int16_t hist1, int16_t hist2)
{
    // Sixteen signed 2-bit samples, four to a byte, follow the frame's first byte. Each byte's high bits come first.
    const auto two_bits = [](std::span<const uint8_t> bytes, uint32_t i)
    {
        const int32_t bits = bytes[i / 4] >> (6 - 2 * (i & 3)) & 3;
        return bits >= 2 ? bits - 4 : bits;
    };

    return DecodeAfcFrames(data, samples, hist1, hist2, kAfc2FrameBytes, kAfc2SampleShift, two_bits);
}

} // namespace ntdlib::wave
