// SPDX-License-Identifier: MIT

// Fits an engine's envelope to SoundFont's: a delay, an attack in a straight line of amplitude, a decay in a straight
// line of decibels, a sustain and a release. An engine's envelope is a list of straight lines.

#pragma once

#include <numbers>
#include <vector>

#include "ntdlib/sequence_export/n64/instruments.h"

namespace ntdlib::sequence_export::n64
{

// A straight line from a level to nothing, whose square is the amplitude, has the energy of a release in a straight
// line of decibels that falls 100 dB in this many times the line's time (2 ln 10).
inline constexpr double kSquaredLineTo100Db = 2.0 * std::numbers::ln10;

// Where an envelope stands, on the scale whose square is the note's amplitude.
struct EnvelopePoint
{
    double updates = 0.0;
    double level = 0.0;
};

struct EnvelopeShape
{
    std::vector<EnvelopePoint> points;
    bool ends = false;          // the end of the list ends the note
    bool squared_lines = false; // the amplitude is the square of the line, and not the line
};

// The level of `shape` after `updates` of its updates.
double LevelAt(const EnvelopeShape& shape, double updates);

// Fits an envelope's shape to a SoundFont's envelope. Returns the amplitude at the top of the envelope. `fade` is what
// the note's release takes from the level in each update. A shape's first point may stand above 0.
double FitEnvelope(Zone& zone, const EnvelopeShape& shape, double updates_per_second, double fade);

} // namespace ntdlib::sequence_export::n64
