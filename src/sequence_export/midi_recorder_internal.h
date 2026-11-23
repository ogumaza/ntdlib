// SPDX-License-Identifier: MIT

// Definitions that the parts of the MIDI recorder share (midi_recorder.cpp, midi_recorder_outputs.cpp and
// midi_recorder_notes.cpp): the recorder's rules and the helpers that read its tracks' events. The three files build
// as one translation unit. The helpers therefore keep internal linkage. Only those files include this header.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/midi/recorder.h"
#include "seqlib/sf2/units.h"

#include "ntdlib/nwsnd/envelope.h"
#include "ntdlib/sequence_export/envelope.h"
#include "ntdlib/sequence_export/filter.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/resampling.h"
#include "ntdlib/sequence_export/settings.h"
#include "sequence_export/performer_engine.h"
#include "sequence_export/performer_records.h"

namespace ntdlib::sequence_export
{

using seqlib::sf2::SoundFontPan;
using seqlib::sf2::ToTimecents;

namespace
{

// The performer's rules for recording MIDI (seqlib/midi/recorder.h). nw::snd gives a frame's notes the settings that
// the frame ends with. A setting therefore goes before every event of its tick but a sound off. A part switch drops the
// new part's last values. A pitch that moves 0.0005 semitones or less isn't sent.
seqlib::midi::RecorderOptions RecordingRules()
{
    seqlib::midi::RecorderOptions options;
    options.settings_before_all_events = true;
    options.pitch_tolerance = 0.0005f;

    return options;
}

// The last value of setting `kind` sent to the part that `output`'s track plays on, or -1 if none was.
int LastSent(const seqlib::midi::TrackOutput& output, EventKind kind)
{
    const auto sent = output.sent.find({output.part, kind});
    return sent != output.sent.end() ? sent->second : -1;
}

// The first of `events` at tick `tick`, or their end. The events of a tick come last.
std::vector<TrackEvent>::iterator TickStart(std::vector<TrackEvent>& events, uint32_t tick)
{
    auto first = events.end();
    while (first != events.begin() && std::prev(first)->tick == tick)
    {
        --first;
    }

    return first;
}

// The notes that the note-ons from `first` to `last` strike.
std::set<uint32_t> NotesStruck(std::vector<TrackEvent>::const_iterator first,
                               std::vector<TrackEvent>::const_iterator last)
{
    std::set<uint32_t> struck;
    for (auto it = first; it != last; ++it)
    {
        if (it->kind == EventKind::kNoteOn)
        {
            struck.insert(it->note);
        }
    }

    return struck;
}

// The preset of each part that a note-on of `events` from `first` on strikes, before `first`: its last preset event's
// preset, or -1 for none.
std::map<uint8_t, int> PresetsBefore(const std::vector<TrackEvent>& events,
                                     std::vector<TrackEvent>::const_iterator first)
{
    std::map<uint8_t, int> sent;
    for (auto it = first; it != events.end(); ++it)
    {
        if (it->kind == EventKind::kNoteOn && !sent.contains(it->part))
        {
            const uint8_t part = it->part;
            auto is_part_preset = [part](const TrackEvent& e)
            {
                return e.kind == EventKind::kPreset && e.part == part;
            };
            const auto back = std::find_if(std::make_reverse_iterator(first), events.rend(), is_part_preset);
            sent[part] = back != events.rend() ? static_cast<int>(back->preset) : -1;
        }
    }

    return sent;
}

} // namespace

} // namespace ntdlib::sequence_export
