// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/konami/performance.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/konami/driver.h"
#include "ntdlib/konami/seqformat.h"
#include "ntdlib/konami/sequencer.h"
#include "ntdlib/sequence_export/agb/settings.h"

#include "converted_notes.h"

namespace ntdlib::sequence_export::konami
{

using ntdlib::agb::kFrameRate;
using ntdlib::agb::Rom;
using ntdlib::konami::Command;
using ntdlib::konami::DriverInfo;
using ntdlib::konami::IsPsgTrack;
using ntdlib::konami::kPsgTracks;
using ntdlib::konami::kTracks;
using ntdlib::konami::Op;
using ntdlib::konami::Revision;
using ntdlib::konami::SongHeader;
using ntdlib::konami::TrackRead;

namespace
{

// A command that a track ran in a frame that plays.
struct RanCommand
{
    uint32_t frame = 0;
    const Command* command = nullptr;
    bool note = false;
};

// Gives the offset of a read from the song's base, at the start of an item of the decoded song. Dungeon Dice Monsters' FA
// holds a delay and a command in its last two bytes. A track reads them after a loop of the song, and the decoded song
// holds them in FA's item. Such a read's offset is the offset of FA.
class ReadOffsets
{
public:
    ReadOffsets(const std::vector<TrackRead>& reads, uint32_t base) : base_(base)
    {
        for (const TrackRead& read : reads)
        {
            if (read.track >= 0 && read.command.op == Op::kLoopPoint)
            {
                loop_points_[read.command.addr] = read.command.length;
            }
        }
    }

    uint32_t Of(uint32_t address) const
    {
        const auto after = loop_points_.upper_bound(address);
        if (after != loop_points_.begin())
        {
            const auto point = std::prev(after);
            if (address < point->first + point->second)
            {
                return point->first - base_;
            }
        }

        return address - base_;
    }

private:
    uint32_t base_;
    std::map<uint32_t, uint32_t> loop_points_; // the length of each loop point that ran, by its address
};

// Each track's commands, without the delays and the reads of a frame that a loop of the song ran again. A frame at or
// after `end` doesn't play.
std::array<std::vector<RanCommand>, kTracks> CommandsOf(const std::vector<TrackRead>& reads, uint32_t end)
{
    std::array<std::vector<RanCommand>, kTracks> commands;
    for (const TrackRead& read : reads)
    {
        if (read.frame >= end)
        {
            continue;
        }
        if (read.track < 0)
        {
            for (std::vector<RanCommand>& track : commands)
            {
                while (!track.empty() && track.back().frame == read.frame)
                {
                    track.pop_back();
                }
            }
            continue;
        }

        const Op op = read.command.op;
        if (op != Op::kDelay)
        {
            const bool note = op == Op::kNote || op == Op::kPsgNote || op == Op::kSampleAtNote || op == Op::kNextNote;
            commands[size_t(read.track)].push_back({read.frame, &read.command, note});
        }
    }

    return commands;
}

// The stretches of each track's reads. A stretch begins at a track's first read, and at each read that doesn't follow
// the one before it. The read before it then says how the stretch began. In the song's last frame, a read only finishes
// the stretch that runs there: a stretch that would begin there belongs to the next pass.
std::array<std::vector<Stretch>, kTracks> StretchesOf(const std::vector<TrackRead>& reads, const ReadOffsets& offsets,
                                                      uint32_t end)
{
    std::array<std::vector<Stretch>, kTracks> stretches;
    std::array<const Command*, kTracks> last = {};
    std::array<uint8_t, kTracks> depth = {};
    std::array<bool, kTracks> looped = {};
    for (const TrackRead& read : reads)
    {
        if (read.frame > end)
        {
            continue;
        }
        if (read.track < 0)
        {
            last.fill(nullptr);
            depth.fill(0);
            looped.fill(true);
            continue;
        }

        const auto t = size_t(read.track);
        const Command& c = read.command;
        const Command* before = last[t];

        // A loop point can skip bytes that belong to it.
        const bool follows = before && (c.addr == before->addr + before->length || before->op == Op::kLoopPoint);
        if (!follows && read.frame == end)
        {
            continue;
        }

        last[t] = &c;
        if (follows)
        {
            stretches[t].back().last = offsets.Of(c.addr);
            continue;
        }

        Stretch stretch;
        stretch.tick = read.frame;
        stretch.first = offsets.Of(c.addr);
        stretch.last = stretch.first;
        if (looped[t])
        {
            stretch.entry = Entry::kLoop;
            looped[t] = false;
        }
        else if (before && before->op == Op::kCall)
        {
            stretch.entry = Entry::kCall;
            depth[t]++;
        }
        else if (before && depth[t] > 0)
        {
            stretch.entry = Entry::kReturn;
            depth[t]--;
        }
        else if (before)
        {
            stretch.entry = Entry::kJump;
        }
        stretch.depth = depth[t];
        stretches[t].push_back(stretch);
    }

    return stretches;
}

// Sets `setting` of the track's mix to `value` at `tick`. A later change in the same tick replaces an earlier one.
void AddChange(std::vector<MixChange>& changes, uint32_t tick, Mix setting, int value)
{
    const auto same = std::find_if(changes.rbegin(), changes.rend(),
                                   [&](const MixChange& c) { return c.tick == tick && c.setting == setting; });
    if (same != changes.rend())
    {
        same->value = int16_t(value);
    }
    else
    {
        changes.push_back({tick, setting, int16_t(value)});
    }
}

// The pan from -64 to 63 of a track's levels on the left and the right, each from 0 to 15.
int PanOfLevels(int left, int right)
{
    const int difference = right - left;
    return int(std::lround(double(difference) * (difference < 0 ? 64 : 63) / 15));
}

// Each track's changes of its volume and pan, from the commands that ran.
std::array<std::vector<MixChange>, kTracks> MixOf(const std::array<std::vector<RanCommand>, kTracks>& commands)
{
    std::array<std::vector<MixChange>, kTracks> mix;
    for (int t = 0; t < kTracks; t++)
    {
        std::vector<MixChange>& changes = mix[size_t(t)];
        for (const RanCommand& ran : commands[size_t(t)])
        {
            const Command& c = *ran.command;
            switch (c.op)
            {
            case Op::kNote:
            case Op::kPsgNote:
            case Op::kSampleAtNote:
            case Op::kVolume:
            case Op::kSetVolume:
                if (c.vol >= 0)
                {
                    AddChange(changes, ran.frame, Mix::kVolume, c.vol);
                }
                break;

            case Op::kNextNote:
                if (c.vol >= 0 && t + 1 < kTracks)
                {
                    AddChange(mix[size_t(t + 1)], ran.frame, Mix::kVolume, c.vol);
                }
                break;

            case Op::kPairVolumes:
                AddChange(changes, ran.frame, Mix::kVolume, c.value & 15);
                if (t + 1 < kTracks)
                {
                    AddChange(mix[size_t(t + 1)], ran.frame, Mix::kVolume, c.value >> 4);
                }
                break;

            case Op::kPan:
                AddChange(changes, ran.frame, Mix::kPan, c.value - 0x40);
                break;

            case Op::kPanLevels:
                AddChange(changes, ran.frame, Mix::kPan, PanOfLevels(c.value >> 4, c.value & 15));
                break;

            case Op::kPsgPan:
                AddChange(changes, ran.frame, Mix::kPan,
                          PanOfLevels(((c.value >> (t + 4)) & 1) * 15, ((c.value >> t) & 1) * 15));
                break;

            default:
                break;
            }
        }
    }

    // A track's change from the track before it comes at its tick among the track's changes.
    for (std::vector<MixChange>& changes : mix)
    {
        std::stable_sort(changes.begin(), changes.end(),
                         [](const MixChange& a, const MixChange& b) { return a.tick < b.tick; });
    }

    return mix;
}

// The stretch of `stretches` that runs the read at `offset` by `tick`: the last that began by then and holds it.
uint32_t StretchOf(const std::vector<Stretch>& stretches, uint32_t offset, uint32_t tick)
{
    for (size_t i = stretches.size(); i-- > 0;)
    {
        const Stretch& s = stretches[i];
        if (s.tick <= tick && s.first <= offset && offset <= s.last)
        {
            return uint32_t(i);
        }
    }

    return 0;
}

// What started a note of a track.
struct Started
{
    int track = 0;
    NoteCause cause = NoteCause::kNone;
    const Command* command = nullptr;
};

// Finds the commands behind the notes of each track.
class NoteCommands
{
public:
    NoteCommands(const std::array<std::vector<RanCommand>, kTracks>& commands, Revision revision)
        : commands_(commands), revision_(revision)
    {
    }

    // Returns what started a note on `track` at `frame`.
    Started Find(int track, uint32_t frame) const
    {
        // A note command of the track's.
        if (const Command* c = InFrame(track, frame, /*notes=*/true, /*next_track=*/false))
        {
            return {track, NoteCause::kNote, c};
        }

        // A note command of the track before it, whose note plays on this track.
        const bool from_before = FromTrackBefore(track);
        if (from_before)
        {
            if (const Command* c = InFrame(track - 1, frame, /*notes=*/true, /*next_track=*/true))
            {
                return {track - 1, NoteCause::kNote, c};
            }
        }

        // Another command in the note's frame.
        if (const Command* c = InFrame(track, frame, /*notes=*/false, /*next_track=*/false))
        {
            return {track, NoteCause::kOther, c};
        }
        if (from_before)
        {
            if (const Command* c = InFrame(track - 1, frame, /*notes=*/false, /*next_track=*/false))
            {
                return {track - 1, NoteCause::kOther, c};
            }
        }

        // The sound of an earlier note.
        if (const Command* c = BeforeFrame(track, frame, false))
        {
            return {track, NoteCause::kEarlier, c};
        }
        if (from_before)
        {
            if (const Command* c = BeforeFrame(track - 1, frame, true))
            {
                return {track - 1, NoteCause::kEarlier, c};
            }
        }

        return {};
    }

private:
    // Returns true if a note on `track` can come from the track before it.
    bool FromTrackBefore(int track) const
    {
        if (track == 0)
        {
            return false;
        }

        return revision_ == Revision::kDungeonDiceMonsters ||
               (ntdlib::konami::OlderOutputStage(revision_) && IsPsgTrack(track));
    }

    // Returns true if `ran` is a note of its track, or with `next_track` a note that can play on the next track.
    bool IsNote(const RanCommand& ran, bool next_track) const
    {
        if (!ran.note)
        {
            return false;
        }

        const bool played_on_next = ran.command->op == Op::kNextNote;
        return next_track ? played_on_next || revision_ != Revision::kDungeonDiceMonsters : !played_on_next;
    }

    // Returns the last note command, or else with `notes` false the last other command, of `track` in `frame`.
    const Command* InFrame(int track, uint32_t frame, bool notes, bool next_track) const
    {
        const std::vector<RanCommand>& commands = commands_[size_t(track)];
        const auto after = std::upper_bound(commands.begin(), commands.end(), frame,
                                            [](uint32_t f, const RanCommand& ran) { return f < ran.frame; });
        for (auto it = after; it != commands.begin();)
        {
            --it;
            if (it->frame != frame)
            {
                break;
            }
            if (!notes || IsNote(*it, next_track))
            {
                return it->command;
            }
        }

        return nullptr;
    }

    // Returns the last note command of `track` before `frame`.
    const Command* BeforeFrame(int track, uint32_t frame, bool next_track) const
    {
        const std::vector<RanCommand>& commands = commands_[size_t(track)];
        const auto after = std::lower_bound(commands.begin(), commands.end(), frame,
                                            [](const RanCommand& ran, uint32_t f) { return ran.frame < f; });
        for (auto it = after; it != commands.begin();)
        {
            --it;
            if (IsNote(*it, next_track))
            {
                return it->command;
            }
        }

        return nullptr;
    }

    const std::array<std::vector<RanCommand>, kTracks>& commands_;
    Revision revision_;
};

std::string TrackNameOf(int t)
{
    static const char* const kPsgNames[kPsgTracks] = {"Square 1", "Square 2", "Wave", "Noise"};
    return IsPsgTrack(t) ? kPsgNames[t] : "Voice " + std::to_string(t - kPsgTracks);
}

} // namespace

Performance PerformSong(const Rom& rom, const DriverInfo& info, int song, const agb::ConvertSettings& settings)
{
    Performance perf;
    SongHeader header;
    if (!ntdlib::konami::ReadSongHeader(rom, info.song_table, song, info.revision, header) ||
        !rom.Contains(header.base))
    {
        perf.warnings.push_back("its header can't be read");
        return perf;
    }

    perf.base = header.base;
    const ConvertedSong converted = ConvertedNotes(rom, info, song, settings);
    perf.warnings = converted.warnings;
    perf.truncated = converted.truncated;
    perf.end_tick = converted.end_frame;
    perf.seconds = double(converted.end_frame) / kFrameRate;
    perf.midi_end_tick = converted.end_tick;
    if (converted.loops)
    {
        perf.loop = seqlib::midi::Performance::Loop{converted.loop_start, converted.loop_end};
        perf.loops = settings.loops;
    }

    perf.timebase = uint32_t(std::max(std::lround(converted.quarter_frames), 1L));
    if (converted.quarter_frames <= 0)
    {
        perf.timebase = 24;
    }
    perf.tempo.push_back({0, 60 * kFrameRate / perf.timebase});
    for (int t = 0; t < kTracks; t++)
    {
        perf.track_names[size_t(t)] = TrackNameOf(t);
    }

    const ReadOffsets offsets(converted.reads, header.base);
    perf.stretches = StretchesOf(converted.reads, offsets, converted.end_frame);
    const std::array<std::vector<RanCommand>, kTracks> commands = CommandsOf(converted.reads, converted.end_frame);
    perf.mix = MixOf(commands);

    const NoteCommands starts(commands, info.revision);
    std::array<std::optional<uint32_t>, kTracks> preset;
    for (const ConvertedNote& n : converted.notes)
    {
        std::vector<TrackEvent>& events = perf.tracks[size_t(n.track)];
        const uint32_t number = uint32_t(n.bank) * 128 + uint32_t(n.program);
        if (preset[size_t(n.track)] != number)
        {
            preset[size_t(n.track)] = number;
            TrackEvent event;
            event.tick = n.on_frame;
            event.kind = EventKind::kPreset;
            event.preset = number;
            events.push_back(event);
        }

        const Started started = starts.Find(n.track, n.on_frame);
        NoteCommand command;
        if (started.command)
        {
            command.offset = offsets.Of(started.command->addr);
            command.stretch = StretchOf(perf.stretches[size_t(started.track)], command.offset, n.on_frame);
        }
        const uint32_t id = uint32_t(perf.note_commands.size()) + 1;
        perf.note_commands.push_back(command);
        perf.note_starts.push_back({uint8_t(started.track), started.cause});
        perf.midi_places.push_back({n.on_tick, n.off_tick});

        TrackEvent on;
        on.tick = n.on_frame;
        on.kind = EventKind::kNoteOn;
        on.key = uint8_t(n.key);
        on.value = uint8_t(n.velocity);
        on.note = id;
        events.push_back(on);

        TrackEvent off = on;
        off.tick = n.off_frame;
        off.kind = EventKind::kNoteOff;
        off.value = 0;
        events.push_back(off);
    }

    return perf;
}

} // namespace ntdlib::sequence_export::konami
