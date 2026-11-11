// SPDX-License-Identifier: MIT

#include "wave/kaiser_sinc.h"

#include <algorithm>
#include <cmath>

namespace ntdlib::wave::detail
{
namespace
{

// The double nearest pi, and the window's beta.
constexpr double kPi = 3.141592653589793;
constexpr double kBeta = 8.0;

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

} // namespace

double KaiserWindow(double x, double half_width)
{
    const double t = x / half_width;

    return BesselI0(kBeta * std::sqrt(std::max(0.0, 1.0 - t * t))) / BesselI0(kBeta);
}

double LowPass(double cutoff, double x)
{
    const double y = cutoff * x;
    if (y == 0.0)
    {
        return cutoff;
    }

    return cutoff * Sine(kPi * y) / (kPi * y);
}

} // namespace ntdlib::wave::detail
