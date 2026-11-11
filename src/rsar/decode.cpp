// SPDX-License-Identifier: MIT

// Wave decoding (see decode.h).

#include "ntdlib/rsar/decode.h"

#include "ntdlib/rsar/formats.h"
#include "ntdlib/wave/adpcm.h"
#include "ntdlib/wave/decode.h"
#include "sar/sample_steps.h"

namespace ntdlib::rsar
{

Pcm DecodeWave(const Wave& w)
{
    // NW4R SND plays only the first two channels (Bank::NoteOn, main.dol 0x80035b68). The Wii's PCM16 samples are
    // big-endian. DSP-ADPCM sums in 64 bits.
    wave::EncodedWave encoded;
    encoded.encoding = sar::detail::DecoderEncoding(w.encoding);
    encoded.big_endian = true;
    encoded.sum = wave::DspAdpcmSum::kWide;
    encoded.sample_rate = w.sample_rate;
    encoded.loop = w.loop;
    encoded.loop_start = w.loop_start;
    encoded.loop_end = w.loop_end;
    for (const WaveChannel& ch : w.channels)
    {
        sar::detail::AddEncodedChannel(encoded, ch.data, ch.adpcm);
    }

    return wave::DecodeWave(encoded);
}

} // namespace ntdlib::rsar
