// SPDX-License-Identifier: MIT

// A MusyX song (SNG). A song arranges patterns on up to 64 tracks. Each track plays its patterns at the ticks that its
// entries give, on a MIDI channel, and can loop back to an earlier entry. A pattern holds notes, program changes and
// controllers, and two streams for the pitch wheel and the mod wheel. A beat is 384 ticks. The song's tempo can change
// at ticks that a tempo table gives. In a song with sections, each track belongs to one of 16 sections, and each
// section loops on its own. The layout is that of MusyX 2 on the GameCube. The files are big-endian there.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ntdlib::musyx
{

// Ticks in a beat.
inline constexpr uint32_t kTicksPerBeat = 384;

// A track entry's pattern number for the end of the track, and for a loop back to an earlier entry.
inline constexpr uint16_t kEndOfTrack = 0xffff;
inline constexpr uint16_t kLoopBack = 0xfffe;

// A track entry. A pattern's entry starts it at `tick`. The pattern's notes play `transpose` keys higher and
// `velocity_add` louder. The entry can change the channel's program and set its volume (CC7). A loop entry goes back
// to the entry `loop_target` when the song reaches `tick`.
struct TrackEntry
{
    uint32_t tick = 0;
    uint16_t pattern = kEndOfTrack;
    uint8_t program = 0xff; // 0xff for none
    uint8_t volume = 0xff;  // 0xff for none
    int8_t transpose = 0;
    int8_t velocity_add = 0;
    uint16_t loop_target = 0;
};

// A track: its number (0 to 63), its MIDI channel, its section and its entries. The entries end with an end or a loop
// entry.
struct Track
{
    uint8_t number = 0;
    uint8_t channel = 0;
    uint8_t section = 0;
    std::vector<TrackEntry> entries;
};

// The kinds of a pattern's events.
enum class PatternEventKind : uint8_t
{
    kNote,
    kProgram,
    kController,
};

// An event of a pattern, at a tick from the pattern's start. A note has a key, a velocity and a length in ticks. A
// program change has its program in `value`. A controller event sets `controller` to `value`. Besides MIDI's
// controllers, MusyX has these: 130 (0x82), and the commands 0x68 (a synchronised crossfade), 0x69 and 0x6a (the
// channel's priority) that go as controller numbers. The two MIDI messages 0x79 (reset all controllers) and 0x7b (all
// notes off) also go as controller numbers.
struct PatternEvent
{
    uint32_t tick = 0;
    PatternEventKind kind = PatternEventKind::kNote;
    uint8_t key = 0;
    uint8_t velocity = 0;
    uint16_t length = 0;
    uint8_t controller = 0;
    uint8_t value = 0;
};

// A point of a pattern's pitch-wheel or mod-wheel stream: the stream's value from `tick` on. The pitch wheel starts at
// 0x2000 at the pattern's start, and the mod wheel at 0. Both have 14 bits.
struct StreamPoint
{
    uint32_t tick = 0;
    int32_t value = 0;
};

// A pattern. The pattern ends at `end_tick`. A stream's points after that tick don't play.
struct Pattern
{
    std::vector<PatternEvent> events;
    uint32_t end_tick = 0;
    std::vector<StreamPoint> pitch_wheel;
    std::vector<StreamPoint> mod_wheel;
};

// A tempo change at `tick`, in beats per minute. A song that keeps its tempo in 1024ths of a beat per minute
// (Song::fine_tempo) keeps these that way too.
struct TempoChange
{
    uint32_t tick = 0;
    uint32_t tempo = 0;
};

// A song.
struct Song
{
    uint32_t tempo = 120;    // the tempo at the start, in beats per minute or 1024ths of them
    bool fine_tempo = false; // the tempos are in 1024ths of a beat per minute
    bool sections = false;   // each track follows its section's loop

    // The tick that a loop goes back to: one for the whole song, or one for each section.
    std::array<uint32_t, 16> loop_starts{};

    std::vector<TempoChange> tempo_changes;
    std::vector<Track> tracks;     // the tracks that play, by number
    std::vector<Pattern> patterns; // by number, up to the highest that a track plays
};

// Reads the song `file`. Throws FormatError if it's damaged.
Song ReadSong(std::span<const uint8_t> file);

} // namespace ntdlib::musyx
