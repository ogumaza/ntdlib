// SPDX-License-Identifier: MIT

// The model of the C JAudio for Pikmin (jac): the pitch arithmetic, the generator of random numbers and an
// oscillator's steps, on values built for the test, and the wave archives that a scene's load can read
// (SceneArchives) on wave systems built for the test. The keys' pitches follow equal temperament, within the rounding
// of the game's table. The pitches of fractions of a semitone are Pikmin's. An oscillator's time counts in 600ths of a
// second at the DSP's rate, 32,028.5 Hz, in frames of 80 samples.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"
#include "xsflib/testing/near.h"

#include "ntdlib/jac/bank.h"
#include "ntdlib/jac/calc.h"
#include "ntdlib/jac/oscillator.h"
#include "ntdlib/jaudio/wave_system.h"
#include "ntdlib/jaudio1/calc.h"
#include "ntdlib/jaudio2/dsp.h"

using namespace xsflib::testing;

namespace
{

namespace jac = ntdlib::jac;

// Whether `value` is within a relative 1e-5 of `expected`.
bool NearRatio(float value, double expected)
{
    return Near(value, expected, 1e-5 * std::fabs(expected));
}

// The pitch of a key against key 60 doubles every 12 keys. A key above 127 reads as 127.
void TestKeyPitch()
{
    XSFLIB_CHECK(NearRatio(jac::KeyPitch(60), 1.0));
    XSFLIB_CHECK(NearRatio(jac::KeyPitch(72), 2.0));
    XSFLIB_CHECK(NearRatio(jac::KeyPitch(48), 0.5));
    XSFLIB_CHECK(NearRatio(jac::KeyPitch(61), std::pow(2.0, 1.0 / 12.0)));
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

    XSFLIB_CHECK(NearRatio(jac::PitchToCent(0.0f, 1.0f), 1.0));
    XSFLIB_CHECK(NearRatio(jac::PitchToCent(1.0f, 3.0f), 2.0));
    XSFLIB_CHECK(NearRatio(jac::PitchToCent(-0.5f, 1.5f), std::pow(2.0, -3.0 / 12.0)));
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
    XSFLIB_CHECK(NearRatio(second, 0.5 / frames));
    XSFLIB_CHECK(NearRatio(third, 0.5 * 2.0 / frames));
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

namespace nj = ntdlib::jaudio;
using Names = std::vector<std::string>;

// A group of `waves` waves of 64 bytes each in archive `name`. C-DF names them with ids from `first_id`. The scene's
// external mode is `mode`, and C-EX holds `cex`.
nj::WaveGroup Group(const std::string& name, uint16_t first_id, uint16_t waves, uint32_t mode,
                    const std::vector<uint32_t>& cex)
{
    nj::WaveGroup group{.archive = name};
    std::vector<uint32_t> cdf;
    for (uint16_t i = 0; i < waves; i++)
    {
        group.archive_waves.push_back({.offset = 64u * i, .size = 64});
        cdf.push_back(first_id + i);
    }
    group.scene.external_mode = mode;
    group.scene.cdf = cdf;
    if (!cex.empty())
    {
        group.scene.cex = cex;
    }

    return group;
}

// The wave systems of the tests below. The system at place 2 has four scenes. Scene 1's C-EX names waves of scenes 0
// and 2, and a scene 9 that it doesn't have. Scene 3 has the same words with an external mode of 0. A second system of
// number 2 comes later, and Banks leaves it out.
std::vector<nj::WaveSystem> SceneSystems()
{
    nj::WaveSystem system{.number = 2};
    system.groups = {Group("s0.aw", 0x10, 4, 0, {}), Group("s1.aw", 0x20, 2, 1, {0x00000011, 0x00020031, 0x00090001}),
                     Group("s2.aw", 0x30, 2, 0, {}), Group("s3.aw", 0x40, 1, 0, {0x00000011, 0x00020031})};
    nj::WaveSystem later{.number = 2};
    later.groups = {Group("later.aw", 0x10, 1, 0, {})};

    return {system, later};
}

// SceneArchives: the scene's archive, and with an external mode other than 0 the archives of the scenes that C-EX's
// words name. A scene or a system that the systems don't have reads nothing.
void TestSceneArchives()
{
    const std::vector<nj::WaveSystem> systems = SceneSystems();
    XSFLIB_CHECK(jac::SceneArchives(systems, 2, 0) == (Names{"s0.aw"}));
    XSFLIB_CHECK(jac::SceneArchives(systems, 2, 1) == (Names{"s0.aw", "s1.aw", "s2.aw"}));
    XSFLIB_CHECK(jac::SceneArchives(systems, 2, 3) == (Names{"s3.aw"}));
    XSFLIB_CHECK(jac::SceneArchives(systems, 2, 4).empty());
    XSFLIB_CHECK(jac::SceneArchives(systems, 3, 0).empty());
    XSFLIB_CHECK(jac::SceneArchives(systems, 0x102, 0).empty());
}

// Against the model: every archive that a scene's set asks the archive callback of Banks for is in SceneArchives,
// whatever the scenes set before it.
void TestSceneArchivesAgainstBanks()
{
    const std::vector<nj::WaveSystem> systems = SceneSystems();
    for (const std::vector<uint32_t>& order : std::vector<std::vector<uint32_t>>{{0}, {1}, {3}, {1, 0, 2}, {0, 2, 1}})
    {
        std::set<std::string> asked;
        std::vector<uint8_t> bytes(0x100, 0x11);
        ntdlib::jaudio2::DspMemory memory(0x100000);
        jac::Banks banks(
            {}, systems,
            [&asked, &bytes](const std::string& name)
            {
                asked.insert(name);
                return &bytes;
            },
            memory, 0x10000, 0x80000);
        std::set<std::string> allowed;
        for (const uint32_t scene : order)
        {
            const Names names = jac::SceneArchives(systems, 2, scene);
            allowed.insert(names.begin(), names.end());
            XSFLIB_CHECK(banks.SetScene(2, scene, true));
        }
        XSFLIB_CHECK(!asked.empty() && std::includes(allowed.begin(), allowed.end(), asked.begin(), asked.end()));
        if (order == std::vector<uint32_t>{1})
        {
            XSFLIB_CHECK((asked == std::set<std::string>{"s0.aw", "s1.aw", "s2.aw"}));
        }
    }
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
        TestSceneArchives();
        TestSceneArchivesAgainstBanks();
    };

    return RunTest("jac", tests);
}
