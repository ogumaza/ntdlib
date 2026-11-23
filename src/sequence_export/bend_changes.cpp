// SPDX-License-Identifier: MIT

// The bends that the parts' pitches stand for (see bend_changes.h).

#include "sequence_export/bend_changes.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace ntdlib::sequence_export
{

void AddBendChange(BendChanges& changes, int number, uint8_t part, const BendChange& change)
{
    // The changes stay in order of time, as the lookup in BendOf requires.
    std::vector<BendChange>& list = changes[{number, part}];
    const auto later = std::ranges::upper_bound(list, std::pair(change.tick, change.after), {},
                                                [](const BendChange& c) { return std::pair(c.tick, c.after); });
    list.insert(later, change);
}

std::pair<float, double> BendOf(const BendChanges& changes, int number, const seqlib::midi::TrackEvent& e)
{
    const auto part = changes.find({number, e.part});
    if (part == changes.end())
    {
        return {e.semitones, 0.0};
    }

    const std::vector<BendChange>& list = part->second;
    const auto later = std::ranges::upper_bound(list, std::pair(e.tick, e.after), {},
                                                [](const BendChange& c) { return std::pair(c.tick, c.after); });
    if (later == list.begin())
    {
        return {e.semitones, 0.0};
    }

    const BendChange& change = *std::prev(later);
    return {change.bend.value_or(e.semitones), change.rounding};
}

bool SamePitch(const BendChanges& changes, int number, const seqlib::midi::TrackEvent& a,
               const seqlib::midi::TrackEvent& b)
{
    const auto [bend_a, rounding_a] = BendOf(changes, number, a);
    const auto [bend_b, rounding_b] = BendOf(changes, number, b);
    return bend_a == bend_b && std::abs(double{a.semitones} - double{b.semitones}) <= std::max(rounding_a, rounding_b);
}

} // namespace ntdlib::sequence_export
