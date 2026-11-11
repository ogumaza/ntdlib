// SPDX-License-Identifier: MIT

// The arithmetic of JAudio1's CPU side as the Switch release of Pikmin 2 builds it for AArch64: the pitch of a key
// (JASDriver::key2pitch_c5), of a fraction of semitones (JASPlayer::pitchToCent), and JMath's table of 2048 sines.
// Unlike the Galaxy games' build, this one fuses no product into a sum: every step rounds on its own.

#pragma once

#include <array>
#include <cstdint>

#include "ntdlib/jaudio2/calc.h"

namespace ntdlib::jaudio1
{

using jaudio2::ToInt32;
using jaudio2::ToUint32;

// π/2 in single precision.
inline constexpr float kHalfPi = 1.57079637f;

// JMath::TRandom_fast_: x = x × 0x19660d + 0x3c6ef35f. JASInstRand, JAI (JAInter::Const::random), JALCalc
// (getRandom_0_1) and the sound code (PSSystem::oRandom) each draw from a generator of this kind. The game starts each
// generator at 0.
struct RandomFast
{
    uint32_t value = 0;

    // nextFloat_0_1: the next number from 1 to 2, from the generator's top 23 bits, minus 1.
    float Next();
};

// The step of JASPlayer::getRandomS32's generator: the next value from the last two, `v0` the older. The C JAudio's
// GetRandom_s32 takes the same step.
int32_t NextRandomS32(int32_t v0, int32_t v1);

// The pitch of `key` against C5 (60): 2 to the (key - 60) / 12, from main's table. The key is clamped to 0 to 127.
float Key2Pitch(int32_t key);

// JASPlayer::pitchToCent: the pitch of `value` × 4 × `range` semitones. The whole semitones come from Key2Pitch, and
// the fraction from a table of 64 steps of a semitone.
float PitchToCent(float value, float range);

// JMASinRadian. JMath's table of sines (TSinCosTable<2048>) holds the sine of i × 2π / 2048 at entry i. JMASinRadian
// reads it with the angle times 2048 / 2π, truncated, in 11 bits. A negative angle reads the table with the angle's
// magnitude and negates the entry.
float SinRadian(float radians);

// A track's level of the delay between its outputs (the field at +0x2e0): 128 times the level, truncated, then one of
// the two delays. The Switch build tests the byte's sign by bit 7 of the 32-bit value. The C JAudio's OSf32tos8 and the
// code around it give the same delays. The first is the left output's, the second the right output's.
std::array<uint8_t, 2> DelaySamples(float level);

// Whether a condition (its low 4 bits) holds for the 16-bit `value` of register 3 (JASSeqParser::conditionCheck, and
// the C JAudio's __ConditionCheck): 0 always, 1 zero, 2 not zero, 3 one, 4 negative, 5 not negative. No other
// condition holds.
bool ConditionHolds(uint16_t value, uint32_t condition);

} // namespace ntdlib::jaudio1
