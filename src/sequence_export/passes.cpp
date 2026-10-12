// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/passes.h"

#include <algorithm>
#include <cstddef>
#include <set>

#include "ntdlib/sequence_export/filter.h"

namespace ntdlib::sequence_export
{

// True for controller events that update a track's channel settings.
bool IsTrackSetting(EventKind kind)
{
    switch (kind)
    {
    case EventKind::kVolume:
    case EventKind::kExpression:
    case EventKind::kPan:
    case EventKind::kPitch:
    case EventKind::kModulation:
    case EventKind::kReverb:
    case EventKind::kChorus:
        return true;
    default:
        return kind == kFilterEvent || kind == kSfzLowPassEvent || kind == kSfzBiquadEvent;
    }
}

std::map<SettingKey, TrackEvent> SettingsAt(const seqlib::midi::Performance& perf, uint32_t tick)
{
    // A track's first part starts at its key's pitch without an event (seqlib::midi::TrackOutput::pitch).
    std::map<SettingKey, TrackEvent> settings;
    for (int t = 0; t < 16; t++)
    {
        settings[{t, 0, EventKind::kPitch}] = TrackEvent{0, EventKind::kPitch};
        for (const TrackEvent& e : perf.tracks[t])
        {
            if (e.tick > tick || (e.tick == tick && e.after > 0))
            {
                break;
            }

            if (IsTrackSetting(e.kind) || e.kind == EventKind::kPreset)
            {
                settings[{t, e.part, e.kind}] = e;
            }
        }
    }

    return settings;
}

PassStart PassStartAt(const seqlib::midi::Performance& perf, const std::vector<PartSwitch>& part_switches,
                      uint32_t tick)
{
    PassStart start;
    start.settings = SettingsAt(perf, tick);

    for (const seqlib::midi::Performance::Tempo& tempo : perf.tempo)
    {
        if (tempo.tick > tick)
        {
            break;
        }

        start.bpm = tempo.bpm;
    }

    for (const auto& [switch_tick, t, part] : part_switches)
    {
        if (switch_tick > tick)
        {
            break;
        }

        start.parts[static_cast<std::size_t>(t)] = part;
    }

    return start;
}

bool SameSetting(const TrackEvent& a, const TrackEvent& b)
{
    switch (a.kind)
    {
    case EventKind::kPitch:
        return a.semitones == b.semitones;
    case EventKind::kPreset:
        return a.preset == b.preset;
    default:
        return a.value == b.value;
    }
}

bool RepeatsAsTheGame(const seqlib::midi::Performance& perf, const std::vector<PartSwitch>& part_switches,
                      uint32_t start, uint32_t end, const LoopRules& rules)
{
    const PassStart at_start = PassStartAt(perf, part_switches, start);
    const PassStart next = PassStartAt(perf, part_switches, end);
    const uint32_t length = end - start;

    // The tempo: a repeat starts with the tempo at the loop's end. A tempo event on the loop's first tick replaces that
    // with the start's tempo. When the game's next pass starts with the start's tempo, RestoreAtLoopStart sends that
    // tempo again, and the repeat starts with it too.
    double end_bpm = at_start.bpm;
    bool tempo_sent = false;
    for (const seqlib::midi::Performance::Tempo& tempo : perf.tempo)
    {
        tempo_sent = tempo_sent || tempo.tick == start;
        if (tempo.tick < end)
        {
            end_bpm = tempo.bpm;
        }
    }
    const double repeat_bpm = tempo_sent || next.bpm == at_start.bpm ? at_start.bpm : end_bpm;

    auto bpm_at = [&](uint32_t tick, uint32_t since, double otherwise)
    {
        double bpm = otherwise;
        for (const seqlib::midi::Performance::Tempo& tempo : perf.tempo)
        {
            if (tempo.tick > tick)
            {
                break;
            }

            if (tempo.tick >= since)
            {
                bpm = tempo.bpm;
            }
        }

        return bpm;
    };

    // A note of a pass: its tick in the pass, key, velocity and part, and the settings of its part that it plays with.
    // For a note of the loop, those are the settings that the pass sends before the note. The others come from the
    // point that a MIDI player loops from.
    struct Strike
    {
        uint32_t offset = 0;
        uint8_t key = 0;
        uint8_t velocity = 0;
        uint8_t part = 0;
        std::map<EventKind, TrackEvent> settings;
    };

    for (int t = 0; t < 16; t++)
    {
        const auto track = static_cast<std::size_t>(t);
        std::vector<Strike> loop;
        std::vector<Strike> game;
        std::array<std::map<EventKind, TrackEvent>, 16> settings{}; // each part's settings so far in the events
        std::array<std::map<EventKind, TrackEvent>, 16> sent{};     // each part's settings sent since the loop's start
        std::array<std::map<EventKind, TrackEvent>, 16> at_end{};   // each part's settings at the loop's end
        std::array<std::set<EventKind>, 16> in_first_tick{};        // each part's settings sent at the loop's start

        // A track's first part starts at its key's pitch, and with the settings of `rules.seeded` at 0, without events
        // (seqlib::midi::TrackOutput::pitch, and the SFZ filters that MidiRecorder's constructor seeds in
        // performer.cpp).
        settings[0][EventKind::kPitch] = TrackEvent{0, EventKind::kPitch};
        for (const EventKind kind : rules.seeded)
        {
            settings[0][kind] = TrackEvent{0, kind};
        }

        bool ended = false;
        for (const TrackEvent& e : perf.tracks[track])
        {
            if (e.tick >= end + length)
            {
                break;
            }
            if (e.part >= 16)
            {
                continue;
            }

            if (!ended && e.tick >= end)
            {
                at_end = settings;
                ended = true;
            }

            if (IsTrackSetting(e.kind) || e.kind == EventKind::kPreset)
            {
                settings[e.part][e.kind] = e;
                if (e.tick >= start)
                {
                    sent[e.part][e.kind] = e;
                }
                if (e.tick == start && e.after == 0)
                {
                    in_first_tick[e.part].insert(e.kind);
                }
            }
            else if (e.kind == EventKind::kNoteOn && e.tick >= start)
            {
                const bool in_loop = e.tick < end;
                (in_loop ? loop : game)
                    .push_back({e.tick - (in_loop ? start : end), e.key, e.value, e.part,
                                in_loop ? sent[e.part] : settings[e.part]});
            }
        }

        if (!ended)
        {
            at_end = settings;
        }

        if (loop.size() != game.size())
        {
            return false;
        }

        // What a repeat starts with on a part where the pass hasn't sent a setting: the loop end's value, or the loop
        // start's value where RestoreAtLoopStart sends it again. On the part that takes the track's new notes,
        // RestoreAtLoopStart sends it if the loop's first tick doesn't.
        auto repeat_value = [&](uint8_t part, EventKind kind) -> const TrackEvent*
        {
            const auto first = at_start.settings.find({t, part, kind});
            const auto again = next.settings.find({t, next.parts[track], kind});
            if (part == at_start.parts[track] && !in_first_tick[part].contains(kind) &&
                first != at_start.settings.end() && again != next.settings.end() &&
                SameSetting(first->second, again->second))
            {
                return &first->second;
            }

            const auto kept = at_end[part].find(kind);
            return kept != at_end[part].end() ? &kept->second : nullptr;
        };

        for (std::size_t k = 0; k < loop.size(); k++)
        {
            const Strike& a = loop[k];
            const Strike& b = game[k];
            if (a.offset != b.offset || a.key != b.key || a.velocity != b.velocity ||
                bpm_at(start + a.offset, start, repeat_bpm) != bpm_at(end + b.offset, 0, at_start.bpm))
            {
                return false;
            }

            std::set<EventKind> kinds;
            const std::map<EventKind, TrackEvent>& kept = at_end[a.part];
            for (const auto* list : {&a.settings, &b.settings, &kept})
            {
                for (const auto& [kind, e] : *list)
                {
                    kinds.insert(kind);
                }
            }

            for (EventKind kind : kinds)
            {
                const auto sent_before = a.settings.find(kind);
                const TrackEvent* repeat =
                    sent_before != a.settings.end() ? &sent_before->second : repeat_value(a.part, kind);
                const auto played = b.settings.find(kind);
                if ((repeat == nullptr) != (played == b.settings.end()) ||
                    (repeat && !SameSetting(*repeat, played->second)))
                {
                    return false;
                }
            }
        }
    }

    return true;
}

void RestoreAtLoopStart(seqlib::midi::Performance& perf, const std::vector<PartSwitch>& part_switches,
                        const PassStart& next_pass, const LoopRules& rules)
{
    const uint32_t start = perf.loop->start;
    const uint32_t end = perf.loop->end;

    // The tempo is handled like the settings below. The tempo's changes are in one list, sorted by tick.
    const PassStart at_loop_start = PassStartAt(perf, part_switches, start);
    auto tempo_at = [&](uint32_t tick, bool in_tick)
    {
        double bpm = at_loop_start.bpm;
        for (const seqlib::midi::Performance::Tempo& tempo : perf.tempo)
        {
            if (tempo.tick > start && (tempo.tick < tick || (in_tick && tempo.tick == tick)))
            {
                bpm = tempo.bpm;
            }
        }

        return bpm;
    };

    const bool tempo_sent = std::ranges::any_of(perf.tempo, [start](const auto& tempo) { return tempo.tick == start; });
    if (!tempo_sent && next_pass.bpm == at_loop_start.bpm &&
        (tempo_at(end, false) != at_loop_start.bpm || tempo_at(end, true) != at_loop_start.bpm))
    {
        auto later = [start](const seqlib::midi::Performance::Tempo& tempo)
        {
            return tempo.tick > start;
        };
        perf.tempo.insert(std::ranges::find_if(perf.tempo, later), {start, at_loop_start.bpm});
    }

    for (const auto& [key, at_start] : at_loop_start.settings)
    {
        // The game gives the setting back its value at the loop's start. Only the part that the track's new notes go to
        // counts. A track sends every setting again when it moves to another part (Switch).
        const auto [t, part, kind] = key;
        const auto track = static_cast<std::size_t>(t);
        if (part != at_loop_start.parts[track])
        {
            continue;
        }
        const auto next = next_pass.settings.find({t, next_pass.parts[track], kind});
        if (next == next_pass.settings.end() || !SameSetting(next->second, at_start))
        {
            continue;
        }

        // It isn't sent at the loop's start, and the loop ends with another value. A MIDI player may take the loop
        // end's tick or leave it out. Both count.
        std::vector<TrackEvent>& events = perf.tracks[track];
        bool sent = false;
        const TrackEvent* before_end = &at_start;
        const TrackEvent* at_end = &at_start;
        for (const TrackEvent& e : events)
        {
            if (e.part != part || e.kind != kind || e.tick < start || e.tick > end || (e.tick == end && e.after > 0))
            {
                continue;
            }

            if (e.tick == start && e.after == 0)
            {
                sent = true;
            }
            if (e.tick < end)
            {
                before_end = &e;
            }
            at_end = &e;
        }
        if (sent || (SameSetting(*before_end, at_start) && SameSetting(*at_end, at_start)))
        {
            continue;
        }

        // Send it with the loop start's other settings, ahead of its notes. With `rules.preset_with_settings`, it goes
        // after the presets there too.
        auto past_settings = [start, &rules](const TrackEvent& e)
        {
            const bool setting = IsTrackSetting(e.kind) || (rules.preset_with_settings && e.kind == EventKind::kPreset);
            return e.tick > start || (e.tick == start && !setting);
        };
        const auto at = std::find_if(events.begin(), events.end(), past_settings);
        TrackEvent e = at_start;
        e.tick = start;
        e.after = 0;
        events.insert(at, e);
    }
}

// Scale factor that keeps rounded CC11 values within 127 for a maximum unscaled value of `peak` (Perform).
double LevelScale(double peak)
{
    return peak >= 127.5 ? 127.0 / peak : 1.0;
}

// Fits the parts that each track of `perf` used (Performer::PartsUsed) into MIDI's 16 channels. Gives them out as the
// MIDI writer does: a channel for the first part of each track that plays notes, then the other parts, track by track.
// Returns the parts each track may have.
std::array<uint8_t, 16> FitParts(const seqlib::midi::Performance& perf, const std::array<uint8_t, 16>& used)
{
    std::array<bool, 16> plays{};
    int channels = 0;
    for (int t = 0; t < 16; t++)
    {
        auto note_on = [](const TrackEvent& e)
        {
            return e.kind == EventKind::kNoteOn;
        };
        plays[t] = std::ranges::any_of(perf.tracks[t], note_on);
        channels += plays[t] ? 1 : 0;
    }

    std::array<uint8_t, 16> limits;
    limits.fill(1);
    for (int t = 0; t < 16; t++)
    {
        if (plays[t])
        {
            const int extra = std::min(std::max<int>(used[t], 1) - 1, 16 - channels);
            limits[t] = static_cast<uint8_t>(1 + extra);
            channels += extra;
        }
    }

    return limits;
}

} // namespace ntdlib::sequence_export
