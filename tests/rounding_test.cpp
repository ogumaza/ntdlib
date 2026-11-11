// SPDX-License-Identifier: MIT

// Checks the roundings in rounding.h: a value rounded up to a multiple in the type of the value, negative values
// included, and a float rounded to the nearest 32-bit integer, halves away from 0, at the ends of the range.

#include "ntdlib/rounding.h"

#include <cstdint>
#include <limits>
#include <type_traits>

#include "xsflib/testing/check.h"

namespace
{

using ntdlib::AlignUp;
using ntdlib::RoundToInt32;
using xsflib::testing::Check;

void TestAlignUp()
{
    Check(AlignUp(uint64_t{0}, 32) == 0 && AlignUp(uint64_t{1}, 32) == 32 && AlignUp(uint64_t{64}, 32) == 64,
          "a value rounds up to a multiple, and a multiple stays");
    Check(AlignUp(uint64_t{10}, 6) == 12 && AlignUp(uint32_t{13}, 3) == 15, "an alignment needn't be a power of 2");
    Check(AlignUp(uint64_t{0xFFFFFFFF}, 0x10) == 0x100000000, "64 bits keep a value past 2^32");
    Check(AlignUp(uint32_t{0xFFFFFFFD}, 4) == 0, "32 bits wrap past 2^32");
    Check(std::is_same_v<decltype(AlignUp(uint32_t{1}, uint64_t{4})), uint32_t>, "the result has the value's type");
    Check(AlignUp(int64_t{-5}, 4) == -4 && AlignUp(-4, 4) == -4 && AlignUp(-3, 4) == 0 && AlignUp(5, 4) == 8,
          "a negative value rounds up toward 0");
}

void TestRoundToInt32()
{
    const float kInfinity = std::numeric_limits<float>::infinity();

    Check(RoundToInt32(1.4f) == 1 && RoundToInt32(1.6f) == 2 && RoundToInt32(-1.6f) == -2,
          "a float rounds to the nearest integer");
    Check(RoundToInt32(2.5f) == 3 && RoundToInt32(-2.5f) == -3 && RoundToInt32(0.5f) == 1, "a half rounds away from 0");
    Check(RoundToInt32(2147483520.0f) == 2147483520 && RoundToInt32(-2147483648.0f) == INT32_MIN,
          "the floats at the range's ends convert exactly");
    Check(RoundToInt32(3e9f) == INT32_MAX && RoundToInt32(kInfinity) == INT32_MAX && RoundToInt32(-3e9f) == INT32_MIN &&
              RoundToInt32(-kInfinity) == INT32_MIN,
          "a float past the range gives the integer at its end");
}

} // namespace

int main()
{
    TestAlignUp();
    TestRoundToInt32();

    return xsflib::testing::Summary("rounding");
}
