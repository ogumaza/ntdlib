// SPDX-License-Identifier: MIT

#include "ntdlib/wave/adpcm.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace ntdlib::wave
{
namespace
{

// DSP-ADPCM: 14 samples in each 8-byte frame, after a byte that holds the predictor and the scale.
constexpr std::size_t kDspAdpcmFrameBytes = 8;
constexpr std::size_t kDspAdpcmFrameSamples = 14;

// AFC: 16 samples in each 9-byte frame, after a byte that holds the scale and the predictor.
constexpr std::size_t kAfcFrameBytes = 9;
constexpr uint32_t kAfcFrameSamples = 16;

// The predictor pairs that JAudio2 gives the GameCube's DSP program for AFC, in 2048ths.
constexpr std::array<std::array<int32_t, 2>, 16> kAfcCoefficients{{
    {0, 0},
    {2048, 0},
    {0, 2048},
    {1024, 1024},
    {4096, -2048},
    {3584, -1536},
    {3072, -1024},
    {4608, -2560},
    {4200, -2248},
    {4800, -2300},
    {5120, -3072},
    {2048, -2048},
    {1024, -1024},
    {-1024, 1024},
    {-1024, 0},
    {-2048, 0},
}};

// The standard IMA-ADPCM tables.
constexpr int kImaIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
constexpr int kImaStep[89] = {7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,   21,    23,
                              25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,   73,    80,
                              88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,  253,   279,
                              307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,  876,   963,
                              1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749, 3024,  3327,
                              3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487,
                              12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

// The value of a 4-bit two's complement nibble.
int32_t SignedNibble(int32_t nibble)
{
    return nibble >= 8 ? nibble - 16 : nibble;
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
    int predictor = sample;
    int step_index = std::min<int>(index, 88);
    for (uint32_t i = 0; i < samples; i++)
    {
        const std::size_t byte = i / 2;
        if (byte >= data.size())
        {
            break;
        }

        const int nibble = (i & 1) ? (data[byte] >> 4) : (data[byte] & 0x0f); // low nibble first
        const int step = kImaStep[step_index];
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

        predictor = std::clamp(predictor + diff, -32768, 32767);
        step_index = std::clamp(step_index + kImaIndex[nibble], 0, 88);
        out.push_back(static_cast<int16_t>(predictor));
    }

    return out;
}

std::vector<int16_t> DecodeAfc(std::span<const uint8_t> data, uint32_t samples, int16_t hist1, int16_t hist2)
{
    // Each frame starts with a byte whose high nibble gives the scale (a shift) and whose low nibble picks the
    // predictor pair. Sixteen signed nibbles follow, the high nibble of each byte first.
    std::vector<int16_t> out;
    out.reserve(samples);
    int32_t h1 = hist1, h2 = hist2;
    for (std::size_t frame = 0; out.size() < samples; frame++)
    {
        const std::size_t base = frame * kAfcFrameBytes;
        if (base + kAfcFrameBytes > data.size())
        {
            break;
        }

        const int32_t shift = data[base] >> 4;
        const auto& [c1, c2] = kAfcCoefficients[data[base] & 0x0f];
        for (uint32_t i = 0; i < kAfcFrameSamples && out.size() < samples; i++)
        {
            const uint8_t byte = data[base + 1 + i / 2];
            const int32_t nibble = SignedNibble((i & 1) ? (byte & 0x0f) : (byte >> 4));
            const int64_t v = (int64_t{nibble} << shift << 11) + int64_t{c1} * h1 + int64_t{c2} * h2;
            const auto s = static_cast<int16_t>(std::clamp<int64_t>(v >> 11, -32768, 32767));
            h2 = h1;
            h1 = s;
            out.push_back(s);
        }
    }

    return out;
}

} // namespace ntdlib::wave
