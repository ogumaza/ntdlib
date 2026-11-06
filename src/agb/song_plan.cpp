// SPDX-License-Identifier: MIT

#include "ntdlib/agb/song_plan.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <tuple>
#include <utility>
#include <vector>

namespace ntdlib::agb
{

uint64_t LoopLength(const std::vector<uint64_t>& lengths)
{
    const uint64_t longest = *std::max_element(lengths.begin(), lengths.end());
    const uint64_t most = longest * kMaxLoopFactor;
    uint64_t length = 1;
    for (uint64_t l : lengths)
    {
        const uint64_t times = length / std::gcd(length, l);
        if (times > most / l)
        {
            return longest;
        }

        length = times * l;
    }

    return length;
}

SongPlan PlanSong(const std::vector<TrackLayout>& tracks, int loops, LoopStart start_pass)
{
    SongPlan plan;
    uint64_t longest_end = 0;
    uint64_t last_command = 0;
    std::vector<uint64_t> lengths;
    for (const TrackLayout& layout : tracks)
    {
        if (layout.loops && layout.loop_end > layout.loop_start)
        {
            plan.loops = true;
            plan.loop_start = std::max(plan.loop_start, layout.loop_start);
            lengths.push_back(layout.loop_end - layout.loop_start);
        }
        else if (!layout.loops)
        {
            longest_end = std::max(longest_end, layout.end);
            last_command = std::max(last_command, layout.last_command);
        }
    }

    if (plan.loops)
    {
        const uint64_t length = LoopLength(lengths);
        plan.loop_start = std::max(plan.loop_start, last_command) + (start_pass == LoopStart::kSecondPass ? length : 0);
        plan.loop_end = plan.loop_start + length;
        plan.end = plan.loop_start + uint64_t(loops) * length;
    }
    else
    {
        plan.end = longest_end;
    }

    return plan;
}

PassComparison::PassComparison(uint64_t start, uint64_t length) : start_(start), length_(length)
{
}

void PassComparison::Add(uint64_t on, int channel, int key, std::vector<double> settings)
{
    if (on < start_ || length_ == 0)
    {
        return;
    }

    const uint64_t pass = (on - start_) / length_;
    if (pass < 2)
    {
        passes_[pass].push_back({on - start_ - pass * length_, channel, key, std::move(settings)});
    }
}

bool PassComparison::Alike() const
{
    std::array<std::vector<Start>, 2> sorted = passes_;
    for (std::vector<Start>& pass : sorted)
    {
        std::sort(pass.begin(), pass.end());
    }

    return sorted[0] == sorted[1];
}

bool PassComparison::Alike(double tolerance) const
{
    const auto earlier = [](const Start& a, const Start& b)
    {
        return std::tie(a.at, a.channel, a.key) < std::tie(b.at, b.channel, b.key);
    };
    std::array<std::vector<Start>, 2> sorted = passes_;
    for (std::vector<Start>& pass : sorted)
    {
        std::stable_sort(pass.begin(), pass.end(), earlier);
    }

    const auto near = [&](const Start& a, const Start& b)
    {
        if (std::tie(a.at, a.channel, a.key) != std::tie(b.at, b.channel, b.key) ||
            a.settings.size() != b.settings.size())
        {
            return false;
        }

        for (size_t i = 0; i < a.settings.size(); i++)
        {
            if (!(std::fabs(a.settings[i] - b.settings[i]) <= tolerance))
            {
                return false;
            }
        }

        return true;
    };

    return std::equal(sorted[0].begin(), sorted[0].end(), sorted[1].begin(), sorted[1].end(), near);
}

bool PassComparison::Start::operator<(const Start& other) const
{
    return std::tie(at, channel, key, settings) < std::tie(other.at, other.channel, other.key, other.settings);
}

bool PassComparison::Start::operator==(const Start& other) const
{
    return std::tie(at, channel, key, settings) == std::tie(other.at, other.channel, other.key, other.settings);
}

} // namespace ntdlib::agb
