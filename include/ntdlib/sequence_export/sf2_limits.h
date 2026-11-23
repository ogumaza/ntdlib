// SPDX-License-Identifier: MIT

// SoundFont's envelope limits in the units of the envelope fits.

#pragma once

namespace ntdlib::sequence_export
{

// SoundFont's slowest decay or release, seqlib::sf2::kLongestTimecents (8000 timecents), in milliseconds for a 100 dB
// fall: 1000 * 2^(8000 / 1200). The value is written out because std::exp2 isn't a constant expression, and a literal
// gives the same double on every platform.
inline constexpr double kSlowestSf2DecayMs = 101593.6673259648;

} // namespace ntdlib::sequence_export
