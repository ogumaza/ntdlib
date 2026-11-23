// SPDX-License-Identifier: MIT

// The rules for the passes of a sequence's main loop. A performer records the passes as MIDI events. The functions here
// find the settings that a pass starts with, and test whether a MIDI player that loops on a pass plays the repeats as
// the game plays the next pass. ChooseSteadyPass chooses the pass to mark. MainLoopPasses holds the passes that a
// performer counts, and the marked pass. RestoreAtLoopStart sends again, at the loop's start, the settings that the
// game gives back there. FitParts fits a performance's parts into MIDI's 16 channels, and LevelScale fits the levels
// into CC11. SetLevelCut records the cut that LevelScale makes.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "seqlib/midi/performance.h"

#include "ntdlib/sequence_export/performance_base.h"

namespace ntdlib::sequence_export
{

using EventKind = seqlib::midi::EventKind;
using TrackEvent = seqlib::midi::TrackEvent;

// True for controller events that update a track's channel settings.
bool IsTrackSetting(EventKind kind);

// A setting of a track's part: its track, part and kind of event.
using SettingKey = std::tuple<int, uint8_t, EventKind>;

// A switch of a track's new notes to another part: the tick, the track and the new part. A performer lists them in
// order of tick.
using PartSwitch = std::tuple<uint32_t, int, uint8_t>;

// How a pass of the main loop starts: each track part's settings and the tempo after the commands of its first tick,
// and the part each track's new notes go to.
struct PassStart
{
    std::map<SettingKey, TrackEvent> settings;
    double bpm = 120.0;
    std::array<uint8_t, 16> parts{};
};

// What ChooseSteadyPass selected: whether the performance stops, the pass of the main loop it marked, if any, and
// whether the performance plays one more pass to mark it. SearchSteadyPass can mark a block of `span` consecutive
// passes, and gives the pass that the performance plays on toward (`toward`). A choice that plays on toward the block
// of the whole period gives the block's first pass in `marked` before the performance has played the block. Where
// fewer than `loops` plays of that block fit the file's limit, `plays` gives the plays that fit. A choice that marks
// nothing gives the whole period that the search found in `whole_span`. A later run makes the choice again without the
// probes, and keeps it.
struct LoopChoice
{
    bool stop = false;
    std::optional<std::size_t> marked;
    bool extend = false;
    std::size_t span = 1;
    std::optional<std::size_t> toward;
    std::size_t left_out = 0;   // the strikes that the test of the marked pass left out (LoopRules::unstarted)
    std::size_t plays = 0;      // the plays of the marked block, where not `loops`
    std::size_t whole_span = 0; // the passes over which the tracks' loops line up (WholeSpan), where nothing is marked
};

// The main loop's choices of an earlier run, for a later run to make again (MainLoopPasses). With `check_last`, the
// later run tests the pass that the last choice marked again on its parts, before it stops (MarkSteadyPass).
struct LoopReplay
{
    std::vector<LoopChoice> choices;
    bool check_last = false;
};

// The rules in which the performers differ.
struct LoopRules
{
    // The settings besides the pitch that a track's first part starts with at 0, without events. sequence_export's
    // performer gives the SFZ low-pass and biquad.
    std::vector<EventKind> seeded;

    // Whether the settings that RestoreAtLoopStart sends again go after the presets at the loop's first tick, rather
    // than ahead of them.
    bool preset_with_settings = false;

    // The settings that RepeatsAsTheGame leaves out when it compares a repeat with the game's next pass.
    std::vector<EventKind> ignored = {};

    // Whether a repeat's pitch `a` on track `track` plays as the next pass's pitch `b`, where the two differ. A
    // performer that plays a bend at the pitch that a timer rounds it to takes them as the same where both stand for
    // the same bend and are within the timer's rounding. Without it, pitches must be the same.
    std::function<bool(int track, const TrackEvent& a, const TrackEvent& b)> same_pitch = {};

    // Whether the note that a repeat strikes at `repeat_tick` on track `track` plays as the note that the game strikes
    // at `game_tick`, in what the settings at the two strikes don't show. Without it, the settings decide.
    std::function<bool(int track, uint32_t repeat_tick, uint32_t game_tick)> plays_alike = {};

    // Whether the note of key `key` that track `track` strikes at `tick` got no channel. RepeatsAsTheGame leaves out a
    // strike whose twin in the other pass got none: the driver drops other notes in each pass, and a loop can repeat
    // only one pass's. Without it, every strike counts.
    std::function<bool(int track, uint32_t tick, uint8_t key)> unstarted = {};

    // Whether RestoreAtLoopStart sends the start value of `kind` again on `part` of `track`, where the loop ends with
    // another value. No answer leaves the setting to the rule for the others: the part that takes the track's new
    // notes, where the game sets the value back.
    std::function<std::optional<bool>(int track, uint8_t part, EventKind kind)> restores = {};
};

// The settings of each track's parts after the commands of `tick`, from `perf`'s events.
std::map<SettingKey, TrackEvent> SettingsAt(const seqlib::midi::Performance& perf, uint32_t tick);

// How a pass of `perf` that starts at `tick` starts. `part_switches` are the performance's part switches.
PassStart PassStartAt(const seqlib::midi::Performance& perf, const std::vector<PartSwitch>& part_switches,
                      uint32_t tick);

// True if events `a` and `b` give a setting the same value.
bool SameSetting(const TrackEvent& a, const TrackEvent& b);

// True if a MIDI player that loops on the pass from `start` to `end` plays each repeat's notes as the game plays the
// notes of the next pass. `perf` holds both passes. Each repeat must have the same notes at the same ticks, with the
// same settings and tempo. Pitches may differ where `rules.same_pitch` takes them as the same. Each pair of notes must
// also play alike where `rules.plays_alike` is set. A strike whose twin got no channel (`rules.unstarted`) counts as
// no strike. The MIDI player keeps the loop end's values, except for those that the pass sends before a note and
// those that RestoreAtLoopStart sends again at the loop's start. The notes of a pass and of the next may go to
// different parts of a track, each with the track's settings.
bool RepeatsAsTheGame(const seqlib::midi::Performance& perf, const std::vector<PartSwitch>& part_switches,
                      uint32_t start, uint32_t end, const LoopRules& rules);

// True if each note that the pass of `perf` from `start` to `end` strikes lasts as long as its twin in the next pass,
// within `tolerance` ticks. A note's twin is the note of the next pass that its track strikes at the same offset, with
// the same key and velocity, in the same order. A note that has no end in `perf`, or whose twin has none, counts as
// alike. A track that strikes other notes in the two passes doesn't repeat.
bool LengthsRepeat(const seqlib::midi::Performance& perf, uint32_t start, uint32_t end, double tolerance);

// Sends at the main loop's start each setting, and the tempo, that moves during the loop and that the game gives back
// its value at the loop's start in `next_pass`. A MIDI player that loops on the markers otherwise keeps the loop end's
// value. MIDI only sends a value when it changes. Where `rules.restores` answers for a setting, the answer decides in
// place of `next_pass`, on any part.
void RestoreAtLoopStart(seqlib::midi::Performance& perf, const std::vector<PartSwitch>& part_switches,
                        const PassStart& next_pass, const LoopRules& rules);

// Selects the first of the loop owner's passes that plays as the pass after it does (RepeatsAsTheGame). A MIDI player
// that loops on the markers plays each repeat as the marked pass. The search starts at the marked pass. `owner_passes`
// are the start and end ticks of the owner's counted passes, `marked` the marked one of them, and `last_draw` the last
// tick a track drew a random number in. `extended` is set if the performance has played one more pass to mark one.
// `loops` is the number of times to play the main loop. `play_on(until)` plays a copy of the performer on up to, not
// including, tick `until`. The function that `play_on` returns tests whether the copy's pass from a start to an end
// plays as the pass after it (RepeatsAsTheGame).
template <typename PlayOn>
LoopChoice ChooseSteadyPass(const std::vector<std::pair<uint32_t, uint32_t>>& owner_passes, std::size_t marked,
                            const std::optional<uint32_t>& last_draw, bool extended, int loops, PlayOn play_on)
{
    // Settings that the first pass's setup leaves, or that the game keeps from pass to pass, can make a pass play
    // otherwise than the pass after it. Notes that only the first pass strikes and later passes hold can do the same. A
    // loop that draws random numbers plays each pass otherwise anyway. Such a loop keeps its first pass.
    if (owner_passes.empty() || (last_draw && *last_draw >= owner_passes[0].first))
    {
        return {.stop = true};
    }

    // A copy of the performer plays the two passes after the owner's last pass. Each pass and the next then have the
    // pass after them to compare with.
    const auto [last_start, last_end] = owner_passes.back();
    const uint32_t length = last_end - last_start;
    const auto repeats = play_on(last_end + 2 * length);
    for (std::size_t i = marked; i < owner_passes.size(); i++)
    {
        const auto [start, end] = owner_passes[i];
        if (repeats(start, end))
        {
            return {.stop = static_cast<int>(owner_passes.size() - i) >= loops, .marked = i};
        }
    }

    // No pass so far plays as the next one does. If the next pass plays as the one after it, play that pass too.
    // Otherwise keep the first.
    if (!extended && repeats(last_end, last_end + length))
    {
        return {.extend = true};
    }

    return {.stop = true};
}

// How far SearchSteadyPass looks for the steady pass. The DS and DSE performers set it. The other performers keep
// ChooseSteadyPass's rule: the passes played, and the pass after them.
struct PassSearch
{
    // The last pass, counted from the owner's first, that the search plays on to where the probe shows that it
    // repeats. The passes already played are always tested.
    std::size_t last_pass = 4;

    // The most passes in a block. Where no single pass repeats, the search tests blocks of 2 to `longest_block`
    // consecutive passes that start no later than pass 2.
    std::size_t longest_block = 3;

    // The longest that a file may play, in ms (kMaxSeconds). The search plays on to no pass that ends later.
    double max_ms = 0.0;
};

// An owner's pass that a probe counted: its start and end ticks, and the time of its end in ms.
struct CountedPass
{
    uint32_t start = 0;
    uint32_t end = 0;
    double end_ms = 0.0;
};

// The most of the owner's passes that a whole period can span (WholeSpan). A longer period plays for well over
// kProbeSeconds, and the search doesn't test it.
constexpr std::size_t kMostWholeSpan = 1000;

// The owner's passes over which the loops of the tracks that play line up: the least common multiple of their lengths
// in `lengths`, in passes of `pass` ticks, the owner's pass among them. 0 where a length or `pass` is 0, and
// kMostWholeSpan + 1 for a period of more than kMostWholeSpan passes.
std::size_t WholeSpan(uint32_t pass, const std::vector<uint32_t>& lengths);

// Lists in `out` the approximation of a main loop whose tracks loop at other lengths, over `whole_span` passes
// (WholeSpan). Each repeat plays every track from the marked pass, where the game plays the tracks of the longer
// loops otherwise.
void ListWholePeriod(PerformanceBase& out, std::size_t whole_span);

// The tests that a probe of SearchSteadyPass runs on a pass, from the strictest.
enum class PassTest
{
    kSettled, // kStrict, and each note lasts as long as its twin in the next pass, within the performer's tolerance
    kStrict,  // RepeatsAsTheGame with every strike
    kExcused, // RepeatsAsTheGame without the strikes whose twins got no channel (LoopRules::unstarted)
};

// The owner's passes in `owner_passes`, the ones played, then those that `probe` counted after them (CountedPass). A
// probe that moved the markers to another loop counted no pass of the owner after them.
template <typename Probe>
std::vector<CountedPass> ProbedPasses(const std::vector<std::pair<uint32_t, uint32_t>>& owner_passes,
                                      const Probe& probe)
{
    std::vector<CountedPass> passes;
    for (const auto& [start, end] : owner_passes)
    {
        passes.push_back({start, end, 0.0});
    }

    const std::size_t played = owner_passes.size();
    const std::vector<CountedPass>& counted = probe.passes;
    bool same = counted.size() > played;
    for (std::size_t i = 0; same && i < played; i++)
    {
        same = counted[i].start == owner_passes[i].first && counted[i].end == owner_passes[i].second;
    }
    if (same)
    {
        passes.insert(passes.end(), counted.begin() + static_cast<std::ptrdiff_t>(played), counted.end());
    }

    return passes;
}

// The DS and DSE performers' search for the steady pass, in place of ChooseSteadyPass. It selects the first of the
// owner's passes, from `marked` on, that plays as the pass after it does, as ChooseSteadyPass does.
// `play_on(until, enough)` plays a copy of the performer on up to tick `until`. Where `enough` isn't 0, the copy stops
// once it has played the owner's first `enough` passes to their end. It returns a probe: `probe(start, end, test)` runs
// a PassTest on the pass from `start` to `end`, and `probe.passes` holds the owner's passes that the probe played to
// their end, the played ones first (CountedPass). `probe.whole_span` gives the whole period at the probe's end
// (WholeSpan), or 0 where a track that plays hasn't looped or the performer doesn't follow the tracks' loops. A pass is
// tested only where the probe played the next pass to its end. Where no pass played so far repeats, a probe that plays
// further tests later passes, up to pass `search.last_pass`, and then blocks of passes. Where that probe doesn't start,
// the first probe tests the blocks. The performance plays on, one pass at a time, toward the first that repeats.
// `ahead` bounds that: the search marks no pass past `ahead` passes after the ones played.
//
// The search runs in three tiers, each only where the one before finds nothing. The first looks for a settled pass: one
// that repeats with its notes' lengths (PassTest::kSettled), and whose next pass does too. A pass whose notes end
// otherwise than in the game's later passes then gives way to a later one. The second runs the whole search with the
// strict test, and the third with the strikes whose twins got no channel left out. A pass that repeats exactly plays
// the game's notes in every repeat. Where nothing repeats, the choice keeps the first pass, as ChooseSteadyPass's does.
//
// Where nothing repeats, a last tier tests the whole period: the block of the probe's `whole_span` passes, over which
// the loops of every track line up. Tracks that loop at other lengths repeat together only there. The tier runs the
// same three tests on the block. It marks the block where it repeats and at least one play fits the file's limit, and
// gives the plays that fit where `loops` plays don't (LoopChoice::plays). The performance then plays on to the block's
// end (MainLoopPasses). A choice that marks nothing gives the whole period (LoopChoice::whole_span).
//
// Each call tests the passes up to pass `reach`. The performance can play on past a pass that the strict tier chose,
// until it has played that pass `loops` times. A later call then reaches further, and a settled pass there wins over
// the strict one. A check on fewer parts can move its loop the same way. The pass it moves to is settled.
template <typename PlayOn>
LoopChoice SearchSteadyPass(const std::vector<std::pair<uint32_t, uint32_t>>& owner_passes, std::size_t marked,
                            const std::optional<uint32_t>& last_draw, int loops, PlayOn play_on,
                            const PassSearch& search, std::size_t ahead)
{
    if (owner_passes.empty() || (last_draw && *last_draw >= owner_passes[0].first))
    {
        return {.stop = true};
    }

    const std::size_t played = owner_passes.size();
    const auto [last_start, last_end] = owner_passes.back();
    const uint32_t length = last_end - last_start;
    const std::size_t reach = std::min(std::max(search.last_pass, played + 1), played + ahead);

    // The choice for a pass or block from index `first` to `last` that repeats: a mark where the performance has played
    // it, or one more pass toward it where pass `last` ends within the file's limit. The passes end in order. The pass
    // after the played ones therefore ends within the limit too.
    const auto choose = [&](const std::vector<CountedPass>& passes, std::size_t first, std::size_t last,
                            std::size_t span) -> std::optional<LoopChoice>
    {
        if (last < played)
        {
            return LoopChoice{.stop = static_cast<int>(played - first) >= loops * static_cast<int>(span),
                              .marked = first,
                              .span = span};
        }

        if (last < reach && last < passes.size() && passes[last].end_ms <= search.max_ms)
        {
            return LoopChoice{.extend = true, .toward = last};
        }

        return std::nullopt;
    };

    // The first settled pass. The first later pass that settles ends the tier, whether the performance can play on to
    // it or not.
    const auto settled = [&](const auto& probe, const std::vector<CountedPass>& passes) -> std::optional<LoopChoice>
    {
        for (std::size_t i = marked; i < reach && i + 2 < passes.size(); i++)
        {
            if (probe(passes[i].start, passes[i].end, PassTest::kSettled) &&
                probe(passes[i + 1].start, passes[i + 1].end, PassTest::kSettled))
            {
                return choose(passes, i, i, 1);
            }
        }

        return std::nullopt;
    };

    const auto find = [&](const auto& probe, const std::vector<CountedPass>& passes, bool blocks,
                          PassTest test) -> std::optional<LoopChoice>
    {
        for (std::size_t i = marked; i < played && i + 1 < passes.size(); i++)
        {
            if (probe(passes[i].start, passes[i].end, test))
            {
                return choose(passes, i, i, 1);
            }
        }

        for (std::size_t j = std::max(played, marked); j < reach && j + 1 < passes.size(); j++)
        {
            if (probe(passes[j].start, passes[j].end, test))
            {
                if (const auto choice = choose(passes, j, j, 1))
                {
                    return choice;
                }
                break;
            }
        }

        // Blocks of passes, for a loop whose passes repeat in a longer period. A block repeats where the next block
        // has its length and plays as it does.
        for (std::size_t span = 2; blocks && span <= search.longest_block; span++)
        {
            for (std::size_t i = marked; i <= 1 && i + 2 * span <= passes.size(); i++)
            {
                const uint32_t start = passes[i].start;
                const uint32_t end = passes[i + span - 1].end;
                if (passes[i + 2 * span - 1].end - passes[i + span].start != end - start || !probe(start, end, test))
                {
                    continue;
                }
                if (const auto choice = choose(passes, i, i + span - 1, span))
                {
                    return choice;
                }
            }
        }

        return std::nullopt;
    };

    // The near probe plays the pass after the ones played, as ChooseSteadyPass tests it. It stops once it has counted
    // the pass after that one: the test requires it. The far probe plays as far as the later passes and the blocks
    // require: pass `reach` and the pass after it, or two blocks of the longest span from pass 2. It starts only where
    // a pass after the near probe's can count. A pass counts once the probe has played it to its end, and only a pass
    // whose next pass counts is tested. The passes can differ in length. Each probe therefore plays on half a pass or
    // half as far again, at most.
    const auto near = play_on(last_end + 2 * length + length / 2, played + 2);
    const std::vector<CountedPass> near_passes = ProbedPasses(owner_passes, near);
    const std::size_t needed = std::max(reach + 1, 1 + 2 * search.longest_block);
    const bool may_play_on = near_passes.size() > played && near_passes[played].end_ms <= search.max_ms;
    const bool far = needed > played + 2 && (may_play_on || played >= 2);
    std::optional<std::remove_cvref_t<decltype(near)>> further;
    std::vector<CountedPass> further_passes;
    const auto play_further = [&]()
    {
        if (far && !further)
        {
            further.emplace(play_on(last_end + static_cast<uint32_t>((needed - played) * length * 3 / 2), 0));
            further_passes = ProbedPasses(owner_passes, *further);
        }

        return further.has_value();
    };

    // The settled pass: on the near probe, then on a probe that stops at the end of the second pass after pass `reach`.
    // The call tests no pass after pass `reach`, and a test requires the two after it. The second probe runs only where
    // the far probe starts and the performance can play on: the pass after the played ones ends within the file's
    // limit, and pass `reach` comes after them.
    if (const auto choice = settled(near, near_passes))
    {
        return *choice;
    }
    if (far && may_play_on && reach > played)
    {
        const std::size_t enough = reach + 2;
        const auto settling = play_on(last_end + static_cast<uint32_t>((enough - played) * length * 3 / 2), enough);
        if (const auto choice = settled(settling, ProbedPasses(owner_passes, settling)))
        {
            return *choice;
        }
    }

    // Then the strict test and the excuse: the near probe first, then the far probe where it starts. Without the far
    // probe, the near probe tests the blocks.
    for (const PassTest test : {PassTest::kStrict, PassTest::kExcused})
    {
        if (const auto choice = find(near, near_passes, !far, test))
        {
            return *choice;
        }
        if (play_further())
        {
            if (const auto choice = find(*further, further_passes, true, test))
            {
                return *choice;
            }
        }
    }

    // The whole period, from the first call only: a check and a performance that plays on toward a pass don't reach
    // it. The far probe reaches the loops of more tracks than the near one. The block starts no later than pass 1. A
    // choice that marks nothing gives the period.
    const std::size_t whole = std::max(near.whole_span, play_further() ? further->whole_span : 0);
    if (whole <= search.longest_block || whole > kMostWholeSpan || ahead < search.last_pass || marked > 1)
    {
        return {.stop = true, .whole_span = whole};
    }

    // Passes of about the near probe's length that end past the limit, a pass's slack aside, leave the probe out: not
    // one play of the block fits.
    if (near_passes.size() > played + 1)
    {
        const double pass_ms = near_passes[played + 1].end_ms - near_passes[played].end_ms;
        const std::size_t last = marked + whole - 1;
        const double estimate =
            near_passes[played].end_ms + pass_ms * static_cast<double>(last > played ? last - played : 0);
        if (estimate > search.max_ms + pass_ms)
        {
            return {.stop = true, .whole_span = whole};
        }
    }

    // The probe plays the block `loops` times and once more, and three blocks at least for the settled test.
    const std::size_t wanted = static_cast<std::size_t>(std::max(loops, 1));
    const std::size_t enough = 1 + std::max(wanted + 1, std::size_t{3}) * whole;
    const auto period = play_on(last_end + static_cast<uint32_t>((enough - played) * length * 3 / 2), enough);
    const std::vector<CountedPass> period_passes = ProbedPasses(owner_passes, period);

    // The block from pass `i` repeats where the next block has its length and plays as it does.
    const auto repeats = [&](std::size_t i, PassTest test)
    {
        if (i + 2 * whole > period_passes.size())
        {
            return false;
        }

        const uint32_t start = period_passes[i].start;
        const uint32_t end = period_passes[i + whole - 1].end;
        return period_passes[i + 2 * whole - 1].end - period_passes[i + whole].start == end - start &&
               period(start, end, test);
    };

    for (const PassTest test : {PassTest::kSettled, PassTest::kStrict, PassTest::kExcused})
    {
        for (std::size_t i = marked; i <= 1; i++)
        {
            if (!repeats(i, test) || (test == PassTest::kSettled && !repeats(i + whole, test)))
            {
                continue;
            }

            // The plays of the block that end within the file's limit, `loops` at most.
            std::size_t plays = 0;
            while (plays < wanted && i + (plays + 1) * whole <= period_passes.size() &&
                   period_passes[i + (plays + 1) * whole - 1].end_ms <= search.max_ms)
            {
                plays++;
            }
            if (plays == 0)
            {
                continue;
            }

            const std::size_t fewer = plays < wanted ? plays : 0;
            if (i + whole <= played)
            {
                return {.stop = played - i >= plays * whole, .marked = i, .span = whole, .plays = fewer};
            }
            return {.marked = i, .extend = true, .span = whole, .toward = i + whole - 1, .plays = fewer};
        }
    }

    return {.stop = true, .whole_span = std::max(whole, period.whole_span)};
}

// The passes of a performance's main loop that a performer has counted, and the marked pass. Each counted pass is a
// loop of the performance (CountPass). Only the passes of one loop, the owner, can be marked. The performer's rules
// choose the owner (Restart). MarkSteadyPass marks the pass that ChooseSteadyPass selects. A block of passes that
// SearchSteadyPass marks before it has played `loops` times stays marked until it has. A later run of the sequence
// makes the first run's choices again (Choices).
//
// A run on other parts checks the earlier run's last choice (LoopReplay::check_last). Its notes and settings go to
// other parts, and the marked pass may not repeat there. The check tests the marked pass again, and the passes after
// it, and plays one more pass where the next pass repeats. It never marks an earlier pass. A check keeps a marked block
// of passes as it is. A single pass of the block can repeat on other parts where the block doesn't. Where nothing
// repeats, the earlier run's choice stays: that run tested its pass with each note on a part of the track's.
class MainLoopPasses
{
public:
    // MarkSteadyPass makes the choices in `replay`, from an earlier run (Choices), before it selects others.
    explicit MainLoopPasses(std::vector<LoopChoice> replay);
    explicit MainLoopPasses(LoopReplay replay);

    // The owner, if any: a track and the start of its loop.
    const std::optional<std::pair<int, uint32_t>>& Owner() const
    {
        return owner_;
    }

    // The choices that MarkSteadyPass made, for a later run to make again.
    const std::vector<LoopChoice>& Choices() const
    {
        return choices_;
    }

    // Records the strikes that the test of the pass that the last choice marked left out (LoopChoice::left_out).
    void SetLeftOut(std::size_t count)
    {
        if (!choices_.empty())
        {
            choices_.back().left_out = count;
        }
    }

    // The strikes that the test of the marked pass left out, from the last choice that marked one.
    std::size_t LeftOut() const
    {
        const auto marking = std::ranges::find_if(choices_ | std::views::reverse,
                                                  [](const LoopChoice& choice) { return choice.marked.has_value(); });
        return marking != (choices_ | std::views::reverse).end() ? marking->left_out : 0;
    }

    // The owner's counted passes: start and end ticks.
    const std::vector<std::pair<uint32_t, uint32_t>>& Passes() const
    {
        return passes_;
    }

    // Drops `out`'s main loop and the counted passes. The passes of `owner`, if any, can be marked from now on.
    void Restart(PerformanceBase& out, const std::optional<std::pair<int, uint32_t>>& owner);

    // Counts the pass of `loop` from tick `start` to tick `end` as a loop of `out`. A pass of the owner can be marked
    // later. Without a loop, `out` takes the pass as its loop.
    void CountPass(PerformanceBase& out, const std::pair<int, uint32_t>& loop, uint32_t start, uint32_t end);

    // Marks in `out` the owner's pass that ChooseSteadyPass selects, or makes the earlier run's next choice.
    // `last_draw`, `loops` and `play_on` are as ChooseSteadyPass takes them. Where `play_on(until, enough)` returns a
    // probe that counts the passes, SearchSteadyPass selects in place of ChooseSteadyPass, with `search`. `search`
    // mustn't be null then. `whole_span` is the performance's whole period at the call (WholeSpan). A choice that marks
    // nothing keeps the longer of it and the period that the search found, and the performance lists it
    // (ListWholePeriod). Returns true if the performance can stop, false if it requires another pass first.
    template <typename PlayOn>
    bool MarkSteadyPass(PerformanceBase& out, const std::optional<uint32_t>& last_draw, int loops, PlayOn play_on,
                        const PassSearch* search = nullptr, std::size_t whole_span = 0);

private:
    std::optional<std::pair<int, uint32_t>> owner_;
    std::vector<std::pair<uint32_t, uint32_t>> passes_; // the owner's counted passes, start and end ticks
    std::size_t marked_ = 0;                            // the marked one of them
    std::size_t span_ = 1;                              // the passes in the marked block
    bool extended_ = false;                             // one more pass has played to mark one (LoopChoice::extend)
    std::optional<std::size_t> toward_;                 // the pass that the performance plays on to (SearchSteadyPass)
    std::optional<LoopChoice> heading_;                 // the whole period's block that it plays on to
    std::vector<LoopChoice> replay_;                    // an earlier run's choices to make again
    std::size_t replayed_ = 0;                          // how many of them have been made
    bool check_last_ = false;                           // test the last of them again (LoopReplay::check_last)
    std::optional<LoopChoice> checked_;                 // the choice under a check. It stays without a better one
    std::vector<LoopChoice> choices_;                   // the choices made
};

template <typename PlayOn>
bool MainLoopPasses::MarkSteadyPass(PerformanceBase& out, const std::optional<uint32_t>& last_draw, int loops,
                                    PlayOn play_on, const PassSearch* search, std::size_t whole_span)
{
    // SearchSteadyPass plays on toward the pass that it found, and no further.
    const std::size_t ahead = !toward_                     ? (search ? search->last_pass : 0)
                              : *toward_ >= passes_.size() ? *toward_ + 1 - passes_.size()
                                                           : 0;

    // A probe that counts the passes (CountedPass) goes to SearchSteadyPass, and requires `search`. Only
    // SearchSteadyPass marks blocks.
    const auto select = [&](bool extended, std::size_t passes_ahead)
    {
        if constexpr (requires { play_on(uint32_t{}, std::size_t{}).passes; })
        {
            return SearchSteadyPass(passes_, marked_, last_draw, loops, play_on, *search, passes_ahead);
        }
        else
        {
            return ChooseSteadyPass(passes_, marked_, last_draw, extended, loops, play_on);
        }
    };

    // The plays of a marked block: `loops`, or fewer that fit the file's limit.
    const auto plays = [loops](const LoopChoice& marking)
    {
        return marking.plays > 0 ? marking.plays : static_cast<std::size_t>(loops);
    };

    LoopChoice choice;
    if (replayed_ < replay_.size())
    {
        choice = replay_[replayed_++];
        if (check_last_ && replayed_ == replay_.size() && choice.stop)
        {
            // The check starts at the earlier run's pass, and it may play one more pass (ChooseSteadyPass's extension)
            // whether or not that run did. A marked block of passes isn't tested again.
            checked_ = choice;
            if (choice.marked)
            {
                marked_ = *choice.marked;
                span_ = choice.span;
            }
            choice = span_ > 1 ? LoopChoice{.stop = true} : select(false, 1);
        }
    }
    else if (heading_)
    {
        // The performance plays on to the end of the whole period's block that the search tested, then marks it.
        const std::size_t first = *heading_->marked;
        const std::size_t span = heading_->span;
        choice = passes_.size() < first + span ? LoopChoice{.extend = true, .toward = first + span - 1}
                                               : LoopChoice{.stop = passes_.size() - first >= plays(*heading_) * span,
                                                            .marked = first,
                                                            .span = span,
                                                            .plays = heading_->plays};
    }
    else if (span_ > 1 && !choices_.empty() && choices_.back().marked && !choices_.back().stop)
    {
        // A block that this run marked plays `loops` full times, or the plays that fit (LoopChoice::plays). The
        // search tested it, and the passes after the mark play it again.
        choice = {.stop = passes_.size() - marked_ >= plays(choices_.back()) * span_,
                  .marked = marked_,
                  .span = span_,
                  .plays = choices_.back().plays};
    }
    else
    {
        choice = select(extended_, ahead);
    }
    if (choice.toward)
    {
        toward_ = choice.toward;
    }
    if (choice.marked && choice.extend)
    {
        heading_ = choice;
    }
    else if (choice.marked)
    {
        heading_.reset();
    }

    // A check that finds no pass keeps the earlier run's choice.
    if (checked_ && choice.stop && !choice.marked)
    {
        choice = *checked_;
    }
    if (choice.stop && !choice.marked)
    {
        choice.whole_span = std::max(choice.whole_span, whole_span);
    }
    choices_.push_back(choice);
    if (choice.stop && !choice.marked && choice.whole_span > 1)
    {
        ListWholePeriod(out, choice.whole_span);
    }
    if (choice.marked && *choice.marked + choice.span <= passes_.size())
    {
        marked_ = *choice.marked;
        span_ = choice.span;
        out.loop = seqlib::midi::Performance::Loop{passes_[marked_].first, passes_[marked_ + span_ - 1].second};
        out.loops = static_cast<int>((passes_.size() - marked_) / span_);

        // A performer chooses again once `out` has played `loops` times. A block that plays fewer times
        // (LoopChoice::plays) is chosen again after each pass, until it has played them.
        if (choice.plays > 0 && !choice.stop)
        {
            out.loops = std::max(out.loops, loops - 1);
        }
    }
    extended_ = extended_ || choice.extend;

    return choice.stop;
}

// Scale factor that keeps rounded CC11 values within 127 for a maximum unscaled value of `peak` (Perform).
double LevelScale(double peak);

// Records in `performance` the cut that LevelScale makes for a peak of `applied`, the peak that its last pass scaled
// for: the MIDI file's levels in dB (PerformanceBase::level_cut_db), and the approximation. The SFZ files restore the
// gain. A peak that LevelScale leaves makes no cut.
void SetLevelCut(PerformanceBase& performance, double applied);

// What FitParts does with the channels that the tracks' first parts leave. A part plays a note of a key that one of its
// notes still plays on another layer: a further channel of the part. The MIDI writer gives a part's layers the channels
// that the parts leave. Without a channel, the writer cuts the earlier note of that key short.
enum class SpareChannels
{
    kForParts,  // gives every spare channel to the tracks' other parts, track by track
    kForLayers, // keeps a channel for each layer that the parts' notes require, then gives the rest to other parts
};

// Fits the parts that each track of `perf` used (Performer::PartsUsed) into MIDI's 16 channels. Gives them out as the
// MIDI writer does: a channel for the first part of each track that plays notes, then the other parts, track by track.
// `spare` says whether the layers' channels come first. Returns the parts each track may have.
std::array<uint8_t, 16> FitParts(const seqlib::midi::Performance& perf, const std::array<uint8_t, 16>& used,
                                 SpareChannels spare = SpareChannels::kForParts);

// The parts that each track of `perf` may have, when the parts that the tracks used, `used`, don't fit MIDI's
// channels (FitParts with `spare`). Nothing when they fit.
std::optional<std::array<uint8_t, 16>> FittedParts(const seqlib::midi::Performance& perf,
                                                   const std::array<uint8_t, 16>& used,
                                                   SpareChannels spare = SpareChannels::kForParts);

// What a run of a performer gives RunUntilSettled: the performance, its highest CC11 value before the level scale, the
// parts each MIDI track used (FittedParts) and the main loop's choices (MainLoopPasses::Choices).
template <typename P>
struct RunOutcome
{
    P performance;
    double level_peak = 0.0;
    std::array<uint8_t, 16> parts_used{};
    std::vector<LoopChoice> choices;
};

// Runs a performer until its levels and parts settle, four runs at most, and returns the last run's performance with
// its level cut (SetLevelCut). It runs again when CC11 would pass 127, scaling every value equally (LevelScale), or on
// fewer parts when the tracks' parts don't fit MIDI's channels (FittedParts, the layers' channels first). Each run
// makes the earlier run's loop choices again. The run on fewer parts checks the last of them (LoopReplay::check_last).
// `run(level_scale, part_limits, replay)` runs the performer and returns its RunOutcome<P>.
template <typename P, typename Run>
P RunUntilSettled(Run run)
{
    std::array<uint8_t, 16> part_limits;
    part_limits.fill(16);
    double peak = 0.0;
    double applied = 0.0;
    bool limited = false;
    LoopReplay replay;
    P performance;
    for (int pass = 0; pass < 4; pass++)
    {
        RunOutcome<P> outcome = run(LevelScale(peak), part_limits, replay);
        performance = std::move(outcome.performance);
        applied = peak;
        replay = {std::move(outcome.choices), false};

        bool parts_fit = true;
        if (!limited)
        {
            if (const auto fitted = FittedParts(performance, outcome.parts_used, SpareChannels::kForLayers))
            {
                part_limits = *fitted;
                limited = true;
                parts_fit = false;
                replay.check_last = true;
            }
        }
        if (parts_fit && LevelScale(outcome.level_peak) == LevelScale(peak))
        {
            break;
        }

        peak = outcome.level_peak;
    }

    SetLevelCut(performance, applied);

    return performance;
}

} // namespace ntdlib::sequence_export
