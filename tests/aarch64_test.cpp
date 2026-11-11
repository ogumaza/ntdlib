// SPDX-License-Identifier: MIT

// Checks AArch64's floating-point minimum and maximum (ordinary numbers, NaNs and the signs of zeros), and the
// reciprocal's estimate and step. On an AArch64 host, the processor checks a sweep of operands too.

#include "ntdlib/aarch64.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

#include "xsflib/testing/check.h"

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace
{

using xsflib::testing::Check;

const float kNan = std::numeric_limits<float>::quiet_NaN();
const float kInfinity = std::numeric_limits<float>::infinity();

void TestNumbers()
{
    Check(ntdlib::aarch64::Fmax(1.0f, 2.0f) == 2.0f && ntdlib::aarch64::Fmax(-3.0f, -4.0f) == -3.0f, "fmax of numbers");
    Check(ntdlib::aarch64::Fmin(1.0f, 2.0f) == 1.0f && ntdlib::aarch64::Fmin(-3.0f, -4.0f) == -4.0f, "fmin of numbers");
    Check(ntdlib::aarch64::Fminnm(3.0f, 2.0f) == 2.0f && ntdlib::aarch64::Fminnm(0.5f, 1.0f) == 0.5f,
          "fminnm of numbers");
    Check(ntdlib::aarch64::Fmaxnm(3.0f, 2.0f) == 3.0f && ntdlib::aarch64::Fmaxnm(-0.5f, -1.0f) == -0.5f,
          "fmaxnm of numbers");
}

void TestNans()
{
    // fmax and fmin give NaN when either operand is a NaN. fminnm gives the number.
    Check(std::isnan(ntdlib::aarch64::Fmax(kNan, 1.0f)) && std::isnan(ntdlib::aarch64::Fmax(1.0f, kNan)),
          "fmax of a NaN");
    Check(std::isnan(ntdlib::aarch64::Fmin(kNan, 1.0f)) && std::isnan(ntdlib::aarch64::Fmin(1.0f, kNan)),
          "fmin of a NaN");
    Check(ntdlib::aarch64::Fminnm(kNan, 1.0f) == 1.0f, "fminnm of a NaN");
    Check(ntdlib::aarch64::Fmaxnm(kNan, 1.0f) == 1.0f && ntdlib::aarch64::Fmaxnm(1.0f, kNan) == 1.0f,
          "fmaxnm of a NaN");
}

void TestZeros()
{
    // -0 counts as below +0, whichever operand comes first.
    Check(!std::signbit(ntdlib::aarch64::Fmax(-0.0f, 0.0f)) && !std::signbit(ntdlib::aarch64::Fmax(0.0f, -0.0f)),
          "fmax of zeros");
    Check(std::signbit(ntdlib::aarch64::Fmin(-0.0f, 0.0f)) && std::signbit(ntdlib::aarch64::Fmin(0.0f, -0.0f)),
          "fmin of zeros");
    Check(!std::signbit(ntdlib::aarch64::Fmaxnm(-0.0f, 0.0f)) && !std::signbit(ntdlib::aarch64::Fmaxnm(0.0f, -0.0f)),
          "fmaxnm of zeros");
}

uint32_t Bits(float value)
{
    return std::bit_cast<uint32_t>(value);
}

void TestReciprocalEstimate()
{
    // The table's ends: 1 has a = 256 and the estimate 511, and the largest significand has a = 511 and 256.
    Check(Bits(ntdlib::aarch64::Frecpe(1.0f)) == 0x3F7F8000u, "frecpe of 1");
    Check(Bits(ntdlib::aarch64::Frecpe(-2.0f)) == 0xBEFF8000u, "frecpe of -2");
    Check(Bits(ntdlib::aarch64::Frecpe(3.0f)) == 0x3EAA8000u, "frecpe of 3");
    Check(Bits(ntdlib::aarch64::Frecpe(std::bit_cast<float>(0x3FFFFFFFu))) == 0x3F000000u,
          "frecpe of the largest significand");

    // A result's exponent of 0 or -1 gives a subnormal, and a subnormal operand a large result.
    Check(Bits(ntdlib::aarch64::Frecpe(0x1p126f)) == 0x007FC000u, "frecpe of 2^126");
    Check(Bits(ntdlib::aarch64::Frecpe(0x1p127f)) == 0x003FE000u, "frecpe of 2^127");
    Check(Bits(ntdlib::aarch64::Frecpe(0x1p-127f)) == 0x7EFF8000u, "frecpe of 2^-127");
    Check(Bits(ntdlib::aarch64::Frecpe(0x1p-128f)) == 0x7F7F8000u, "frecpe of 2^-128");

    // Below 2^-128, zero, infinity and NaN.
    Check(Bits(ntdlib::aarch64::Frecpe(0x1p-129f)) == 0x7F800000u, "frecpe below 2^-128");
    Check(Bits(ntdlib::aarch64::Frecpe(-0.0f)) == 0xFF800000u, "frecpe of -0");
    Check(Bits(ntdlib::aarch64::Frecpe(kInfinity)) == 0, "frecpe of infinity");
    Check(Bits(ntdlib::aarch64::Frecpe(std::bit_cast<float>(0x7F800001u))) == 0x7FC00001u, "frecpe of a NaN");
}

void TestReciprocalStep()
{
    Check(ntdlib::aarch64::Frecps(2.0f, 0.5f) == 1.0f && ntdlib::aarch64::Frecps(1.0f, 3.0f) == -1.0f,
          "frecps of numbers");

    // (1 + 2^-23)(2 - 2^-22) is 2 - 2^-45. Rounded on its own, the product would be 2, and the step 0.
    Check(ntdlib::aarch64::Frecps(1.0f + 0x1p-23f, 2.0f - 0x1p-22f) == 0x1p-45f, "frecps rounds once");

    Check(ntdlib::aarch64::Frecps(kInfinity, 0.0f) == 2.0f && ntdlib::aarch64::Frecps(-0.0f, kInfinity) == 2.0f,
          "frecps of infinity and 0");
    Check(ntdlib::aarch64::Frecps(kInfinity, 2.0f) == -kInfinity &&
              ntdlib::aarch64::Frecps(-1.0f, kInfinity) == kInfinity,
          "frecps of infinity");
    Check(Bits(ntdlib::aarch64::Frecps(std::bit_cast<float>(0x7FC00002u), 1.0f)) == 0xFFC00002u &&
              Bits(ntdlib::aarch64::Frecps(kNan, std::bit_cast<float>(0x7F800003u))) == 0x7FC00003u,
          "frecps of NaNs");
}

#if defined(__aarch64__)
// Every 65,537th bit pattern, and the patterns 1 and 0x7FFFFF above it, against the processor. The step takes each
// pattern with its estimate and with a scrambled pattern.
void TestAgainstProcessor()
{
    int estimate_errors = 0;
    int step_errors = 0;
    for (uint64_t pattern = 0; pattern <= 0xFFFFFFFFu; pattern += 65537)
    {
        for (uint32_t offset : {0u, 1u, 0x7FFFFFu})
        {
            const float x = std::bit_cast<float>(static_cast<uint32_t>(pattern) + offset);
            if (std::isnan(x))
            {
                continue;
            }

            const float estimate = vrecpes_f32(x);
            estimate_errors += Bits(ntdlib::aarch64::Frecpe(x)) != Bits(estimate);
            const float y = std::bit_cast<float>(static_cast<uint32_t>(pattern * 2654435761u >> 7));
            if (!std::isnan(y))
            {
                step_errors += Bits(ntdlib::aarch64::Frecps(x, estimate)) != Bits(vrecpss_f32(x, estimate));
                step_errors += Bits(ntdlib::aarch64::Frecps(x, y)) != Bits(vrecpss_f32(x, y));
            }
        }
    }

    Check(estimate_errors == 0, "frecpe against the processor");
    Check(step_errors == 0, "frecps against the processor");
}
#endif

} // namespace

int main()
{
    TestNumbers();
    TestNans();
    TestZeros();
    TestReciprocalEstimate();
    TestReciprocalStep();
#if defined(__aarch64__)
    TestAgainstProcessor();
#endif

    return xsflib::testing::Summary("aarch64");
}
