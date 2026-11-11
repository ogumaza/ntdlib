// SPDX-License-Identifier: MIT

// The model of the C JAudio for Pikmin (jac): the pitch arithmetic, the generator of random numbers and an
// oscillator's steps, on values built for the test. The keys' pitches follow equal temperament, within the rounding of
// the game's table. The pitches of fractions of a semitone are Pikmin's. An oscillator's time counts in 600ths of a
// second at the DSP's rate, 32,028.5 Hz, in frames of 80 samples.

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

#include "xsflib/testing/check.h"

#include "ntdlib/jac/calc.h"
#include "ntdlib/jac/oscillator.h"
#include "ntdlib/jaudio1/calc.h"

using namespace xsflib::testing;

namespace
{

namespace jac = ntdlib::jac;

bool Near(float value, double expected)
{
    return std::fabs(static_cast<double>(value) - expected) <= 1e-5 * std::fabs(expected);
}

// The pitch of a key against key 60 doubles every 12 keys. A key above 127 reads as 127.
void TestKeyPitch()
{
    XSFLIB_CHECK(Near(jac::KeyPitch(60), 1.0));
    XSFLIB_CHECK(Near(jac::KeyPitch(72), 2.0));
    XSFLIB_CHECK(Near(jac::KeyPitch(48), 0.5));
    XSFLIB_CHECK(Near(jac::KeyPitch(61), std::pow(2.0, 1.0 / 12.0)));
    XSFLIB_CHECK(jac::KeyPitch(1000) == jac::KeyPitch(127));
}

// A bend of `value` × 4 × `range` semitones: whole semitones from the keys' table, and 64ths of a semitone from the
// fractions' table. A bend of a quarter semitone down plays key 59 and step 48 of the fractions. The expected values
// come from Pikmin's main: key 59 from C5BASE_PITCHTABLE (0x6b02fc in the data), and steps 32 and 48 from KEY_TABLE
// (0x5bf2d8 in the read-only data). The table's steps are 1/63.5 of a semitone, not 1/64.
void TestPitchToCent()
{
    constexpr float kKey59 = std::bit_cast<float>(0x3f71a1cbu);
    constexpr float kStep32 = std::bit_cast<float>(0x3f83c7d6u);
    constexpr float kStep48 = std::bit_cast<float>(0x3f85b670u);

    XSFLIB_CHECK(Near(jac::PitchToCent(0.0f, 1.0f), 1.0));
    XSFLIB_CHECK(Near(jac::PitchToCent(1.0f, 3.0f), 2.0));
    XSFLIB_CHECK(Near(jac::PitchToCent(-0.5f, 1.5f), std::pow(2.0, -3.0 / 12.0)));
    XSFLIB_CHECK(jac::PitchToCent(0.125f, 1.0f) == kStep32);
    XSFLIB_CHECK(jac::PitchToCent(-0.0625f, 1.0f) == kKey59 * kStep48);
}

// The generator moves its last value into V0 and its new one into V1. GetRandom_s32 takes JAudio1's step.
void TestRandom()
{
    jac::Random random;
    XSFLIB_CHECK(random.V0() == 0x1000 && random.V1() == 0x5555);

    const int32_t first = random.S32();

    XSFLIB_CHECK(first == ntdlib::jaudio1::NextRandomS32(0x1000, 0x5555));
    XSFLIB_CHECK(random.V0() == 0x5555 && random.V1() == first);

    jac::Random a;
    jac::Random b;
    const uint32_t unsigned_value = a.U32();
    const uint32_t unlimited = b.ULimit(0);

    XSFLIB_CHECK((unsigned_value & 0x80000000u) == 0);
    XSFLIB_CHECK(unlimited == unsigned_value);
    XSFLIB_CHECK(a.ULimit(10) < 10);

    for (int i = 0; i < 100; i++)
    {
        const float value = a.Sf32();
        XSFLIB_CHECK(value >= -1.0f && value <= 1.0f);
    }
}

// Without settings, an oscillator gives 1.
void TestOscillatorWithoutData()
{
    jac::OscState state;
    state.state = jac::OscState::kStart;

    const float level = jac::OscToOfs(nullptr, state);

    XSFLIB_CHECK(level == 1.0f && state.value == 1.0f);
}

// A linear move from 0 to 0.5 over 3 time units, then a hold. The move lasts 3 × 0.66726 frames, about 2. Each frame
// moves the level by 0.5 over that many frames. The frame that passes the move's end takes the hold.
void TestOscillatorMoveThenHold()
{
    const std::array<jac::OscPoint, 2> kPoints = {{{0, 3, 0x4000}, {jac::kPointHold, 0, 0}}};
    jac::OscData data;
    data.attack = {kPoints.data(), kPoints.size()};
    jac::OscState state;
    state.state = jac::OscState::kStart;
    const double frames = 3.0 * jac::OscTimeScale();

    const float first = jac::OscToOfs(&data, state);
    const float second = jac::OscToOfs(&data, state);
    const float third = jac::OscToOfs(&data, state);
    const float fourth = jac::OscToOfs(&data, state);
    const float fifth = jac::OscToOfs(&data, state);

    XSFLIB_CHECK(std::fabs(first) < 1e-6f);
    XSFLIB_CHECK(Near(second, 0.5 / frames));
    XSFLIB_CHECK(Near(third, 0.5 * 2.0 / frames));
    XSFLIB_CHECK(fourth == 0.5f && fifth == 0.5f);
    XSFLIB_CHECK(state.state == jac::OscState::kHold);
}

// A table that stops at once leaves the oscillator stopped. A point past the table's data reads as a stop.
void TestOscillatorStop()
{
    const std::array<jac::OscPoint, 1> kPoints = {{{jac::kPointStop, 0, 0}}};
    jac::OscData data;
    data.attack = {kPoints.data(), kPoints.size()};
    jac::OscState state;
    state.state = jac::OscState::kStart;

    jac::OscToOfs(&data, state);

    XSFLIB_CHECK(state.state == jac::OscState::kStop);
    XSFLIB_CHECK(data.attack.At(5).mode == jac::kPointStop);
}

} // namespace

int main()
{
    const auto tests = []
    {
        TestKeyPitch();
        TestPitchToCent();
        TestRandom();
        TestOscillatorWithoutData();
        TestOscillatorMoveThenHold();
        TestOscillatorStop();
    };

    return RunTest("jac", tests);
}
