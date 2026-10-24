// SPDX-License-Identifier: MIT

// A band-limited interpolator of a channel's samples: the windowed sinc of a high-quality sample-rate converter. The
// kernel is a sinc under a Kaiser window with a beta of 8. Up to a step of 1, where the output runs as fast as the
// input or faster, an output takes the 32 inputs on each side of its position, and the cutoff lies at 0.92 of the
// input's Nyquist frequency. The response then stays within 0.01 dB of full level up to 0.84 of that frequency, and
// lies at least 77 dB down from it on. A step above 1 lowers the cutoff to 0.92 of the output's Nyquist frequency. The
// step is rounded up to an eighth of an octave, and the window widens with it, up to 128 inputs on each side for steps
// of 4 and more. Beyond a step of 4, the cutoff falls further, to keep the window's transition band below the output's
// Nyquist frequency. At every step up to 8, the response lies at least 77 dB down above the output's Nyquist frequency.
// Steps above 8 take the kernel of 8. Each kernel lies in a table of 256 phases for each input sample. An output
// interpolates linearly between the two phases on each side of its position.
//
// Every platform gives the same outputs. The tables come from a fixed sequence of operations in double precision,
// without library math: the sine from its Taylor series, and the window's Bessel function from its power series. Each
// phase's weights sum to 1, and an output adds its products in 8 running sums in a fixed order.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ntdlib::wave
{

class SincKernel
{
public:
    // The inputs on each side of an output at a step of 1 or less, and at most.
    static constexpr int kHalfWidth = 32;
    static constexpr int kMaxHalfWidth = 128;

    // The phases of each input sample, and the kernels: one for steps up to 1, then one for each eighth of an octave
    // of steps up to 8.
    static constexpr int kPhases = 256;
    static constexpr int kKernels = 25;

    // The kernel for a step of `step` inputs to each output. Steps above 8 take the kernel of 8. Each kernel is worked
    // out the first time a step asks for it, safely when several threads ask at once.
    static const SincKernel& ForStep(double step);

    // The inputs on each side of an output's position that the kernel takes.
    int HalfWidth() const
    {
        return half_width_;
    }

    // The cutoff, as a share of the input's Nyquist frequency.
    double Cutoff() const
    {
        return cutoff_;
    }

    // The output at `fraction` 65536ths of the way from input[HalfWidth() - 1] to input[HalfWidth()]: the sum of the
    // 2 * HalfWidth() inputs, each times the kernel at its distance from the position.
    float At(const float* input, uint32_t fraction) const;

    // The weight of input k (0 to 2 * HalfWidth() - 1) for an output `phase` 256ths of the way (0 to kPhases).
    float Weight(int phase, int k) const
    {
        return rows_[static_cast<std::size_t>(phase) * taps_ + static_cast<std::size_t>(k)];
    }

private:
    explicit SincKernel(int index);

    int half_width_ = kHalfWidth;
    std::size_t taps_ = 2 * kHalfWidth;
    double cutoff_ = 0.0;
    std::vector<float> rows_; // kPhases + 1 rows of the weights
};

} // namespace ntdlib::wave
