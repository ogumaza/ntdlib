// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/agb/module_bars.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ntdlib/sequence_export/agb/midi.h"

namespace ntdlib::sequence_export::agb
{

namespace
{

// Target tempo range in BPM, with the upper bound excluded. Doubling the rows per beat halves the tempo, so exactly one
// power-of-two choice falls within a twofold range.
constexpr double kSlowestBeat = 105;
constexpr double kFastestBeat = 210;

// The most rows that a beat, or an eighth note in 12/8, can have.
constexpr uint32_t kMaxBeatRows = 64;

// The quarter notes in a bar of 4/4, and in one of 12/8.
constexpr uint32_t kSimpleBarQuarters = 4;
constexpr uint32_t kCompoundBarQuarters = 6;

// Writes the time signature at `tick` for a bar of `ticks` MIDI ticks.
void BarSignature(MidiTrack& conductor, uint32_t tick, size_t ticks, const Meter& meter)
{
    size_t note = meter.compound ? meter.quarter_ticks / 2 : meter.quarter_ticks;
    int pow2 = meter.compound ? 3 : 2;
    while (ticks % note != 0 && note % 2 == 0 && pow2 < 7)
    {
        note /= 2;
        pow2++;
    }
    conductor.TimeSignature(tick, int(ticks / note), pow2);
}

} // namespace

std::vector<OrderRun> OrderRuns(const std::vector<ModuleStep>& steps)
{
    std::vector<OrderRun> runs;
    int last_row = -1;
    for (size_t i = 0; i < steps.size(); i++)
    {
        const ModuleStep& s = steps[i];
        if (!s.row)
        {
            continue;
        }

        if (runs.empty() || s.order != runs.back().order || s.pattern_row <= last_row)
        {
            if (!runs.empty())
            {
                runs.back().steps = i - runs.back().start;
            }
            runs.push_back({i, 0, s.order, s.pattern_row});
        }
        last_row = s.pattern_row;
    }
    if (!runs.empty())
    {
        runs.back().steps = steps.size() - runs.back().start;
    }

    return runs;
}

size_t BarTicks(const Meter& meter)
{
    return size_t(meter.quarter_ticks) * (meter.compound ? kCompoundBarQuarters : kSimpleBarQuarters);
}

Meter ChooseMeter(const std::vector<uint32_t>& ticks, const std::vector<OrderRun>& runs, uint32_t row_ticks,
                  double tick_seconds)
{
    const auto bpm = [&](uint32_t beat_ticks)
    {
        return 60 / (beat_ticks * tick_seconds);
    };

    // Double the rows per quarter note (4/4) or eighth note (12/8) until the beat tempo falls below kFastestBeat, or
    // the row limit is reached.
    uint32_t quarter_rows = 1, eighth_rows = 1;
    while (bpm(quarter_rows * row_ticks) >= kFastestBeat && quarter_rows < kMaxBeatRows)
    {
        quarter_rows *= 2;
    }
    while (bpm(3 * eighth_rows * row_ticks) >= kFastestBeat && eighth_rows < kMaxBeatRows)
    {
        eighth_rows *= 2;
    }
    const Meter simple = {quarter_rows * row_ticks, false};
    const Meter compound = {2 * eighth_rows * row_ticks, true};

    // The MIDI ticks of the runs that make whole bars.
    const auto whole = [&](const Meter& meter)
    {
        size_t sum = 0;
        for (size_t r = 1; r + 1 < runs.size(); r++)
        {
            const size_t length = ticks[runs[r].start + runs[r].steps] - ticks[runs[r].start];
            if (length % BarTicks(meter) == 0)
            {
                sum += length;
            }
        }

        return sum;
    };

    if (bpm(3 * eighth_rows * row_ticks) >= kSlowestBeat && whole(compound) > whole(simple))
    {
        return compound;
    }

    return simple;
}

std::vector<uint32_t> BarStarts(const std::vector<ModuleStep>& steps, const std::vector<uint32_t>& ticks,
                                const std::vector<OrderRun>& runs, size_t bar)
{
    std::vector<uint32_t> starts;
    if (runs.empty())
    {
        return starts;
    }

    const OrderRun& first = runs[0];
    const bool cut = runs.size() > 1 && runs[1].order != first.order;
    const size_t pickup = cut ? (ticks[first.start + first.steps] - ticks[first.start]) % bar : 0;
    for (const OrderRun& run : runs)
    {
        const size_t row = steps[run.start].row_ticks;
        const size_t from = run.order == first.order ? pickup : 0;
        size_t at = size_t(run.first_row) * row;
        for (size_t i = run.start; i < run.start + run.steps; i++)
        {
            if (steps[i].row && (i == 0 || at == 0 || (at >= from && (at - from) % bar == 0)))
            {
                starts.push_back(ticks[i]);
            }
            at += ticks[i + 1] - ticks[i];
        }
    }

    return starts;
}

void WriteBarSignatures(MidiTrack& conductor, const std::vector<uint32_t>& bars, const Meter& meter)
{
    size_t bar_ticks = 0;
    for (size_t b = 0; b + 1 < bars.size(); b++)
    {
        const size_t ticks = size_t(bars[b + 1] - bars[b]);
        if (ticks != bar_ticks)
        {
            BarSignature(conductor, bars[b], ticks, meter);
            bar_ticks = ticks;
        }
    }
    if (bar_ticks == 0)
    {
        BarSignature(conductor, 0, BarTicks(meter), meter);
    }
}

} // namespace ntdlib::sequence_export::agb
