// SPDX-License-Identifier: MIT

#include "sequence_export/ntr/stands.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include "seqlib/midi/performance.h"
#include "seqlib/midi/recorder.h"

#include "ntdlib/sequence_export/passes.h"

namespace ntdlib::sequence_export::ntr
{

namespace
{

using ntdlib::sequence_export::PartSwitch;
using ntdlib::sequence_export::PassStart;
using seqlib::midi::EventKind;
using seqlib::midi::TrackEvent;

// The index of an LFO setting in Strikes::stands.
std::size_t StandIndex(EventKind kind)
{
    switch (kind)
    {
    case EventKind::kPan:
        return 0;
    case EventKind::kExpression:
        return 1;
    default:
        return 2;
    }
}

// The events of one LFO setting on one part of a track in the pass from `start` to `end`: the last before `end`, and
// the first of the pass, by tick and `after`.
struct PassEvents
{
    const TrackEvent* last = nullptr;
    const TrackEvent* first = nullptr;
};

// Whether a note that strikes at `strike` comes after `e`: in a later tick, or in the same tick with `e` at the tick's
// start.
bool StrikesAfter(const TrackEvent& strike, const TrackEvent& e)
{
    return e.tick < strike.tick || (e.tick == strike.tick && e.after == 0.0);
}

// Whether every note of `part` of `track` that strikes in the pass before its first event of `kind` plays as the game's
// note with the loop end's value `kept` (KeptValuesRepeat). `kept_is_start` says whether `kept` is the raw value at the
// pass's start. With `excuse`, a note whose twin got no channel counts as no note.
bool EarlyStrikesRepeat(const std::vector<TrackEvent>& events, const Strikes& strikes, int track, uint8_t part,
                        EventKind kind, const PassEvents& pass, uint32_t start, uint32_t end, bool kept_is_start,
                        bool excuse)
{
    const uint8_t kept = pass.last->value;
    for (const TrackEvent& e : events)
    {
        if (e.kind != EventKind::kNoteOn || e.part != part || e.tick < start || e.tick >= end)
        {
            continue;
        }
        if (pass.first != nullptr && StrikesAfter(e, *pass.first))
        {
            continue;
        }

        // A note that follows a pan LFO plays the course that the pass writes for it. On the start's raw value, it
        // starts as in the first play.
        const bool follows = strikes.follows.contains({track, e.tick, part});
        if (follows && kept_is_start)
        {
            continue;
        }

        // A note whose twin in the game's next pass got no channel counts as no strike (RepeatsAsTheGame).
        if (excuse && strikes.unstarted.contains({track, e.tick + (end - start), e.key}))
        {
            continue;
        }

        const auto game = strikes.stands.find({track, e.tick + (end - start)});
        if (follows || game == strikes.stands.end() || game->second[StandIndex(kind)] != kept)
        {
            return false;
        }
    }

    return true;
}

} // namespace

bool IsLfoSetting(EventKind kind)
{
    return kind == EventKind::kPan || kind == EventKind::kExpression || kind == EventKind::kModulation;
}

seqlib::midi::Performance TestList(const seqlib::midi::Performance& perf, const StandLists& stands)
{
    // Within a tick: the stands at the tick's start, the performance's events, then the other stands. The performance's
    // events go by time, and at one time the settings come first. The file has the events in the recorded order, and a
    // player applies the settings of one time together, before it plays the notes. The recorder can put a setting
    // after a NoteOn of its time, behind a NoteOff or a CC120 of another part. A program change, a NoteOn, a NoteOff
    // and a CC120 keep their recorded order: a program change counts only for the NoteOns after it.
    struct Placed
    {
        uint32_t tick = 0;
        int group = 0;
        double after = 0.0;
        bool other = false; // not a setting
        TrackEvent event;
    };

    seqlib::midi::Performance test = perf;
    for (std::size_t t = 0; t < test.tracks.size(); t++)
    {
        std::vector<Placed> placed;
        for (const TrackEvent& e : perf.tracks[t])
        {
            if (!IsLfoSetting(e.kind))
            {
                placed.push_back({e.tick, 1, e.after, !seqlib::midi::IsSetting(e.kind), e});
            }
        }
        if (t < stands.size())
        {
            for (const TrackEvent& e : stands[t])
            {
                placed.push_back({e.tick, e.after == 0.0 ? 0 : 2, 0.0, false, e});
            }
        }
        std::ranges::stable_sort(placed, {},
                                 [](const Placed& p) { return std::tuple(p.tick, p.group, p.after, p.other); });

        std::vector<TrackEvent>& events = test.tracks[t];
        events.clear();
        events.reserve(placed.size());
        for (const Placed& p : placed)
        {
            events.push_back(p.event);
        }
    }

    return test;
}

bool StandReturns(const seqlib::midi::Performance& test, const PassStart& at_start, const PassStart* next, uint32_t end,
                  int track, uint8_t part, EventKind kind)
{
    const auto stood = at_start.settings.find({track, part, kind});
    if (stood == at_start.settings.end())
    {
        return false;
    }

    // The part stands for the start's value at the loop's end.
    const TrackEvent* last = nullptr;
    for (const TrackEvent& e : test.tracks[static_cast<std::size_t>(track)])
    {
        if (e.tick < end && e.part == part && e.kind == kind)
        {
            last = &e;
        }
    }
    if (last != nullptr && last->value == stood->second.value)
    {
        return true;
    }

    // The game sets the stand back on the part that takes the new notes.
    const auto t = static_cast<std::size_t>(track);
    if (next == nullptr || part != at_start.parts[t])
    {
        return false;
    }
    const auto again = next->settings.find({track, next->parts[t], kind});

    return again != next->settings.end() && again->second.value == stood->second.value;
}

bool KeptValuesRepeat(const seqlib::midi::Performance& played, const seqlib::midi::Performance& test,
                      const std::vector<PartSwitch>& switches, const Strikes& strikes, uint32_t start, uint32_t end,
                      bool excuse)
{
    const PassStart at_start = PassStartAt(played, switches, start);
    const PassStart test_start = PassStartAt(test, switches, start);
    const PassStart test_next = PassStartAt(test, switches, end);
    for (int track = 0; track < static_cast<int>(played.tracks.size()); track++)
    {
        // Each LFO setting's last event before the loop's end and first event of the pass, by part.
        const std::vector<TrackEvent>& events = played.tracks[static_cast<std::size_t>(track)];
        std::map<std::pair<uint8_t, EventKind>, PassEvents> on_part;
        for (const TrackEvent& e : events)
        {
            if (!IsLfoSetting(e.kind) || e.tick >= end)
            {
                continue;
            }

            PassEvents& pass = on_part[{e.part, e.kind}];
            if (pass.last == nullptr || std::pair(e.tick, e.after) >= std::pair(pass.last->tick, pass.last->after))
            {
                pass.last = &e;
            }
            if (e.tick >= start &&
                (pass.first == nullptr || std::pair(e.tick, e.after) < std::pair(pass.first->tick, pass.first->after)))
            {
                pass.first = &e;
            }
        }

        // A value that the loop's end leaves and that the loop's start doesn't send again.
        for (const auto& [place, pass] : on_part)
        {
            const auto [part, kind] = place;
            const auto at = at_start.settings.find({track, part, kind});
            if (at == at_start.settings.end() || StandReturns(test, test_start, &test_next, end, track, part, kind))
            {
                continue;
            }
            if (!EarlyStrikesRepeat(events, strikes, track, part, kind, pass, start, end,
                                    pass.last->value == at->second.value, excuse))
            {
                return false;
            }
        }
    }

    return true;
}

} // namespace ntdlib::sequence_export::ntr
