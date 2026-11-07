// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/rare/performance.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "converted_notes.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/rare/commands.h"
#include "ntdlib/rare/driver.h"
#include "ntdlib/sequence_export/agb/settings.h"

namespace ntdlib::sequence_export::rare
{

using ntdlib::agb::Rom;
using ntdlib::rare::DriverInfo;
using ntdlib::rare::kTempoScale;

namespace
{

// The tracks that a performance holds.
constexpr size_t kTracks = 16;

// Sets the volume of the track's mix to `value` at `tick`. A later change in the same tick replaces an earlier one.
void AddChange(std::vector<MixChange>& changes, uint32_t tick, int value)
{
    if (!changes.empty() && changes.back().tick == tick)
    {
        changes.back().value = int16_t(value);
        return;
    }

    changes.push_back({tick, Mix::kVolume, int16_t(value)});
}

// The stretch of `stretches` that runs the command at `offset` by `tick`: the last that began by then and holds it.
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
    const ConvertedTune tune = ConvertedNotes(rom, info, song, settings);
    perf.warnings = tune.warnings;
    perf.truncated = tune.truncated;
    if (!tune.valid)
    {
        return perf;
    }

    perf.base = ntdlib::rare::SequenceAddress(rom, info, ntdlib::rare::SequenceOfSong(info, song));
    perf.timebase = tune.ticks_per_quarter;
    perf.seconds = tune.seconds;
    perf.end_tick = uint32_t(tune.end);
    if (tune.loops)
    {
        perf.loop = seqlib::midi::Performance::Loop{uint32_t(tune.loop_start), uint32_t(tune.loop_end)};
        perf.loops = settings.loops;
    }
    for (const auto& [tick, tempo] : tune.tempos)
    {
        if (tempo != 0)
        {
            perf.tempo.push_back({uint32_t(tick), 60000000.0 / (double(tempo) * kTempoScale)});
        }
    }
    for (size_t t = 0; t < kTracks; t++)
    {
        perf.track_names[t] = "Track " + std::to_string(t);
    }

    // The stretches that began before the end, and each track's first.
    for (const ntdlib::rare::TrackStretch& s : tune.stretches)
    {
        std::vector<Stretch>& stretches = perf.stretches[s.track];
        if (s.tick >= tune.end && !stretches.empty())
        {
            continue;
        }

        Stretch stretch;
        stretch.tick = uint32_t(s.tick);
        stretch.first = s.first - perf.base;
        stretch.last = s.last - perf.base;
        stretch.entry = s.loop ? Entry::kLoop : Entry::kOpen;
        stretches.push_back(stretch);
    }

    for (const ConvertedVolume& v : tune.volumes)
    {
        if (size_t(v.track) < kTracks)
        {
            AddChange(perf.mix[size_t(v.track)], uint32_t(v.tick), v.value);
        }
    }

    std::array<std::optional<uint32_t>, kTracks> preset;
    for (const ConvertedNote& n : tune.notes)
    {
        std::vector<TrackEvent>& events = perf.tracks[size_t(n.track)];
        const auto on_tick = uint32_t(n.on);
        const auto program = uint32_t(std::max(n.program, 0));
        if (preset[size_t(n.track)] != program)
        {
            preset[size_t(n.track)] = program;
            TrackEvent event;
            event.tick = on_tick;
            event.kind = EventKind::kPreset;
            event.preset = program;
            events.push_back(event);
        }

        NoteCommand command;
        command.offset = n.command - perf.base;
        command.stretch = StretchOf(perf.stretches[size_t(n.track)], command.offset, on_tick);
        const uint32_t id = uint32_t(perf.note_commands.size()) + 1;
        perf.note_commands.push_back(command);

        TrackEvent on;
        on.tick = on_tick;
        on.kind = EventKind::kNoteOn;
        on.key = uint8_t(n.key);
        on.value = uint8_t(n.velocity);
        on.note = id;
        events.push_back(on);

        TrackEvent off = on;
        off.tick = std::max(uint32_t(n.off), on_tick + 1);
        off.kind = EventKind::kNoteOff;
        off.value = 0;
        events.push_back(off);
    }

    // Each track's events in the order of their ticks.
    for (std::vector<TrackEvent>& events : perf.tracks)
    {
        std::stable_sort(events.begin(), events.end(),
                         [](const TrackEvent& a, const TrackEvent& b) { return a.tick < b.tick; });
    }

    return perf;
}

} // namespace ntdlib::sequence_export::rare
