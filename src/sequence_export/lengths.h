// SPDX-License-Identifier: MIT

// The limits on the length of a performance, for every performer (performer.cpp, jaudio/performance.cpp and
// ntr/performance.cpp).

#pragma once

#include <cstdint>

namespace ntdlib::sequence_export
{

// Short loops, such as sustained sound effects, repeat until the performance reaches this duration. The loop markers go
// around the pass that ChooseSteadyPass selects.
inline constexpr double kMinLoopSeconds = 10.0;

// Longest performance, in seconds of music.
inline constexpr double kMaxSeconds = 900.0;

// Maximum duration after a held sound stops changing. This covers tracks waiting on endless looping notes. In the
// NintendoWare performer, it also covers held notes after a sequence's timing has stopped (Performer::PlayOutStopped).
inline constexpr double kHoldSeconds = 10.0;

// Whether a finished performance ends at its last sound. A finished performance ends at its last change or where its
// last sound dies away, whichever comes later. The last change can be a tempo change, a controller's change or a
// track's end after a wait. When it comes more than kHoldSeconds after the last sound has died away, the performance
// ends where the sound dies away. Nothing after that point can be heard, and a player would show the silence as part
// of the piece. `sound_end` and `last_change` are in seconds. A held sound ends by kHoldSeconds instead, and a loop
// keeps its markers.
inline constexpr bool EndsAtLastSound(double sound_end, double last_change)
{
    return last_change - sound_end > kHoldSeconds;
}

// Shortest note or wait that a track holds until the game stops the sound. That's 200 beats at the default timebase,
// minutes at a typical tempo. Games use such notes and waits for sounds they stop themselves.
inline constexpr int32_t kHeldTicks = 9600;

// Longest release tail kept after the end.
inline constexpr double kMaxTailSeconds = 10.0;

} // namespace ntdlib::sequence_export
