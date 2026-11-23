// SPDX-License-Identifier: MIT

// The loops of a sound on the model of Nintendo SPD's driver (player.h), and the length of a sound for a rip.
//
// A track loops when its 0xF9 jumps back to its 0xF8. The sound's loop is the loop of the track with the longest: from
// the frame of its first 0xF8 to the frame of its first 0xF9. A track that 0xF4 holds reads no events, and a sound
// whose every track that reads events has looped plays its loop again from then on.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "ntdlib/spd/player.h"

namespace ntdlib::spd
{

// The loop marks of a track: the frame of its first 0xF8 and the frames of its jumps back.
struct TrackLoop
{
    std::optional<uint32_t> start;
    std::vector<uint32_t> jumps;
};

// Follows the loop marks of the tracks that a sound plays, frame by frame.
class LoopWatch
{
public:
    // Notes the marks of the frame `frame` that `player` has just run.
    void Note(const Player& player, uint32_t frame);

    // Whether a track of `player` reads events: a playing player's track that hasn't ended and that 0xF4 doesn't hold.
    static bool Reading(const Player& player);

    // Whether a playing player fades or moves to a level.
    static bool Fading(const Player& player);

    // Whether every track that reads events has jumped back at least `passes` times. False when no track reads events.
    bool Looped(const Player& player, uint32_t passes) const;

    // The marks of the track with the longest loop. Nothing when no track jumped back after its loop's start.
    std::optional<TrackLoop> Longest() const;

private:
    std::map<std::pair<int, int>, TrackLoop> loops_;
};

// The length of a sound for a rip, in frames of the driver from the frame of its start message.
struct SoundLength
{
    // The sound's loop: its start and the frames of a pass. Nothing for a sound that ends.
    std::optional<uint32_t> loop_start;
    uint32_t loop_frames = 0;

    // For a sound that ends: the frame after its last frame with a channel of a note at a volume above 0. A sound ends
    // when nothing plays, or when no track reads events, no player fades and no channel can sound again: each channel
    // is free, or at a volume of 0 outside its attack, such as a note held at a sustain level of 0. A sound whose
    // channels hold their sustain with waves that end, and nothing else changes, ends there too: the hardware stops
    // each channel at its wave's end. The model doesn't follow that.
    std::optional<uint32_t> end;

    // For a sound that holds a note without end: the frame from which nothing changes, with no track that reads events
    // and no fade, while a channel above volume 0 plays a wave that loops, a square wave or noise.
    std::optional<uint32_t> held;

    // Whether a channel of a note played at a volume above 0. The capture reverb's channels don't count.
    bool audible = false;
};

// Starts sound `sound` (from 1) of the block in slot `slot` (0 for set A) on `player`, and plays it until every track
// that reads events has looped once, until it ends or holds a note without end, or for `max_frames`. A sound that does
// none of these in time has no loop, no end and no hold.
SoundLength MeasureSound(Player& player, int slot, int sound, uint32_t max_frames);

} // namespace ntdlib::spd
