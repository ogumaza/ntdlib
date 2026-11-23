// SPDX-License-Identifier: MIT

// The runs of a zone's keys that share one tuning, for the DS's drivers. SDAT's driver (ntr/instruments.h) and DSE's
// driver (dse/instruments.h) each work out a channel's timer from the key and drop the fraction of a division. A zone
// then tunes each run of keys to the rate at which the timer plays them.

#pragma once

#include <cstdint>
#include <set>
#include <vector>

namespace ntdlib::sequence_export
{

// A run of a zone's keys, and the key whose tuning they take: -1 where the zone holds none of the played keys.
struct KeyRun
{
    int low_key = 0;
    int high_key = 0;
    int tuned_key = -1;
};

// The keys from `low_key` to `high_key` in runs that share one tuning. `tuning(key)` gives the tuning at a key, in any
// type whose values compare equal for equal tunings. Without `keys`, each key takes its tuning. With `keys`, the keys
// that the sequence plays, each key takes the tuning of the played key itself, or else of the nearest played key below
// it, or else of the lowest played key above it. A key that no note plays then adds no run. A zone that holds none of
// `keys` is one run whose tuned key is -1. A range whose low key is above its high key gives no run.
template <typename F>
std::vector<KeyRun> TuningRuns(int low_key, int high_key, const std::set<uint8_t>* keys, F tuning)
{
    if (low_key > high_key)
    {
        return {};
    }

    int played = -1;
    if (keys != nullptr)
    {
        const auto first = keys->lower_bound(static_cast<uint8_t>(low_key));
        if (first == keys->end() || *first > high_key)
        {
            return {{.low_key = low_key, .high_key = high_key, .tuned_key = -1}};
        }
        played = *first;
    }

    std::vector<KeyRun> runs;
    decltype(tuning(0)) run_tuning{};
    for (int key = low_key; key <= high_key; key++)
    {
        if (keys != nullptr && keys->contains(static_cast<uint8_t>(key)))
        {
            played = key;
        }

        const int tuned = keys == nullptr ? key : played;
        const auto key_tuning = tuning(tuned);
        if (runs.empty() || key_tuning != run_tuning)
        {
            runs.push_back({.low_key = key, .high_key = key, .tuned_key = tuned});
            run_tuning = key_tuning;
        }
        runs.back().high_key = key;
    }

    return runs;
}

} // namespace ntdlib::sequence_export
