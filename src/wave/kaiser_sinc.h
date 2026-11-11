// SPDX-License-Identifier: MIT

// The windowed sinc that the band-limited interpolator (sinc.h) and the band-limited steps (blep.h) use: a low-pass
// under a Kaiser window with a beta of 8. The functions sum the series of the sine and the Bessel function in double
// precision. Every platform then gives the same values.

#pragma once

namespace ntdlib::wave::detail
{

// The cutoff at a step of 1, as a share of the Nyquist frequency.
inline constexpr double kCutoff = 0.92;

// The Kaiser window at `x` inputs from the middle of a span of `half_width` inputs on each side.
double KaiserWindow(double x, double half_width);

// The low-pass at `cutoff` of the Nyquist frequency, at `x` inputs from the output: cutoff * sinc(cutoff * x).
double LowPass(double cutoff, double x);

} // namespace ntdlib::wave::detail
