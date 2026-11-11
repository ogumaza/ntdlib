// SPDX-License-Identifier: MIT

// The constants and the arithmetic that the two players of the N64 Music Tools share. mus_player.cpp contains the
// readers of banks and songs and the early player. mus_late_player.cpp contains the late player.

// The addresses in the comments are places in the code of Pokémon Stadium, the first game that this model was checked
// against.

#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace ntdlib::n64::mus_player_common
{

inline constexpr int kRest = 96;
inline constexpr uint16_t kEndlessLength = 32767;
inline constexpr int kTicksShift = 8;
inline constexpr int kTick = 1 << kTicksShift;
inline constexpr int kMostCommandsInAStep = 100000;
inline constexpr int kReleasePhase = 4;
inline constexpr int kAttackPhase = 1;
inline constexpr int kDecayPhase = 2;
inline constexpr uint64_t kNeverOff = 0x7FFFFFFF;

// A channel's envelope bytes.
inline constexpr uint32_t kEnvelopeSize = 7;
inline constexpr uint32_t kEnvelopeAttack = 2;
inline constexpr uint32_t kEnvelopePeak = 3;
inline constexpr uint32_t kEnvelopeDecay = 4;
inline constexpr uint32_t kEnvelopeSustain = 5;
inline constexpr uint32_t kEnvelopeRelease = 6;

inline constexpr double kVibratoUnitsPerSemitone = 50.0;
inline constexpr double kDistortUnitsPerSemitone = 100.0;
inline constexpr double kBendRangeUnit = 1.0 / 64.0;
inline constexpr double kBendCentre = 64.0;
inline constexpr double kSemitoneExponent = 0.08333333333333333;
inline constexpr double kMostRatio = 2.0;
inline constexpr uint32_t kTempoScaleShift = 7;
inline constexpr int kTempoNumerator = 3;
inline constexpr int kTempoShift = 13;
inline constexpr int kTempoBeat = 120;
inline constexpr int kDefaultTempoNumerator = 0x6000;

// libultra's sinf, from the values in the game's code at 0x80056ED0.
inline float Sine(float value)
{
    constexpr std::array<double, 4> kPolynomial = {-0.16666659550427756, 0.008333066246082155, -0.0001980960290193795,
                                                   2.605780637968037e-06};
    constexpr double kInversePi = 0.3183098861837907;
    constexpr double kPiHigh = 3.1415926218032837;
    constexpr double kPiLow = 3.178650954705639e-08;
    constexpr int kExponentOfSmall = 230;
    constexpr int kExponentOfNoReduction = 255;
    constexpr int kExponentOfLargest = 310;

    const auto bits = std::bit_cast<uint32_t>(value);
    const int exponent = static_cast<int>((bits >> 22) & 0x1FF);
    const auto series = [&](double x)
    {
        const double square = x * x;
        const double poly =
            kPolynomial[0] + square * (kPolynomial[1] + square * (kPolynomial[2] + square * kPolynomial[3]));

        return x * square * poly + x;
    };

    if (exponent < kExponentOfNoReduction)
    {
        return exponent >= kExponentOfSmall ? static_cast<float>(series(static_cast<double>(value))) : value;
    }

    if (exponent >= kExponentOfLargest)
    {
        return std::isnan(value) ? value : 0.0F;
    }

    const auto wide = static_cast<double>(value);
    const double scaled = wide * kInversePi;
    const auto turns = static_cast<int32_t>(scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5);
    const auto count = static_cast<double>(turns);
    const double reduced = wide - count * kPiHigh - count * kPiLow;
    const auto result = static_cast<float>(series(reduced));

    return (turns & 1) != 0 ? -result : result;
}

// The game's power of 2, at 0x8003BCE4.
inline float PowerOfTwo(float value)
{
    constexpr std::array<double, 6> kPolynomial = {0.693147180559945,   0.240226506959101,   0.0555041086648216,
                                                   0.00961812910762848, 0.00133335581464284, 0.000154035303933816};
    if (value == 0.0F)
    {
        return 1.0F;
    }

    const float x = value > 0.0F ? value : -value;
    const float square = x * x;
    const float fourth = square * square;
    const double sum = static_cast<double>(x) * kPolynomial[0] + 1.0 + static_cast<double>(square) * kPolynomial[1] +
                       static_cast<double>(square * x) * kPolynomial[2] + static_cast<double>(fourth) * kPolynomial[3] +
                       static_cast<double>(fourth * x) * kPolynomial[4] +
                       static_cast<double>(fourth * square) * kPolynomial[5];

    return static_cast<float>(value > 0.0F ? sum : 1.0 / sum);
}

} // namespace ntdlib::n64::mus_player_common
