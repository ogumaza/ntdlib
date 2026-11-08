// SPDX-License-Identifier: MIT

// Tests of the enhanced mixing mode of R&D1's player against its rules (src/rd1/enhanced_mix.h). No console has the
// mode. Each expected value follows from those rules. A voice plays a sample of one constant point. The
// band-limited interpolator gives such a sample back unchanged. The voice's level on a side is its gain times its
// volume over 2^14.

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/rd1/driver.h"
#include "ntdlib/rd1/game_driver.h"
#include "ntdlib/testing/agb_cart.h"
#include "rd1/enhanced_mix.h"

using xsflib::testing::Check;

namespace
{

namespace rd1 = ntdlib::rd1;
using ntdlib::agb::kEnhancedRate;
using ntdlib::agb::kRomBase;
using ntdlib::agb::kSecondCycles;
using ntdlib::agb::Rom;
using ntdlib::testing::agb_files::CartImage;

// The console's period and its samples in a frame, and the FIFOs' lead.
constexpr uint32_t kPeriod = 1253;
constexpr size_t kFrameSamples = 200;
constexpr uint64_t kLead = 480;

// A sample of 64 points of 64. It loops whole.
constexpr uint32_t kSample = kRomBase + 0x100;
constexpr int kPoint = 64;
constexpr uint32_t kSamplePoints = 64;

// The console's samples in each of the mode's.
const double kRatio = double(kSecondCycles) / (double(kPeriod) * double(kEnhancedRate));

Rom SampleRom()
{
    CartImage cart(0x1000);
    for (uint32_t i = 0; i < kSamplePoints; i++)
    {
        cart.Put8(kSample + i, uint8_t(kPoint));
    }

    return cart.ToRom();
}

// The mode's samples up to the console's `console`.
uint64_t Scaled(uint64_t console)
{
    return console * kPeriod * uint64_t(kEnhancedRate) / kSecondCycles;
}

void PutWord(std::array<uint8_t, 32>& record, size_t at, uint32_t value)
{
    for (size_t b = 0; b < 4; b++)
    {
        record[at + b] = uint8_t(value >> (8 * b));
    }
}

// A voice that steps one point a console's sample from point `place`, with `volume` and gains of 64 on FIFO A and 32 on
// FIFO B.
rd1::MixVoice Voice(uint32_t place, int volume, bool filtered = false)
{
    rd1::MixVoice voice;
    voice.record[0] = 3;
    voice.record[1] = uint8_t(volume);
    voice.record[2] = 32;
    voice.record[3] = 64;
    PutWord(voice.record, 4, kSample);
    PutWord(voice.record, 0xC, place << 14);
    PutWord(voice.record, 0x10, 0);
    PutWord(voice.record, 0x14, kSamplePoints << 14);
    PutWord(voice.record, 0x18, 1u << 14);
    voice.voice = 0;
    voice.track = 2;
    voice.filtered = filtered;

    return voice;
}

bool Near(float value, double expected)
{
    return std::fabs(double(value) - expected) <= 0.005 * std::fabs(expected) + 1e-6;
}

// The lead and the frames' lengths follow the console's samples. The first sample plays from the timer's first
// overflow.
void TestLengths()
{
    const Rom rom = SampleRom();
    rd1::EnhancedMix mix(rom, kPeriod, kLead);

    Check(mix.Lead() == Scaled(kLead + 1), "the lead lasts as long as the console's");

    mix.Frame({}, {}, kFrameSamples, true);

    Check(mix.Out()[0].size() == Scaled(kLead + 1 + kFrameSamples) - Scaled(kLead + 1) &&
              mix.Out()[1].size() == mix.Out()[0].size(),
          "a frame lasts as long as the console's samples");
}

// A voice's levels: 64 x 64 x 128 / 2^14 = 32 on FIFO A and 16 on FIFO B. A voice that goes on from the last frame
// moves to its new levels in a straight line. One that doesn't takes them at once. So does a voice after a frame
// without samples.
void TestLevels()
{
    const Rom rom = SampleRom();
    rd1::EnhancedMix mix(rom, kPeriod, kLead);

    mix.Frame({Voice(32, 128)}, {}, kFrameSamples, true);

    bool flat = true;
    for (size_t i = 0; i < mix.Out()[0].size(); i++)
    {
        flat = flat && Near(mix.Out()[0][i], 32.0) && Near(mix.Out()[1][i], 16.0);
    }

    Check(flat, "a voice that wasn't on takes its levels at once");
    Check(Near(mix.Parts()[2][0], 32.0) && Near(mix.Parts()[2][1], 16.0) && mix.Parts()[0][0] == 0.0f,
          "the voice's part goes to its track");

    // The frame ends at point 32 + 200: past the end, 40 into the loop.
    const uint32_t kNext = (32 + kFrameSamples - kSamplePoints) % kSamplePoints;

    mix.Frame({Voice(kNext, 64)}, {}, kFrameSamples, true);

    const std::vector<float>& a = mix.Out()[0];
    const size_t n = a.size();
    const double half = 32.0 - 16.0 * double(n / 2) / double(n);
    Check(Near(a[0], 32.0) && Near(a[n / 2], half) && Near(a[n - 1], 32.0 - 16.0 * double(n - 1) / double(n)),
          "a voice that goes on moves to its levels across the frame");

    mix.Frame({Voice(33, 128)}, {}, kFrameSamples, true);

    Check(Near(mix.Out()[0][0], 32.0), "a voice from another place takes its levels at once");

    const uint32_t kAfter = (33 + 2 * kFrameSamples) % kSamplePoints;

    mix.Frame({Voice((33 + kFrameSamples) % kSamplePoints, 128)}, {}, kFrameSamples, false);

    bool silent = true;
    for (float x : mix.Out()[0])
    {
        silent = silent && x == 0.0f;
    }

    Check(silent && mix.Parts()[2][0] == 0.0f, "a frame without samples is silent");

    mix.Frame({Voice(kAfter, 64)}, {}, kFrameSamples, true);

    Check(Near(mix.Out()[0][0], 16.0), "after a frame without samples, a voice takes its levels at once");
}

// The filter of CC72 is a one-pole filter. At the console's rate, its coefficient k is c / 256, where c is the low 7
// bits of the setting times 2. At the mode's rate, the coefficient is k / (k + kRatio (1 - k)). Its state starts at 0.
// A low-pass's first output is the input times 1 less the coefficient, and a high-pass's the input times the
// coefficient. In high-pass mode, a voice through the filter gains volume x (256 - setting) x the gain << 9 >> 16:
// 128 x 64 x 2 << 9 >> 16 = 128. Its level on FIFO A is then 64 x 256 / 2^14 = 1, and the input 64.
void TestFilter()
{
    const Rom rom = SampleRom();
    const double kConsoleCoefficient = 128.0 / 256.0;
    const double kCoefficient = kConsoleCoefficient / (kConsoleCoefficient + kRatio * (1.0 - kConsoleCoefficient));
    rd1::MixSettings settings;
    settings.filter = 0x40;
    rd1::EnhancedMix low(rom, kPeriod, kLead);

    low.Frame({Voice(32, 128, true)}, settings, kFrameSamples, true);

    Check(Near(low.Out()[0][0], 32.0 * (1.0 - kCoefficient)), "the low-pass takes the voice through its coefficient");
    Check(Near(low.Out()[0][1], 32.0 * (1.0 - kCoefficient * kCoefficient)), "the low-pass's state moves on");

    settings.filter = 0xC0;
    settings.filter_gain = 2;
    rd1::EnhancedMix high(rom, kPeriod, kLead);

    high.Frame({Voice(32, 128, true)}, settings, kFrameSamples, true);

    Check(Near(high.Out()[0][0], 64.0 * kCoefficient), "the high-pass takes the voice and its gain");

    rd1::EnhancedMix past(rom, kPeriod, kLead);

    past.Frame({Voice(32, 128, false)}, settings, kFrameSamples, true);

    Check(Near(past.Out()[0][0], 32.0), "a voice that isn't through the filter keeps its level");
}

} // namespace

int main()
{
    TestLengths();
    TestLevels();
    TestFilter();

    return xsflib::testing::Summary("rd1_enhanced");
}
