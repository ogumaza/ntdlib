// SPDX-License-Identifier: MIT

// Checks ShortestDecimal against texts worked out by hand from each float's value.

#include "ntdlib/shortest_decimal.h"

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

} // namespace

int main()
{
    TestExactValues();
    TestNearestFloat();
    TestNineDigits();

    return xsflib::testing::Summary("shortest_decimal");
}
