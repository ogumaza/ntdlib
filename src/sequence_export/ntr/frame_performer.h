// SPDX-License-Identifier: MIT

// What the performers of the DS's sound archives (ntr/performance*.cpp) and of DSE (dse/performance*.cpp) share. Each
// runs its driver one step at a time and records the sequence's ticks at MIDI's tempo. A frame here is the driver's
// step: a frame of the DS's driver, or a tick of DSE's driver. The header contains the state of the recording, the
// clock that places the frames among the ticks, the probe of the main loop and the steps of a performance's end.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "seqlib/midi/performance.h"
#include "seqlib/midi/recorder.h"

#include "ntdlib/sequence_export/passes.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/performance_base.h"
#include "sequence_export/lengths.h"

namespace ntdlib::sequence_export::detail
{

// How the recorder places events (seqlib/midi/recorder.h). A setting replaces an earlier value of its kind anywhere in
// its tick. A part switch sends the new part the old part's values. No tick's frame starts before tick 0.
inline constexpr seqlib::midi::RecorderOptions kFrameRecorderOptions{
    .replace_anywhere_in_tick = true, .resend_on_switch = true, .frame_ticks_from_zero = true};

// The rules of the main loop's passes (ntdlib/sequence_export/passes.h). The notes take no settings that MIDI seeds.
inline const LoopRules kFrameLoopRules{.seeded = {}, .preset_with_settings = false};

// How far the performer looks for the steady pass (SearchSteadyPass): up to pass 4, then blocks of up to three passes,
// and no further than a file's limit.
inline const PassSearch kFramePassSearch{.last_pass = 4, .longest_block = 3, .max_ms = kMaxSeconds * 1000.0};

// A probe of the main loop (the performer's PlayOn): a test of a pass from a start to an end (PassTest), the owner's
// passes that the probe counted, and the whole period at its end (WholeSpan).
struct FrameProbe
{
    std::function<bool(uint32_t, uint32_t, PassTest)> test;
    std::vector<CountedPass> passes;
    std::size_t whole_span = 0;

    bool operator()(uint32_t start, uint32_t end, PassTest kind) const
    {
        return test(start, end, kind);
    }
};

// A pass of the main loop that waits for the end of the driver's frame: the track, and the ticks it started and ended
// in.
struct PendingLoop
{
    int track = 0;
    uint32_t start = 0;
    uint32_t end = 0;
};

// What a performer records, apart from the performance and the driver: the recorder and its MIDI tracks, the main
// loop's passes and the clock. A probe copies it and puts it back (the performer's PlayOn).
struct FrameState
{
    // A frame of the driver lasts `frame_ms`. A tick lasts `ms_per_tick` until the sequence's tempo sets it. The
    // recorder multiplies CC11 by `level_scale` and gives each MIDI track at most `part_limits` parts. The main loop
    // makes its first choices as `replay` gives them.
    FrameState(const LoopReplay& replay, double frame_ms, double ms_per_tick, double level_scale,
               const std::array<uint8_t, 16>& part_limits)
        : main_loop(replay), frame_ms(frame_ms), ms_per_tick(ms_per_tick)
    {
        recorder.SetLevelScale(level_scale);
        recorder.SetPartLimits(part_limits);
        for (std::size_t t = 0; t < outputs.size(); t++)
        {
            outputs[t].midi_track = static_cast<uint8_t>(t);
        }
    }

    // The time of frame `at` from the frame of tick 0, in milliseconds.
    double FrameMs(uint64_t at) const
    {
        return static_cast<double>(at) * frame_ms - static_cast<double>(first_frame) * frame_ms;
    }

    // The nominal time of tick `at` in milliseconds. A tick after the last counts on at the last tick's tempo.
    double TickMs(uint32_t at) const
    {
        if (at < tick_ms.size())
        {
            return tick_ms[at];
        }

        return nominal_ms + static_cast<double>(at - tick) * ms_per_tick;
    }

    // The first tick at or after `ms`.
    uint32_t TickAt(double ms) const
    {
        if (ms <= nominal_ms)
        {
            return static_cast<uint32_t>(std::lower_bound(tick_ms.begin(), tick_ms.end(), ms) - tick_ms.begin());
        }

        return tick + static_cast<uint32_t>(std::ceil((ms - nominal_ms) / ms_per_tick));
    }

    // How far after the last tick the frame that has run starts, in ticks at MIDI's tempo. The events of the tick's
    // frame go where the frame starts (Performance::frame_ticks).
    double FrameAfter() const
    {
        if (frame == tick_frame)
        {
            return 0.0;
        }

        return std::max((FrameMs(frame) - nominal_ms) / ms_per_tick, 0.0);
    }

    // The sequence's first tick, in the frame that runs: tick 0 starts there (`frame_ticks`, Performance::frame_ticks).
    void StartTicks(std::vector<double>& frame_ticks)
    {
        ticked = true;
        first_frame = frame;
        tick_frame = frame;
        tick_ms.push_back(0.0);
        frame_ticks.push_back(0.0);
    }

    // The sequence's next tick, in the frame that runs, at MIDI's tempo. The tick's frame starts where the frame does,
    // or with the tick before (`frame_ticks`).
    void NextTick(std::vector<double>& frame_ticks)
    {
        tick++;
        tick_frame = frame;
        nominal_ms += ms_per_tick;
        tick_ms.push_back(nominal_ms);
        const double start = static_cast<double>(tick) + (FrameMs(frame) - nominal_ms) / ms_per_tick;
        frame_ticks.push_back(std::max(start, frame_ticks.back()));
    }

    // The end of the frame that runs, in which a voice sounds where `sounding`.
    void FinishFrame(bool sounding)
    {
        if (sounding)
        {
            last_sound_frame = frame;
        }
        frame++;
    }

    // Counts a pass of the main loop's `owner` from tick `start` to the last tick. Once the main loop has played
    // `loops` passes and kMinLoopSeconds, the pass of track `track` waits for the frame's end (PendingLoop).
    void CountPass(PerformanceBase& out, const std::pair<int, uint32_t>& owner, int track, uint32_t start, int loops)
    {
        main_loop.CountPass(out, owner, start, tick);
        if (out.loops >= loops && TickMs(tick) >= kMinLoopSeconds * 1000.0)
        {
            pending = PendingLoop{track, start, tick};
        }
    }

    // The end of a sound that the main loop holds: kHoldSeconds after the last change, and no later than the last
    // tick.
    double HeldSoundEnd() const
    {
        return std::min(last_change_ms + kHoldSeconds * 1000.0, TickMs(tick));
    }

    seqlib::midi::Recorder recorder{kFrameRecorderOptions};
    std::array<seqlib::midi::TrackOutput, 16> outputs{};
    MainLoopPasses main_loop;
    std::optional<PendingLoop> pending;

    double frame_ms = 0.0;    // a frame of the driver in milliseconds
    bool ticked = false;      // the sequence has run its first tick
    uint32_t tick = 0;        // the last tick
    uint64_t frame = 0;       // the frame that runs, or the frames run between ticks
    uint64_t first_frame = 0; // the frame of tick 0
    uint64_t tick_frame = 0;  // the frame of the last tick
    double nominal_ms = 0.0;  // the last tick's time at MIDI's tempo
    double ms_per_tick = 0.0;
    std::vector<double> tick_ms; // each tick's nominal time
    uint64_t last_sound_frame = 0;
    double last_change_ms = 0.0; // the time of the last note-on or setting
};

// Plays the copy that a probe makes of a performer on, one frame at a time through `advance`. `advance` returns false
// once the sequence has ended. The copy plays on to tick `until` and kProbeSeconds at most. Where `enough` isn't 0, it
// stops one tick after the end of the owner's pass number `enough`. The search tests only the passes whose next pass
// the copy counted (SearchSteadyPass).
template <typename Advance>
void PlayProbe(FrameState& s, uint32_t until, std::size_t enough, const Advance& advance)
{
    while (s.tick < until && advance())
    {
        s.pending.reset();
        const std::vector<std::pair<uint32_t, uint32_t>>& counted = s.main_loop.Passes();
        if ((enough > 0 && counted.size() >= enough && s.tick > counted[enough - 1].second) ||
            s.FrameMs(s.frame) >= kProbeSeconds * 1000.0)
        {
            break;
        }
    }
}

// The passes that the main loop has counted, with the time of each pass's end.
inline std::vector<CountedPass> CountedPasses(const FrameState& s)
{
    std::vector<CountedPass> passes;
    for (const auto& [start, end] : s.main_loop.Passes())
    {
        passes.push_back({start, end, s.TickMs(end)});
    }

    return passes;
}

// The whole period over the owner's last pass of the tracks that play, whose loops last `lengths` ticks (WholeSpan). 0
// before the owner's first pass.
inline std::size_t LastPassSpan(const FrameState& s, const std::vector<uint32_t>& lengths)
{
    const std::vector<std::pair<uint32_t, uint32_t>>& passes = s.main_loop.Passes();
    if (passes.empty() || passes.back().second <= passes.back().first)
    {
        return 0;
    }

    return WholeSpan(passes.back().second - passes.back().first, lengths);
}

// Records the volume and pan `values` of a track or channel in tick `tick`, in `changes`, where they differ from the
// values that `mixed` holds from the last change. A change replaces the tick's earlier change of its setting.
inline void RecordMixValues(std::vector<MixChange>& changes, std::array<int16_t, 2>& mixed,
                            const std::array<std::pair<Mix, int16_t>, 2>& values, uint32_t tick)
{
    for (std::size_t i = 0; i < values.size(); i++)
    {
        const auto [setting, value] = values[i];
        if (value == mixed[i])
        {
            continue;
        }

        mixed[i] = value;
        const auto matches = [&](const MixChange& change)
        {
            return change.tick == tick && change.setting == setting;
        };
        const auto same = std::find_if(changes.rbegin(), changes.rend(), matches);
        if (same != changes.rend())
        {
            same->value = value;
        }
        else
        {
            changes.push_back({tick, setting, value});
        }
    }
}

// Whether a performance that has reached kMaxSeconds at `now` ms has finished: nothing sounds where `sounding` is
// false, and the last sound died away more than kHoldSeconds before. A performance that hasn't finished is truncated.
inline bool FinishedAtTimeLimit(const FrameState& s, double now, bool sounding)
{
    return !sounding && now - s.FrameMs(s.last_sound_frame) > kHoldSeconds * 1000.0;
}

// The tick at which a performance that stops at the end of a pass ends, or nothing for one that doesn't. A main loop
// that holds a sound ends at `held_sound_end` ms, kHoldSeconds after its last change, without loop markers. Any other
// stop ends at `stop_tick`.
inline std::optional<uint32_t> PassEndTick(PerformanceBase& out, const FrameState& s,
                                           const std::optional<double>& held_sound_end,
                                           const std::optional<uint32_t>& stop_tick)
{
    if (held_sound_end)
    {
        out.holds = true;
        out.DropLoop();
        return std::min(s.TickAt(*held_sound_end), s.tick);
    }

    return stop_tick;
}

// Leaves out the entries of each list of `lists` from tick `end` on.
template <typename Lists>
void DropFromTick(Lists& lists, uint32_t end)
{
    for (auto& list : lists)
    {
        std::erase_if(list, [end](const auto& entry) { return entry.tick >= end; });
    }
}

// Ends a performance that stops at the end of a pass at tick `end`, after its last notes have ended there. The
// stretches that begin in the end's tick, and the mix changes there, belong to the next pass.
template <typename Stretches, typename MixChanges>
void EndAtPass(PerformanceBase& out, const FrameState& s, uint32_t end, Stretches& stretches, MixChanges& mix)
{
    out.end_tick = end;
    out.seconds = s.TickMs(end) / 1000.0;
    seqlib::midi::ClampNoteOffs(out, out.end_tick);
    DropFromTick(stretches, end);
    DropFromTick(mix, end);
}

// Plays the release tails of a finished sequence out, up to kMaxTailSeconds, one frame at a time through `run_frame`,
// while `sounding` returns true. Returns the time in milliseconds where the last sound ends.
template <typename Sounding, typename RunFrame>
double PlayTails(FrameState& s, const Sounding& sounding, const RunFrame& run_frame)
{
    const auto limit = s.frame + static_cast<uint64_t>(kMaxTailSeconds * 1000.0 / s.frame_ms);
    while (sounding() && s.frame < limit)
    {
        run_frame();
    }

    return s.FrameMs(sounding() ? s.frame : s.last_sound_frame + 1);
}

// Ends a performance that doesn't stop at the end of a pass, at `end_ms`, once its notes have ended `after` ticks after
// the last tick. The performance lasts until its last event or the end, whichever comes later. A finished sequence
// whose last change comes long after its last sound ends where the sound dies away (EndsAtLastSound). `drop_from(end)`
// then leaves out the editor's records from tick `end` on.
template <typename DropFrom>
void EndAtLastEvent(PerformanceBase& out, const FrameState& s, double end_ms, double after, bool finished,
                    const DropFrom& drop_from)
{
    uint32_t last_event = 0;
    for (const std::vector<seqlib::midi::TrackEvent>& events : out.tracks)
    {
        for (const seqlib::midi::TrackEvent& e : events)
        {
            last_event = std::max(last_event, e.tick + static_cast<uint32_t>(std::ceil(e.after)));
        }
    }
    out.end_tick = std::max({last_event, s.tick + static_cast<uint32_t>(std::ceil(after))});
    out.seconds = std::max(end_ms, s.TickMs(s.tick)) / 1000.0;

    if (finished && EndsAtLastSound(end_ms / 1000.0, std::max(s.TickMs(last_event), s.TickMs(s.tick)) / 1000.0))
    {
        const uint32_t end = s.TickAt(end_ms);
        seqlib::midi::CutAt(out, end);
        drop_from(end);
        std::erase_if(out.tempo, [end](const auto& tempo) { return tempo.tick > end; });
        out.end_tick = end;
        out.seconds = end_ms / 1000.0;
    }
    seqlib::midi::ClampNoteOffs(out, out.end_tick);
}

// Adds the recorder's approximations and warnings to `out`.
inline void AddRecorderNotes(PerformanceBase& out, const seqlib::midi::Recorder& recorder)
{
    for (const auto& [what, count] : recorder.Approximations())
    {
        out.approximations[what] += count;
    }
    out.warnings.insert(out.warnings.end(), recorder.Warnings().begin(), recorder.Warnings().end());
}

// Runs a performer until its levels and parts settle (RunUntilSettled), and returns the last run's performance.
// `make(level_scale, part_limits, replay)` returns a new performer with those arguments.
template <typename P, typename MakePerformer>
P PerformUntilSettled(const MakePerformer& make)
{
    const auto run = [&make](double level_scale, const std::array<uint8_t, 16>& part_limits, const LoopReplay& replay)
    {
        auto performer = make(level_scale, part_limits, replay);
        P performance = performer.Run();
        return RunOutcome<P>{std::move(performance), performer.LevelPeak(), performer.PartsUsed(),
                             performer.LoopChoices()};
    };

    return RunUntilSettled<P>(run);
}

} // namespace ntdlib::sequence_export::detail
