// SPDX-License-Identifier: MIT

// Decoded waves for seqlib's loop preparation.

#pragma once

#include "seqlib/loop.h"

namespace ntdlib::sequence_export
{

// Returns a decoded wave from sar or rsar in seqlib's form: its channels and its loop. ntdlib's core doesn't use
// seqlib. Its decoded waves therefore can't derive from seqlib's Pcm.
template <typename DecodedWave>
seqlib::Pcm ToSeqlib(const DecodedWave& pcm)
{
    seqlib::Pcm out;
    out.loop = pcm.loop;
    out.loop_start = pcm.loop_start;
    out.loop_end = pcm.loop_end;
    out.channels = pcm.channels;

    return out;
}

} // namespace ntdlib::sequence_export
