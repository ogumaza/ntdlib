// SPDX-License-Identifier: MIT

// The low-pass of the RSP's resampler, applied to a wave's samples before a SoundFont or an SFZ player plays them.
//
// Every engine that ntdlib models plays each voice through the RSP's resampler, at every pitch, 1.0 included. The
// resampler reads 4 samples through one of 64 filters, picked by the fraction of its place in the wave. Each filter is
// a low-pass: about 2.3 dB down at a quarter of the wave's rate and 7.4 dB down at half of it, on average. A note at
// another pitch than the wave's steps through the 64 filters about equally often. The conversion's waves therefore go
// through one symmetric filter whose response is the root mean square of the 64 filters' responses. It adds no delay.
//
// The filter doesn't follow two details of EAD's engines. The middle and later ones drop every other sample of a note
// played more than an octave up, before the resampler. The early one filters a note's samples once more in the frames
// that cross the wave's loop or end.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "seqlib/pcm.h"

namespace ntdlib::sequence_export::n64
{

// The filter's taps in 1.15 format, from the centre tap outwards. The centre tap plus twice the others is 1.0.
inline constexpr std::size_t kResamplerLowPassTaps = 5;
const std::array<int16_t, kResamplerLowPassTaps>& ResamplerLowPass();

// `pcm` through the filter, at the same length. Before the first sample, the filter reads zeros, which the resampler's
// state holds at a voice's start. A loop is filtered as it repeats: its first samples read its last ones, and its last
// samples read its first ones. The samples before the loop read the loop's first samples after them. A wave without a
// loop reads zeros after its end. A sample that the filter takes past 16 bits is limited, and `clipped` counts it.
seqlib::Pcm ApplyResamplerLowPass(const seqlib::Pcm& pcm, std::size_t* clipped = nullptr);

} // namespace ntdlib::sequence_export::n64
