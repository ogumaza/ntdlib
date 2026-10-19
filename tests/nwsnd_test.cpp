// SPDX-License-Identifier: MIT

// Tests of the sound library's model: the tables, the consoles' arithmetic, the helpers, the envelope and the LFO. The
// Wii U's helpers, envelope and LFO are checked value by value against what the library's code computes.

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/envelope.h"
#include "ntdlib/nwsnd/tables.h"
#include "ntdlib/nwsnd/util.h"
#include "ntdlib/sound_build.h"
#include "xsflib/testing/check.h"

namespace
{

using ntdlib::nwsnd::AdshrStatus;
using ntdlib::nwsnd::CafePolicy;
using ntdlib::nwsnd::CtrPolicy;
using ntdlib::nwsnd::NxPolicy;
using ntdlib::nwsnd::RevolutionPolicy;
namespace nwsnd = ntdlib::nwsnd;
namespace tables = ntdlib::nwsnd::tables;

constexpr float kNan = std::numeric_limits<float>::quiet_NaN();

void TestTables()
{
    XSFLIB_CHECK(tables::kSemitone[0] == 1.0f);
    XSFLIB_CHECK(tables::kFine[0] == 1.0f);
    XSFLIB_CHECK(tables::kDecibel[904] == 1.0f);
    XSFLIB_CHECK(tables::kDecibelSquare[127] == 0);
    XSFLIB_CHECK(tables::kLpfFreq[23] == 16000);

    // The Wii U's sine table holds the sine in its first column, and the Switch's table in its second.
    XSFLIB_CHECK(tables::kSin[64][0] == 1.0f);
    XSFLIB_CHECK(tables::kSinCos[64][1] == 1.0f);
}

void TestArithmetic()
{
    // A NaN converts to the most negative integer on the PowerPC, and to 0 on AArch64.
    XSFLIB_CHECK(CafePolicy::ToInt32(kNan) == INT32_MIN);
    XSFLIB_CHECK(NxPolicy::ToInt32(kNan) == 0);
    XSFLIB_CHECK(CafePolicy::ToInt32(-2.5f) == -2 && NxPolicy::ToInt32(-2.5f) == -2);
    XSFLIB_CHECK(CafePolicy::ToInt32(3e9f) == INT32_MAX && NxPolicy::ToInt32(3e9f) == INT32_MAX);

    // The PowerPC's shifts take the amount's low six bits. AArch64's shifts take the low five.
    XSFLIB_CHECK(CafePolicy::Shift(1, 33) == 0);
    XSFLIB_CHECK(NxPolicy::Shift(1, 33) == 2);
    XSFLIB_CHECK(CafePolicy::Shift(8, -33) == 0);
    XSFLIB_CHECK(NxPolicy::Shift(8, -33) == 4);
    XSFLIB_CHECK(CafePolicy::Shift(-8, -40) == -1);
    XSFLIB_CHECK(CafePolicy::Shift(5, 2) == 20 && NxPolicy::Shift(5, 2) == 20);
    XSFLIB_CHECK(CafePolicy::Shift(-20, -2) == -5 && NxPolicy::Shift(-20, -2) == -5);

    // A fall to exactly 0 gives -0 when it's fused and negated, and +0 when it's rounded twice.
    XSFLIB_CHECK(CafePolicy::Fall(1.0f, 1.0f, 1) == 0.0f && std::signbit(CafePolicy::Fall(1.0f, 1.0f, 1)));
    XSFLIB_CHECK(NxPolicy::Fall(1.0f, 1.0f, 1) == 0.0f && !std::signbit(NxPolicy::Fall(1.0f, 1.0f, 1)));
    XSFLIB_CHECK(CafePolicy::Fall(100.0f, 0.5f, 10) == 95.0f && NxPolicy::Fall(100.0f, 0.5f, 10) == 95.0f);

    // fsel takes the second operand for NaN. fmax keeps the NaN.
    XSFLIB_CHECK(CafePolicy::Max(kNan, -1.0f) == -1.0f);
    XSFLIB_CHECK(std::isnan(NxPolicy::Max(kNan, -1.0f)));
    XSFLIB_CHECK(CafePolicy::Max(0.25f, -1.0f) == 0.25f && NxPolicy::Max(0.25f, -1.0f) == 0.25f);
    XSFLIB_CHECK(CafePolicy::AtLeastZero(-3.0f) == 0.0f && NxPolicy::AtLeastZero(-3.0f) == 0.0f);

    // The square LFO and the LFO's wrap differ only for NaN.
    XSFLIB_CHECK(CafePolicy::Square(kNan) == 0.0f && NxPolicy::Square(kNan) == 1.0f);
    XSFLIB_CHECK(CafePolicy::Square(0.5f) == 1.0f && NxPolicy::Square(0.5f) == 1.0f);
    XSFLIB_CHECK(CafePolicy::Square(0.25f) == 0.0f && NxPolicy::Square(0.25f) == 0.0f);
    XSFLIB_CHECK(CafePolicy::Wrapped(kNan) && !NxPolicy::Wrapped(kNan));
    XSFLIB_CHECK(CafePolicy::Wrapped(1.0f) && NxPolicy::Wrapped(1.0f));

    // The curves' quarter turns.
    XSFLIB_CHECK(CafePolicy::Sine(0.25f) == 1.0f && NxPolicy::Sine(0.25f) == 1.0f);
    XSFLIB_CHECK(CafePolicy::Sine(0.0f) == 0.0f && NxPolicy::Sine(0.0f) == 0.0f);
    XSFLIB_CHECK(CafePolicy::Triangle(0.25f) == 1.0f && NxPolicy::Triangle(0.25f) == 1.0f);
    XSFLIB_CHECK(CafePolicy::Triangle(0.75f) == -1.0f && NxPolicy::Triangle(0.75f) == -1.0f);

    // The sines at an eighth of a turn, against sin(pi / 4). nn::util's table is fitted for interpolation. The Switch's
    // sine is checked there to within 1e-4.
    XSFLIB_CHECK(std::abs(CafePolicy::Sine(0.125f) - std::sqrt(0.5f)) < 1e-6f);
    XSFLIB_CHECK(std::abs(NxPolicy::Sine(0.125f) - std::sqrt(0.5f)) < 1e-4f);
}

void TestUtil()
{
    XSFLIB_CHECK(nwsnd::CalcPitchRatio(0) == 1.0f);
    XSFLIB_CHECK(nwsnd::CalcPitchRatio(3072) == 2.0f);
    XSFLIB_CHECK(nwsnd::CalcPitchRatio(-3072) == 0.5f);
    XSFLIB_CHECK(nwsnd::CalcPitchRatio(256) == tables::kSemitone[1]);
    XSFLIB_CHECK(nwsnd::ToUint64(kNan) == 0 && nwsnd::ToUint64(0.5f) == 0 && nwsnd::ToUint64(7.9f) == 7);

    // The volume holds to -90.4 and +6 dB. The Wii U holds NaN to -90.4 dB. On the Switch it gives 0 dB.
    XSFLIB_CHECK(nwsnd::CalcVolumeRatio<CafePolicy>(0.0f) == 1.0f && nwsnd::CalcVolumeRatio<NxPolicy>(0.0f) == 1.0f);
    XSFLIB_CHECK(nwsnd::CalcVolumeRatio<CafePolicy>(100.0f) == tables::kDecibel[964]);
    XSFLIB_CHECK(nwsnd::CalcVolumeRatio<CafePolicy>(-200.0f) == tables::kDecibel[0]);
    XSFLIB_CHECK(nwsnd::CalcVolumeRatio<CafePolicy>(kNan) == tables::kDecibel[0]);
    XSFLIB_CHECK(nwsnd::CalcVolumeRatio<NxPolicy>(kNan) == tables::kDecibel[904]);

    // The low-pass's cutoffs from the bottom step to the top.
    XSFLIB_CHECK(nwsnd::CalcLpfFreq<CafePolicy>(0.0f) == 80 && nwsnd::CalcLpfFreq<NxPolicy>(0.0f) == 80);
    XSFLIB_CHECK(nwsnd::CalcLpfFreq<CafePolicy>(1.0f) == 16000 && nwsnd::CalcLpfFreq<NxPolicy>(1.0f) == 16000);
    XSFLIB_CHECK(nwsnd::CalcLpfFreq<CafePolicy>(0.5f) == tables::kLpfFreq[10]);
    XSFLIB_CHECK(nwsnd::CalcLpfFreq<NxPolicy>(0.5f) == tables::kLpfFreq[10]);
    XSFLIB_CHECK(nwsnd::CalcLpfFreq<CafePolicy>(kNan) == 80 && nwsnd::CalcLpfFreq<NxPolicy>(kNan) == 80);

    // The 3DS multiplies by 29.999998 where the Wii U divides by 1/30. Of the floats from the first step to 0.9, one
    // lands on another step: 0x1.57ab4cp-2 is the Wii U's step 6 and the 3DS's step 5.
    XSFLIB_CHECK(nwsnd::LpfFreqIndex<CafePolicy>(0x1.57ab4cp-2f) == 6);
    XSFLIB_CHECK(nwsnd::LpfFreqIndex<NxPolicy>(0x1.57ab4cp-2f) == 6);
    XSFLIB_CHECK(nwsnd::LpfFreqIndex<CtrPolicy>(0x1.57ab4cp-2f) == 5);
    XSFLIB_CHECK(nwsnd::CalcLpfFreq<CtrPolicy>(0.0f) == 80 && nwsnd::CalcLpfFreq<CtrPolicy>(1.0f) == 16000);
    XSFLIB_CHECK(nwsnd::CalcLpfFreq<CtrPolicy>(-1.0f) == 80 && nwsnd::CalcLpfFreq<CtrPolicy>(2.0f) == 16000);
    XSFLIB_CHECK(nwsnd::CalcLpfFreq<CtrPolicy>(0.5f) == tables::kLpfFreq[10] &&
                 nwsnd::CalcLpfFreq<CtrPolicy>(kNan) == 80);

    // The Wii's builds before November 2008 work the cutoff out as 32000 * 2^(10 (value - 1)) Hz, truncated, from 31 Hz
    // to 32000 Hz. A NaN gives the lowest cutoff. So does a value of 0.
    const ntdlib::SndBuild old_wii{.date = 20080919};
    XSFLIB_CHECK(nwsnd::CalcLpfFreq<RevolutionPolicy>(0.0f, old_wii) == 31 &&
                 nwsnd::CalcLpfFreq<RevolutionPolicy>(0.5f, old_wii) == 1000);
    XSFLIB_CHECK(nwsnd::CalcLpfFreq<RevolutionPolicy>(57.0f / 64.0f, old_wii) == 14993);
    XSFLIB_CHECK(nwsnd::CalcLpfFreq<RevolutionPolicy>(1.0f, old_wii) == 32000 &&
                 nwsnd::CalcLpfFreq<RevolutionPolicy>(2.0f, old_wii) == 32000);
    XSFLIB_CHECK(nwsnd::CalcLpfFreq<RevolutionPolicy>(-1.0f, old_wii) == 31 &&
                 nwsnd::CalcLpfFreq<RevolutionPolicy>(kNan, old_wii) == 31);

    // Later builds take the cutoff from kLpfFreq, as the Wii U does. The other consoles' cutoffs don't depend on the
    // build.
    const ntdlib::SndBuild new_wii{.date = 20081120};
    for (const float value : {0.0f, 0.13f, 0.5f, 57.0f / 64.0f, 0.95f, 1.0f, 2.0f, -1.0f, kNan})
    {
        XSFLIB_CHECK(nwsnd::CalcLpfFreq<RevolutionPolicy>(value, new_wii) == nwsnd::CalcLpfFreq<CafePolicy>(value));
    }
    XSFLIB_CHECK(nwsnd::CalcLpfFreq<RevolutionPolicy>(0.0f, new_wii) == 80 &&
                 nwsnd::CalcLpfFreq<RevolutionPolicy>(57.0f / 64.0f, new_wii) == tables::kLpfFreq[22]);
    XSFLIB_CHECK(nwsnd::CalcLpfFreq<CafePolicy>(0.5f, CafePolicy::Build{}) == tables::kLpfFreq[10]);

    // A centred pan, and surround output: the Wii U takes the surround curves, and the Switch only if the sound's
    // settings select them.
    const nwsnd::PanInfo kInfo;
    const auto kStereo = nwsnd::OutputMode::kStereo;
    const auto kSurround = nwsnd::OutputMode::kSurround;
    nwsnd::PanInfo asks;
    asks.surround_curves = true;

    XSFLIB_CHECK(nwsnd::CalcPanRatio<CafePolicy>(0.0f, kInfo, kStereo) == tables::kStereoPan[0][128]);
    XSFLIB_CHECK(nwsnd::CalcPanRatio<NxPolicy>(0.0f, kInfo, kStereo) == tables::kStereoPan[0][128]);
    XSFLIB_CHECK(nwsnd::CalcPanRatio<CafePolicy>(-1.0f, kInfo, kSurround) == tables::kSurroundPan[0][0]);
    XSFLIB_CHECK(nwsnd::CalcPanRatio<NxPolicy>(-1.0f, kInfo, kSurround) == tables::kStereoPan[0][0]);
    XSFLIB_CHECK(nwsnd::CalcPanRatio<NxPolicy>(-1.0f, asks, kSurround) == tables::kSurroundPan[0][0]);
    XSFLIB_CHECK(nwsnd::CalcSurroundPanRatio<CafePolicy>(0.0f, kInfo) == tables::kStereoPan[0][0]);
    XSFLIB_CHECK(nwsnd::CalcSurroundPanRatio<NxPolicy>(2.0f, kInfo) == tables::kStereoPan[0][256]);

    // The Switch's one-pole low-pass from its tables, and none for a rate it has no table for.
    const auto biquad = nwsnd::CalcLowPassBiquad(16000, 48000, true);

    XSFLIB_CHECK(biquad[0] == tables::kLowPass48k[23][0] && biquad[3] == tables::kLowPass48k[23][3]);
    XSFLIB_CHECK(nwsnd::CalcLowPassBiquad(16000, 44100, true)[0] == 0);

    nwsnd::Random random;

    const uint32_t start = random.State();
    const uint16_t first = random.Next();

    XSFLIB_CHECK(start == 0x12345678);
    XSFLIB_CHECK(first == static_cast<uint16_t>((0x12345678u * 0x19660Du + 0x3C6EF35Fu) >> 16));
}

// The biquad presets' rows on each console: the value times the table's last row, or value * (2 - value) times it for
// the band-passes, truncated and held to the table. The expected rows are fixed.
void TestBiquadPresets()
{
    // 111 * 0.5 is 55.5, 96 * 90 / 127 is 68.03, and 121 * 0.5 * 1.5 is 90.75.
    XSFLIB_CHECK(nwsnd::BiquadPresetRow<CafePolicy>(1, 0.5f) == 55);
    XSFLIB_CHECK(nwsnd::BiquadPresetRow<RevolutionPolicy>(2, 90.0f / 127.0f) == 68);
    XSFLIB_CHECK(nwsnd::BiquadPresetRow<CtrPolicy>(3, 0.5f) == 90);
    XSFLIB_CHECK(nwsnd::BiquadPresetRow<NxPolicy>(4, 1.0f) == 92 && nwsnd::BiquadPresetRow<NxPolicy>(5, 1.0f) == 92);

    // The rows don't hold the value to 1. A low-pass at 2 takes the last row, and a band-pass at 255 / 127, past
    // value * (2 - value)'s zero, the first. NaN and the integer's limits are held to the table on every console.
    XSFLIB_CHECK(nwsnd::BiquadPresetRow<RevolutionPolicy>(1, 2.0f) == 111);
    XSFLIB_CHECK(nwsnd::BiquadPresetRow<RevolutionPolicy>(3, 255.0f / 127.0f) == 0);
    XSFLIB_CHECK(nwsnd::BiquadPresetRow<CafePolicy>(1, kNan) == 0 && nwsnd::BiquadPresetRow<NxPolicy>(1, kNan) == 0);
    XSFLIB_CHECK(nwsnd::BiquadPresetRow<CtrPolicy>(1, kNan) == 0 &&
                 nwsnd::BiquadPresetRow<RevolutionPolicy>(1, kNan) == 0);
    XSFLIB_CHECK(nwsnd::BiquadPresetRow<CafePolicy>(2, 1e30f) == 96 &&
                 nwsnd::BiquadPresetRow<CtrPolicy>(2, -1e30f) == 0);

    // Each type's table. The Switch's types 6 to 10 are the same filters made for 48 kHz. The other consoles have no
    // preset past type 5, and no console has one at type 0 or past type 10.
    XSFLIB_CHECK(nwsnd::BiquadPresetCoefficients<CafePolicy>(1, 0.5f) == tables::kBiquadLpf[55]);
    XSFLIB_CHECK(nwsnd::BiquadPresetCoefficients<CafePolicy>(2, 0.5f) == tables::kBiquadHpf[48]);
    XSFLIB_CHECK(nwsnd::BiquadPresetCoefficients<CafePolicy>(3, 0.5f) == tables::kBiquadBpf512[90]);
    XSFLIB_CHECK(nwsnd::BiquadPresetCoefficients<CafePolicy>(4, 0.5f) == tables::kBiquadBpf1024[69]);
    XSFLIB_CHECK(nwsnd::BiquadPresetCoefficients<CafePolicy>(5, 0.5f) == tables::kBiquadBpf2048[69]);
    XSFLIB_CHECK(nwsnd::BiquadPresetCoefficients<NxPolicy>(6, 0.5f) == tables::kBiquadLpf48k[55]);
    XSFLIB_CHECK(nwsnd::BiquadPresetCoefficients<NxPolicy>(7, 0.5f) == tables::kBiquadHpf48k[48]);
    XSFLIB_CHECK(nwsnd::BiquadPresetCoefficients<NxPolicy>(8, 0.5f) == tables::kBiquadBpf512_48k[90]);
    XSFLIB_CHECK(nwsnd::BiquadPresetCoefficients<NxPolicy>(9, 0.5f) == tables::kBiquadBpf1024_48k[69]);
    XSFLIB_CHECK(nwsnd::BiquadPresetCoefficients<NxPolicy>(10, 0.5f) == tables::kBiquadBpf2048_48k[69]);
    XSFLIB_CHECK(nwsnd::BiquadPresetCoefficients<CafePolicy>(6, 0.5f) == nullptr);
    XSFLIB_CHECK(nwsnd::BiquadPresetCoefficients<CtrPolicy>(6, 0.5f) == nullptr);
    XSFLIB_CHECK(nwsnd::BiquadPresetCoefficients<RevolutionPolicy>(6, 0.5f) == nullptr);
    XSFLIB_CHECK(nwsnd::BiquadPresetCoefficients<NxPolicy>(0, 0.5f) == nullptr);
    XSFLIB_CHECK(nwsnd::BiquadPresetCoefficients<NxPolicy>(11, 0.5f) == nullptr);
}

template <typename Policy>
void TestEnvelope(const char* name)
{
    // The fastest attack reaches 0 dB at once. The hold then lasts (hold + 1)^2 / 4 ms, and the decay falls to the
    // sustain level.
    nwsnd::CurveAdshr<Policy> envelope;
    envelope.SetAttack(127);
    envelope.SetHold(3);
    envelope.SetDecay(100);
    envelope.SetSustain(64);

    envelope.Update(1);
    xsflib::testing::Check(envelope.GetStatus() == AdshrStatus::kHold, std::string(name) + ": the hold starts");
    envelope.Update(4);
    xsflib::testing::Check(envelope.GetStatus() == AdshrStatus::kDecay, std::string(name) + ": the decay starts");

    for (int i = 0; i < 1000 && envelope.GetStatus() == AdshrStatus::kDecay; ++i)
    {
        envelope.Update(3);
    }

    xsflib::testing::Check(envelope.GetStatus() == AdshrStatus::kSustain && envelope.Level() < 0.0f,
                           std::string(name) + ": the decay reaches the sustain level");

    envelope.SetStatus(AdshrStatus::kRelease);
    const float before = envelope.Level();

    envelope.Update(3);

    xsflib::testing::Check(envelope.Level() < before, std::string(name) + ": the release falls");

    // The LFO starts at its phase and advances by its speed.
    nwsnd::CurveLfo<Policy> lfo;
    lfo.param.depth = 1.0f;
    lfo.param.speed = 250.0f;
    lfo.param.range = 1;
    nwsnd::Random random;

    lfo.Update(1);

    xsflib::testing::Check(lfo.GetValue(random) == 1.0f, std::string(name) + ": the sine at a quarter turn");
    lfo.param.curve = 1;
    xsflib::testing::Check(lfo.GetValue(random) == 1.0f, std::string(name) + ": the triangle at a quarter turn");
    lfo.param.curve = 2;
    xsflib::testing::Check(lfo.GetValue(random) == 0.25f, std::string(name) + ": the saw at a quarter turn");
}

// The Wii U's helpers at more points: the pitch's octaves and table steps, the volume's steps, the pan's curves and
// bypass, the low-pass's ends and the random numbers.
void TestCafeUtil()
{
    // The tables at the points where their values are exact, and the sine table's wrap.
    XSFLIB_CHECK(tables::kSin[0][2] == 1.0f && tables::kSin[256][0] == tables::kSin[0][0]);
    XSFLIB_CHECK(tables::kLpfFreq[0] == 80);

    // Octaves double and halve exactly, and semitones and 1/256 semitones come from the tables.
    XSFLIB_CHECK(nwsnd::CalcPitchRatio(-3 * 3072) == 0.125f);
    XSFLIB_CHECK(nwsnd::CalcPitchRatio(7 * 256 + 5) == tables::kSemitone[7] * tables::kFine[5]);
    XSFLIB_CHECK(nwsnd::CalcPitchRatio(3072 + 256) == 2.0f * tables::kSemitone[1]);
    XSFLIB_CHECK(nwsnd::CalcPitchRatio(-1) == 0.5f * tables::kSemitone[11] * tables::kFine[255]);

    // An octave up doubles every ratio exactly. Every ratio is within 2e-7 of 2^(pitch / 3072). Both hold over three
    // octaves either way.
    int wrong_octave = 0;
    int inexact = 0;
    for (int pitch = -3 * 3072; pitch <= 3 * 3072; ++pitch)
    {
        wrong_octave += nwsnd::CalcPitchRatio(pitch + 3072) != 2.0f * nwsnd::CalcPitchRatio(pitch);
        inexact += std::abs(nwsnd::CalcPitchRatio(pitch) / std::exp2(pitch / 3072.0) - 1.0) > 2e-7;
    }

    XSFLIB_CHECK(wrong_octave == 0 && inexact == 0);

    // Steps of 0.1 dB from -90.4 dB, truncated.
    XSFLIB_CHECK(nwsnd::CalcVolumeRatio<CafePolicy>(-6.05f) == tables::kDecibel[904 - 60]);

    // The square-root curve's centre is unity when the curve is scaled to its centre.
    nwsnd::PanInfo info;
    info.center_zero_db = true;

    XSFLIB_CHECK(nwsnd::CalcPanRatio<CafePolicy>(0.0f, info, nwsnd::OutputMode::kStereo) == 1.0f);

    // A pan beyond either end is held there.
    info = {};

    XSFLIB_CHECK(nwsnd::CalcPanRatio<CafePolicy>(-5.0f, info, nwsnd::OutputMode::kStereo) == tables::kStereoPan[0][0]);
    XSFLIB_CHECK(nwsnd::CalcPanRatio<CafePolicy>(5.0f, info, nwsnd::OutputMode::kStereo) == tables::kStereoPan[0][256]);

    // Surround output takes the surround curves unless the pan bypasses them.
    info.curve = 1;

    XSFLIB_CHECK(nwsnd::CalcPanRatio<CafePolicy>(0.25f, info, nwsnd::OutputMode::kSurround) ==
                 tables::kSurroundPan[1][160]);
    info.front_bypass = true;

    XSFLIB_CHECK(nwsnd::CalcPanRatio<CafePolicy>(0.25f, info, nwsnd::OutputMode::kSurround) ==
                 tables::kStereoPan[1][160]);

    // A surround pan runs from the front at 0 to the rear at 2, on the stereo curves.
    info = {};

    XSFLIB_CHECK(nwsnd::CalcSurroundPanRatio<CafePolicy>(1.0f, info) == tables::kStereoPan[0][128]);
    XSFLIB_CHECK(nwsnd::CalcSurroundPanRatio<CafePolicy>(3.0f, info) == tables::kStereoPan[0][256]);

    // Low-pass values beyond either end are held there.
    XSFLIB_CHECK(nwsnd::CalcLpfFreq<CafePolicy>(2.0f) == 16000 && nwsnd::CalcLpfFreq<CafePolicy>(-1.0f) == 80);

    // The random numbers are a linear congruential generator's top 16 bits.
    nwsnd::Random random;
    const uint32_t kFirst = 0x12345678u * 0x19660Du + 0x3C6EF35Fu;
    const uint32_t kSecond = kFirst * 0x19660Du + 0x3C6EF35Fu;

    XSFLIB_CHECK(random.Next() == kFirst >> 16);
    XSFLIB_CHECK(random.Next() == kSecond >> 16);
}

// The Wii U's envelope, value by value.
void TestCafeEnvelope()
{
    using CurveAdshr = nwsnd::CurveAdshr<CafePolicy>;

    // A new envelope has the fastest attack, and reads 0 dB at once.
    CurveAdshr envelope;

    XSFLIB_CHECK(envelope.GetValue() == 0.0f);

    // An attack multiplies the level by its entry each millisecond until it's within 1/32 of 0.1 dB of 0.
    envelope.SetAttack(100);
    envelope.SetHold(3);
    envelope.Reset(CurveAdshr::kInitialDecibel);
    const float kFirst = CurveAdshr::kInitialDecibel * 10.0f * tables::kAttack[100];

    envelope.Update(1);
    XSFLIB_CHECK(envelope.GetValue() == kFirst / 10.0f);
    envelope.Update(10000);
    XSFLIB_CHECK(envelope.GetStatus() == CurveAdshr::Status::kHold && envelope.GetValue() == 0.0f);

    // The hold lasts (3 + 1)^2 / 4 ms. Then the decay falls by its rate each millisecond, in one fused step.
    envelope.SetDecay(10);
    envelope.SetSustain(64);
    const float kRate = 21.0f * 0.0078125f / 5.0f;

    envelope.Update(3);
    XSFLIB_CHECK(envelope.GetStatus() == CurveAdshr::Status::kHold);
    envelope.Update(3);
    XSFLIB_CHECK(envelope.GetStatus() == CurveAdshr::Status::kDecay &&
                 envelope.GetValue() == std::fma(-kRate, 2.0f, 0.0f) / 10.0f);

    // The decay stops at the sustain level.
    envelope.SetDecay(126);

    envelope.Update(1000);

    XSFLIB_CHECK(envelope.GetStatus() == CurveAdshr::Status::kSustain &&
                 envelope.GetValue() == static_cast<float>(tables::kDecibelSquare[64]) / 10.0f);

    // A release falls the same way, from wherever the level is.
    const float kSustain = static_cast<float>(tables::kDecibelSquare[64]);
    envelope.SetRelease(100);
    envelope.SetStatus(CurveAdshr::Status::kRelease);

    envelope.Update(10);

    XSFLIB_CHECK(envelope.GetValue() == std::fma(-(60.0f / 26.0f / 5.0f), 10.0f, kSustain) / 10.0f);
}

// The Wii U's LFO: its delay, its curves along a cycle, the random curve's draws and the phase.
void TestCafeLfo()
{
    // An LFO without depth gives nothing.
    nwsnd::Random random;
    nwsnd::CurveLfo<CafePolicy> lfo;
    lfo.Reset();
    lfo.Update(100);
    XSFLIB_CHECK(lfo.GetValue(random) == 0.0f);

    // A sine at one cycle a second peaks a quarter of a second in, after its delay.
    lfo.param.depth = 0.5f;
    lfo.param.range = 2;
    lfo.param.speed = 1.0f;
    lfo.param.delay = 100;
    lfo.Reset();
    lfo.Update(60);
    XSFLIB_CHECK(lfo.GetValue(random) == 0.0f);
    lfo.Update(290);
    XSFLIB_CHECK(lfo.GetValue(random) == 1.0f);

    // The other curves at the same place, and the triangle's other lines.
    lfo.param.curve = 1;
    XSFLIB_CHECK(lfo.GetValue(random) == 1.0f);
    lfo.param.curve = 2;
    XSFLIB_CHECK(lfo.GetValue(random) == 0.25f);
    lfo.param.curve = 3;
    XSFLIB_CHECK(lfo.GetValue(random) == 0.0f);
    lfo.param.curve = 1;
    lfo.Update(500);
    XSFLIB_CHECK(lfo.GetValue(random) == -1.0f);
    lfo.param.curve = 3;
    XSFLIB_CHECK(lfo.GetValue(random) == 1.0f);
    lfo.param.curve = 9;
    XSFLIB_CHECK(lfo.GetValue(random) == 1.0f);

    // The random curve draws while the cycle has just wrapped, and holds the number until the next wrap.
    nwsnd::Random expected;
    lfo.param.curve = 4;
    lfo.param.delay = 0;
    lfo.Reset();
    lfo.Update(500);
    XSFLIB_CHECK(lfo.GetValue(random) == 1.0f);
    lfo.Update(500);
    const float drawn = static_cast<float>(expected.Next()) / 65535.0f;
    XSFLIB_CHECK(lfo.GetValue(random) == drawn);
    lfo.Update(100);
    XSFLIB_CHECK(lfo.GetValue(random) == drawn);

    // A phase starts the cycle part of the way through.
    lfo.param.curve = 2;
    lfo.param.phase = 127;
    lfo.Reset();
    lfo.Update(250);
    XSFLIB_CHECK(lfo.GetValue(random) == 0.25f);
}

} // namespace

int main()
{
    TestTables();
    TestArithmetic();
    TestUtil();
    TestBiquadPresets();
    TestEnvelope<CafePolicy>("the Wii U");
    TestEnvelope<NxPolicy>("the Switch");
    TestCafeUtil();
    TestCafeEnvelope();
    TestCafeLfo();

    return xsflib::testing::Summary("nwsnd");
}
