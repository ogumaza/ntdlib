// SPDX-License-Identifier: MIT

// Wave decoding (see decode.h).

#include "ntdlib/rsar/decode.h"

#include "ntdlib/rsar/formats.h"
#include "ntdlib/wave/adpcm.h"
#include "ntdlib/wave/decode.h"

namespace ntdlib::rsar
{
namespace
{

// The wave decoder's form of an encoding. The reader rejects any other value.
wave::Encoding EncodingOf(WaveEncoding encoding)
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

} // namespace

Pcm DecodeWave(const Wave& w)
{
    // NW4R SND plays only the first two channels (Bank::NoteOn, main.dol 0x80035b68). The Wii's PCM16 samples are
    // big-endian. DSP-ADPCM sums in 64 bits.
    wave::EncodedWave encoded;
    encoded.encoding = EncodingOf(w.encoding);
    encoded.big_endian = true;
    encoded.sum = wave::DspAdpcmSum::kWide;
    encoded.sample_rate = w.sample_rate;
    encoded.loop = w.loop;
    encoded.loop_start = w.loop_start;
    encoded.loop_end = w.loop_end;
    for (const WaveChannel& ch : w.channels)
    {
        wave::EncodedChannel& channel = encoded.channels.emplace_back();
        channel.data = ch.data;
        if (ch.adpcm)
        {
            channel.coefs = &ch.adpcm->coefs;
            channel.yn1 = ch.adpcm->yn1;
            channel.yn2 = ch.adpcm->yn2;
        }
    }

    return wave::DecodeWave(encoded);
}

} // namespace ntdlib::rsar
