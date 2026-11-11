// SPDX-License-Identifier: MIT

// The constants that the tracks (track.cpp) and their commands (track_commands.cpp) share.

#pragma once

#include "ntdlib/jaudio2/oscillator.h"

namespace ntdlib::jaudio2::detail
{

// The envelopes that a new track's notes take: none for the volume (JASTrack's sEnvOsc) and none for the pitch
// (sPitchEnvOsc). A note then keeps its instrument's.
inline constexpr OscData kEnvOsc{0, 1.0f, {}, {}, 1.0f, 0.0f};
inline constexpr OscData kPitchEnvOsc{1, 1.0f, {}, {}, 1.0f, 0.0f};

} // namespace ntdlib::jaudio2::detail
