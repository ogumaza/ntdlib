// SPDX-License-Identifier: MIT

// Band-limited steps: a signal that holds a level and changes it at times in a console's clocks, sampled at a lower
// rate without aliasing. A chip's square, noise or wave channel gives such a signal. Each change adds the step
// response of a low-pass filter at its time: the integral of a sinc under a Kaiser window with a beta of 8, 32 output
// samples on each side. The cutoff lies at 0.92 of the output's Nyquist frequency. The response then stays within 0.01
// dB of full level up to 0.84 of that frequency, and lies at least 77 dB down from it on. The interpolator of `sinc.h`
// gives the same response at a step of 1.
//
// The step response reaches 32 samples before a change's time. The output therefore runs 32 samples late: sample m
// holds the filtered signal at m - 32 samples, and only the changes before sample m reach it. A change thus has to
// come before the samples after its time are read. The output is the signal's level at its sample's time plus a
// residual that falls to 0 once the response has settled, 64 samples after the change. A held level therefore comes
// out exactly.
//
// Every platform gives the same outputs. The table comes from a fixed sequence of operations in double precision,
// without library math, and the residuals add up in floats in the order of the changes.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ntdlib::wave
{

class BandLimitedSteps
{
public:
    // The output's samples on each side of a change that its step response reaches, and the output's delay in samples.
    static constexpr int kHalfWidth = 32;

    // The phases of the step response's table between two output samples.
    static constexpr int kPhases = 256;

    // Steps for an output of `samples` samples in each `clocks` clocks. Both have to be above 0.
    BandLimitedSteps(uint64_t clocks, uint32_t samples);

    // Changes the level by `delta` at `clock` clocks from the start. The clock can't lie before the time of the last
    // sample read.
    void Step(uint64_t clock, float delta);

    // Writes the next `count` samples of the output to `output`.
    void Read(float* output, std::size_t count);

    // Starts again at sample `sample` with the level `level`. The changes in progress go, and the next sample read is
    // `sample`. A host that runs a chip without its output calls it before the output goes on.
    void Restart(uint64_t sample, double level);

    // The samples read so far.
    uint64_t SamplesRead() const
    {
        return read_;
    }

    // The level of the signal after every change so far.
    double Level() const
    {
        return total_;
    }

    // The step response less 1 at output sample k (0 to 2 * kHalfWidth - 1) after a change, for a change `phase` 256ths
    // of a sample (0 to kPhases) after the sample before it. Sample 0 is the first after the change.
    static float Residual(int phase, int k);

private:
    uint64_t clocks_ = 1;
    uint32_t samples_ = 1;
    uint64_t read_ = 0;  // the samples read
    double level_ = 0.0; // the level at the last sample read
    double total_ = 0.0; // the level after every change

    // From sample read_ on: the changes of the level at each sample, and the residuals.
    std::vector<double> changes_;
    std::vector<float> residuals_;
};

} // namespace ntdlib::wave
