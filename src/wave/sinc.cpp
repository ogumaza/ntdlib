// SPDX-License-Identifier: MIT

#include "ntdlib/wave/sinc.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace ntdlib::wave
{
namespace
{

// The double nearest pi, the cutoff and the window's beta, and the eighth of an octave between two kernels: 2^(1/8).
constexpr double kPi = 3.141592653589793;
constexpr double kCutoff = 0.92;
constexpr double kBeta = 8.0;
constexpr double kEighthOctave = 1.0905077326652577;

// The half width of the window's transition band, times the window's half width, in shares of the Nyquist frequency.
constexpr double kTransition = 2.56;

// sin(x), from its Taylor series once x lies between -pi and pi. The terms to x^31 leave an error far below a float's
// precision.
double Sine(double x)
{
    const double turns = std::floor(x / (2.0 * kPi) + 0.5);
    const double r = x - turns * (2.0 * kPi);
    const double r2 = r * r;
    double term = r;
    double sum = r;
    for (int k = 1; k <= 15; ++k)
    {
        term = -term * r2 / static_cast<double>((2 * k) * (2 * k + 1));
        sum += term;
    }

    return sum;
}

// The modified Bessel function of the first kind and order 0, from its power series: the sum of ((z/2)^k / k!)^2.
double BesselI0(double z)
{
    const double half = z / 2.0;
    double term = 1.0;
    double sum = 1.0;
    for (int k = 1; k <= 50; ++k)
    {
        term *= half / static_cast<double>(k);
        sum += term * term;
    }

    return sum;
}

// The Kaiser window at `x` inputs from the middle of a span of `half_width` inputs on each side.
double Window(double x, double half_width)
{
    const double t = x / half_width;
    return BesselI0(kBeta * std::sqrt(std::max(0.0, 1.0 - t * t))) / BesselI0(kBeta);
}

// The low-pass at `cutoff` of the Nyquist frequency, at `x` inputs from the output: cutoff * sinc(cutoff * x).
double LowPass(double cutoff, double x)
{
    const double y = cutoff * x;
    if (y == 0.0)
    {
        return cutoff;
    }

    return cutoff * Sine(kPi * y) / (kPi * y);
}

} // namespace

SincKernel::SincKernel(int index)
{
    // Each index takes the step up by an eighth of an octave, and widens the window by as much up to a step of 4. The
    // half width is a multiple of 4 inputs. The cutoff falls with the step. Beyond a step of 4, it falls further, to
    // keep the window's transition band below the output's Nyquist frequency. That band spans 2.56 / half width of the
    // input's Nyquist frequency on each side of the cutoff.
    double step = 1.0;
    for (int i = 0; i < index; ++i)
    {
        step *= kEighthOctave;
    }
    half_width_ = 4 * static_cast<int>(std::ceil(kHalfWidth * std::min(step, 4.0) / 4.0));
    cutoff_ = std::min(kCutoff / step, 1.0 / step - kTransition / half_width_);
    taps_ = 2 * static_cast<std::size_t>(half_width_);
    rows_.resize((kPhases + 1) * taps_);

    // Each phase's row of weights, scaled to sum to 1.
    for (int phase = 0; phase <= kPhases; ++phase)
    {
        std::vector<double> row(taps_);
        double sum = 0.0;
        for (std::size_t k = 0; k < taps_; ++k)
        {
            const double x =
                static_cast<double>(static_cast<int>(k) - (half_width_ - 1)) - static_cast<double>(phase) / kPhases;
            row[k] = LowPass(cutoff_, x) * Window(x, half_width_);
            sum += row[k];
        }
        for (std::size_t k = 0; k < taps_; ++k)
        {
            rows_[static_cast<std::size_t>(phase) * taps_ + k] = static_cast<float>(row[k] / sum);
        }
    }
}

const SincKernel& SincKernel::ForStep(double step)
{
    // The first kernel whose step reaches `step`.
    int index = 0;
    double reach = 1.0;
    while (index + 1 < kKernels && step > reach)
    {
        reach *= kEighthOctave;
        ++index;
    }

    static std::array<std::once_flag, kKernels> once;
    static std::array<std::unique_ptr<const SincKernel>, kKernels> kernels;
    const auto make = [index]
    {
        kernels[static_cast<std::size_t>(index)] = std::unique_ptr<const SincKernel>(new SincKernel(index));
    };
    std::call_once(once[static_cast<std::size_t>(index)], make);

    return *kernels[static_cast<std::size_t>(index)];
}

float SincKernel::At(const float* input, uint32_t fraction) const
{
    // The weights between the two phases around the position, and 8 running sums of the products.
    const std::size_t phase = fraction >> 8;
    const float within = static_cast<float>(fraction & 0xFF) * (1.0f / 256.0f);
    const float* before = &rows_[phase * taps_];
    const float* after = before + taps_;
    std::array<float, 8> sums{};
    for (std::size_t k = 0; k < taps_; k += 8)
    {
        for (std::size_t lane = 0; lane < 8; ++lane)
        {
            const float weight = before[k + lane] + within * (after[k + lane] - before[k + lane]);
            sums[lane] += input[k + lane] * weight;
        }
    }

    return ((sums[0] + sums[1]) + (sums[2] + sums[3])) + ((sums[4] + sums[5]) + (sums[6] + sums[7]));
}

} // namespace ntdlib::wave
