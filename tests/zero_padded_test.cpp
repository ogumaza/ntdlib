// SPDX-License-Identifier: MIT

// Checks ZeroPadded in zero_padded.h: leading zeros up to a count of digits, and a longer number kept whole.

#include "ntdlib/zero_padded.h"

#include "xsflib/testing/check.h"

namespace
{

using ntdlib::ZeroPadded;
using xsflib::testing::Check;

void TestZeroPadded()
{
    Check(ZeroPadded(7, 3) == "007", "7 in three digits is 007");
    Check(ZeroPadded(1234, 3) == "1234", "1234 keeps its four digits");
    Check(ZeroPadded(0, 0) == "0", "0 in no digits is 0");
}

} // namespace

int main()
{
    TestZeroPadded();

    return xsflib::testing::Summary("zero_padded");
}
