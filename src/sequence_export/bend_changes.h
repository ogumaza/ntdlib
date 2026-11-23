// SPDX-License-Identifier: MIT

// The bends that the parts' pitches stand for, for the pass tests of the DS's performers (ntr/performance.cpp and
// dse/performance.cpp). A part that plays a bent note alone sends the pitch at which the voice's timer plays the bend,
// and a part that plays several notes sends the bend itself. A pass can play a note alone where the next doesn't. The
// two pitches then differ by no more than the timer's rounding.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "seqlib/midi/performance.h"

namespace ntdlib::sequence_export
{

// A change of the bend that a part's pitch stands for, from `tick` and `after` on: the bend in semitones, or none where
// the part's pitch is the bend itself, and the most by which the pitch can differ from the bend through the timer's
// rounding, 0 where the part plays the bend itself.
struct BendChange
{
    uint32_t tick = 0;
    double after = 0.0;
    std::optional<float> bend;
    double rounding = 0.0;
};

// Each part's changes, by track and part, in order of time.
using BendChanges = std::map<std::pair<int, uint8_t>, std::vector<BendChange>>;

// Records `change` of part `part` of track `number` among the part's changes.
void AddBendChange(BendChanges& changes, int number, uint8_t part, const BendChange& change);

// The bend that pitch event `e` of track `number` stands for, and the timer's rounding of it (BendChange).
std::pair<float, double> BendOf(const BendChanges& changes, int number, const seqlib::midi::TrackEvent& e);

// Whether pitches `a` and `b` of track `number` play as the same: both stand for the same bend, and they differ by no
// more than the larger of the timer's roundings at the two sends.
bool SamePitch(const BendChanges& changes, int number, const seqlib::midi::TrackEvent& a,
               const seqlib::midi::TrackEvent& b);

} // namespace ntdlib::sequence_export
