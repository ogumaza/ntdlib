// SPDX-License-Identifier: MIT

// A sound archive's wave to 16-bit PCM (see decode.h).

#include "ntdlib/wave/decode.h"

#include <cstddef>
#include <utility>

#include "ntdlib/wave/adpcm.h"
#include "ntdlib/wave/pcm.h"
#include "xsflib/binary.h"

namespace ntdlib::wave
{

Pcm DecodeWave(const EncodedWave& wave)
{
    Pcm pcm;
    pcm.sample_rate = wave.sample_rate;
    pcm.loop = wave.loop;
    pcm.loop_start = wave.loop_start;
    pcm.loop_end = wave.loop_end;

    // The sound libraries play only the first two channels. A damaged file's header can list thousands of full-length
    // channels.
    const uint32_t n = wave.loop_end;
    for (std::size_t c = 0; c < wave.channels.size() && c < 2; c++)
    {
        const EncodedChannel& ch = wave.channels[c];
        std::vector<int16_t> s;
        switch (wave.encoding)
        {
        case Encoding::kPcm8:
            s = DecodePcm8(ch.data, n);
            break;

        case Encoding::kPcm16:
            s = DecodePcm16(ch.data, n, wave.big_endian);
            break;

        case Encoding::kDspAdpcm:
            if (!ch.coefs)
            {
                throw xsflib::FormatError("DSP-ADPCM wave without coefficients");
            }

            s = DecodeDspAdpcm(ch.data, n, *ch.coefs, ch.yn1, ch.yn2, wave.sum);
            break;

        case Encoding::kImaAdpcm:
            s = DecodeImaAdpcm(ch.data, n, ch.ima_sample, ch.ima_step_index);
            break;

        case Encoding::kUnknown:
            throw xsflib::FormatError("unknown wave encoding");
        }

        // The decoders stop where the data ends. A header that declares more samples is rejected. Padding to its length
        // could allocate gigabytes.
        if (s.size() < n)
        {
            throw xsflib::FormatError("wave data shorter than the declared sample count");
        }

        pcm.channels.push_back(std::move(s));
    }

    if (pcm.channels.empty())
    {
        throw xsflib::FormatError("wave has no channels");
    }

    if (pcm.loop && pcm.loop_start >= pcm.loop_end)
    {
        pcm.loop = false;
    }

    return pcm;
}

} // namespace ntdlib::wave
