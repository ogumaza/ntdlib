// SPDX-License-Identifier: MIT

// Tests of the enhanced mixing mode of AlphaDream's player against its rules (src/alphadream/enhanced_mix.h). No
// console has the mode. Each expected value follows from those rules. A channel plays a sample of one constant point.
// The band-limited interpolator gives such a sample back unchanged. The channel's level on a side is its volume times
// the envelope's level over 2^16, times the point's offset from 128.

#include <cmath>
#include <cstdint>
#include <vector>

#include "xsflib/testing/check.h"

#include "alphadream/enhanced_mix.h"
#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/alphadream/driver.h"
#include "ntdlib/alphadream/game_driver.h"
#include "ntdlib/testing/agb_cart.h"

using xsflib::testing::Check;

namespace
{

namespace alphadream = ntdlib::alphadream;
using ntdlib::agb::kEnhancedRate;
using ntdlib::agb::kRomBase;
using ntdlib::agb::kSecondCycles;
using ntdlib::agb::Rom;
using ntdlib::testing::agb_files::CartImage;

// The console's period, and the FIFOs' lead.
constexpr uint32_t kPeriod = 1064;
constexpr uint64_t kLead = 288;

// A sample of 64 points of 192: 64 above silence.
constexpr uint32_t kSample = kRomBase + 0x100;
constexpr int kPoint = 192;
constexpr uint32_t kSamplePoints = 64;

// A step of one point a console's sample.
constexpr uint32_t kStep = 1024;

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

// Channel 2 from point `place`, with the envelope's `level` and volumes of 64 on FIFO A and 32 on FIFO B. It loops
// whole unless `once`.
alphadream::MixChannel Channel(uint32_t place, int level, bool once = false)
{
    alphadream::MixChannel channel;
    channel.channel = 2;
    channel.sample.data = kSample;
    channel.sample.loops = !once;
    channel.sample.loop_start = 0;
    channel.sample.end = kSamplePoints;
    channel.place = place << 10;
    const uint32_t next = place + uint32_t(alphadream::kFrameSamples);
    channel.next = (once ? kSamplePoints : next % kSamplePoints) << 10;
    channel.step = kStep;
    channel.level = level;
    channel.volumes = {64, 32};

    return channel;
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
    alphadream::EnhancedMix mix(rom, kPeriod, kLead);

    mix.Frame({}, true);

    Check(mix.Lead() == Scaled(kLead + 1), "the lead lasts as long as the console's");
    Check(mix.Out()[0].size() == Scaled(kLead + 1 + alphadream::kFrameSamples) - Scaled(kLead + 1) &&
              mix.Out()[1].size() == mix.Out()[0].size(),
          "a frame lasts as long as the console's samples");
}

// A channel's levels: 64 x 128 / 2^16 x 64 = 8 on FIFO A and 4 on FIFO B. A channel that goes on from the last frame
// moves to its new levels in a straight line. One that doesn't takes them at once. A frame without samples leaves each
// channel as a frame with samples does.
void TestLevels()
{
    const Rom rom = SampleRom();
    alphadream::EnhancedMix mix(rom, kPeriod, kLead);
    const alphadream::MixChannel first = Channel(32, 128);

    mix.Frame({first}, true);

    bool flat = true;
    for (size_t i = 0; i < mix.Out()[0].size(); i++)
    {
        flat = flat && Near(mix.Out()[0][i], 8.0) && Near(mix.Out()[1][i], 4.0);
    }

    Check(flat, "a channel that wasn't on takes its levels at once");
    Check(Near(mix.Parts()[2][0], 8.0) && Near(mix.Parts()[2][1], 4.0) && mix.Parts()[0][0] == 0.0f,
          "the channel's part goes to its channel");

    mix.Frame({Channel(first.next >> 10, 64)}, true);

    const std::vector<float>& a = mix.Out()[0];
    const size_t n = a.size();
    Check(Near(a[0], 8.0) && Near(a[n / 2], 8.0 - 4.0 * double(n / 2) / double(n)) &&
              Near(a[n - 1], 8.0 - 4.0 * double(n - 1) / double(n)),
          "a channel that goes on moves to its levels across the frame");

    mix.Frame({Channel(33, 128)}, true);
    Check(Near(mix.Out()[0][0], 8.0), "a channel from another place takes its levels at once");

    const alphadream::MixChannel unheard = Channel(Channel(33, 128).next >> 10, 128);

    mix.Frame({unheard}, false);

    bool silent = true;
    for (float x : mix.Out()[0])
    {
        silent = silent && x == 0.0f;
    }

    Check(silent && mix.Parts()[2][0] == 0.0f, "a frame without samples is silent");

    mix.Frame({Channel(unheard.next >> 10, 64)}, true);
    Check(Near(mix.Out()[0][0], 8.0), "after a frame without samples, a channel goes on from its levels");
}

// A sample without a loop is silent past its end: from point 32, the end comes 32 of the console's samples into the
// frame. A silenced channel adds nothing, and its part is 0.
void TestEndAndSilence()
{
    const Rom rom = SampleRom();
    alphadream::EnhancedMix once(rom, kPeriod, kLead);

    once.Frame({Channel(32, 128, true)}, true);

    const std::vector<float>& a = once.Out()[0];
    Check(Near(a[0], 8.0) && a.back() == 0.0f, "a sample without a loop ends at its end");

    alphadream::EnhancedMix silenced(rom, kPeriod, kLead);
    alphadream::MixChannel channel = Channel(32, 128);
    channel.silenced = true;

    silenced.Frame({channel}, true);

    bool silent = true;
    for (float x : silenced.Out()[0])
    {
        silent = silent && x == 0.0f;
    }

    Check(silent && silenced.Parts()[2][0] == 0.0f, "a silenced channel adds nothing");
}

} // namespace

int main()
{
    TestLengths();
    TestLevels();
    TestEndAndSilence();

    return xsflib::testing::Summary("alphadream_enhanced");
}
