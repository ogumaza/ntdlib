// SPDX-License-Identifier: MIT

// The model of JAudio1 for Pikmin 2 (jaudio1): an oscillator's test for a table that repeats a part for ever
// (Oscillator::Cycling), on tables built for the test. At a DSP rate of 48,000 Hz, a point's time counts in steps of
// the oscillator.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "xsflib/testing/check.h"

#include "ntdlib/jaudio1/oscillator.h"

using namespace xsflib::testing;

namespace
{

namespace j1 = ntdlib::jaudio1;

constexpr float kDacRate = 48000.0f;

constexpr int16_t kLinear = 0;
using j1::kPointHold;
using j1::kPointJump;
using j1::kPointStop;

// The cycles of the oscillator's state from its start on: whether each step finds it in the repeated part.
template <std::size_t N>
std::string Cycles(const std::array<j1::OscPoint, N>& points, int steps)
{
    j1::OscData data;
    data.attack = {points.data(), points.size()};
    j1::Oscillator oscillator;
    oscillator.Init();
    oscillator.InitStart(&data, kDacRate);

    std::string cycles;
    for (int i = 0; i < steps; i++)
    {
        cycles += oscillator.Cycling() ? 'c' : '-';
        oscillator.IncCounter(kDacRate);
    }

    return cycles;
}

void TestTimeScale()
{
    XSFLIB_CHECK(j1::Oscillator::TimeScale(kDacRate) == 1.0f);
}

// A table like a vibrato's: 2 steps up, then 3 steps down and 3 steps up for ever. The oscillator reads point 1, the
// jump's target, after the first 2 steps.
void TestCycle()
{
    const std::array<j1::OscPoint, 4> kTable = {{
        {kLinear, 2, 0x7fff},
        {kLinear, 3, 0},
        {kLinear, 3, 0x7fff},
        {kPointJump, 0, 1},
    }};

    XSFLIB_CHECK(Cycles(kTable, 40) == "--" + std::string(38, 'c'));

    // A jump to the first point repeats the whole table from the start.
    const std::array<j1::OscPoint, 3> kWhole = {{
        {kLinear, 2, 0x7fff},
        {kLinear, 2, 0},
        {kPointJump, 0, 0},
    }};

    XSFLIB_CHECK(Cycles(kWhole, 20) == std::string(20, 'c'));
}

// A table that ends in a hold or a stop, or jumps on to a later point, repeats nothing.
void TestNoCycle()
{
    const std::array<j1::OscPoint, 2> kHoldTable = {{{kLinear, 2, 0x7fff}, {kPointHold, 0, 0}}};
    XSFLIB_CHECK(Cycles(kHoldTable, 10) == std::string(10, '-'));

    const std::array<j1::OscPoint, 2> kStopTable = {{{kLinear, 2, 0x7fff}, {kPointStop, 0, 0}}};
    XSFLIB_CHECK(Cycles(kStopTable, 10) == std::string(10, '-'));

    const std::array<j1::OscPoint, 4> kForward = {{
        {kLinear, 2, 0x7fff},
        {kPointJump, 0, 2},
        {kLinear, 2, 0},
        {kPointHold, 0, 0},
    }};

    XSFLIB_CHECK(Cycles(kForward, 10) == std::string(10, '-'));

    // A hold before the jump ends the table there.
    const std::array<j1::OscPoint, 4> kHeld = {{
        {kLinear, 2, 0x7fff},
        {kPointHold, 0, 0},
        {kLinear, 2, 0},
        {kPointJump, 0, 0},
    }};

    XSFLIB_CHECK(Cycles(kHeld, 10) == std::string(10, '-'));

    // A table that runs out of points reads a stop.
    const std::array<j1::OscPoint, 1> kUnended = {{{kLinear, 2, 0x7fff}}};
    XSFLIB_CHECK(Cycles(kUnended, 10) == std::string(10, '-'));
}

// An oscillator without settings, and one in its release, repeat nothing.
void TestOtherStates()
{
    j1::Oscillator none;
    none.Init();
    none.InitStart(nullptr, kDacRate);

    XSFLIB_CHECK(!none.Cycling());

    const std::array<j1::OscPoint, 3> kTable = {{
        {kLinear, 2, 0x7fff},
        {kLinear, 2, 0},
        {kPointJump, 0, 0},
    }};
    const std::array<j1::OscPoint, 2> kReleaseTable = {{{kLinear, 4, 0}, {kPointStop, 0, 0}}};

    j1::OscData data;
    data.attack = {kTable.data(), kTable.size()};
    data.release = {kReleaseTable.data(), kReleaseTable.size()};
    j1::Oscillator oscillator;
    oscillator.Init();
    oscillator.InitStart(&data, kDacRate);
    XSFLIB_CHECK(oscillator.Cycling());

    oscillator.Release(kDacRate);

    XSFLIB_CHECK(oscillator.State().state == j1::Oscillator::kRelease);
    XSFLIB_CHECK(!oscillator.Cycling());
}

} // namespace

int main()
{
    const auto tests = []
    {
        TestTimeScale();
        TestCycle();
        TestNoCycle();
        TestOtherStates();
    };

    return RunTest("jaudio1", tests);
}
