// SPDX-License-Identifier: MIT

// Tests of the ARM11's single-precision arithmetic in round-to-nearest and flush-to-zero mode (ntdlib/ctr_fx/vfp.h).
// A double holds the exact product of two floats, and the exact sum of two floats of nearby exponents. The tests check
// that each result is the float nearest the exact result, with ties to even, and that denormal values flush. PowF has
// to stay within a few units in the last place of the power.

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>

#include "xsflib/testing/check.h"

#include "ntdlib/ctr_fx/vfp.h"

namespace
{

namespace vfp = ntdlib::ctr_fx::vfp;
using xsflib::testing::ErrorOf;

uint32_t Bits(float value)
{
    return std::bit_cast<uint32_t>(value);
}

// Whether `result` is the float nearest the exact value `exact`, with ties to even. `exact` lies in the range of the
// normal floats. The floats on either side of it and their distances from it are exact in double precision.
bool NearestFrom(float result, double exact)
{
    float below = static_cast<float>(exact);
    if (static_cast<double>(below) > exact)
    {
        below = std::nextafter(below, -INFINITY);
    }
    const float above = static_cast<double>(below) == exact ? below : std::nextafter(below, INFINITY);
    const double under = exact - static_cast<double>(below);
    const double over = static_cast<double>(above) - exact;
    float nearest = under < over ? below : above;
    if (under == over)
    {
        nearest = Bits(below) % 2 == 0 ? below : above;
    }

    return Bits(result) == Bits(nearest);
}

// A random float of either sign, with an exponent from 2^-40 to 2^40.
float RandomFloat(std::mt19937& random)
{
    const float significand = std::uniform_real_distribution<float>(1.0f, 2.0f)(random);
    const int exponent = std::uniform_int_distribution<int>(-40, 40)(random);
    const float value = std::ldexp(significand, exponent);

    return random() % 2 == 0 ? value : -value;
}

void TestProducts()
{
    std::mt19937 random(1);
    int wrong = 0;
    for (int i = 0; i < 100000; i++)
    {
        const float a = RandomFloat(random);
        const float b = RandomFloat(random);
        wrong += NearestFrom(vfp::Mul(a, b), static_cast<double>(a) * b) ? 0 : 1;
    }
    XSFLIB_CHECK(wrong == 0);

    // 3 · (1 + 2^-23) lies halfway between two floats. The tie goes to the even one, the higher.
    XSFLIB_CHECK(Bits(vfp::Mul(3.0f, std::nextafter(1.0f, 2.0f))) == 0x40400002u);
    XSFLIB_CHECK(Bits(vfp::Mul(-3.0f, std::nextafter(1.0f, 2.0f))) == 0xc0400002u);

    // Results and inputs below the smallest normal float count as zeros of their signs.
    const float smallest = std::numeric_limits<float>::min();
    XSFLIB_CHECK(Bits(vfp::Mul(smallest, 0.5f)) == 0u);
    XSFLIB_CHECK(Bits(vfp::Mul(-smallest, 0.5f)) == 0x80000000u);
    XSFLIB_CHECK(vfp::Mul(std::numeric_limits<float>::denorm_min(), 1e30f) == 0.0f);
    XSFLIB_CHECK(vfp::Mul(smallest, 1.0f) == smallest);

    // An overflow gives infinity.
    XSFLIB_CHECK(vfp::Mul(3e38f, 10.0f) == INFINITY);
    XSFLIB_CHECK(vfp::Mul(-3e38f, 10.0f) == -INFINITY);
    XSFLIB_CHECK(std::isinf(vfp::Mul(INFINITY, 2.0f)));
    XSFLIB_CHECK(std::isnan(vfp::Mul(INFINITY, 0.0f)));
}

void TestSums()
{
    // Sums of floats whose exponents lie within 20 of each other: a double holds them exactly.
    std::mt19937 random(2);
    int wrong = 0;
    for (int i = 0; i < 100000; i++)
    {
        const float a = RandomFloat(random);
        float b = RandomFloat(random);
        int ea = 0;
        int eb = 0;
        std::frexp(a, &ea);
        std::frexp(b, &eb);
        if (std::abs(ea - eb) > 20)
        {
            b = std::ldexp(b, ea - eb);
        }
        wrong += NearestFrom(vfp::Add(a, b), static_cast<double>(a) + b) ? 0 : 1;
        wrong += NearestFrom(vfp::Sub(a, b), static_cast<double>(a) - b) ? 0 : 1;
    }
    XSFLIB_CHECK(wrong == 0);

    // Ties go to the even float: 1 + 2^-24 to 1, and (1 + 2^-23) + 2^-24 to 1 + 2^-22. 1 - 2^-25 lies halfway between
    // 1 and the float below it, and goes to 1. A sum just past a tie goes to the far float.
    const float half_step = std::ldexp(1.0f, -24);
    XSFLIB_CHECK(Bits(vfp::Add(1.0f, half_step)) == 0x3f800000u);
    XSFLIB_CHECK(Bits(vfp::Add(std::nextafter(1.0f, 2.0f), half_step)) == 0x3f800002u);
    XSFLIB_CHECK(Bits(vfp::Add(1.0f, -std::ldexp(1.0f, -25))) == 0x3f800000u);
    XSFLIB_CHECK(Bits(vfp::Add(1.0f, std::ldexp(std::nextafter(1.0f, 2.0f), -24))) == 0x3f800001u);

    // A tiny addend far below a float's step keeps the float.
    const float tiny = std::ldexp(1.0f, -60);
    XSFLIB_CHECK(vfp::Add(1.0f, tiny) == 1.0f);
    XSFLIB_CHECK(vfp::Add(1.0f, -tiny) == 1.0f);
    XSFLIB_CHECK(vfp::Add(-1.0f, tiny) == -1.0f);
    XSFLIB_CHECK(vfp::Sub(-1.0f, tiny) == -1.0f);

    // x - x is +0 to nearest, and -0 + -0 is -0.
    XSFLIB_CHECK(Bits(vfp::Sub(5.5f, 5.5f)) == 0u);
    XSFLIB_CHECK(Bits(vfp::Add(-0.0f, -0.0f)) == 0x80000000u);
}

void TestQuotients()
{
    std::mt19937 random(3);
    int wrong = 0;
    for (int i = 0; i < 100000; i++)
    {
        const float a = RandomFloat(random);
        const float b = RandomFloat(random);
        const float q = vfp::Div(a, b);

        // The quotient lies between the midpoints of q with the floats on either side of it. A midpoint holds 25
        // significant bits, and its product with b is exact in double precision. On a tie, q is even.
        const double magnitude = std::fabs(static_cast<double>(q));
        const double up = (magnitude + std::nextafter(std::fabs(q), INFINITY)) / 2.0;
        const double down = (magnitude + std::nextafter(std::fabs(q), 0.0f)) / 2.0;
        const double high = up * std::fabs(b);
        const double low = down * std::fabs(b);
        const double target = std::fabs(a);
        const bool inside = low < target && target < high;
        const bool tie = (target == low || target == high) && Bits(q) % 2 == 0;
        const bool sign = std::signbit(q) == (std::signbit(a) != std::signbit(b));
        wrong += (inside || tie) && sign ? 0 : 1;
    }
    XSFLIB_CHECK(wrong == 0);

    // Toward zero would give 0x3eaaaaaa and 0x3f2aaaaa.
    XSFLIB_CHECK(Bits(vfp::Div(1.0f, 3.0f)) == 0x3eaaaaabu);
    XSFLIB_CHECK(Bits(vfp::Div(2.0f, 3.0f)) == 0x3f2aaaabu);
    XSFLIB_CHECK(Bits(vfp::Div(-2.0f, 3.0f)) == 0xbf2aaaabu);
    XSFLIB_CHECK(vfp::Div(6.0f, 3.0f) == 2.0f);
    XSFLIB_CHECK(vfp::Div(-1.0f, 0.0f) == -INFINITY);
}

void TestConversions()
{
    // To nearest: 16,777,219 and 16,777,221 lie halfway between floats, and both go to the even one, 16,777,220.
    // 2^32 - 1 goes to 2^32.
    XSFLIB_CHECK(vfp::FromU32(16777219) == 16777220.0f);
    XSFLIB_CHECK(vfp::FromU32(16777221) == 16777220.0f);
    XSFLIB_CHECK(Bits(vfp::FromU32(0xffffffffu)) == 0x4f800000u);
    XSFLIB_CHECK(vfp::FromS32(-16777219) == -16777220.0f);
    XSFLIB_CHECK(vfp::FromS32(std::numeric_limits<int32_t>::min()) == -2147483648.0f);

    // To integers: toward zero, saturated, and 0 for a NaN.
    XSFLIB_CHECK(vfp::ToS32(-1.9f) == -1);
    XSFLIB_CHECK(vfp::ToS32(127.99f) == 127);
    XSFLIB_CHECK(vfp::ToS32(2.5e9f) == std::numeric_limits<int32_t>::max());
    XSFLIB_CHECK(vfp::ToS32(-3e9f) == std::numeric_limits<int32_t>::min());
    XSFLIB_CHECK(vfp::ToS32(std::numeric_limits<float>::quiet_NaN()) == 0);
    XSFLIB_CHECK(vfp::ToU32(3.99f) == 3u);
    XSFLIB_CHECK(vfp::ToU32(-5.0f) == 0u);
    XSFLIB_CHECK(vfp::ToU32(5e9f) == std::numeric_limits<uint32_t>::max());
}

void TestPower()
{
    // 10^y within 3 units in the last place, over the range of the normal floats.
    int far = 0;
    for (int i = 0; i <= 380000; i++)
    {
        const float y = -static_cast<float>(i) * 1e-4f;
        const double expected = std::pow(10.0, static_cast<double>(y));
        if (expected < std::numeric_limits<float>::min() * 2.0)
        {
            break;
        }

        const double error = std::fabs(vfp::PowF(10.0f, y) - expected) / expected;
        far += error > 3.0 * std::ldexp(1.0, -24) ? 1 : 0;
    }
    XSFLIB_CHECK(far == 0);

    // Other bases, both sides of 1.
    for (const float x : {2.0f, 0.5f, 3.75f, 1000.0f, 0.001f})
    {
        for (const float y : {-0.25f, -1.0f, -3.5f, -10.0f})
        {
            const double expected = std::pow(static_cast<double>(x), static_cast<double>(y));
            XSFLIB_CHECK(std::fabs(vfp::PowF(x, y) - expected) / expected < 4.0 * std::ldexp(1.0, -24));
        }
    }

    // Powers of 2 come out exactly.
    XSFLIB_CHECK(vfp::PowF(2.0f, -3.0f) == 0.125f);
    XSFLIB_CHECK(vfp::PowF(4.0f, -0.5f) == 0.5f);

    // The special values, a power below the normal floats and one above them. A y next to 0 counts as -2^-64: its
    // power lies just below 1, and rounds to 1. A denormal y counts as 0.
    XSFLIB_CHECK(vfp::PowF(10.0f, 0.0f) == 1.0f);
    XSFLIB_CHECK(vfp::PowF(10.0f, -0.0f) == 1.0f);
    XSFLIB_CHECK(vfp::PowF(10.0f, -INFINITY) == 0.0f);
    XSFLIB_CHECK(std::isinf(vfp::PowF(0.5f, -INFINITY)));
    XSFLIB_CHECK(vfp::PowF(10.0f, -50.0f) == 0.0f);
    XSFLIB_CHECK(vfp::PowF(10.0f, -1e-35f) == 1.0f);
    XSFLIB_CHECK(vfp::PowF(0.5f, -1e10f) == INFINITY);
    XSFLIB_CHECK(vfp::PowF(10.0f, -std::numeric_limits<float>::denorm_min()) == 1.0f);

    // The arguments outside its cover.
    XSFLIB_CHECK(!ErrorOf<std::domain_error>([] { vfp::PowF(-10.0f, -1.0f); }).empty());
    XSFLIB_CHECK(!ErrorOf<std::domain_error>([] { vfp::PowF(1.0f, -1.0f); }).empty());
    XSFLIB_CHECK(!ErrorOf<std::domain_error>([] { vfp::PowF(10.0f, 1.0f); }).empty());
    XSFLIB_CHECK(!ErrorOf<std::domain_error>([] { vfp::PowF(10.0f, std::nanf("")); }).empty());
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("ctr_fx_vfp",
                                    []
                                    {
                                        TestProducts();
                                        TestSums();
                                        TestQuotients();
                                        TestConversions();
                                        TestPower();
                                    });
}
