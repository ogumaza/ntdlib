// SPDX-License-Identifier: MIT

// Checks the arithmetic in fused.h: the sign of the negated fused multiply-add's exact zero, and the PowerPC's and
// ARM's conversions of a float to a 32-bit integer, toward zero, at the ends of the integer's range and for NaN.

#include "ntdlib/fused.h"

#include <cmath>
#include <cstdint>
#include <limits>

#include "xsflib/testing/check.h"

namespace
{

using ntdlib::ArmToInt32;
using ntdlib::PowerPcToInt32;
using xsflib::testing::Check;

void TestNegatedFma()
{
    // -(1 * 1 + -1) is an exact zero of operands with opposite signs. The PowerPC gives -0.
    Check(std::signbit(ntdlib::NegatedFma(1.0f, 1.0f, -1.0f)) && std::signbit(ntdlib::NegatedFma(1.0, 1.0, -1.0)),
          "the negated fused multiply-add gives -0 for an exact zero");
    Check(ntdlib::NegatedFma(2.0f, 3.0f, 1.0f) == -7.0f && ntdlib::NegatedFma(2.0, 3.0, 1.0) == -7.0,
          "the negated fused multiply-add of numbers");
}

void TestPowerPcToInt32()
{
    const float kNan = std::numeric_limits<float>::quiet_NaN();
    const float kInfinity = std::numeric_limits<float>::infinity();

    Check(PowerPcToInt32(1.9f) == 1 && PowerPcToInt32(-1.9f) == -1 && PowerPcToInt32(0.0f) == 0,
          "a float converts toward zero");
    // 2^31 - 128 is the largest float below 2^31.
    Check(PowerPcToInt32(2147483520.0f) == 2147483520 && PowerPcToInt32(-2147483520.0f) == -2147483520,
          "the floats nearest the range's ends convert exactly");
    Check(PowerPcToInt32(2147483648.0f) == INT32_MAX && PowerPcToInt32(3e9f) == INT32_MAX &&
              PowerPcToInt32(kInfinity) == INT32_MAX,
          "a float of 2^31 or more gives the largest integer");
    Check(PowerPcToInt32(-2147483648.0f) == INT32_MIN && PowerPcToInt32(-3e9f) == INT32_MIN &&
              PowerPcToInt32(-kInfinity) == INT32_MIN,
          "a float of -2^31 or less gives the most negative integer");
    Check(PowerPcToInt32(kNan) == INT32_MIN, "NaN gives the most negative integer");
}

void TestArmToInt32()
{
    const float kNan = std::numeric_limits<float>::quiet_NaN();
    const float kInfinity = std::numeric_limits<float>::infinity();

    Check(ArmToInt32(1.9f) == 1 && ArmToInt32(-1.9f) == -1 && ArmToInt32(0.0f) == 0,
          "a float converts toward zero on ARM");
    Check(ArmToInt32(2147483520.0f) == 2147483520 && ArmToInt32(-2147483520.0f) == -2147483520,
          "the floats nearest the range's ends convert exactly on ARM");
    Check(ArmToInt32(2147483648.0f) == INT32_MAX && ArmToInt32(3e9f) == INT32_MAX && ArmToInt32(kInfinity) == INT32_MAX,
          "a float of 2^31 or more gives the largest integer on ARM");
    Check(ArmToInt32(-2147483648.0f) == INT32_MIN && ArmToInt32(-3e9f) == INT32_MIN &&
              ArmToInt32(-kInfinity) == INT32_MIN,
          "a float of -2^31 or less gives the most negative integer on ARM");
    Check(ArmToInt32(kNan) == 0, "NaN gives 0 on ARM");
}

} // namespace

int main()
{
    TestNegatedFma();
    TestPowerPcToInt32();
    TestArmToInt32();

    return xsflib::testing::Summary("fused");
}
