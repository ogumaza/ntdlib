// SPDX-License-Identifier: MIT

// Checks AArch64's floating-point minimum and maximum: ordinary numbers, NaNs and the signs of zeros.

#include "ntdlib/aarch64.h"

#include <cmath>
#include <limits>

#include "xsflib/testing/check.h"

namespace
{

using xsflib::testing::Check;

const float kNan = std::numeric_limits<float>::quiet_NaN();

void TestNumbers()
{
    Check(ntdlib::aarch64::Fmax(1.0f, 2.0f) == 2.0f && ntdlib::aarch64::Fmax(-3.0f, -4.0f) == -3.0f, "fmax of numbers");
    Check(ntdlib::aarch64::Fmin(1.0f, 2.0f) == 1.0f && ntdlib::aarch64::Fmin(-3.0f, -4.0f) == -4.0f, "fmin of numbers");
    Check(ntdlib::aarch64::Fminnm(3.0f, 2.0f) == 2.0f && ntdlib::aarch64::Fminnm(0.5f, 1.0f) == 0.5f,
          "fminnm of numbers");
}

void TestNans()
{
    // fmax and fmin give NaN when either operand is a NaN. fminnm gives the number.
    Check(std::isnan(ntdlib::aarch64::Fmax(kNan, 1.0f)) && std::isnan(ntdlib::aarch64::Fmax(1.0f, kNan)),
          "fmax of a NaN");
    Check(std::isnan(ntdlib::aarch64::Fmin(kNan, 1.0f)) && std::isnan(ntdlib::aarch64::Fmin(1.0f, kNan)),
          "fmin of a NaN");
    Check(ntdlib::aarch64::Fminnm(kNan, 1.0f) == 1.0f, "fminnm of a NaN");
}

void TestZeros()
{
    // -0 counts as below +0, whichever operand comes first.
    Check(!std::signbit(ntdlib::aarch64::Fmax(-0.0f, 0.0f)) && !std::signbit(ntdlib::aarch64::Fmax(0.0f, -0.0f)),
          "fmax of zeros");
    Check(std::signbit(ntdlib::aarch64::Fmin(-0.0f, 0.0f)) && std::signbit(ntdlib::aarch64::Fmin(0.0f, -0.0f)),
          "fmin of zeros");
}

} // namespace

int main()
{
    TestNumbers();
    TestNans();
    TestZeros();

    return xsflib::testing::Summary("aarch64");
}
