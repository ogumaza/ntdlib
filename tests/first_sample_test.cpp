// SPDX-License-Identifier: MIT

// Checks FirstSampleMs in first_sample.h: the time of the first pair of a frame that has a sample that isn't 0 on
// either side, and nothing for a frame of zeros.

#include "ntdlib/first_sample.h"

#include <array>
#include <cstdint>
#include <optional>

#include "xsflib/testing/check.h"

namespace
{

using ntdlib::FirstSampleMs;
using xsflib::testing::Check;

void TestFirstSample()
{
    constexpr std::array<int16_t, 8> kZeros = {};
    constexpr std::array<int16_t, 8> kLeft = {0, 0, 0, 0, -1, 0, 5, 5};
    constexpr std::array<int16_t, 8> kRight = {0, 0, 0, 1, 0, 0, 0, 0};
    constexpr std::array<int16_t, 8> kFirst = {3, 0, 0, 0, 0, 0, 0, 0};

    Check(!FirstSampleMs(kZeros, 0.5), "a frame of zeros has no first sample");
    Check(FirstSampleMs(kLeft, 0.5) == 1.0, "a sample of -1 on the left side of the third pair is 1 ms in");
    Check(FirstSampleMs(kRight, 0.5) == 0.5, "a sample of 1 on the right side of the second pair is 0.5 ms in");
    Check(FirstSampleMs(kFirst, 0.5) == 0.0, "a sample in the first pair is at the frame's start");
    Check(!FirstSampleMs({}, 0.5), "an empty frame has no first sample");
}

} // namespace

int main()
{
    TestFirstSample();

    return xsflib::testing::Summary("first_sample");
}
