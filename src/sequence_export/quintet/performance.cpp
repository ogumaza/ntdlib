// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/quintet/performance.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "converted_notes.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/quintet/commands.h"
#include "ntdlib/quintet/driver.h"
#include "ntdlib/sequence_export/agb/settings.h"

namespace ntdlib::sequence_export::quintet
{

using ntdlib::agb::Rom;
using ntdlib::quintet::DriverInfo;

namespace
{

// The names of the channels' tracks.
constexpr const char* kTrackNames[] = {"Square 1", "Square 2", "Wave", "Noise", "PCM A", "PCM B"};

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
    const ConvertedSong converted = ConvertedNotes(rom, info, song, settings);
    perf.warnings = converted.warnings;
    perf.truncated = converted.truncated;
    if (!converted.valid)
    {
        return perf;
    }

    perf.base = ntdlib::quintet::SongAddress(info, song) + ntdlib::quintet::kHeaderBytes;
    perf.timebase = converted.ticks_per_quarter;
    perf.seconds = converted.seconds;
    perf.end_tick = converted.end;
    if (converted.loops)
    {
        perf.loop = seqlib::midi::Performance::Loop{converted.loop_start, converted.loop_end};
        perf.loops = settings.loops;
    }
    for (const auto& [tick, micros] : converted.tempos)
    {
        perf.tempo.push_back({tick, 60000000.0 / double(micros)});
    }
    for (size_t t = 0; t < std::size(kTrackNames); t++)
    {
        perf.track_names[t] = kTrackNames[t];
    }

    // The stretches that began before the end, and each channel's first.
    for (const ConvertedStretch& s : converted.stretches)
    {
        std::vector<Stretch>& stretches = perf.stretches[size_t(s.channel)];
        if (s.tick >= converted.end && !stretches.empty())
        {
            continue;
        }

        Stretch stretch;
        stretch.tick = s.tick;
        stretch.first = s.first - perf.base;
        stretch.last = s.last - perf.base;
        stretch.entry = s.loop ? Entry::kLoop : Entry::kOpen;
        stretch.depth = uint8_t(s.depth);
        stretches.push_back(stretch);
    }

    for (const ConvertedVolume& v : converted.volumes)
    {
        AddChange(perf.mix[size_t(v.channel)], v.tick, v.value);
    }

    std::array<std::optional<uint32_t>, std::size(kTrackNames)> preset;
    for (const ConvertedNote& n : converted.notes)
    {
        std::vector<TrackEvent>& events = perf.tracks[size_t(n.channel)];
        const auto program = uint32_t(n.program);
        if (preset[size_t(n.channel)] != program)
        {
            preset[size_t(n.channel)] = program;
            TrackEvent event;
            event.tick = n.on;
            event.kind = EventKind::kPreset;
            event.preset = program;
            events.push_back(event);
        }

        NoteCommand command;
        command.offset = n.command - perf.base;
        command.stretch = StretchOf(perf.stretches[size_t(n.channel)], command.offset, n.on);
        const uint32_t id = uint32_t(perf.note_commands.size()) + 1;
        perf.note_commands.push_back(command);

        TrackEvent on;
        on.tick = n.on;
        on.kind = EventKind::kNoteOn;
        on.key = uint8_t(n.key);
        on.value = 127;
        on.note = id;
        events.push_back(on);

        TrackEvent off = on;
        off.tick = n.off;
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

} // namespace ntdlib::sequence_export::quintet
