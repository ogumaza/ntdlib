// SPDX-License-Identifier: MIT

// Wave decoding (see decode.h).

#include "ntdlib/sar/decode.h"

#include <cstdint>
#include <span>
#include <string>

#include "xsflib/binary.h"

#include "ntdlib/sar/formats.h"
#include "ntdlib/wave/decode.h"
#include "sar/file_parts.h"
#include "sar/sample_steps.h"

namespace ntdlib::sar
{
namespace
{

// The wave decoder's form of an encoding. Only the 3DS has IMA-ADPCM. Another console's IMA-ADPCM, or any other value,
// can't be decoded.
wave::Encoding EncodingOf(WaveEncoding encoding, Flavor flavor)
{
    if (encoding == WaveEncoding::kImaAdpcm && flavor == Flavor::kCtr)
    {
        return wave::Encoding::kImaAdpcm;
    }

    return detail::DecoderEncoding(encoding);
}

} // namespace

Pcm DecodeWave(std::span<const uint8_t> file, const Options& options)
{
    Wave w;
    try
    {
        w = Wave::Parse(file, options);
    }
    catch (const xsflib::FormatError& e)
    {
        const char* kind = options.flavor == Flavor::kCtr ? "bad CWAV: " : "bad FWAV: ";
        throw xsflib::FormatError(std::string(kind) + e.what());
    }

    // DSP-ADPCM sums in 64 bits on the 3DS, and in 32 bits that wrap on the Wii U and the Switch.
    wave::EncodedWave encoded;
    encoded.encoding = EncodingOf(w.encoding, options.flavor);
    encoded.big_endian = w.big_endian;
    encoded.sum = detail::SumOf(options.flavor);
    encoded.sample_rate = w.sample_rate;
    encoded.loop = w.loop;
    encoded.loop_start = w.loop_start;
    encoded.loop_end = w.loop_end;
    for (const WaveChannel& ch : w.channels)
    {
        wave::EncodedChannel& channel = detail::AddEncodedChannel(encoded, ch.data, ch.adpcm);
        if (ch.ima)
        {
            channel.ima_sample = ch.ima->sample;
            channel.ima_step_index = ch.ima->step_index;
        }
    }

    return wave::DecodeWave(encoded);
}

} // namespace ntdlib::sar
