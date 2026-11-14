// SPDX-License-Identifier: MIT

// Checks ShortestDecimal against texts worked out by hand from each float's value.

#include "ntdlib/shortest_decimal.h"

#include <bit>
#include <cstdint>
#include <limits>

#include "xsflib/testing/check.h"

namespace
{

using ntdlib::ShortestDecimal;
using xsflib::testing::Check;

void TestExactValues()
{
    // These floats are exact. A text with fewer digits names another float.
    Check(ShortestDecimal(1.0f) == "1", "1 has no point");
    Check(ShortestDecimal(0.5f) == "0.5", "a half");
    Check(ShortestDecimal(-0.75f) == "-0.75", "a negative value keeps its sign");
}

void TestNearestFloat()
{
    // 1.3f is 1.2999999523162841796875. Its neighbours are 2^-23 away, about 1.19e-7. "1.3" is 4.8e-8 away, less than
    // half that, and reads back as 1.3f.
    Check(ShortestDecimal(1.3f) == "1.3", "1.3f gives 1.3");
}

void TestNineDigits()
{
    // 8 + 11 * 2^-20 is 8.00001049041748046875. Floats near 8 are 2^-20 apart, about 9.54e-7. A text reads back as
    // this float only within 4.77e-7 of it. The 8-digit texts 8.000010 and 8.000011 are 4.90e-7 and 5.10e-7 away.
    // Every 9-digit text from 8.0000101 to 8.0000109 reads back. 8.0000105 is the nearest of them.
    const float value = 8.0f + 11.0f * 0x1p-20f;
    Check(ShortestDecimal(value) == "8.0000105", "a float that needs 9 digits");
}

void TestTies()
{
    // 1048576.25f is 2^20 + 2^-2. Floats near 2^20 are 2^-3 apart, so a text reads back as this float within 0.0625 of
    // it. 1048576.2 and 1048576.3 are both 0.05 away. Of two texts equally near, the one with the even last digit wins.
    Check(ShortestDecimal(1048576.25f) == "1048576.2", "a tie goes to the even digit");
}

void TestBelowPowerOfTwo()
{
    // 2^-96 is 1.26217744835...e-29. The float above it is 2^-119 (1.50463e-36) away, and the float below half as far.
    // A text reads back as 2^-96 from 3.76158e-37 below it to 7.52316e-37 above it. The nearest 8-digit text,
    // 1.2621774e-29, is 4.83536e-37 below: too far. 1.2621775e-29, 5.16464e-37 above, reads back. No 7-digit text does.
    Check(ShortestDecimal(0x1p-96f) == "0.000000000000000000000000000012621775", "the float below is nearer");
}

void TestWholeNumbers()
{
    // A whole number gives every digit, even where fewer would read back: 1073741800 reads back as 2^30.
    Check(ShortestDecimal(0x1p30f) == "1073741824", "2^30");
    Check(ShortestDecimal(0x1.fffffep127f) == "340282346638528859811704183484516925440", "the largest float");
    Check(ShortestDecimal(-0.0f) == "-0", "a negative zero keeps its sign");
}

void TestSmallest()
{
    // 2^-149 is 1.401e-45. The floats beside it are 0 and 2.803e-45, so a text reads back within 0.7e-45 of it.
    // 1e-45 is 0.401e-45 away.
    Check(ShortestDecimal(0x1p-149f) == "0.000000000000000000000000000000000000000000001", "the smallest float");
}

void TestInfinities()
{
    Check(ShortestDecimal(std::numeric_limits<float>::infinity()) == "inf", "infinity");
    Check(ShortestDecimal(-std::numeric_limits<float>::infinity()) == "-inf", "minus infinity");
}

void TestNans()
{
    // The texts of Microsoft's printf for infinities and NaNs, which std::to_chars uses: "nan" for a quiet NaN,
    // "nan(snan)" for a signalling one (quiet bit clear), and "nan(ind)" for the indefinite NaN that x86 makes (the
    // sign and the quiet bit alone), each with a minus sign for a set sign bit.
    Check(ShortestDecimal(std::bit_cast<float>(uint32_t{0x7fc00000})) == "nan", "a quiet NaN");
    Check(ShortestDecimal(std::bit_cast<float>(uint32_t{0xffc00000})) == "-nan(ind)", "the indefinite NaN");
    Check(ShortestDecimal(std::bit_cast<float>(uint32_t{0xffc00001})) == "-nan", "a negative quiet NaN");
    Check(ShortestDecimal(std::bit_cast<float>(uint32_t{0x7f800001})) == "nan(snan)", "a signalling NaN");
    Check(ShortestDecimal(std::bit_cast<float>(uint32_t{0xff800001})) == "-nan(snan)", "a negative signalling NaN");
}

} // namespace

int main()
{
    TestExactValues();
    TestNearestFloat();
    TestNineDigits();
    TestTies();
    TestBelowPowerOfTwo();
    TestWholeNumbers();
    TestSmallest();
    TestInfinities();
    TestNans();

    return xsflib::testing::Summary("shortest_decimal");
}
