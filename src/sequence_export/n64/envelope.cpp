// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/n64/envelope.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace ntdlib::sequence_export::n64
{

namespace
{

// A fall slower than this has no place in a SoundFont: its longest decay falls 100 dB in about 100 seconds. A note
// holds its level there.
constexpr double kSlowestFallDbPerSecond = 1.0;

// The time that a slow envelope's level is read at, in seconds, for a note that holds it.
constexpr double kTypicalNoteSeconds = 2.0;

// A release in a straight line of amplitude has the energy of a fall of 100 dB in a straight line of dB that takes this
// many times the line's time.
constexpr double kAmplitudeLineTo100Db = 10.0 / 3.0 * std::numbers::ln10;

// A rise whose square is the amplitude has the energy of a straight rise in amplitude that takes this share of its time
// and ends with it.
constexpr double kSquaredAttackShare = 0.6;

} // namespace

double LevelAt(const EnvelopeShape& shape, double updates)
{
    for (std::size_t i = 1; i < shape.points.size(); i++)
    {
        if (updates <= shape.points[i].updates)
        {
            const EnvelopePoint& a = shape.points[i - 1];
            const EnvelopePoint& b = shape.points[i];

            return a.level + (b.level - a.level) * (updates - a.updates) / (b.updates - a.updates);
        }
    }

    return shape.points.back().level;
}

// Fits an envelope's shape to a SoundFont's envelope. Returns the amplitude at the top of the envelope. `fade` is what
// the note's release takes from the level in each update.
double FitEnvelope(Zone& zone, const EnvelopeShape& shape, double updates_per_second, double fade)
{
    const double ms_per_update = 1000.0 / updates_per_second;
    std::size_t top = 0;
    for (std::size_t i = 1; i < shape.points.size(); i++)
    {
        if (shape.points[i].level > shape.points[top].level)
        {
            top = i;
        }
    }

    const double peak = shape.points[top].level;
    if (peak <= 0.0)
    {
        return 0.0;
    }

    const auto amplitude_of = [&](double level)
    {
        return shape.squared_lines ? level * level : level;
    };
    const double rise_ms = shape.points[top].updates * ms_per_update;
    const double start = shape.points[0].level / peak;
    if (start > 0.0)
    {
        // A SoundFont's attack starts from nothing. A shorter attack and a delay give the rise's energy.
        const double energy = (1.0 + start + start * start) / 3.0;
        zone.attack_ms = 2.0 * std::min(energy, 1.0 - energy) * rise_ms;
        zone.delay_ms = rise_ms > zone.attack_ms && energy < 0.5 ? rise_ms - zone.attack_ms : 0.0;
    }
    else
    {
        const double attack_share = shape.squared_lines ? kSquaredAttackShare : 1.0;
        zone.delay_ms = (1.0 - attack_share) * rise_ms;
        zone.attack_ms = attack_share * rise_ms;
    }

    double gain = amplitude_of(peak);
    const double top_updates = shape.points[top].updates;
    const double tail_updates = shape.points.back().updates - top_updates;
    const double final_level = shape.ends ? 0.0 : shape.points.back().level;
    double from = peak;
    if (tail_updates > 0.0 && final_level < peak)
    {
        // The energy of the fall, in updates at the top's level.
        constexpr int kSteps = 256;
        double energy = 0.0;
        for (int i = 0; i < kSteps; i++)
        {
            const double level = amplitude_of(LevelAt(shape, top_updates + (i + 0.5) * tail_updates / kSteps) / peak);
            energy += level * level;
        }

        energy *= tail_updates / kSteps / updates_per_second;

        // A fall at `rate` dB a second that reaches the sustain and holds it, over the same time.
        const double seconds = tail_updates / updates_per_second;
        const double sustain_db = final_level > 0.0 ? -20.0 * std::log10(amplitude_of(final_level / peak)) : 100.0;
        const auto model = [&](double rate)
        {
            const double reach = std::min(sustain_db / rate, seconds);
            const double fall = (1.0 - std::pow(10.0, -rate * reach / 10.0)) * 10.0 / (rate * std::numbers::ln10);
            const double hold = (seconds - reach) * std::pow(10.0, -sustain_db / 10.0);

            return fall + hold;
        };

        double low = 1e-3;
        double high = 1e6;
        for (int i = 0; i < 60; i++)
        {
            const double middle = std::sqrt(low * high);
            if (model(middle) > energy)
            {
                low = middle;
            }
            else
            {
                high = middle;
            }
        }

        const double rate = std::sqrt(low * high);
        if (rate < kSlowestFallDbPerSecond && final_level > 0.0)
        {
            // Too slow for a SoundFont. A note holds what it has after a typical length.
            const double typical = LevelAt(shape, top_updates + kTypicalNoteSeconds * updates_per_second);
            gain = amplitude_of(typical);
            from = typical;
        }
        else
        {
            zone.decay_ms = 100.0 * 1000.0 / rate;
            zone.sustain_db = sustain_db;
            from = final_level > 0.0 ? final_level : peak;
        }
    }
    else if (final_level > peak)
    {
        from = final_level;
    }

    const double line_updates = fade > 0.0 ? from / fade : updates_per_second * 100.0;
    zone.release_ms =
        (shape.squared_lines ? kSquaredLineTo100Db : kAmplitudeLineTo100Db) * line_updates * ms_per_update;

    return gain;
}

} // namespace ntdlib::sequence_export::n64
