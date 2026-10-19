// SPDX-License-Identifier: MIT

// The limits on the length of a performance, for both performers (performer.cpp and jaudio/performance.cpp).

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

// Shortest note or wait that a track holds until the game stops the sound. That's 200 beats at the default timebase,
// minutes at a typical tempo. Games use such notes and waits for sounds they stop themselves.
inline constexpr int32_t kHeldTicks = 9600;

// Longest release tail kept after the end.
inline constexpr double kMaxTailSeconds = 10.0;

} // namespace ntdlib::sequence_export
