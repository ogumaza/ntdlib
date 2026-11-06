// SPDX-License-Identifier: MIT

// Decoded waves for seqlib's loop preparation, and the names of their files.

#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "seqlib/pcm.h"

#include "ntdlib/sequence_export/instruments.h"

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

// The name of the file of the wave with key `key` (BankSet::WaveKey), without its extension: "w<wave archive>_<wave>".
// A wave of a companion from another archive is "c<wave archive>_<wave>" (BankSet::kCompanionWaveArchives).
inline std::string WaveFileName(std::pair<uint32_t, uint32_t> key)
{
    const bool companion = key.first >= BankSet::kCompanionWaveArchives;
    const uint32_t archive = companion ? key.first - BankSet::kCompanionWaveArchives : key.first;

    return (companion ? "c" : "w") + std::to_string(archive) + "_" + std::to_string(key.second);
}

} // namespace ntdlib::sequence_export
