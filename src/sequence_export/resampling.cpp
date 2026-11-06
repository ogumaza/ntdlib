// SPDX-License-Identifier: MIT

// The low-passes that stand in for the consoles' resampling filters (see resampling.h).

#include "ntdlib/sequence_export/resampling.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "ntdlib/sequence_export/settings.h"

namespace ntdlib::sequence_export
{
namespace
{

// The fits of tools/sequence_export/resampling_filters.py: the rate above which the console plays through the filter,
// the SoundFont cutoff and the SFZ cutoff.

// AX's 8 kHz and 12 kHz sets, on the Wii and the Wii U.
constexpr std::array<ResamplingFilter, 2> kAxFilters = {{
    {4.0 / 3.0, 0.2518, 0.1784},
    {1.0, 0.3899, 0.3014},
}};

// The same sets on the Switch. The fits depend on the mixing rate (see the script).
constexpr std::array<ResamplingFilter, 2> kSwitchFilters = {{
    {4.0 / 3.0, 0.2748, 0.1663},
    {1.0, 0.3889, 0.2640},
}};

// The 3DS's kernels 0 and 1.
constexpr std::array<ResamplingFilter, 2> kCtrKernels = {{
    {4.0 / 3.0, 0.2549, 0.1779},
    {1.0, 0.3928, 0.3007},
}};

// The pitch ratio of a note at `key` on `region`, without bend, sweep or LFO.
double KeyPitch(int key, const ResamplingRegion& region)
{
    return std::exp2((key - region.original_key) / 12.0) * static_cast<double>(region.tune);
}

} // namespace

// The 3DS mixes at the DSP's rate, which nn::snd takes as 32728 Hz. AX mixes at 32 kHz on the Wii and the Wii U, and
// the Switch's renderer at 48 kHz. The Wii U plays a voice at a rate of 8 at most, and the Switch at a pitch ratio of
// 1/16 to 16 (resampling.h).
const ResamplingRule kCtrResampling = {32728.0, kCtrKernels};
const ResamplingRule kRevolutionResampling = {32000.0, kAxFilters};
const ResamplingRule kCafeResampling = {32000.0, kAxFilters, 8.0};
const ResamplingRule kSwitchResampling = {48000.0, kSwitchFilters, 0.0, 1.0 / 16.0, 16.0};

double ResamplingRate(int key, const ResamplingRegion& region, const Settings& settings)
{
    const double output = settings.resampling ? settings.resampling->output_rate : 32000.0;
    const double pitch = settings.engine == Engine::kRevolution
                             ? 1.0
                             : std::exp2((key - region.original_key) / 12.0) * static_cast<double>(region.tune);

    return region.sample_rate * pitch / output;
}

const ResamplingFilter* ResamplingFilterOf(int key, const ResamplingRegion& region, const Settings& settings)
{
    const bool interpolated =
        region.interpolation == 0 || settings.engine == Engine::kRevolution || settings.engine == Engine::kSwitch;
    if (!settings.resampling || !interpolated || !(region.tune > 0.0f))
    {
        return nullptr;
    }

    const double rate = ResamplingRate(key, region, settings);
    if (!std::isfinite(rate))
    {
        return nullptr;
    }

    for (const ResamplingFilter& filter : settings.resampling->filters)
    {
        if (rate > filter.above)
        {
            return &filter;
        }
    }

    return nullptr;
}

double ResamplingCutoffHz(double fraction, int key, const ResamplingRegion& region)
{
    return fraction * region.sample_rate * static_cast<double>(region.tune) *
           std::exp2((key - region.original_key) / 12.0);
}

std::pair<double, double> PitchLimits(const ResamplingRegion& region, const Settings& settings)
{
    double lowest = 0.0;
    double highest = std::numeric_limits<double>::infinity();
    if (!settings.resampling)
    {
        return {lowest, highest};
    }

    const ResamplingRule& rule = *settings.resampling;
    if (rule.rate_limit > 0.0 && region.sample_rate != 0)
    {
        highest = rule.rate_limit * rule.output_rate / region.sample_rate;
    }
    if (rule.pitch_ceiling > 0.0)
    {
        highest = std::min(highest, rule.pitch_ceiling);
    }
    lowest = rule.pitch_floor;

    return {lowest, highest};
}

bool PastPitchLimit(int key, const ResamplingRegion& region, const Settings& settings)
{
    if (!(region.tune > 0.0f))
    {
        return false;
    }

    const auto [lowest, highest] = PitchLimits(region, settings);
    const double pitch = KeyPitch(key, region);
    return pitch < lowest || pitch > highest;
}

std::vector<ResamplingSpan> PitchLimitSpans(std::pair<int, int> keys, const ResamplingRegion& region,
                                            const Settings& settings)
{
    if (!(region.tune > 0.0f) || !region.at_limits)
    {
        return {{keys}};
    }

    // The keys from `low` up start at or above the lowest pitch. The keys from `high` up start above the highest.
    const auto [lowest, highest] = PitchLimits(region, settings);
    const auto pitch = [&region](int key)
    {
        return KeyPitch(key, region);
    };
    int low = keys.first;
    while (low <= keys.second && pitch(low) < lowest)
    {
        low++;
    }
    int high = low;
    while (high <= keys.second && pitch(high) <= highest)
    {
        high++;
    }

    const auto cents = [](double ratio)
    {
        return static_cast<int>(std::lround(1200.0 * std::log2(ratio)));
    };
    std::vector<ResamplingSpan> spans;
    if (low > keys.first)
    {
        spans.push_back({{keys.first, low - 1}, nullptr, cents(lowest)});
    }
    if (high > low)
    {
        spans.push_back({{low, high - 1}});
    }
    if (high <= keys.second)
    {
        spans.push_back({{high, keys.second}, nullptr, cents(highest)});
    }

    return spans;
}

std::vector<ResamplingSpan> ResamplingSpans(std::pair<int, int> keys, const ResamplingRegion& region,
                                            const Settings& settings)
{
    std::vector<ResamplingSpan> spans;
    for (const ResamplingSpan& part : PitchLimitSpans(keys, region, settings))
    {
        if (part.limit_cents)
        {
            spans.push_back(part);
            continue;
        }

        for (int key = part.keys.first; key <= part.keys.second; key++)
        {
            const ResamplingFilter* filter = ResamplingFilterOf(key, region, settings);
            if (spans.empty() || spans.back().limit_cents || spans.back().filter != filter)
            {
                spans.push_back({{key, key}, filter});
            }
            else
            {
                spans.back().keys.second = key;
            }
        }
    }

    return spans;
}

} // namespace ntdlib::sequence_export
