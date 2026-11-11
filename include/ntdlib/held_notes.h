// SPDX-License-Identifier: MIT

// The length of a held note: a note or wait that lasts until the game stops the sound. The length analyses of the
// sequence players and the performers use it.

#pragma once

#include <cstdint>

namespace ntdlib
{

// A held note or wait: one of this many ticks or more. That's 200 beats at the default timebase, and minutes at any
// usual tempo. Games use such notes and waits for sounds that they stop themselves. Music's notes are far shorter.
inline constexpr int32_t kHeldTicks = 9600;

} // namespace ntdlib
