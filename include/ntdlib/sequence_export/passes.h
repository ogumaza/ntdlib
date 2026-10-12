// SPDX-License-Identifier: MIT

// The rules for the passes of a sequence's main loop. A performer records the passes as MIDI events. The functions here
// find the settings that a pass starts with, and test whether a MIDI player that loops on a pass plays the repeats as
// the game plays the next pass. ChooseSteadyPass chooses the pass to mark. RestoreAtLoopStart sends again, at the
// loop's start, the settings that the game gives back there. FitParts fits a performance's parts into MIDI's 16
// channels, and LevelScale fits the levels into CC11.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include "seqlib/midi/performance.h"

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
// whether the performance plays one more pass to mark it.
struct LoopChoice
{
    bool stop = false;
    std::optional<std::size_t> marked;
    bool extend = false;
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
// same settings and tempo. The MIDI player keeps the loop end's values, except for those that the pass sends before a
// note and those that RestoreAtLoopStart sends again at the loop's start. The notes of a pass and of the next may go to
// different parts of a track, each with the track's settings.
bool RepeatsAsTheGame(const seqlib::midi::Performance& perf, const std::vector<PartSwitch>& part_switches,
                      uint32_t start, uint32_t end, const LoopRules& rules);

// Sends at the main loop's start each setting, and the tempo, that moves during the loop and that the game gives back
// its value at the loop's start in `next_pass`. A MIDI player that loops on the markers otherwise keeps the loop end's
// value. MIDI only sends a value when it changes.
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
        return {true, std::nullopt, false};
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
            return {static_cast<int>(owner_passes.size() - i) >= loops, i, false};
        }
    }

    // No pass so far plays as the next one does. If the next pass plays as the one after it, play that pass too.
    // Otherwise keep the first.
    if (!extended && repeats(last_end, last_end + length))
    {
        return {false, std::nullopt, true};
    }

    return {true, std::nullopt, false};
}

// Scale factor that keeps rounded CC11 values within 127 for a maximum unscaled value of `peak` (Perform).
double LevelScale(double peak);

// Fits the parts that each track of `perf` used (Performer::PartsUsed) into MIDI's 16 channels. Gives them out as the
// MIDI writer does: a channel for the first part of each track that plays notes, then the other parts, track by track.
// Returns the parts each track may have.
std::array<uint8_t, 16> FitParts(const seqlib::midi::Performance& perf, const std::array<uint8_t, 16>& used);

} // namespace ntdlib::sequence_export
