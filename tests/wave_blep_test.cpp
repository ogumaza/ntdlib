// SPDX-License-Identifier: MIT

// Tests of the band-limited steps (ntdlib/wave/blep.h). The bounds are the ones that the header gives. A held level
// comes out exactly, and the step response is half its height at the change's time, 32 samples late. The filter's
// response, measured from the table, stays within 0.01 dB up to 0.84 of the Nyquist frequency and lies at least 77 dB
// down from it on. A square wave keeps its harmonics, and its aliases lie at least 77 dB below its fundamental.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/wave/blep.h"

using xsflib::testing::Check;

namespace
{

using ntdlib::wave::BandLimitedSteps;

constexpr double kPi = 3.14159265358979323846;

// The GBA's period: 804 samples in each frame of 280,896 cycles of its 16,777,216 Hz clock.
constexpr uint64_t kClocks = 280896;
constexpr uint32_t kSamples = 804;
constexpr double kClockHz = 16777216.0;
constexpr double kRate = kClockHz * kSamples / kClocks;

// The levels before a change and long after it are exact, and the response is half its height at the change's time
// once the output's delay has passed.
void TestStep()
{
    // Both changes lie on a sample's time: sample 0 and sample 2,412.
    BandLimitedSteps steps(kClocks, kSamples);
    std::vector<float> output(1200);
    steps.Step(0, 2.0f);
    steps.Step(kClocks * 3, 3.0f);
    steps.Read(output.data(), output.size());
    Check(output[0] == 0.0f, "before the first change");
    Check(std::fabs(output[32] - 1.0f) < 1e-6f, "half the first change at its time: " + std::to_string(output[32]));
    for (std::size_t m = 64; m < output.size(); ++m)
    {
        Check(output[m] == 2.0f, "sample " + std::to_string(m) + " holds 2 exactly");
    }
    Check(steps.Level() == 5.0, "the level after both changes");

    // The second change lies on sample 2,412's time. Sample 2,444 holds the response there.
    std::vector<float> rest(1400);
    steps.Read(rest.data(), rest.size());
    const std::size_t middle = 2412 + 32 - 1200;
    Check(std::fabs(rest[middle] - 3.5f) < 1e-6f,
          "half the second change at its time: " + std::to_string(rest[middle]));
    for (std::size_t m = 0; m <= 2412 - 1200; ++m)
    {
        Check(rest[m] == 2.0f, "before the second change");
    }
    for (std::size_t m = 2412 + 64 - 1200; m < rest.size(); ++m)
    {
        Check(rest[m] == 5.0f, "sample " + std::to_string(m + 1200) + " holds 5 exactly");
    }
}

// The response is antisymmetric about the change's time: a residual and its mirror image sum to -1.
void TestSymmetry()
{
    double worst = 0.0;
    for (int phase = 0; phase <= BandLimitedSteps::kPhases; ++phase)
    {
        for (int k = 0; k < 2 * BandLimitedSteps::kHalfWidth; ++k)
        {
            const double sum = static_cast<double>(BandLimitedSteps::Residual(phase, k)) +
                               BandLimitedSteps::Residual(BandLimitedSteps::kPhases - phase, 63 - k);
            worst = std::max(worst, std::fabs(sum + 1.0));
        }
    }
    Check(worst < 1e-6, "the residuals' worst asymmetry is " + std::to_string(worst));
    Check(BandLimitedSteps::Residual(0, 63) == 0.0f, "the response ends at 1");
    Check(BandLimitedSteps::Residual(BandLimitedSteps::kPhases, 0) == -1.0f, "the response starts at 0");
}

// The filter's response in decibels at `frequency` cycles per output sample: the Fourier transform of the step
// response's slope, from the table's values at each 256th of a sample.
double ResponseDb(double frequency)
{
    constexpr int kPhases = BandLimitedSteps::kPhases;
    double re = 0.0;
    double im = 0.0;
    double before = 0.0;
    for (int j = 1; j <= 2 * BandLimitedSteps::kHalfWidth * kPhases; ++j)
    {
        // The step response j 256ths of a sample after the window's start: phase p of sample k after the change.
        const int phase = (kPhases - j % kPhases) % kPhases;
        const int k = (j + phase) / kPhases - 1;
        const double at = BandLimitedSteps::Residual(phase, k) + 1.0;
        const double angle = 2.0 * kPi * frequency * (j - 0.5) / kPhases;
        re += (at - before) * std::cos(angle);
        im += (at - before) * std::sin(angle);
        before = at;
    }

    return 20.0 * std::log10(std::hypot(re, im));
}

// Within 0.01 dB up to 0.84 of the Nyquist frequency, and at least 77 dB down from it to twice it.
void TestResponse()
{
    double ripple = 0.0;
    double stop = -300.0;
    for (int i = 0; i <= 1000; ++i)
    {
        const double frequency = i / 1000.0;
        const double db = ResponseDb(frequency);
        if (frequency <= 0.42)
        {
            ripple = std::max(ripple, std::fabs(db));
        }
        if (frequency >= 0.5)
        {
            stop = std::max(stop, db);
        }
    }
    Check(ripple <= 0.01, "passband " + std::to_string(ripple) + " dB");
    Check(stop <= -77.0, "stopband " + std::to_string(stop) + " dB");
}

// Between two phases, the response interpolates. A change 128.5 256ths after a sample and one 127.5 256ths after it
// give mirror images.
void TestBetweenPhases()
{
    // 512 clocks to a sample: clock 5,377 lies 257/512 of a sample after sample 10, and clock 5,375 255/512 after it.
    std::vector<float> later(80);
    std::vector<float> earlier(80);
    BandLimitedSteps a(512, 1);
    a.Step(5377, 1.0f);
    a.Read(later.data(), later.size());
    BandLimitedSteps b(512, 1);
    b.Step(5375, 1.0f);
    b.Read(earlier.data(), earlier.size());
    double worst = 0.0;
    for (std::size_t k = 0; k < 64; ++k)
    {
        worst = std::max(worst, std::fabs(static_cast<double>(later[11 + k]) + earlier[11 + 63 - k] - 1.0));
    }
    Check(worst < 1e-5, "the worst asymmetry between phases is " + std::to_string(worst));
}

// A square wave of 0 and 1 at 1,234.5 Hz: its edges every 6,795 clocks. The samples come in frames of 804. Each
// frame's edges come before its samples are read.
std::vector<float> Square(std::size_t frames, bool whole)
{
    constexpr uint64_t kHalfPeriod = 6795;
    BandLimitedSteps steps(kClocks, kSamples);
    std::vector<float> output(frames * kSamples);
    uint64_t edge = 0;
    float level = 0.0f;
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const uint64_t end = (frame + 1) * kClocks;
        for (; edge < end; edge += kHalfPeriod)
        {
            steps.Step(edge, level == 0.0f ? 1.0f : -1.0f);
            level = 1.0f - level;
        }
        if (!whole)
        {
            steps.Read(output.data() + frame * kSamples, kSamples);
        }
    }
    if (whole)
    {
        steps.Read(output.data(), output.size());
    }

    return output;
}

// The amplitude of the component at `frequency` Hz, through a Blackman-Harris window.
double Amplitude(const std::vector<float>& signal, double frequency)
{
    double re = 0.0;
    double im = 0.0;
    double gain = 0.0;
    const double n = static_cast<double>(signal.size());
    for (std::size_t i = 0; i < signal.size(); ++i)
    {
        const double t = 2.0 * kPi * static_cast<double>(i) / n;
        const double window =
            0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2.0 * t) - 0.01168 * std::cos(3.0 * t);
        const double angle = 2.0 * kPi * frequency * static_cast<double>(i) / kRate;
        re += window * signal[i] * std::cos(angle);
        im += window * signal[i] * std::sin(angle);
        gain += window;
    }

    return 2.0 * std::hypot(re, im) / gain;
}

// The square's harmonics keep their amplitudes, and its aliases lie at least 77 dB below its fundamental.
void TestSquare()
{
    const std::vector<float> framed = Square(100, false);
    Check(framed == Square(100, true), "frames read one by one give the samples of one read");

    // 65,536 samples after the first frame.
    const std::vector<float> signal(framed.begin() + kSamples, framed.begin() + kSamples + 65536);
    const double fundamental = kClockHz / (2.0 * 6795.0);
    const double nyquist = kRate / 2.0;
    const double bin = kRate / static_cast<double>(signal.size());

    // The odd harmonics up to 0.84 of the Nyquist frequency: 2 / (k pi).
    double ripple = 0.0;
    for (int k = 1; k * fundamental <= 0.84 * nyquist; k += 2)
    {
        const double expected = 2.0 / (k * kPi);
        ripple = std::max(ripple, std::fabs(20.0 * std::log10(Amplitude(signal, k * fundamental) / expected)));
    }
    Check(ripple <= 0.01, "the harmonics lie within " + std::to_string(ripple) + " dB");

    // The aliases of the odd harmonics above the Nyquist frequency, up to 20 times the rate. An alias within 10 bins of
    // a harmonic below the Nyquist frequency, or of 0 Hz, takes that component's leakage, and is left out.
    const double reference = Amplitude(signal, fundamental);
    double loudest = -300.0;
    int measured = 0;
    for (int k = 1; k * fundamental < 20.0 * kRate; k += 2)
    {
        const double harmonic = k * fundamental;
        if (harmonic <= nyquist)
        {
            continue;
        }
        const double alias = std::fabs(harmonic - kRate * std::round(harmonic / kRate));
        bool near = alias < 10.0 * bin;
        for (int j = 1; j * fundamental <= nyquist && !near; j += 2)
        {
            near = std::fabs(alias - j * fundamental) < 10.0 * bin;
        }
        if (near)
        {
            continue;
        }
        loudest = std::max(loudest, 20.0 * std::log10(Amplitude(signal, alias) / reference));
        ++measured;
    }
    Check(measured > 100, "aliases measured: " + std::to_string(measured));
    Check(loudest <= -77.0, "the loudest alias lies at " + std::to_string(loudest) + " dB");
}

// A change can come at the time of the last sample read, but not before it.
void TestLateStep()
{
    BandLimitedSteps steps(kClocks, kSamples);
    std::vector<float> output(100);
    steps.Read(output.data(), output.size());

    // Sample 99's time lies at 34,587.6 clocks.
    steps.Step(34588, 1.0f);
    bool refused = false;
    try
    {
        steps.Step(34587, 1.0f);
    }
    catch (const std::invalid_argument&)
    {
        refused = true;
    }
    Check(refused, "a change before sample 99's time is refused");
    Check(steps.SamplesRead() == 100, "samples read");

    refused = false;
    try
    {
        static_cast<void>(BandLimitedSteps(0, kSamples));
    }
    catch (const std::invalid_argument&)
    {
        refused = true;
    }
    Check(refused, "a period without clocks is refused");
}

// A restart drops the changes in progress, and goes on from its sample at its level.
void TestRestart()
{
    BandLimitedSteps steps(kClocks, kSamples);
    steps.Step(1000, 4.0f);
    steps.Restart(500, -2.0);
    Check(steps.SamplesRead() == 500 && steps.Level() == -2.0, "the restart's sample and level");

    // Sample 600's time lies at 209,624 clocks.
    steps.Step(209624, 1.0f);
    std::vector<float> output(200);
    steps.Read(output.data(), output.size());
    Check(output[0] == -2.0f && output[100] == -2.0f, "the level before the change");
    Check(std::fabs(output[132] + 1.5f) < 1e-3f, "half the change at its time: " + std::to_string(output[132]));
    Check(output[199] == -1.0f, "the level after the change");
}

} // namespace

int main()
{
    TestStep();
    TestSymmetry();
    TestResponse();
    TestBetweenPhases();
    TestSquare();
    TestLateStep();
    TestRestart();

    return xsflib::testing::Summary("Band-limited steps");
}
