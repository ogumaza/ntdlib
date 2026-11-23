// SPDX-License-Identifier: MIT

// The pan of a mixer that pans linearly, as the DS's sound unit does, in the terms of SoundFont's and SFZ's players.
//
// At a pan of p, from 0 to 127 with 64 at the centre, the left side plays at (128 - p) / 128 and the right at p / 128.
// A pan of 127 plays the right side at 128/128. SoundFont and SFZ players pan by the sin/cos law. PanPosition gives
// the pan, from -1 for full left to 1 for full right, at which that law gives the two sides the mixer's balance.
// PanGain gives the mixer's level of both sides together, against that law's level. Off the sides, the mixer plays
// quieter: at the centre, 0.707 times as loud. A note's level takes the gain of its track's pan in CC11, and the gain
// of its zone's pan against the centre's in the zone. A note at the centre then plays at 0.707. PanController gives
// CC10's value for the pan's position.
//
// Files for a mixer that pans linearly by itself keep the pans as they are. CC10 contains the pan. The pan generator of
// a zone contains the position that a CC10 of the same value gives (LinearPanPosition). Neither contains a level of the
// pan.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace ntdlib::sequence_export
{

inline double PanPosition(int pan)
{
    const int right = pan >= 127 ? 128 : std::clamp(pan, 0, 127);
    return 4.0 / std::numbers::pi * std::atan2(right, 128 - right) - 1.0;
}

inline uint8_t PanController(int pan)
{
    return static_cast<uint8_t>(std::clamp<long>(std::lround(64.0 + 64.0 * PanPosition(pan)), 0, 127));
}

// The position, from -1 for full left to 1 for full right, that CC10 gives a value of `pan` by MIDI's RP-036: 0 and 1
// at full left, 64 at the centre, and an even step of 1/63 between 1 and 127.
inline double LinearPanPosition(int pan)
{
    return (std::clamp(pan, 1, 127) - 64) / 63.0;
}

inline double PanGain(int pan)
{
    const int right = pan >= 127 ? 128 : std::clamp(pan, 0, 127);
    return std::hypot(right, 128 - right) / 128.0;
}

// The attenuation in centibels that every zone of a SoundFont takes: 30.1 cB, the gain of a side against the centre. A
// zone panned fully to one side then plays at full level. A SoundFont zone can't amplify its sample, and a zone's pan
// raises its level by up to that gain. One reference for every SoundFont keeps the balance between the files of a game,
// whatever pans their zones have. The SoundFonts then play 3.0 dB under the game.
inline double PanCut()
{
    return 200.0 * std::log10(PanGain(0) / PanGain(64));
}

} // namespace ntdlib::sequence_export
