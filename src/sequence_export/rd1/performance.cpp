// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/rd1/performance.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "converted_notes.h"
#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/rd1/commands.h"
#include "ntdlib/rd1/driver.h"
#include "ntdlib/rd1/sequencer.h"
#include "ntdlib/sequence_export/agb/settings.h"

namespace ntdlib::sequence_export::rd1
{

using ntdlib::agb::kFrameRate;
using ntdlib::agb::Rom;
using ntdlib::rd1::DriverInfo;
using ntdlib::rd1::kMaxTracks;
using ntdlib::rd1::TrackRead;

namespace
{

constexpr int kControlStatus = 0xB0;
constexpr int kVolumeController = 7;
constexpr int kPanController = 10;

// CC10's value at the centre.
constexpr int kCentre = 64;

using TrackStretches = std::array<std::vector<Stretch>, kMaxTracks>;
using TrackMix = std::array<std::vector<MixChange>, kMaxTracks>;

// The stretches of each track's reads. A stretch begins at a track's first read, and at each read that doesn't follow
// the one before it: the loop took the track back. At the song's end, a read only finishes the stretch that runs
// there. A stretch that would begin there belongs to the next pass.
TrackStretches StretchesOf(const std::vector<TrackRead>& reads, uint32_t base, uint64_t end)
{
    TrackStretches stretches;
    std::array<const TrackRead*, kMaxTracks> last = {};
    for (const TrackRead& read : reads)
    {
        if (read.tick > end || read.track < 0 || read.track >= kMaxTracks)
        {
            continue;
        }

        const auto t = size_t(read.track);
        const TrackRead* before = last[t];
        const bool follows = before && read.address == before->next;
        if (!follows && read.tick == end)
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
        stretch.tick = uint32_t(read.tick);
        stretch.first = read.address - base;
        stretch.last = stretch.first;
        stretch.entry = before ? Entry::kLoop : Entry::kOpen;
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

// Each track's changes of its volume and pan, from the controllers that it read before the song's end. An event that
// leaves out its status byte starts with its first data byte.
TrackMix MixOf(const Rom& rom, const std::vector<TrackRead>& reads, uint64_t end)
{
    TrackMix mix;
    for (const TrackRead& read : reads)
    {
        if (read.tick >= end || read.track < 0 || read.track >= kMaxTracks || (read.status & 0xF0) != kControlStatus)
        {
            continue;
        }

        const uint32_t data = read.address + ((rom.U8(read.address) & 0x80) ? 1 : 0);
        const int controller = rom.U8(data);
        const int value = rom.U8(data + 1) & 0x7F;
        std::vector<MixChange>& changes = mix[size_t(read.track)];
        if (controller == kVolumeController)
        {
            AddChange(changes, uint32_t(read.tick), Mix::kVolume, value);
        }
        else if (controller == kPanController)
        {
            AddChange(changes, uint32_t(read.tick), Mix::kPan, value - kCentre);
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
    if (!converted.valid)
    {
        return perf;
    }

    perf.base = converted.midi;
    perf.seconds = converted.seconds;
    perf.end_tick = uint32_t(converted.end);
    perf.midi_end_tick = converted.end_tick;
    if (converted.loops)
    {
        perf.loop = seqlib::midi::Performance::Loop{uint32_t(converted.loop_start), uint32_t(converted.loop_end)};
        perf.loops = settings.loops;
    }

    perf.timebase = uint32_t(converted.division);
    bool first = true;
    for (const auto& [tick, step] : converted.tempos)
    {
        // A frame moves the song on by `step` 256ths of a tick. The first tempo goes at the start. The MIDI file puts
        // it there too.
        if (tick < converted.end || first)
        {
            perf.tempo.push_back(
                {first ? 0 : uint32_t(tick), 60.0 * kFrameRate * double(step) / (256.0 * double(converted.division))});
        }
        first = false;
    }

    for (int c = 0; c < 16; c++)
    {
        perf.track_names[size_t(c)] = "Channel " + std::to_string(c);
    }

    perf.stretches = StretchesOf(converted.reads, perf.base, converted.end);
    perf.mix = MixOf(rom, converted.reads, converted.end);

    std::array<std::optional<uint32_t>, 16> preset;
    for (const ConvertedNote& n : converted.notes)
    {
        const auto c = size_t(n.channel & 15);
        std::vector<TrackEvent>& events = perf.tracks[c];
        const auto tick = uint32_t(n.on);
        const uint32_t number = uint32_t(converted.bank) * 128 + uint32_t(n.program);
        if (preset[c] != number)
        {
            preset[c] = number;
            TrackEvent event;
            event.tick = tick;
            event.kind = EventKind::kPreset;
            event.preset = number;
            events.push_back(event);
        }

        NoteCommand command;
        command.offset = n.command - perf.base;
        if (n.track >= 0 && n.track < kMaxTracks)
        {
            command.stretch = StretchOf(perf.stretches[size_t(n.track)], command.offset, tick);
        }
        const uint32_t id = uint32_t(perf.note_commands.size()) + 1;
        perf.note_commands.push_back(command);
        perf.note_tracks.push_back(uint8_t(n.track));
        perf.midi_places.push_back({n.on_tick, n.off_tick});

        TrackEvent on;
        on.tick = tick;
        on.kind = EventKind::kNoteOn;
        on.key = uint8_t(n.key);
        on.value = uint8_t(n.velocity);
        on.note = id;
        events.push_back(on);

        TrackEvent off = on;
        off.tick = std::max(uint32_t(n.off), tick + 1);
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

} // namespace ntdlib::sequence_export::rd1
