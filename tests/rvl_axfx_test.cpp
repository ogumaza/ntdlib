// SPDX-License-Identifier: MIT

// Unit tests for the Wii's effects for AX's aux buses (rvl/axfx): the delay's echoes, as AXFXDelayCallback's integer
// arithmetic gives them, the frame that each effect leaves alone after a Settings, the effects' range checks, and the
// chorus's lines. The expected values come from the SDK functions' definitions in the headers' comments.

#include <cstdint>
#include <limits>

#include "xsflib/testing/check.h"

#include "ntdlib/rvl/axfx/axfx.h"
#include "ntdlib/rvl/axfx/chorus.h"
#include "ntdlib/rvl/axfx/delay.h"
#include "ntdlib/rvl/axfx/reverb_hi.h"
#include "ntdlib/rvl/axfx/reverb_std.h"

namespace
{

using ntdlib::rvl::axfx::AuxFrame;
using ntdlib::rvl::axfx::Build;
using ntdlib::rvl::axfx::ChorusExp;
using ntdlib::rvl::axfx::Delay;
using ntdlib::rvl::axfx::kChannelCount;
using ntdlib::rvl::axfx::kSamplesPerFrame;
using ntdlib::rvl::axfx::ReverbHiExp;
using ntdlib::rvl::axfx::ReverbStdExp;

// A frame whose samples differ in each channel and place.
AuxFrame Ramp()
{
    AuxFrame frame;
    for (int channel = 0; channel < kChannelCount; channel++)
    {
        for (int i = 0; i < kSamplesPerFrame; i++)
        {
            frame.channels[channel][i] = (channel + 1) * 1000 + i;
        }
    }

    return frame;
}

bool Silent(const AuxFrame& frame)
{
    for (const auto& channel : frame.channels)
    {
        for (const int32_t sample : channel)
        {
            if (sample != 0)
            {
                return false;
            }
        }
    }

    return true;
}

// A delay of 1 ms holds 32 samples. A feedback of 50 % and an output of 100 % give factors of 64 and 128 in 128ths.
// An impulse of 1,000 leaves the line after 32 samples, then comes round at half its level every 32 samples. Each
// echo's product shifts right by 7.
void TestDelayEchoes()
{
    Delay delay;
    const Delay::Params params = {{1, 1, 1}, {50, 50, 50}, {100, 100, 100}};
    XSFLIB_CHECK(Delay::GetMemSize(params) == 3 * 32 * 4);
    XSFLIB_CHECK(delay.Settings(params));

    // The frame after Settings passes through.
    AuxFrame skipped = Ramp();
    delay.Process(skipped);
    XSFLIB_CHECK(skipped.channels == Ramp().channels);

    AuxFrame first;
    first.channels[1][0] = 1000;
    delay.Process(first);
    AuxFrame second;
    delay.Process(second);
    XSFLIB_CHECK(first.channels[1][32] == 1000 && first.channels[1][64] == 500 && second.channels[1][0] == 250 &&
                 second.channels[1][32] == 125 && second.channels[1][64] == 62 && second.channels[1][1] == 0);
    first.channels[1][32] = 0;
    first.channels[1][64] = 0;
    second.channels[1][0] = 0;
    second.channels[1][32] = 0;
    second.channels[1][64] = 0;
    XSFLIB_CHECK(Silent(first) && Silent(second));

    // A negative sample keeps its sign through the shift.
    Delay negative;
    XSFLIB_CHECK(negative.Init({{1, 1, 1}, {0, 0, 0}, {50, 50, 50}}));
    AuxFrame ignored;
    negative.Process(ignored);
    AuxFrame impulse;
    impulse.channels[0][0] = -3;
    negative.Process(impulse);
    XSFLIB_CHECK(impulse.channels[0][32] == -2);
}

// The delay refuses a delay of 0, a feedback of 100 % and an output above 100 %. It then leaves frames as they are.
void TestDelayRefusals()
{
    Delay delay;
    XSFLIB_CHECK(!delay.Init({{0, 1, 1}, {0, 0, 0}, {100, 100, 100}}));
    XSFLIB_CHECK(!delay.Init({{1, 1, 1}, {0, 100, 0}, {100, 100, 100}}));
    XSFLIB_CHECK(!delay.Init({{1, 1, 1}, {0, 0, 0}, {100, 100, 101}}));
    AuxFrame frame = Ramp();
    delay.Process(frame);
    XSFLIB_CHECK(frame.channels == Ramp().channels);
}

// The chorus's three lines hold 100 ms of float samples each. Its Init refuses a delay below 0.1 ms and a feedback of
// 1. Silence stays silent.
void TestChorus()
{
    XSFLIB_CHECK(ChorusExp::GetMemSize() == 3 * 3200 * 4);
    const ChorusExp::Params params = {.delay_time = 10.0f, .depth = 0.5f, .rate = 1.0f, .out_gain = 1.0f};
    ChorusExp chorus;
    ChorusExp::Params short_delay = params;
    short_delay.delay_time = 0.05f;
    ChorusExp::Params full_feedback = params;
    full_feedback.feedback = 1.0f;
    XSFLIB_CHECK(!chorus.Init(short_delay) && !chorus.Init(full_feedback));
    XSFLIB_CHECK(chorus.Settings(params));

    AuxFrame skipped = Ramp();
    chorus.Process(skipped);
    XSFLIB_CHECK(skipped.channels == Ramp().channels);
    for (int frame = 0; frame < 10; frame++)
    {
        AuxFrame silence;
        chorus.Process(silence);
        XSFLIB_CHECK(Silent(silence));
    }
}

// The reverbs take each gain from 0 to 1. A NaN gain passes, as it passes the games' comparisons. A pre-delay longer
// than the longest that Init allocates fails. After Settings, the next frame passes through, and silence stays silent.
void TestReverbs()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const ReverbHiExp::Params hi = {.pre_delay_time_max = 0.02f,
                                    .pre_delay_time = 0.01f,
                                    .fused_time = 1.0f,
                                    .coloration = 0.5f,
                                    .damping = 0.5f,
                                    .early_gain = 0.5f,
                                    .fused_gain = 1.0f,
                                    .out_gain = 1.0f};
    for (const Build build : {Build::kOctober2006, Build::kDecember2006, Build::kSeptember2008})
    {
        ReverbHiExp reverb(build);
        ReverbHiExp::Params loud = hi;
        loud.out_gain = 1.5f;
        ReverbHiExp::Params late = hi;
        late.pre_delay_time = 0.03f;
        ReverbHiExp::Params undefined = hi;
        undefined.coloration = nan;
        XSFLIB_CHECK(!reverb.Init(loud) && !reverb.Init(late) && reverb.Init(undefined));
        XSFLIB_CHECK(reverb.Settings(hi));

        AuxFrame skipped = Ramp();
        reverb.Process(skipped);
        XSFLIB_CHECK(skipped.channels == Ramp().channels);
        AuxFrame silence;
        reverb.Process(silence);
        XSFLIB_CHECK(Silent(silence));
    }

    const ReverbStdExp::Params standard = {.pre_delay_time_max = 0.02f,
                                           .pre_delay_time = 0.01f,
                                           .fused_time = 1.0f,
                                           .coloration = 0.5f,
                                           .damping = 0.5f,
                                           .early_gain = 0.5f,
                                           .fused_gain = 1.0f,
                                           .out_gain = 1.0f};
    ReverbStdExp reverb;
    ReverbStdExp::Params negative = standard;
    negative.damping = -0.25f;
    XSFLIB_CHECK(!reverb.Init(negative));
    XSFLIB_CHECK(reverb.Settings(standard));

    AuxFrame skipped = Ramp();
    reverb.Process(skipped);
    XSFLIB_CHECK(skipped.channels == Ramp().channels);
    AuxFrame silence;
    reverb.Process(silence);
    XSFLIB_CHECK(Silent(silence));
}

} // namespace

int main()
{
    TestDelayEchoes();
    TestDelayRefusals();
    TestChorus();
    TestReverbs();

    return xsflib::testing::Summary("Wii AXFX");
}
