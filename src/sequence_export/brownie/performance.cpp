// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/brownie/performance.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "converted_notes.h"
#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/brownie/commands.h"
#include "ntdlib/brownie/driver.h"
#include "ntdlib/brownie/sequencer.h"
#include "ntdlib/brownie/song.h"
#include "ntdlib/sequence_export/agb/settings.h"

namespace ntdlib::sequence_export::brownie
{

using ntdlib::agb::kFrameRate;
using ntdlib::agb::Rom;
using ntdlib::brownie::ChannelName;
using ntdlib::brownie::ChannelRead;
using ntdlib::brownie::CommandOf;
using ntdlib::brownie::CommandSize;
using ntdlib::brownie::DriverInfo;
using ntdlib::brownie::kChannelCount;
using ntdlib::brownie::kFirstSampleChannel;
using ntdlib::brownie::LayoutOf;
using ntdlib::brownie::Region;
using ntdlib::brownie::Revision;
using ntdlib::brownie::SongAddress;

namespace
{

// The commands that a stretch's first read can follow.
constexpr uint8_t kRepeatEnd = 0xF5;
constexpr uint8_t kCall = 0xF6;
constexpr uint8_t kReturn = 0xF7;

// The commands that set a channel's mix.
constexpr uint8_t kSamplePan = 0xEA;
constexpr uint8_t kVolume = 0xF2;
constexpr uint8_t kPsgPan = 0xFD;

// Returns the track name of channel `c` of revision `r`, as the MIDI file gives it.
std::string TrackNameOf(Revision r, int c)
{
    std::string name = ChannelName(r, c);
    name[0] = char(name[0] - 'a' + 'A');

    return name;
}

// The offsets of the region that holds a sound's list count from this address.
uint32_t BaseOf(const Rom& rom, const DriverInfo& info, int song)
{
    const uint32_t list = SongAddress(rom, info, song);
    for (const Region& region : ntdlib::brownie::FindRegions(rom, info))
    {
        if (list >= region.start && list < region.start + region.size)
        {
            return region.start;
        }
    }

    return list;
}

// The stretches of each channel's reads. A stretch begins at a channel's first read, and at each read that doesn't
// follow the one before it. The command before it then says how the stretch began. In the sound's last frame, a read
// only finishes the stretch that runs there: a stretch that would begin there belongs to the next pass.
std::array<std::vector<Stretch>, 16> StretchesOf(const Rom& rom, const DriverInfo& info,
                                                 const std::vector<ChannelRead>& reads, uint32_t base, uint32_t end)
{
    std::array<std::vector<Stretch>, 16> stretches;
    std::array<std::optional<uint32_t>, kChannelCount> last;
    std::array<uint8_t, kChannelCount> depth = {};
    for (const ChannelRead& read : reads)
    {
        if (read.frame > end)
        {
            continue;
        }

        const size_t c = read.channel;
        const std::optional<uint32_t> before = last[c];
        uint8_t command = 0;
        bool follows = false;
        if (before)
        {
            const uint8_t byte = rom.U8(*before);
            command = CommandOf(info.revision, byte);
            follows = read.address == *before + CommandSize(info.revision, byte);
        }
        if (!follows && read.frame == end)
        {
            continue;
        }

        depth[c] = command == kCall ? 1 : command == kReturn ? 0 : depth[c];
        last[c] = read.address;
        const uint32_t offset = read.address - base;
        if (follows)
        {
            stretches[c].back().last = offset;
            continue;
        }

        Stretch stretch;
        stretch.tick = read.frame;
        stretch.first = offset;
        stretch.last = offset;
        stretch.entry = !before                 ? Entry::kOpen
                        : command == kCall      ? Entry::kCall
                        : command == kReturn    ? Entry::kReturn
                        : command == kRepeatEnd ? Entry::kLoop
                                                : Entry::kJump;
        stretch.depth = depth[c];
        stretches[c].push_back(stretch);
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

// The pan from -64 to 63 of a channel's levels on the left and the right, each from 0 to 15.
int PanOfLevels(int left, int right)
{
    const int difference = right - left;
    return int(std::lround(double(difference) * (difference < 0 ? 64 : 63) / 15));
}

// Each channel's changes of its volume and pan, from the commands that ran before `end`.
std::array<std::vector<MixChange>, 16> MixOf(const Rom& rom, const DriverInfo& info,
                                             const std::vector<ChannelRead>& reads, uint32_t end)
{
    std::array<std::vector<MixChange>, 16> mix;
    for (const ChannelRead& read : reads)
    {
        if (read.frame >= end)
        {
            continue;
        }

        std::vector<MixChange>& changes = mix[read.channel];
        const uint8_t byte = rom.U8(read.address);
        if (byte < 0xE0)
        {
            continue;
        }

        const uint8_t value = rom.U8(read.address + 1);
        switch (CommandOf(info.revision, byte))
        {
        case kVolume:
            AddChange(changes, read.frame, Mix::kVolume, value);
            break;

        case kSamplePan:
            // SetVoiceLevels and a note read these levels only in the Sword of Mana revision.
            if (read.channel >= kFirstSampleChannel && info.revision != Revision::kMagicalVacation)
            {
                AddChange(changes, read.frame, Mix::kPan, PanOfLevels(value >> 4, value & 15));
            }
            break;

        case kPsgPan:
            // The driver shifts the byte's first bits of NR51 to the channel's.
            if (read.channel < kFirstSampleChannel)
            {
                AddChange(changes, read.frame, Mix::kPan, PanOfLevels(((value >> 4) & 1) * 15, (value & 1) * 15));
            }
            break;

        default:
            break;
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
    if (converted.end_frame == 0 && converted.reads.empty())
    {
        return perf;
    }

    perf.base = BaseOf(rom, info, song);
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
    for (int c = 0; c < LayoutOf(info.revision).channels; c++)
    {
        perf.track_names[size_t(c)] = TrackNameOf(info.revision, c);
    }

    perf.stretches = StretchesOf(rom, info, converted.reads, perf.base, converted.end_frame);
    perf.mix = MixOf(rom, info, converted.reads, converted.end_frame);

    std::array<std::optional<uint32_t>, kChannelCount> preset;
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

        NoteCommand command;
        command.offset = n.command - perf.base;
        command.stretch = StretchOf(perf.stretches[size_t(n.track)], command.offset, n.command_frame);
        const uint32_t id = uint32_t(perf.note_commands.size()) + 1;
        perf.note_commands.push_back(command);
        perf.note_causes.push_back(n.on_frame == n.command_frame ? NoteCause::kNote : NoteCause::kEarlier);
        perf.midi_places.push_back({n.on_tick, n.off_tick});

        TrackEvent on;
        on.tick = n.on_frame;
        on.kind = EventKind::kNoteOn;
        on.key = uint8_t(n.key);
        on.value = 127;
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

} // namespace ntdlib::sequence_export::brownie
