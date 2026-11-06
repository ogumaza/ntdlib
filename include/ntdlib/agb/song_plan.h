// SPDX-License-Identifier: MIT

// The plan of a song's conversion from its tracks' loops and ends, as every GBA driver's conversion makes it.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace ntdlib::agb
{

// The most times as long as the longest of its tracks' loops that a song's loop can be.
constexpr uint64_t kMaxLoopFactor = 8;

// Returns the length of a song's loop from its looping tracks' loop lengths, which are above 0: the shortest length
// that each of them divides, so that every track is back where its loop started, or the longest of them if that would
// be more than kMaxLoopFactor times as long.
uint64_t LoopLength(const std::vector<uint64_t>& lengths);

// A track's loop and end in its driver's ticks, read from its commands without timing them. Each driver's ScanTrack()
// says which command counts as the last command of a track without a loop.
struct TrackLayout
{
    bool loops = false;
    uint64_t loop_start = 0;   // the tick that the loop starts at, the first time through
    uint64_t loop_end = 0;     // the tick at which the track goes back to the loop's start
    uint64_t end = 0;          // the end of a track without a loop
    uint64_t last_command = 0; // the tick by which a track without a loop has played its last command
};

// The stretch of a song that a conversion covers: its loop, and the tick at which the MIDI file ends.
struct SongPlan
{
    bool loops = false;
    uint64_t loop_start = 0; // the latest of the looping tracks' loop starts and the other tracks' last commands
    uint64_t loop_end = 0;   // the loop start plus the length LoopLength() gives for the tracks' loops
    uint64_t end = 0;
};

// The longest silence that a song without a loop keeps after its last sound has died away, in seconds. A song can end
// long after that point: a tempo change, a controller's change or a track's end can come after a long wait. When the
// end lies further than this after the last sound, the song ends where that sound dies away. Nothing after that point
// can be heard, and a player would show the silence as part of the song.
constexpr double kMaxSilentEndSeconds = 10.0;

// The pass through a song's loop that the marked loop starts at.
enum class LoopStart
{
    kFirstPass,
    kSecondPass, // the song plays the first pass before the marked loop
};

// Plans a conversion from the layouts of a song's tracks: looping songs play `loops` passes; other songs run until the
// last track ends. Tracks can have different loop points and lengths. The overall loop starts once all looping tracks
// have entered their loops and all other tracks have played their last command. It ends when all looping tracks return
// to their loop starts, subject to the length limit in LoopLength(). `start_pass` is the pass through the loop that the
// marked loop starts at.
SongPlan PlanSong(const std::vector<TrackLayout>& tracks, int loops, LoopStart start_pass);

// Collects the notes that start in a song's first two passes through its loop, to tell whether a player that loops on
// the markers can repeat the first. A driver can carry a setting, such as a pan, a level, a program or the tempo, from
// the loop's end into the next pass, so that the first pass, which comes from the song's start, plays unlike the later
// ones. The marked loop then starts at the second pass.
class PassComparison
{
public:
    // The first pass runs from `start` for `length` of the driver's units of time, and the second for another `length`.
    PassComparison(uint64_t start, uint64_t length);

    // Adds a note that starts at `on` on channel or track `channel`, with its key and the settings it starts with, in
    // whatever form the driver has them. A note that starts outside the two passes is left out.
    void Add(uint64_t on, int channel, int key, std::vector<double> settings);

    // Returns true if the two passes start the same notes at the same times from their starts, with the same settings.
    bool Alike() const;

    // Returns true if the two passes start the same notes at the same times from their starts. Their settings may
    // differ by `tolerance` at most. Notes that start at the same time on the same channel with the same key are
    // compared in the order they were added.
    bool Alike(double tolerance) const;

private:
    struct Start
    {
        bool operator<(const Start& other) const;
        bool operator==(const Start& other) const;

        uint64_t at = 0;
        int channel = 0;
        int key = 0;
        std::vector<double> settings;
    };

    uint64_t start_;
    uint64_t length_;
    std::array<std::vector<Start>, 2> passes_;
};

} // namespace ntdlib::agb
