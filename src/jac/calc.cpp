// SPDX-License-Identifier: MIT

#include "ntdlib/jac/calc.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "ntdlib/jaudio1/calc.h"
#include "ntdlib/jaudio2/calc.h"

namespace ntdlib::jac
{

namespace
{

using jaudio2::ToInt32;

// The coefficients of the system's sinf (0x5aa160 in the SDK's module, with its table at 0xb698f0), by their bits:
// 2/π and π/2 for the reduction, then the cosine's polynomial from x^0 to x^8 and the sine's from x^3 to x^7. The
// system's table holds the cosine's polynomial a second time with each sign turned, for the third and fourth
// quarters.
constexpr uint64_t kInverseHalfPiBits = 0x3fe45f306dc9c883;
constexpr uint64_t kHalfPiBits = 0x3ff921fb54442d18;
constexpr std::array<uint64_t, 5> kCosineBits = {0x3ff0000000000000, 0xbfdffffffd0c621c, 0x3fa55553e1068f19,
                                                 0xbf56c087e89a359d, 0x3ef99343027bf8c3};
constexpr std::array<uint64_t, 3> kSineBits = {0xbfc555545995a603, 0x3f81107605230bc4, 0xbf2994eb3774cf24};

double Coefficient(uint64_t bits)
{
    return std::bit_cast<double>(bits);
}

// The system's polynomials in double precision, with each step rounded as the system's code rounds it.
double SinePolynomial(double x, double x2)
{
    const double x3 = x * x2;
    const double x5 = x2 * x3;
    const double low = x + x3 * Coefficient(kSineBits[0]);
    const double high = Coefficient(kSineBits[1]) + x2 * Coefficient(kSineBits[2]);

    return low + x5 * high;
}

double CosinePolynomial(double x2, bool turned)
{
    std::array<double, 5> c{};
    for (std::size_t i = 0; i < c.size(); i++)
    {
        c[i] = turned ? -Coefficient(kCosineBits[i]) : Coefficient(kCosineBits[i]);
    }
    const double x4 = x2 * x2;
    const double x6 = x2 * x4;
    const double low = (c[0] + x2 * c[1]) + x4 * c[2];
    const double high = c[3] + x2 * c[4];

    return low + x6 * high;
}

// The system's sinf for an argument of magnitude below 120: tiny arguments as they are, those below 0.75 by the sine's
// polynomial, and the rest reduced by the nearest multiple of π/2 to the polynomial of their quarter. The system
// reduces larger arguments another way. The tables take arguments from 0 to π/2 only.
float SystemSinf(float y)
{
    const uint32_t top = (std::bit_cast<uint32_t>(y) >> 20) & 0x7ff;
    const double x = y;
    if (top < 0x3f4)
    {
        if (top < 0x398)
        {
            return y;
        }

        return static_cast<float>(SinePolynomial(x, x * x));
    }

    const double r = Coefficient(kInverseHalfPiBits) * x;
    const int64_t quarter = std::llround(r);
    const double reduced = x - Coefficient(kHalfPiBits) * std::round(r);
    const double x2 = reduced * reduced;
    if ((quarter & 1) != 0)
    {
        return static_cast<float>(CosinePolynomial(x2, (quarter & 2) != 0));
    }

    const double sign = (quarter & 2) != 0 ? -1.0 : 1.0;
    return static_cast<float>(SinePolynomial(sign * reduced, x2));
}

constexpr std::size_t kTableSize = 257;

// Jac_InitSinTable's two tables, the second after the first as in the game's memory (SINTABLE at 0x7ca9c0 in main's
// bss). The first: the sines of i × π/2 / 256 in single precision. The second: the sines of (0.32612 + i × 0.34776 /
// 256) × π/2, worked out in double precision with π/2 and 0.32612 rounded to single precision first.
const std::array<float, 2 * kTableSize>& Tables()
{
    static const std::array<float, 2 * kTableSize> kTables = []
    {
        const float kHalfPi = std::bit_cast<float>(0x3fc90fdbu);
        const float kStep = std::bit_cast<float>(0x3b800000u);    // 1/256
        const double kScale = Coefficient(0x3fd641b328b6d86f);    // 0.34776
        const double kFineStep = Coefficient(0x3f70000000000000); // 1/256
        const double kBase = Coefficient(0x3fd4df2660000000);     // 0.32612 in single precision
        const double kTurn = Coefficient(0x3ff921fb60000000);     // π/2 in single precision
        std::array<float, 2 * kTableSize> tables{};
        for (std::size_t i = 0; i < kTableSize; i++)
        {
            const float index = static_cast<float>(i);
            tables[i] = SystemSinf((index * kHalfPi) * kStep);
            const double angle = ((static_cast<double>(index) * kScale) * kFineStep + kBase) * kTurn;
            tables[kTableSize + i] = SystemSinf(static_cast<float>(angle));
        }

        return tables;
    }();

    return kTables;
}

// The tables' entry at `index` from the first table's start, within the two tables.
float TableAt(int64_t index)
{
    const std::array<float, 2 * kTableSize>& tables = Tables();
    const int64_t last = static_cast<int64_t>(tables.size()) - 1;

    return tables[static_cast<std::size_t>(std::clamp<int64_t>(index, 0, last))];
}

} // namespace

int32_t Random::S32()
{
    const int32_t next = jaudio1::NextRandomS32(v0_, v1_);
    v0_ = v1_;
    v1_ = next;

    return next;
}

float Random::Sf32()
{
    const int32_t r = S32();
    const float value = static_cast<float>(r & 0xffffff) / 16777215.0f;

    return r < 0 ? -value : value;
}

float Sinf3(float x)
{
    return TableAt(ToInt32(x * 256.0f));
}

float SurroundCurve(float x)
{
    return TableAt(static_cast<int64_t>(kTableSize) + ToInt32(x * 256.0f));
}

// The game's C5BASE_PITCHTABLE (0x6b02fc in main's data) and KEY_TABLE (0x5bf2d8 in main's ro) hold the values of
// JAudio1's tables. Jam_PitchToCent works the pitch out as JASPlayer::pitchToCent does.
float PitchToCent(float value, float range)
{
    return jaudio1::PitchToCent(value, range);
}

float KeyPitch(uint32_t key)
{
    return jaudio1::Key2Pitch(static_cast<int32_t>(std::min<uint32_t>(key, 127)));
}

} // namespace ntdlib::jac
