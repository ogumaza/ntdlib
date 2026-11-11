// SPDX-License-Identifier: MIT

// What a game can set through a sequence sound's handle before the sequence's first tick: the tracks that play, the
// tempo ratio and the variables. Games use them to play one sequence in several arrangements. A player of a sound
// archive gives them in place of the game's code. So can a description of a game's effects (ArchiveEffects). It can
// give the tracks that a game plays of some sounds, and the values that the game gives some of their variables.

#pragma once

#include <cstdint>
#include <map>
#include <vector>

namespace ntdlib::rvl::snd
{

// Every track of a sequence, bit N for track N.
constexpr uint16_t kAllTracks = 0xffff;

struct Arrangement
{
    // The tracks that play, bit N for track N. The others get a track volume of 0 (SeqPlayer::SetTrackVolume,
    // 0x8003e8b0 in Wii Sports' main.dol).
    uint16_t tracks = kAllTracks;

    // The tempo ratio, by which the clock multiplies the tempo (SeqPlayer::SetTempoRatio, 0x8003e818).
    float tempo_ratio = 1.0f;

    // The variables, by number: 0 to 15 are the player's variables, and 16 to 31 the global ones.
    std::map<int, int16_t> variables = {};
};

// The tracks that a game plays of a sound. As the sound starts, the game sets the volumes of the others to 0 through
// the sound's handle (Animal Crossing: City Folk).
struct SoundTracks
{
    uint32_t sound = 0; // the index in the archive's sound table
    uint16_t tracks = kAllTracks;
};

// The values that a game gives some variables of a sound. The game writes them through the sound's handle once the
// sound has started (Wii Sports' golf, for its wind).
struct SoundVariables
{
    uint32_t sound = 0; // the index in the archive's sound table
    std::map<int, int16_t> variables = {};
};

// The variables that `variables` gives `sound`: none for a sound that it doesn't list.
inline std::map<int, int16_t> VariablesOf(const std::vector<SoundVariables>& variables, uint32_t sound)
{
    for (const SoundVariables& entry : variables)
    {
        if (entry.sound == sound)
        {
            return entry.variables;
        }
    }

    return {};
}

// The tracks that `tracks` gives `sound`: every track for a sound that it doesn't list.
inline uint16_t TracksOf(const std::vector<SoundTracks>& tracks, uint32_t sound)
{
    for (const SoundTracks& entry : tracks)
    {
        if (entry.sound == sound)
        {
            return entry.tracks;
        }
    }

    return kAllTracks;
}

} // namespace ntdlib::rvl::snd
