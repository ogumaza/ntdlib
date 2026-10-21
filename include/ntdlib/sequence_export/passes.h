// SPDX-License-Identifier: MIT

// The rules for the passes of a sequence's main loop. A performer records the passes as MIDI events. The functions here
// find the settings that a pass starts with, and test whether a MIDI player that loops on a pass plays the repeats as
// the game plays the next pass. ChooseSteadyPass chooses the pass to mark. MainLoopPasses holds the passes that a
// performer counts, and the marked pass. RestoreAtLoopStart sends again, at the loop's start, the settings that the
// game gives back there. FitParts fits a performance's parts into MIDI's 16 channels, and LevelScale fits the levels
// into CC11. SetLevelCut records the cut that LevelScale makes.

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

// The passes of a performance's main loop that a performer has counted, and the marked pass. Each counted pass is a
// loop of the performance (CountPass). Only the passes of one loop, the owner, can be marked. The performer's rules
// choose the owner (Restart). MarkSteadyPass marks the pass that ChooseSteadyPass selects. A later run of the sequence
// makes the first run's choices again (Choices).
class MainLoopPasses
{
public:
    // MarkSteadyPass makes the choices in `replay`, from an earlier run (Choices), before it selects others.
    explicit MainLoopPasses(std::vector<LoopChoice> replay);

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

    // Drops `out`'s main loop and the counted passes. The passes of `owner`, if any, can be marked from now on.
    void Restart(PerformanceBase& out, const std::optional<std::pair<int, uint32_t>>& owner);

    // Counts the pass of `loop` from tick `start` to tick `end` as a loop of `out`. A pass of the owner can be marked
    // later. Without a loop, `out` takes the pass as its loop.
    void CountPass(PerformanceBase& out, const std::pair<int, uint32_t>& loop, uint32_t start, uint32_t end);

    // Marks in `out` the owner's pass that ChooseSteadyPass selects, or makes the earlier run's next choice.
    // `last_draw`, `loops` and `play_on` are as ChooseSteadyPass takes them. Returns true if the performance can stop,
    // false if it requires another pass first.
    template <typename PlayOn>
    bool MarkSteadyPass(PerformanceBase& out, const std::optional<uint32_t>& last_draw, int loops, PlayOn play_on);

private:
    std::optional<std::pair<int, uint32_t>> owner_;
    std::vector<std::pair<uint32_t, uint32_t>> passes_; // the owner's counted passes, start and end ticks
    std::size_t marked_ = 0;                            // the marked one of them
    bool extended_ = false;                             // one more pass has played to mark one (LoopChoice::extend)
    std::vector<LoopChoice> replay_;                    // an earlier run's choices to make again
    std::size_t replayed_ = 0;                          // how many of them have been made
    std::vector<LoopChoice> choices_;                   // the choices made
};

template <typename PlayOn>
bool MainLoopPasses::MarkSteadyPass(PerformanceBase& out, const std::optional<uint32_t>& last_draw, int loops,
                                    PlayOn play_on)
{
    const LoopChoice choice = replayed_ < replay_.size()
                                  ? replay_[replayed_++]
                                  : ChooseSteadyPass(passes_, marked_, last_draw, extended_, loops, play_on);
    choices_.push_back(choice);
    if (choice.marked && *choice.marked < passes_.size())
    {
        marked_ = *choice.marked;
        const auto [start, end] = passes_[marked_];
        out.loop = seqlib::midi::Performance::Loop{start, end};
        out.loops = static_cast<int>(passes_.size() - marked_);
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

} // namespace ntdlib::sequence_export
