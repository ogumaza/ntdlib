// SPDX-License-Identifier: MIT

#include "ntdlib/ctr_fx/vfp.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace ntdlib::ctr_fx::vfp
{
namespace
{

constexpr float kSmallest = std::numeric_limits<float>::min(); // the smallest normal float

// The bit patterns of powf's tables and coefficients. kLogHigh[k] is 16·log2(1 + k/16) rounded to the nearest
// multiple of 1/256, and kLogLow[k] the rest, rounded to nearest. kPowHigh[j] is 2^(j/16) rounded to nearest with the
// low 12 bits of its pattern cleared, kPowLow[j] the rest rounded to nearest, and kPow[j] 2^(j/16) rounded to nearest.
constexpr std::array<uint32_t, 16> kLogHigh{
    0x00000000, 0x3fb30000, 0x402e0000, 0x407e0000, 0x40a4e000, 0x40c8e000, 0x40eb4000, 0x41061000,
    0x4115c000, 0x4124d000, 0x41335000, 0x41414000, 0x414eb000, 0x415ba000, 0x41683000, 0x41744000,
};
constexpr std::array<uint32_t, 16> kLogLow{
    0x00000000, 0x3a7dbeb2, 0x3851cfdf, 0xbafa51bf, 0xbac3da19, 0xb98aedc2, 0xba2c1fcd, 0xbafaff2a,
    0x37d1cfdf, 0x3a70979a, 0x368e4789, 0x389d5be7, 0xb9980ac0, 0x3a948f55, 0xbac092da, 0x3ac6b362,
};
constexpr std::array<uint32_t, 16> kPowHigh{
    0x3f800000, 0x3f85a000, 0x3f8b9000, 0x3f91c000, 0x3f983000, 0x3f9ef000, 0x3fa5f000, 0x3fad5000,
    0x3fb50000, 0x3fbd0000, 0x3fc56000, 0x3fce2000, 0x3fd74000, 0x3fe0c000, 0x3feac000, 0x3ff52000,
};
constexpr std::array<uint32_t, 16> kPowLow{
    0x00000000, 0x39ac367d, 0x39383c7d, 0x38f4dceb, 0x397e0a32, 0x39264c12, 0x39ed6a9b, 0x3983eea4,
    0x391e667f, 0x398a39f6, 0x3965422b, 0x391182a4, 0x391f995b, 0x39cdeec3, 0x37c6e7dd, 0x392fa2a5,
};
constexpr std::array<uint32_t, 16> kPow{
    0x3f800000, 0x3f85aac3, 0x3f8b95c2, 0x3f91c3d3, 0x3f9837f0, 0x3f9ef532, 0x3fa5fed7, 0x3fad583f,
    0x3fb504f3, 0x3fbd08a4, 0x3fc5672a, 0x3fce248c, 0x3fd744fd, 0x3fe0ccdf, 0x3feac0c7, 0x3ff5257d,
};
constexpr uint32_t kThird = 0x3eaaaaab;     // 1/3, the series' second coefficient
constexpr uint32_t kLogSeries = 0x3e4ce27e; // its third
constexpr uint32_t kLog2eHigh = 0x4238b000; // 32/ln 2 in 12 significant bits
constexpr uint32_t kLog2eLow = 0xbbb89ad4;  // the rest of 32/ln 2
constexpr uint32_t kLog2e = 0x4238aa3b;     // 32/ln 2
constexpr uint32_t kPowCubic = 0x376359fc;  // the coefficients of 2^(f/16) - 1, from the cubic's
constexpr uint32_t kPowSquare = 0x3a75ffc9;
constexpr uint32_t kPowLinear = 0x3d317218; // ln 2 / 16

float FromBits(uint32_t bits)
{
    return std::bit_cast<float>(bits);
}

uint32_t Bits(float value)
{
    return std::bit_cast<uint32_t>(value);
}

// An input as the VFP reads it: a denormal value counts as a zero of its sign.
float Flushed(float value)
{
    return std::fabs(value) < kSmallest ? std::copysign(0.0f, value) : value;
}

// The float nearest the exact result `value` + `error`, with ties to even. `value` is the result rounded to nearest in
// double precision, and `error` the rounding's error, of which only the sign counts. A sum, product or quotient of two
// floats never lies nearer a tie between two floats than a double can tell without lying on it. The double therefore
// rounds to the right float. An exact result below the smallest normal float flushes to a zero of its sign, before any
// rounding. An overflow gives an infinity.
float Nearest(double value, double error)
{
    if (std::isnan(value))
    {
        return std::numeric_limits<float>::quiet_NaN();
    }

    const bool smaller = error != 0.0 && (error < 0.0) != (value < 0.0); // the exact result lies nearer zero
    const double magnitude = std::fabs(value);
    if (magnitude < kSmallest || (magnitude == kSmallest && smaller))
    {
        return std::copysign(0.0f, static_cast<float>(value));
    }

    return static_cast<float>(value);
}

// The value with the low 12 bits of its pattern rounded away: 12 significant bits, the halves away from zero.
float High(float value)
{
    return FromBits((Bits(value) + 2048) >> 12 << 12);
}

// 16·log2(x) in two parts: the high part has 12 significant bits.
struct Log16
{
    float high;
    float low;
};

Log16 LogOf(uint32_t x)
{
    // x = 2^e · m, with m = 1 + k/16 + a remainder of up to 1/32 either way.
    const uint32_t top = (x + 0x40000) >> 19;
    const uint32_t k = top & 15;
    const int32_t e = static_cast<int32_t>((top << 20) >> 24) - 127;
    const float m = FromBits(x - (static_cast<uint32_t>(e) << 23));
    const float c = Add(1.0f, Mul(FromS32(static_cast<int32_t>(k)), 0.0625f));

    // u = (m - c) / (m + c), in two parts.
    const float sum = Add(m, c);
    const float sum_high = High(sum);
    const float sum_low = Add(Sub(c, sum_high), m);
    const float inverse = Div(1.0f, sum);
    const float u_high = High(Mul(Sub(m, c), inverse));
    float rest = Sub(Sub(m, c), Mul(u_high, sum_high));
    rest = Sub(rest, Mul(u_high, sum_low));
    const float u_low = Mul(rest, inverse);

    // log2(m/c) = 2u/ln 2 · (1 + u²/3 + u⁴·c₅), times 16, in two parts.
    const float u2 = Mul(Add(u_high, u_low), Add(u_high, u_low));
    const float series = Mul(Add(FromBits(kThird), Mul(u2, FromBits(kLogSeries))), u2);
    const float a_high = Mul(u_high, 1.0f);
    const float a_low = Add(Mul(u_low, 1.0f), Mul(series, Add(u_high, u_low)));
    const float b_high = High(Add(a_high, a_low));
    const float b_low = Add(Sub(a_high, b_high), a_low);
    const float c_high = Mul(b_high, FromBits(kLog2eHigh));
    const float c_low = Add(Mul(b_high, FromBits(kLog2eLow)), Mul(b_low, FromBits(kLog2e)));
    const float d_high = High(Add(c_high, c_low));
    const float d_low = Add(Sub(c_high, d_high), c_low);

    // Add 16·log2(c) from the table and 16·e.
    const float table_high = FromBits(kLogHigh[k]);
    const float table_low = FromBits(kLogLow[k]);
    const float exponent = FromS32(e * 16);
    const float total = Add(Add(Add(Add(d_low, table_low), d_high), table_high), exponent);
    const float high = High(total);
    const float error = Sub(Sub(Sub(Sub(high, exponent), table_high), d_high), table_low);

    return {high, Sub(d_low, error)};
}

} // namespace

float Add(float a, float b)
{
    // The sum and its error, exactly (Knuth's two-sum).
    const double x = Flushed(a);
    const double y = Flushed(b);
    const double sum = x + y;
    const double y_part = sum - x;
    const double error = (x - (sum - y_part)) + (y - y_part);

    return Nearest(sum, error);
}

float Sub(float a, float b)
{
    return Add(a, -b);
}

float Mul(float a, float b)
{
    // Two significands of 24 bits: a double holds the product exactly.
    return Nearest(static_cast<double>(Flushed(a)) * Flushed(b), 0.0);
}

float Div(float a, float b)
{
    // The remainder of the quotient rounded to nearest is exact. Its sign gives the rounding's direction.
    const double x = Flushed(a);
    const double y = Flushed(b);
    const double quotient = x / y;
    if (!std::isfinite(quotient) || quotient == 0.0)
    {
        return Nearest(quotient, 0.0);
    }

    const double remainder = std::fma(-quotient, y, x);
    const double error = remainder == 0.0 ? 0.0 : ((remainder < 0.0) == (y < 0.0) ? 1.0 : -1.0);

    return Nearest(quotient, error);
}

float FromU32(uint32_t value)
{
    return Nearest(static_cast<double>(value), 0.0);
}

float FromS32(int32_t value)
{
    return Nearest(static_cast<double>(value), 0.0);
}

uint32_t ToU32(float value)
{
    const float v = Flushed(value);
    if (std::isnan(v) || v <= 0.0f)
    {
        return 0;
    }
    if (v >= 4294967296.0f)
    {
        return std::numeric_limits<uint32_t>::max();
    }

    return static_cast<uint32_t>(v);
}

int32_t ToS32(float value)
{
    const float v = Flushed(value);
    if (std::isnan(v))
    {
        return 0;
    }
    if (v >= 2147483648.0f)
    {
        return std::numeric_limits<int32_t>::max();
    }
    if (v <= -2147483648.0f)
    {
        return std::numeric_limits<int32_t>::min();
    }

    return static_cast<int32_t>(v);
}

float PowF(float x, float y)
{
    const uint32_t x_bits = Bits(x);
    if (!(x > 0.0f) || !std::isnormal(x) || x_bits - 0x3f7ff000u <= 0x2000u || std::isnan(y) || y > 0.0f)
    {
        throw std::domain_error("PowF covers a positive normal x away from 1 and a y of 0 or less");
    }

    // The special values: x^0 is 1, and x^-∞ is 0 above 1 and ∞ below. A y next to 0 counts as -2^-64.
    y = Flushed(y);
    if (y == 0.0f)
    {
        return 1.0f;
    }
    if (std::isinf(y))
    {
        return x > 1.0f ? 0.0f : std::numeric_limits<float>::infinity();
    }
    if (std::fabs(y) < FromBits(0x1f800000))
    {
        y = -FromBits(0x1f800000);
    }

    // p = 16·y·log2(x), in two parts.
    const Log16 log = LogOf(x_bits);
    const float y_high = High(y);
    const float y_low = Sub(y, y_high);
    const float product = Mul(log.high, y_high);
    const float cross = Add(Mul(log.high, y_low), Mul(log.low, Add(y_high, y_low)));
    const float p_high = High(Add(product, cross));
    const float p_low = Add(Sub(product, p_high), cross);
    const float p = Add(p_high, p_low);

    // 2^(p/16) = 2^(n/16) · 2^(f/16), with n the nearest integer to p, its halves away from zero.
    const int32_t n = ToS32(Add(p < 0.0f ? -0.5f : 0.5f, p));
    const int32_t whole = n >> 4;
    const std::size_t j = static_cast<std::size_t>(n & 15);
    const float f = Add(Sub(p_high, FromS32(n)), p_low);
    const float cubic = Add(FromBits(kPowSquare), Mul(f, FromBits(kPowCubic)));
    const float q = Mul(Add(FromBits(kPowLinear), Mul(f, cubic)), f);
    const float fraction = Add(Add(FromBits(kPowLow[j]), Mul(FromBits(kPow[j]), q)), FromBits(kPowHigh[j]));

    // Scale by 2^whole, in two steps where 2^whole lies outside the normal range. A power below the range flushes to 0,
    // and a power above it gives infinity.
    if (std::abs(whole) < 126)
    {
        return Mul(FromBits(static_cast<uint32_t>(whole + 127) << 23), Mul(1.0f, fraction));
    }
    if (std::abs(whole) >= 192)
    {
        return whole < 0 ? 0.0f : std::numeric_limits<float>::infinity();
    }

    const int32_t half = whole / 2;
    const float first = Mul(FromBits(static_cast<uint32_t>(half + 127) << 23), Mul(1.0f, fraction));

    return Mul(first, FromBits(static_cast<uint32_t>(whole - half + 127) << 23));
}

} // namespace ntdlib::ctr_fx::vfp
