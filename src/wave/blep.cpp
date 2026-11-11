// SPDX-License-Identifier: MIT

#include "ntdlib/wave/blep.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "wave/kaiser_sinc.h"

namespace ntdlib::wave
{
namespace
{

constexpr int kHalfWidth = BandLimitedSteps::kHalfWidth;
constexpr int kPhases = BandLimitedSteps::kPhases;
constexpr int kTaps = 2 * kHalfWidth;

// The filter's impulse response at `x` output samples from its middle: the low-pass at the cutoff under the Kaiser
// window.
double Impulse(double x)
{
    return detail::LowPass(detail::kCutoff, x) * detail::KaiserWindow(x, kHalfWidth);
}

// The residuals: kPhases + 1 rows of kTaps. Row p holds the residuals of a change p 256ths of a sample after the
// sample before it.
std::vector<float> MakeResiduals()
{
    // The step response at each 256th of a sample over the window's span: the impulse response's integral from the
    // span's start, by Simpson's rule over each 256th, scaled to end at 1.
    constexpr int kPoints = kTaps * kPhases;
    std::vector<double> response(kPoints + 1);
    constexpr double kSpacing = 1.0 / kPhases;
    double sum = 0.0;
    double before = Impulse(-kHalfWidth);
    for (int i = 0; i < kPoints; ++i)
    {
        const double start = -kHalfWidth + i * kSpacing;
        const double middle = Impulse(start + kSpacing / 2.0);
        const double after = Impulse(start + kSpacing);
        sum += (before + 4.0 * middle + after) * (kSpacing / 6.0);
        response[static_cast<std::size_t>(i + 1)] = sum;
        before = after;
    }

    // A change p 256ths after sample i lies (k + 1) * 256 - p 256ths before sample i + 1 + k. The output runs
    // kHalfWidth samples late. The response there is therefore the one at that distance from the span's start.
    std::vector<float> residuals((kPhases + 1) * kTaps);
    for (int phase = 0; phase <= kPhases; ++phase)
    {
        for (int k = 0; k < kTaps; ++k)
        {
            const double at = response[static_cast<std::size_t>((k + 1) * kPhases - phase)] / sum;
            residuals[static_cast<std::size_t>(phase * kTaps + k)] = static_cast<float>(at - 1.0);
        }
    }

    return residuals;
}

const std::vector<float>& Residuals()
{
    static const std::vector<float> kResiduals = MakeResiduals();

    return kResiduals;
}

} // namespace

BandLimitedSteps::BandLimitedSteps(uint64_t clocks, uint32_t samples) : clocks_(clocks), samples_(samples)
{
    if (clocks == 0 || samples == 0)
    {
        throw std::invalid_argument("a period of band-limited steps without clocks or samples");
    }

    Residuals();
}

float BandLimitedSteps::Residual(int phase, int k)
{
    return Residuals()[static_cast<std::size_t>(phase * kTaps + k)];
}

void BandLimitedSteps::Step(uint64_t clock, float delta)
{
    // The change's position in samples: sample `index`, and remainder / clocks_ of a sample after it.
    const uint64_t scaled = clock * samples_;
    const uint64_t index = scaled / clocks_;
    const uint64_t remainder = scaled % clocks_;
    if (index + 1 < read_)
    {
        throw std::invalid_argument("a band-limited step before the last sample read");
    }

    // The two rows of residuals around the position, and the share of the way from the first to the second.
    const uint64_t in_phases = remainder * kPhases;
    const std::size_t phase = static_cast<std::size_t>(in_phases / clocks_);
    const float within = static_cast<float>(static_cast<double>(in_phases % clocks_) / static_cast<double>(clocks_));
    const float* first = &Residuals()[phase * kTaps];
    const float* second = first + kTaps;

    // The level changes at the first sample after the change, and the residuals cover the next kTaps samples.
    const std::size_t start = static_cast<std::size_t>(index + 1 - read_);
    if (residuals_.size() < start + kTaps)
    {
        changes_.resize(start + kTaps);
        residuals_.resize(start + kTaps);
    }

    changes_[start] += delta;
    total_ += delta;
    for (std::size_t k = 0; k < kTaps; ++k)
    {
        const float residual = first[k] + within * (second[k] - first[k]);
        residuals_[start + k] += delta * residual;
    }
}

void BandLimitedSteps::Read(float* output, std::size_t count)
{
    const std::size_t held = std::min(count, residuals_.size());
    for (std::size_t i = 0; i < held; ++i)
    {
        level_ += changes_[i];
        output[i] = static_cast<float>(level_ + residuals_[i]);
    }
    std::fill(output + held, output + count, static_cast<float>(level_));

    // The samples left move to the front.
    changes_.erase(changes_.begin(), changes_.begin() + static_cast<std::ptrdiff_t>(held));
    residuals_.erase(residuals_.begin(), residuals_.begin() + static_cast<std::ptrdiff_t>(held));
    read_ += count;
}

void BandLimitedSteps::Restart(uint64_t sample, double level)
{
    read_ = sample;
    level_ = level;
    total_ = level;
    changes_.clear();
    residuals_.clear();
}

} // namespace ntdlib::wave
