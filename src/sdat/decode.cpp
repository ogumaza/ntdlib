// SPDX-License-Identifier: MIT

#include "ntdlib/sdat/decode.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "ntdlib/ntr/sound.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/wave/adpcm.h"
#include "ntdlib/wave/pcm.h"

namespace ntdlib::sdat
{

namespace
{

std::vector<int16_t> DecodeData(WaveEncoding encoding, std::span<const uint8_t> data, uint32_t samples)
{
    switch (encoding)
    {
    case WaveEncoding::kPcm8:
        return wave::DecodePcm8(data, samples);
    case WaveEncoding::kPcm16:
        return wave::DecodePcm16(data, samples, false);
    case WaveEncoding::kImaAdpcm:
        return DecodeAdpcm(data, samples);
    }

    throw xsflib::FormatError("unknown wave encoding " + std::to_string(static_cast<int>(encoding)));
}

} // namespace

std::vector<int16_t> DecodeAdpcm(std::span<const uint8_t> data, uint32_t samples)
{
    std::vector<int16_t> out;
    if (data.size() < 4)
    {
        return out;
    }

    wave::ImaAdpcmState state = ntr::AdpcmHeader(xsflib::LoadLe32(data.data()));
    out.reserve(std::min<std::size_t>(samples, (data.size() - 4) * 2));
    for (std::size_t i = 4; i < data.size() && out.size() < samples; i++)
    {
        state = ntr::AdpcmStep(state, data[i] & 0x0F);
        out.push_back(state.sample);
        if (out.size() < samples)
        {
            state = ntr::AdpcmStep(state, data[i] >> 4);
            out.push_back(state.sample);
        }
    }

    return out;
}

std::vector<int16_t> DecodeWave(const Wave& wave)
{
    const uint32_t samples = wave.Samples();
    std::vector<int16_t> out = DecodeData(wave.encoding, wave.data, samples);
    if (out.size() < samples)
    {
        throw xsflib::FormatError("a wave's data ends before its loop's end");
    }

    return out;
}

std::vector<int16_t> DecodeStream(const Stream& stream, uint32_t channel)
{
    std::vector<int16_t> out;
    out.reserve(stream.samples);
    for (uint32_t block = 0; block < stream.blocks; block++)
    {
        const uint32_t samples = block + 1 == stream.blocks ? stream.last_block_samples : stream.block_samples;
        const std::vector<int16_t> part = DecodeData(stream.encoding, stream.Block(channel, block), samples);
        out.insert(out.end(), part.begin(), part.end());
    }

    return out;
}

} // namespace ntdlib::sdat
