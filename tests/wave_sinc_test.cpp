// SPDX-License-Identifier: MIT

// Tests of the band-limited interpolator (ntdlib/wave/sinc.h). The bounds are the ones that the header gives: the
// kernel's response, measured in double precision from its weights, its rows' sums, and the outputs on a sine that it
// passes.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/wave/sinc.h"

using xsflib::testing::Check;

namespace
{

using ntdlib::wave::SincKernel;

constexpr double kPi = 3.14159265358979323846;

// The kernel's response in decibels at `frequency`, as a share of the input's Nyquist frequency, for an output
// `phase` 256ths of the way between two inputs.
double ResponseDb(const SincKernel& kernel, int phase, double frequency)
{
    double re = 0.0;
    double im = 0.0;
    for (int k = 0; k < 2 * kernel.HalfWidth(); ++k)
    {
        const double x = (k - (kernel.HalfWidth() - 1)) - phase / 256.0;
        re += kernel.Weight(phase, k) * std::cos(kPi * frequency * x);
        im += kernel.Weight(phase, k) * std::sin(kPi * frequency * x);
    }

    return 20.0 * std::log10(std::hypot(re, im));
}

// The phases that the tests measure: every 16th, and the last.
std::vector<int> Phases()
{
    std::vector<int> phases;
    for (int phase = 0; phase < SincKernel::kPhases; phase += 16)
    {
        phases.push_back(phase);
    }
    phases.push_back(SincKernel::kPhases);

    return phases;
}

// Every row sums to 1, within a float's rounding of each weight.
void TestRowSums()
{
    for (double step : {1.0, 1.3, 2.0, 4.0, 8.0})
    {
        const SincKernel& kernel = SincKernel::ForStep(step);
        for (int phase = 0; phase <= SincKernel::kPhases; ++phase)
        {
            double sum = 0.0;
            for (int k = 0; k < 2 * kernel.HalfWidth(); ++k)
            {
                sum += kernel.Weight(phase, k);
            }
            Check(std::fabs(sum - 1.0) < 1e-5, "step " + std::to_string(step) + " phase " + std::to_string(phase) +
                                                   " sums to " + std::to_string(sum));
        }
    }
}

// Up to a step of 1: 32 inputs on each side, within 0.01 dB up to 0.84 of the input's Nyquist frequency, and at
// least 77 dB down from it on.
void TestUpToOne()
{
    for (double step : {0.25, 0.75, 1.0})
    {
        const SincKernel& kernel = SincKernel::ForStep(step);
        Check(kernel.HalfWidth() == 32, "step " + std::to_string(step) + " half width");
        double ripple = 0.0;
        double stop = -300.0;
        for (const int phase : Phases())
        {
            for (int i = 0; i <= 1000; ++i)
            {
                const double frequency = i / 1000.0;
                const double db = ResponseDb(kernel, phase, frequency);
                if (frequency <= 0.84)
                {
                    ripple = std::max(ripple, std::fabs(db));
                }
                if (frequency >= 1.0)
                {
                    stop = std::max(stop, db);
                }
            }
        }
        Check(ripple <= 0.01, "step " + std::to_string(step) + " passband " + std::to_string(ripple) + " dB");
        Check(stop <= -77.0, "step " + std::to_string(step) + " stopband " + std::to_string(stop) + " dB");
    }
}

// Above a step of 1: at least 77 dB down above the output's Nyquist frequency at every step up to 8, and within 0.01
// dB up to 0.84 of the cutoff up to a step of 4.
void TestAboveOne()
{
    for (double step : {1.01, 1.2, 1.5, 2.0, 3.0, 4.0, 5.0, 6.5, 8.0})
    {
        const SincKernel& kernel = SincKernel::ForStep(step);
        const double nyquist = 1.0 / step;
        Check(kernel.Cutoff() <= 0.92 * nyquist, "step " + std::to_string(step) + " cutoff");
        double ripple = 0.0;
        double stop = -300.0;
        for (const int phase : Phases())
        {
            for (int i = 0; i <= 2000; ++i)
            {
                const double frequency = i / 2000.0;
                const double db = ResponseDb(kernel, phase, frequency);
                if (frequency <= 0.84 / 0.92 * kernel.Cutoff())
                {
                    ripple = std::max(ripple, std::fabs(db));
                }
                if (frequency >= nyquist)
                {
                    stop = std::max(stop, db);
                }
            }
        }
        if (step <= 4.0)
        {
            Check(ripple <= 0.01, "step " + std::to_string(step) + " passband " + std::to_string(ripple) + " dB");
        }
        Check(stop <= -77.0, "step " + std::to_string(step) + " stopband " + std::to_string(stop) + " dB");
    }
}

// The window widens with the step up to 128 inputs on each side, and steps of the same kernel share it.
void TestKernels()
{
    Check(SincKernel::ForStep(2.0).HalfWidth() > 32, "step 2 widens");
    Check(SincKernel::ForStep(4.0).HalfWidth() == 128, "step 4 half width");
    Check(SincKernel::ForStep(100.0).HalfWidth() == 128, "step 100 half width");
    Check(&SincKernel::ForStep(100.0) == &SincKernel::ForStep(8.0), "steps above 8 take the kernel of 8");
    Check(&SincKernel::ForStep(0.5) == &SincKernel::ForStep(1.0), "steps up to 1 share a kernel");
    Check(&SincKernel::ForStep(1.0) != &SincKernel::ForStep(1.0001), "a step above 1 takes another kernel");
}

// At a step of 1, a sine at a fifth of the Nyquist frequency comes out at every position within 75 dB of its value
// there: the passband's error and the interpolation's between phases.
void TestSine()
{
    const SincKernel& kernel = SincKernel::ForStep(1.0);
    const double frequency = 0.2;
    std::vector<float> input(static_cast<std::size_t>(2 * kernel.HalfWidth()));
    double worst = 0.0;
    for (uint32_t fraction = 0; fraction < 65536; fraction += 97)
    {
        const double position = kernel.HalfWidth() - 1 + fraction / 65536.0;
        for (std::size_t k = 0; k < input.size(); ++k)
        {
            input[k] = static_cast<float>(std::sin(kPi * frequency * (static_cast<double>(k) - position) + 0.3));
        }
        const double expected = std::sin(0.3);
        worst = std::max(worst, std::fabs(kernel.At(input.data(), fraction) - expected));
    }
    Check(20.0 * std::log10(worst) <= -75.0, "the sine's worst error is " + std::to_string(worst));
}

} // namespace

int main()
{
    TestRowSums();
    TestUpToOne();
    TestAboveOne();
    TestKernels();
    TestSine();

    return xsflib::testing::Summary("Band-limited interpolator");
}
