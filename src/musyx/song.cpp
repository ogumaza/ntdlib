// SPDX-License-Identifier: MIT

#include "ntdlib/musyx/song.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "xsflib/binary.h"

namespace ntdlib::musyx
{
namespace
{

// The header: the offsets of the track table, the pattern table, the channel map and the tempo table, then the tempo
// with its flags, and the loop start. A song with sections has 16 loop starts and the offset of its section map.
constexpr std::size_t kTrackTable = 0x00;
constexpr std::size_t kPatternTable = 0x04;
constexpr std::size_t kChannelMap = 0x08;
constexpr std::size_t kTempoTable = 0x0c;
constexpr std::size_t kInfo = 0x10;
constexpr std::size_t kLoopStarts = 0x14;
constexpr std::size_t kSectionMap = 0x54;
constexpr uint32_t kSectionsBit = 0x80000000;
constexpr uint32_t kFineTempoBit = 0x40000000;
constexpr uint32_t kTempoMask = 0x0fffffff;

constexpr std::size_t kTracks = 64;
constexpr std::size_t kChannels = 16;
constexpr std::size_t kSections = 16;
constexpr std::size_t kEntrySize = 12;

// The tick that ends the tempo table.
constexpr uint32_t kEndOfTempos = 0xffffffff;

// A pattern starts with 12 bytes: its header's size, and the offsets of its pitch-wheel and mod-wheel streams from the
// song's start. Each event then starts with a 16-bit delta time and a key byte and a velocity byte. A note goes on with
// a 16-bit length.
constexpr std::size_t kPatternHeader = 12;
constexpr uint8_t kEndKey = 0xff;
constexpr uint8_t kCommandBit = 0x80;

// The bytes that end a stream.
constexpr uint8_t kEndOfStream0 = 0x80;
constexpr uint8_t kEndOfStream1 = 0x00;

// The first values of the pitch-wheel and mod-wheel streams.
constexpr int32_t kPitchWheelCentre = 0x2000;

// MusyX's extended controller that a velocity byte of 1 sets.
constexpr uint8_t kController130 = 0x82;

// Bounds that only damaged songs reach: the entries of a track and the events of a pattern.
constexpr std::size_t kMostEntries = 0x10000;
constexpr std::size_t kMostEvents = 0x100000;

// Reads a stream's value: one byte, or two if the first has bit 7 set. A signed value has 7 or 15 bits.
uint32_t StreamNumber(const xsflib::Reader& r, std::size_t& at)
{
    const uint8_t first = r.U8(at++);
    if ((first & 0x80) == 0)
    {
        return first;
    }

    return static_cast<uint32_t>(first & 0x7f) << 8 | r.U8(at++);
}

// Reads the stream at `offset`: pairs of a delta time and a delta of the value, up to the bytes 0x80 0x00. The value
// starts at `first`. An offset of 0 means no stream.
std::vector<StreamPoint> ReadStream(const xsflib::Reader& r, uint32_t offset, int32_t first)
{
    std::vector<StreamPoint> points;
    if (offset == 0)
    {
        return points;
    }

    uint32_t tick = 0;
    int32_t value = first;
    for (std::size_t at = offset; !(r.U8(at) == kEndOfStream0 && r.U8(at + 1) == kEndOfStream1);)
    {
        tick += StreamNumber(r, at);
        const uint8_t first_byte = r.U8(at);
        const uint32_t raw = StreamNumber(r, at);
        const bool wide = (first_byte & 0x80) != 0;
        const int32_t sign_bit = wide ? 0x4000 : 0x40;
        const int32_t delta = (static_cast<int32_t>(raw) & sign_bit) != 0 ? static_cast<int32_t>(raw) - 2 * sign_bit
                                                                          : static_cast<int32_t>(raw);
        value += delta;
        points.push_back({tick, value});
        if (points.size() > kMostEvents)
        {
            throw xsflib::FormatError("song has a stream that never ends");
        }
    }

    return points;
}

// Reads the pattern at `offset`.
Pattern ReadPattern(const xsflib::Reader& r, uint32_t offset)
{
    Pattern p;
    r.Sub(offset, kPatternHeader);
    p.pitch_wheel = ReadStream(r, r.U32(offset + 4), kPitchWheelCentre);
    p.mod_wheel = ReadStream(r, r.U32(offset + 8), 0);

    // An event's delta time counts from the event before, a rest included. A key byte with bit 7 set makes a command:
    // a velocity byte of 0 changes the program, 1 sets controller 130, and one with bit 7 set sets the controller in
    // its low 7 bits. MusyX ignores the other commands. A key and a velocity of 0 make a rest.
    uint32_t tick = 0;
    for (std::size_t at = offset + kPatternHeader;;)
    {
        tick += r.U16(at);
        const uint8_t key = r.U8(at + 2);
        const uint8_t velocity = r.U8(at + 3);
        if (key == kEndKey && velocity == kEndKey)
        {
            p.end_tick = tick;
            break;
        }
        if (p.events.size() > kMostEvents)
        {
            throw xsflib::FormatError("song has a pattern that never ends");
        }

        if ((key & kCommandBit) != 0)
        {
            at += 4;
            const auto value = static_cast<uint8_t>(key & 0x7f);
            if (velocity == 0)
            {
                p.events.push_back({.tick = tick, .kind = PatternEventKind::kProgram, .value = value});
            }
            else if (velocity == 1)
            {
                p.events.push_back({.tick = tick,
                                    .kind = PatternEventKind::kController,
                                    .controller = kController130,
                                    .value = value});
            }
            else if ((velocity & kCommandBit) != 0)
            {
                p.events.push_back({.tick = tick,
                                    .kind = PatternEventKind::kController,
                                    .controller = static_cast<uint8_t>(velocity & 0x7f),
                                    .value = value});
            }
            continue;
        }
        if (key == 0 && velocity == 0)
        {
            at += 4;
            continue;
        }

        p.events.push_back(
            {.tick = tick, .kind = PatternEventKind::kNote, .key = key, .velocity = velocity, .length = r.U16(at + 4)});
        at += 6;
    }

    return p;
}

// Reads the entries of the track at `offset`, up to its end or loop entry.
std::vector<TrackEntry> ReadEntries(const xsflib::Reader& r, uint32_t offset)
{
    std::vector<TrackEntry> entries;
    for (std::size_t at = offset;; at += kEntrySize)
    {
        TrackEntry& e = entries.emplace_back();
        e.tick = r.U32(at);
        e.program = r.U8(at + 4);
        e.volume = r.U8(at + 5);
        e.pattern = r.U16(at + 8);
        e.transpose = static_cast<int8_t>(r.U8(at + 10));
        e.velocity_add = static_cast<int8_t>(r.U8(at + 11));
        e.loop_target = r.U16(at + 10);
        if (e.pattern == kEndOfTrack || e.pattern == kLoopBack)
        {
            break;
        }
        if (entries.size() > kMostEntries)
        {
            throw xsflib::FormatError("song has a track that never ends");
        }
    }

    const TrackEntry& last = entries.back();
    if (last.pattern == kLoopBack && last.loop_target >= entries.size() - 1)
    {
        throw xsflib::FormatError("song has a track that loops to entry " + std::to_string(last.loop_target) + " of " +
                                  std::to_string(entries.size()));
    }

    return entries;
}

} // namespace

Song ReadSong(std::span<const uint8_t> file)
{
    const xsflib::Reader r(file, true);
    Song song;
    const uint32_t info = r.U32(kInfo);
    song.sections = (info & kSectionsBit) != 0;
    song.fine_tempo = (info & kFineTempoBit) != 0;
    song.tempo = info & kTempoMask;
    song.loop_starts[0] = r.U32(kLoopStarts);
    if (song.sections)
    {
        for (std::size_t s = 1; s < kSections; s++)
        {
            song.loop_starts[s] = r.U32(kLoopStarts + 4 * s);
        }
    }

    // The tempo table: pairs of a tick and a tempo, up to the tick 0xffffffff.
    if (const uint32_t tempos = r.U32(kTempoTable); tempos != 0)
    {
        for (std::size_t at = tempos; r.U32(at) != kEndOfTempos; at += 8)
        {
            song.tempo_changes.push_back({r.U32(at), r.U32(at + 4)});
            if (song.tempo_changes.size() > kMostEntries)
            {
                throw xsflib::FormatError("song has a tempo table that never ends");
            }
        }
    }

    // The tracks, with their channels and sections.
    const uint32_t tracks = r.U32(kTrackTable);
    const uint32_t channels = r.U32(kChannelMap);
    const uint32_t sections = song.sections ? r.U32(kSectionMap) : 0;
    uint32_t patterns = 0;
    for (std::size_t t = 0; t < kTracks; t++)
    {
        const uint32_t offset = r.U32(tracks + 4 * t);
        if (offset == 0)
        {
            continue;
        }

        Track& track = song.tracks.emplace_back();
        track.number = static_cast<uint8_t>(t);
        track.channel = r.U8(channels + t);
        track.section = song.sections ? r.U8(sections + t) : 0;
        if (track.channel >= kChannels || track.section >= kSections)
        {
            throw xsflib::FormatError("song has track " + std::to_string(t) + " on channel " +
                                      std::to_string(track.channel) + " in section " + std::to_string(track.section));
        }

        track.entries = ReadEntries(r, offset);
        for (const TrackEntry& e : track.entries)
        {
            if (e.pattern != kEndOfTrack && e.pattern != kLoopBack)
            {
                patterns = std::max<uint32_t>(patterns, e.pattern + 1u);
            }
        }
    }

    // The patterns that the tracks play.
    const uint32_t pattern_table = r.U32(kPatternTable);
    for (uint32_t p = 0; p < patterns; p++)
    {
        song.patterns.push_back(ReadPattern(r, r.U32(pattern_table + 4 * std::size_t{p})));
    }

    return song;
}

} // namespace ntdlib::musyx
