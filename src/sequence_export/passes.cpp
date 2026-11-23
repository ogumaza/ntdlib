// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/passes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "seqlib/midi/performance.h"

#include "ntdlib/sequence_export/filter.h"
#include "ntdlib/sequence_export/performance_base.h"

namespace ntdlib::sequence_export
{

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
    // Each part starts at its key's pitch without an event. The recorder sends a part no pitch of 0 before it has sent
    // the part another pitch (seqlib::midi::TrackOutput::pitch). The pitch names its part for LoopRules::same_pitch.
    std::map<SettingKey, TrackEvent> settings;
    for (int t = 0; t < 16; t++)
    {
        for (uint8_t part = 0; part < 16; part++)
        {
            settings[{t, part, EventKind::kPitch}] = TrackEvent{.kind = EventKind::kPitch, .part = part};
        }
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

namespace
{

// A note of a pass: its tick in the pass, key, velocity and part, and the settings of its part that it plays with. For
// a note of the loop, those are the settings that the pass sends before the note. The others come from the point that
// a MIDI player loops from.
struct Strike
{
    uint32_t offset = 0;
    uint8_t key = 0;
    uint8_t velocity = 0;
    uint8_t part = 0;
    std::map<EventKind, TrackEvent> settings;
};

// A track's notes in the loop and in the game's next pass.
struct TrackPasses
{
    std::vector<Strike> loop;
    std::vector<Strike> game;
    std::array<std::map<EventKind, TrackEvent>, 16> at_end{}; // each part's settings at the loop's end
    std::array<std::set<EventKind>, 16> in_first_tick{};      // each part's settings sent at the loop's start
};

// The notes of `track` in the pass from `start` to `end` and in the pass after it.
TrackPasses PassesOfTrack(const seqlib::midi::Performance& perf, std::size_t track, uint32_t start, uint32_t end,
                          const LoopRules& rules)
{
    const uint32_t length = end - start;
    TrackPasses passes;
    std::array<std::map<EventKind, TrackEvent>, 16> settings{}; // each part's settings so far in the events
    std::array<std::map<EventKind, TrackEvent>, 16> sent{};     // each part's settings sent since the loop's start

    // Each part starts at its key's pitch without an event (seqlib::midi::TrackOutput::pitch, as in SettingsAt). A
    // track's first part also starts with the settings of `rules.seeded` at 0, without events (the SFZ filters that
    // MidiRecorder's constructor seeds in midi_recorder.cpp).
    for (uint8_t part = 0; part < settings.size(); part++)
    {
        settings[part][EventKind::kPitch] = TrackEvent{.kind = EventKind::kPitch, .part = part};
    }
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
            passes.at_end = settings;
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
                passes.in_first_tick[e.part].insert(e.kind);
            }
        }
        else if (e.kind == EventKind::kNoteOn && e.tick >= start)
        {
            const bool in_loop = e.tick < end;
            (in_loop ? passes.loop : passes.game)
                .push_back({e.tick - (in_loop ? start : end), e.key, e.value, e.part,
                            in_loop ? sent[e.part] : settings[e.part]});
        }
    }

    if (!ended)
    {
        passes.at_end = std::move(settings);
    }

    return passes;
}

// The tempo that a repeat of the loop from `start` to `end` starts with (RepeatsAsTheGame). `at_start` gives the pass
// at the loop's start, and `next` the game's next pass.
double RepeatBpm(const seqlib::midi::Performance& perf, uint32_t start, uint32_t end, const PassStart& at_start,
                 const PassStart& next)
{
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

    return tempo_sent || next.bpm == at_start.bpm ? at_start.bpm : end_bpm;
}

// The tempo at `tick`: that of the last tempo event at `since` or later, or else `otherwise`.
double BpmAt(const seqlib::midi::Performance& perf, uint32_t tick, uint32_t since, double otherwise)
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
}

// Whether a repeat's setting `repeat` on track `t` plays as the next pass's setting `played` does: the same value, or
// pitches that the rules take as the same (LoopRules::same_pitch).
bool RepeatsSetting(int t, const TrackEvent& repeat, const TrackEvent& played, const LoopRules& rules)
{
    if (repeat.kind == EventKind::kPitch && repeat.semitones != played.semitones)
    {
        return rules.same_pitch && rules.same_pitch(t, repeat, played);
    }

    return SameSetting(repeat, played);
}

// Whether the notes of track `t` in the pass from `start` to `end` repeat as the game's next pass plays them, at
// tempo `repeat_bpm` (RepeatsAsTheGame).
bool TrackRepeats(const seqlib::midi::Performance& perf, int t, uint32_t start, uint32_t end, const LoopRules& rules,
                  const PassStart& at_start, const PassStart& next, double repeat_bpm)
{
    const auto track = static_cast<std::size_t>(t);
    TrackPasses passes = PassesOfTrack(perf, track, start, end, rules);
    if (rules.unstarted)
    {
        // A loop strike's twin strikes in the next pass, at `end` plus its offset, and a game strike's twin in the
        // loop, at `start` plus its offset.
        std::erase_if(passes.loop, [&](const Strike& s) { return rules.unstarted(t, end + s.offset, s.key); });
        std::erase_if(passes.game, [&](const Strike& s) { return rules.unstarted(t, start + s.offset, s.key); });
    }
    if (passes.loop.size() != passes.game.size())
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
        if (part == at_start.parts[track] && !passes.in_first_tick[part].contains(kind) &&
            first != at_start.settings.end() && again != next.settings.end() &&
            SameSetting(first->second, again->second))
        {
            return &first->second;
        }

        const auto kept = passes.at_end[part].find(kind);
        return kept != passes.at_end[part].end() ? &kept->second : nullptr;
    };

    for (std::size_t k = 0; k < passes.loop.size(); k++)
    {
        const Strike& a = passes.loop[k];
        const Strike& b = passes.game[k];
        if (a.offset != b.offset || a.key != b.key || a.velocity != b.velocity ||
            BpmAt(perf, start + a.offset, start, repeat_bpm) != BpmAt(perf, end + b.offset, 0, at_start.bpm))
        {
            return false;
        }
        if (rules.plays_alike && !rules.plays_alike(t, start + a.offset, end + b.offset))
        {
            return false;
        }

        std::set<EventKind> kinds;
        const std::map<EventKind, TrackEvent>& kept = passes.at_end[a.part];
        for (const auto* list : {&a.settings, &b.settings, &kept})
        {
            for (const auto& [kind, e] : *list)
            {
                if (std::ranges::find(rules.ignored, kind) == rules.ignored.end())
                {
                    kinds.insert(kind);
                }
            }
        }

        for (EventKind kind : kinds)
        {
            const auto sent_before = a.settings.find(kind);
            const TrackEvent* repeat =
                sent_before != a.settings.end() ? &sent_before->second : repeat_value(a.part, kind);
            const auto played = b.settings.find(kind);
            if ((repeat == nullptr) != (played == b.settings.end()) ||
                (repeat && !RepeatsSetting(t, *repeat, played->second, rules)))
            {
                return false;
            }
        }
    }

    return true;
}

// Sends the tempo at the start of the loop from `start` to `end` again, where the game's next pass `next_pass` starts
// with it and the loop ends with another (RestoreAtLoopStart). `at_loop_start` gives the pass at the loop's start.
void RestoreTempo(seqlib::midi::Performance& perf, uint32_t start, uint32_t end, const PassStart& at_loop_start,
                  const PassStart& next_pass)
{
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
}

// Whether the setting `at_start` of `kind` on `part`, which the loop from `start` to `end` starts with, goes unsent
// at the loop's start in `events`, while the loop ends with another value (RestoreAtLoopStart).
bool EndsOtherwise(const std::vector<TrackEvent>& events, uint8_t part, EventKind kind, uint32_t start, uint32_t end,
                   const TrackEvent& at_start)
{
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

    return !sent && !(SameSetting(*before_end, at_start) && SameSetting(*at_end, at_start));
}

// Inserts the setting `at_start` into `events` at tick `start`, before the first event of that tick that isn't a
// setting (RestoreAtLoopStart). With `rules.preset_with_settings`, the presets count as settings.
void InsertAtLoopStart(std::vector<TrackEvent>& events, const TrackEvent& at_start, uint32_t start,
                       const LoopRules& rules)
{
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

// A note that a track strikes in a pass: its offset from the pass's start, its key and velocity, and how long it lasts
// in ticks, if it ends in the performance (LengthsRepeat).
struct HeldNote
{
    uint32_t offset = 0;
    uint8_t key = 0;
    uint8_t velocity = 0;
    std::optional<double> length;
};

// The notes that `events` strike from tick `start` up to `end`, in order of offset, key and velocity, and otherwise in
// the order struck.
std::vector<HeldNote> HeldNotes(const std::vector<TrackEvent>& events, uint32_t start, uint32_t end)
{
    std::vector<HeldNote> notes;
    std::map<uint32_t, std::pair<std::size_t, double>> sounding; // by note ID: the note's index and its strike
    for (const TrackEvent& e : events)
    {
        if (e.kind == EventKind::kNoteOn && e.tick >= start && e.tick < end)
        {
            sounding[e.note] = {notes.size(), e.tick + e.after};
            notes.push_back({e.tick - start, e.key, e.value, std::nullopt});
        }
        else if (const auto held = sounding.find(e.note); e.kind == EventKind::kNoteOff && held != sounding.end())
        {
            notes[held->second.first].length = e.tick + e.after - held->second.second;
            sounding.erase(held);
        }
    }

    std::ranges::stable_sort(notes, {}, [](const HeldNote& n) { return std::tuple(n.offset, n.key, n.velocity); });

    return notes;
}

} // namespace

bool RepeatsAsTheGame(const seqlib::midi::Performance& perf, const std::vector<PartSwitch>& part_switches,
                      uint32_t start, uint32_t end, const LoopRules& rules)
{
    const PassStart at_start = PassStartAt(perf, part_switches, start);
    const PassStart next = PassStartAt(perf, part_switches, end);
    const double repeat_bpm = RepeatBpm(perf, start, end, at_start, next);
    for (int t = 0; t < 16; t++)
    {
        if (!TrackRepeats(perf, t, start, end, rules, at_start, next, repeat_bpm))
        {
            return false;
        }
    }

    return true;
}

bool LengthsRepeat(const seqlib::midi::Performance& perf, uint32_t start, uint32_t end, double tolerance)
{
    for (const std::vector<TrackEvent>& events : perf.tracks)
    {
        const std::vector<HeldNote> pass = HeldNotes(events, start, end);
        const std::vector<HeldNote> next = HeldNotes(events, end, end + (end - start));
        if (pass.size() != next.size())
        {
            return false;
        }

        for (std::size_t k = 0; k < pass.size(); k++)
        {
            const HeldNote& a = pass[k];
            const HeldNote& b = next[k];
            if (a.offset != b.offset || a.key != b.key || a.velocity != b.velocity)
            {
                return false;
            }
            if (a.length && b.length && std::abs(*a.length - *b.length) > tolerance)
            {
                return false;
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
    RestoreTempo(perf, start, end, at_loop_start, next_pass);

    for (const auto& [key, at_start] : at_loop_start.settings)
    {
        // The game gives the setting back its value at the loop's start. Only the part that the track's new notes go to
        // counts. A track sends every setting again when it moves to another part (Switch). An answer of
        // `rules.restores` decides in place of both tests, on any part.
        const auto [t, part, kind] = key;
        const auto track = static_cast<std::size_t>(t);
        const std::optional<bool> answer = rules.restores ? rules.restores(t, part, kind) : std::nullopt;
        if (answer == false)
        {
            continue;
        }
        if (!answer)
        {
            if (part != at_loop_start.parts[track])
            {
                continue;
            }

            const auto next = next_pass.settings.find({t, next_pass.parts[track], kind});
            if (next == next_pass.settings.end() || !SameSetting(next->second, at_start))
            {
                continue;
            }
        }

        // It isn't sent at the loop's start, and the loop ends with another value. A MIDI player may take the loop
        // end's tick or leave it out. Both count.
        std::vector<TrackEvent>& events = perf.tracks[track];
        if (!EndsOtherwise(events, part, kind, start, end, at_start))
        {
            continue;
        }

        // Send it with the loop start's other settings, ahead of its notes. With `rules.preset_with_settings`, it goes
        // after the presets there too.
        InsertAtLoopStart(events, at_start, start, rules);
    }
}

MainLoopPasses::MainLoopPasses(std::vector<LoopChoice> replay) : replay_(std::move(replay))
{
}

MainLoopPasses::MainLoopPasses(LoopReplay replay) : replay_(std::move(replay.choices)), check_last_(replay.check_last)
{
}

void MainLoopPasses::Restart(PerformanceBase& out, const std::optional<std::pair<int, uint32_t>>& owner)
{
    out.DropLoop();
    owner_ = owner;
    passes_.clear();
    marked_ = 0;
    span_ = 1;
    extended_ = false;
    toward_.reset();
    heading_.reset();
}

void MainLoopPasses::CountPass(PerformanceBase& out, const std::pair<int, uint32_t>& loop, uint32_t start, uint32_t end)
{
    if (owner_ == loop)
    {
        passes_.emplace_back(start, end);
    }

    out.loops++;
    if (!out.loop)
    {
        out.loop = seqlib::midi::Performance::Loop{start, end};
    }
}

double LevelScale(double peak)
{
    return peak >= 127.5 ? 127.0 / peak : 1.0;
}

void SetLevelCut(PerformanceBase& performance, double applied)
{
    if (LevelScale(applied) < 1.0)
    {
        // CC11 acts squared. Scaling it by 127 / peak makes every note 40 log10(peak / 127) dB quieter.
        performance.level_cut_db = 40.0 * std::log10(applied / 127.0);
        char what[160];
        std::snprintf(what, sizeof(what),
                      "volume exceeds MIDI range (SoundFont playback attenuated by %.1f dB; SFZ restores the gain)",
                      performance.level_cut_db);
        performance.approximations[what] = seqlib::midi::NoteCount(performance);
    }
}

std::size_t WholeSpan(uint32_t pass, const std::vector<uint32_t>& lengths)
{
    if (pass == 0)
    {
        return 0;
    }

    // The multiple stops growing past kMostWholeSpan passes. For loops under 2^26 ticks, it stays within 64 bits.
    uint64_t whole = pass;
    for (const uint32_t length : lengths)
    {
        if (length == 0)
        {
            return 0;
        }
        whole = std::lcm(whole, uint64_t{length});
        if (whole / pass > kMostWholeSpan)
        {
            return kMostWholeSpan + 1;
        }
    }

    return static_cast<std::size_t>(whole / pass);
}

void ListWholePeriod(PerformanceBase& out, std::size_t whole_span)
{
    char what[160];
    if (whole_span > kMostWholeSpan)
    {
        std::snprintf(what, sizeof(what), "tracks loop at other lengths (their loops line up after over %zu passes)",
                      kMostWholeSpan);
    }
    else
    {
        std::snprintf(what, sizeof(what), "tracks loop at other lengths (their loops line up every %zu passes)",
                      whole_span);
    }
    out.approximations[what] = seqlib::midi::NoteCount(out);
}

namespace
{

// The layers beyond the first that the parts of a track's `events` require. A part requires as many layers as the most
// notes of one key that it plays at once. The MIDI writer counts its layers the same way (LayerDemand).
int ExtraLayers(const std::vector<TrackEvent>& events)
{
    std::map<std::pair<uint8_t, uint8_t>, int> playing;    // per part and key: the notes that play
    std::map<uint32_t, std::pair<uint8_t, uint8_t>> notes; // per note: its part and key
    std::map<uint8_t, int> layers;                         // per part: the most notes of one key at once
    for (const TrackEvent& e : events)
    {
        if (e.kind == EventKind::kNoteOn)
        {
            const std::pair<uint8_t, uint8_t> at{e.part, e.key};
            notes[e.note] = at;
            layers[e.part] = std::max(layers[e.part], ++playing[at]);
        }
        else if (e.kind == EventKind::kNoteOff)
        {
            const auto it = notes.find(e.note);
            if (it != notes.end())
            {
                playing[it->second]--;
                notes.erase(it);
            }
        }
    }

    int extra = 0;
    for (const auto& [part, most] : layers)
    {
        extra += most - 1;
    }

    return extra;
}

} // namespace

std::array<uint8_t, 16> FitParts(const seqlib::midi::Performance& perf, const std::array<uint8_t, 16>& used,
                                 SpareChannels spare)
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

    // The layers' channels come before the other parts', while every track that plays keeps its first channel.
    if (spare == SpareChannels::kForLayers)
    {
        for (int t = 0; t < 16; t++)
        {
            if (plays[t])
            {
                channels = std::min(channels + ExtraLayers(perf.tracks[t]), 16);
            }
        }
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

std::optional<std::array<uint8_t, 16>> FittedParts(const seqlib::midi::Performance& perf,
                                                   const std::array<uint8_t, 16>& used, SpareChannels spare)
{
    const std::array<uint8_t, 16> fitted = FitParts(perf, used, spare);
    for (int t = 0; t < 16; t++)
    {
        if (fitted[t] < used[t])
        {
            return fitted;
        }
    }

    return std::nullopt;
}

} // namespace ntdlib::sequence_export
