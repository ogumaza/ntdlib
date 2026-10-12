// SPDX-License-Identifier: MIT

// The runs of MIDI values that select the same entry of a bank's tables, for both bank models (instruments.cpp and
// rsar_instruments.cpp).

#pragma once

#include <utility>
#include <vector>

namespace ntdlib::sequence_export
{

// Contiguous runs of the values 0-127 for which `owner(v)` is true.
template <typename F>
std::vector<std::pair<int, int>> Runs(F owner)
{
    std::vector<std::pair<int, int>> runs;
    for (int v = 0; v < 128; v++)
    {
        if (!owner(v))
        {
            continue;
        }

        if (!runs.empty() && runs.back().second == v - 1)
        {
            runs.back().second = v;
        }
        else
        {
            runs.push_back({v, v});
        }
    }

    return runs;
}

} // namespace ntdlib::sequence_export
