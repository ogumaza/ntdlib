// SPDX-License-Identifier: MIT

// The pan of a mixer that pans linearly, as the DS's sound unit does, in the terms of SoundFont's and SFZ's players.
//
// At a pan of p, from 0 to 127 with 64 at the centre, the left side plays at (128 - p) / 128 and the right at p / 128.
// A pan of 127 plays the right side at 128/128. SoundFont and SFZ players pan by the sin/cos law. PanPosition gives
// the pan, from -1 for full left to 1 for full right, at which that law gives the two sides the mixer's balance.
// PanGain gives the mixer's level of both sides together, against that law's level. Off the sides, the mixer plays
// quieter: at the centre, 0.707 times as loud. A note's level takes the gain of its track's pan in CC11, and the gain
// of its zone's pan against the centre's in the zone. A note at the centre then plays at 0.707.

#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ntdlib::sequence_export
{

inline double PanPosition(int pan)
{
    const int right = pan >= 127 ? 128 : std::clamp(pan, 0, 127);
    return 4.0 / std::numbers::pi * std::atan2(right, 128 - right) - 1.0;
}

inline double PanGain(int pan)
{
    const int right = pan >= 127 ? 128 : std::clamp(pan, 0, 127);
    return std::hypot(right, 128 - right) / 128.0;
}

} // namespace ntdlib::sequence_export
