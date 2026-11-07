// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/rd2/performance.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "converted_notes.h"
#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/rd2/commands.h"
#include "ntdlib/rd2/driver.h"
#include "ntdlib/rd2/sequencer.h"
#include "ntdlib/rd2/song.h"
#include "ntdlib/sequence_export/agb/settings.h"

namespace ntdlib::sequence_export::rd2
{

using ntdlib::agb::kFrameRate;
using ntdlib::agb::Rom;
using ntdlib::rd2::DriverInfo;
using ntdlib::rd2::kCentre;
using ntdlib::rd2::kEffectEcho;
using ntdlib::rd2::kPlayerTracks;
using ntdlib::rd2::kTickUnits;
using ntdlib::rd2::SongEffect;
using ntdlib::rd2::TrackRead;

namespace
{

// The driver's ticks in a quarter note: the performance's timebase.
constexpr int kDriverTicksPerQuarter = 24;

constexpr uint8_t kPanCommand = 0xC3;
constexpr uint8_t kVolumeCommand = 0xE0;
constexpr uint8_t kEchoCommand = 0xE3;
constexpr uint8_t kStartTrackCommand = 0xF8;
constexpr uint8_t kJumpCommand = 0xF0;
constexpr uint8_t kCallCommand = 0xF4;
constexpr uint8_t kEndCommand = 0xFF;

using TrackStretches = std::array<std::vector<Stretch>, 16>;
using TrackMix = std::array<std::vector<MixChange>, 16>;

// The tick nearest to a time in units.
uint32_t TickOfUnits(uint64_t units)
{
    return uint32_t((units + kTickUnits / 2) / kTickUnits);
}

// The stretches of each track's reads. A stretch begins at a track's first read, and at each read that doesn't follow
// the one before it. The read before it then says how the stretch began. At the sequence's end, a read only finishes
// the stretch that runs there: a stretch that would begin there belongs to the next pass.
TrackStretches StretchesOf(const Rom& rom, const DriverInfo& info, const std::vector<TrackRead>& reads, uint32_t base,
                           uint64_t end)
{
    TrackStretches stretches;
    std::array<const TrackRead*, kPlayerTracks> last = {};
    for (const TrackRead& read : reads)
    {
        if (read.units > end)
        {
            continue;
        }

        const auto t = size_t(read.track);
        const TrackRead* before = last[t];
        const bool ends_stretch =
            before && (before->op == kJumpCommand || before->op == kCallCommand || before->op == kEndCommand);
        const bool follows =
            before && !ends_stretch &&
            read.address == before->address + ntdlib::rd2::DecodeCommand(rom, info, before->address, 0, 0, 0).size;
        if (!follows && read.units == end)
        {
            continue;
        }

        last[t] = &read;
        if (follows)
        {
            stretches[t].back().last = read.address - base;
            continue;
        }

        Stretch stretch;
        stretch.tick = TickOfUnits(read.units);
        stretch.first = read.address - base;
        stretch.last = stretch.first;
        stretch.depth = read.depth;
        stretch.note_wait = read.note_wait;
        if (ends_stretch && before->op == kJumpCommand)
        {
            stretch.entry = Entry::kJump;
        }
        else if (ends_stretch && before->op == kCallCommand)
        {
            stretch.entry = Entry::kCall;
        }
        else if (ends_stretch && before->depth > 0)
        {
            stretch.entry = Entry::kReturn;
        }
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

// A track's volume, pan and echo, in the units of Performance::mix.
struct TrackLevels
{
    int volume = 0x80;
    int pan = 0;
    int echo = 0;
};

// Each track's changes of its volume, pan and echo, from the commands that it read before the sequence's end. The reads
// come in the order that the driver ran them. F8 starts a track with the volume, pan and echo of the track that runs
// it. An F8 that starts the track running it gives the values that a track starts with. The started track takes them
// at its next read.
TrackMix MixOf(const Rom& rom, const std::vector<TrackRead>& reads, uint64_t end, bool effect)
{
    TrackMix mix;
    TrackLevels first;
    first.echo = effect ? kEffectEcho : 0;
    std::array<TrackLevels, kPlayerTracks> levels;
    levels.fill(first);
    std::array<std::optional<TrackLevels>, kPlayerTracks> started;
    for (const TrackRead& read : reads)
    {
        if (read.units >= end)
        {
            continue;
        }

        const auto t = size_t(read.track);
        std::vector<MixChange>& changes = mix[t];
        const uint32_t tick = TickOfUnits(read.units);
        TrackLevels& now = levels[t];
        if (started[t])
        {
            const TrackLevels& copy = *started[t];
            if (copy.volume != now.volume)
            {
                AddChange(changes, tick, Mix::kVolume, copy.volume);
            }
            if (copy.pan != now.pan)
            {
                AddChange(changes, tick, Mix::kPan, copy.pan);
            }
            if (copy.echo != now.echo)
            {
                AddChange(changes, tick, Mix::kSendA, copy.echo);
            }
            now = copy;
            started[t].reset();
        }

        const int value = rom.U8(read.address + 1);
        if (read.op == kVolumeCommand)
        {
            now.volume = value;
            AddChange(changes, tick, Mix::kVolume, value);
        }
        else if (read.op == kPanCommand)
        {
            now.pan = value - kCentre;
            AddChange(changes, tick, Mix::kPan, now.pan);
        }
        else if (read.op == kEchoCommand)
        {
            now.echo = value;
            AddChange(changes, tick, Mix::kSendA, value);
        }
        else if (read.op == kStartTrackCommand && value < kPlayerTracks)
        {
            started[size_t(value)] = value == read.track ? first : now;
        }
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

} // namespace

Performance PerformSong(const Rom& rom, const DriverInfo& info, int song, const agb::ConvertSettings& settings)
{
    Performance perf;
    const ConvertedSong converted = ConvertedNotes(rom, info, song, settings);
    perf.warnings = converted.warnings;
    perf.truncated = converted.truncated;
    if (converted.tempos.empty())
    {
        return perf;
    }

    perf.base = ntdlib::rd2::SequenceAddress(info, ntdlib::rd2::SequenceOfSong(info, song));
    perf.seconds = converted.seconds;
    perf.end_tick = uint32_t((converted.end + kTickUnits - 1) / kTickUnits);
    perf.midi_end_tick = converted.end_tick;
    if (converted.loops)
    {
        perf.loop = seqlib::midi::Performance::Loop{TickOfUnits(converted.loop_start), TickOfUnits(converted.loop_end)};
        perf.loops = settings.loops;
    }

    perf.timebase = kDriverTicksPerQuarter;
    for (const auto& [units, tempo] : converted.tempos)
    {
        // A frame at a tempo of T lasts T units.
        perf.tempo.push_back(
            {TickOfUnits(units), 60.0 * kFrameRate * tempo / double(kDriverTicksPerQuarter * kTickUnits)});
    }
    for (int t = 0; t < kPlayerTracks; t++)
    {
        perf.track_names[size_t(t)] = "Track " + std::to_string(t);
    }

    perf.stretches = StretchesOf(rom, info, converted.reads, perf.base, converted.end);
    perf.mix = MixOf(rom, converted.reads, converted.end, SongEffect(info, song) != nullptr);

    std::array<std::optional<uint32_t>, kPlayerTracks> preset;
    for (const ConvertedNote& n : converted.notes)
    {
        std::vector<TrackEvent>& events = perf.tracks[size_t(n.track)];
        const uint32_t tick = TickOfUnits(n.units);
        const uint32_t number = uint32_t(n.bank) * 128 + uint32_t(n.instrument);
        if (preset[size_t(n.track)] != number)
        {
            preset[size_t(n.track)] = number;
            TrackEvent event;
            event.tick = tick;
            event.kind = EventKind::kPreset;
            event.preset = number;
            events.push_back(event);
        }

        NoteCommand command;
        command.offset = n.command - perf.base;
        command.stretch = StretchOf(perf.stretches[size_t(n.track)], command.offset, tick);
        const uint32_t id = uint32_t(perf.note_commands.size()) + 1;
        perf.note_commands.push_back(command);
        perf.midi_places.push_back({n.on_tick, n.off_tick});

        TrackEvent on;
        on.tick = tick;
        on.kind = EventKind::kNoteOn;
        on.key = uint8_t(n.key);
        on.value = uint8_t(n.velocity);
        on.note = id;
        events.push_back(on);

        TrackEvent off = on;
        off.tick = std::max(TickOfUnits(n.off_tick), tick + 1);
        off.kind = EventKind::kNoteOff;
        off.value = 0;
        events.push_back(off);
    }

    return perf;
}

} // namespace ntdlib::sequence_export::rd2
