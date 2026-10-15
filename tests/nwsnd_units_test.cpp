// SPDX-License-Identifier: MIT

// Unit tests of the commands' units (ntdlib/commands/units.h) and of NintendoWare's values in them
// (ntdlib/nwsnd/units.h). Every value of every parameter converts to its unit and back unchanged. The expected units
// come from the sequence player's use of each value (SequenceTrack::CommandProc and the policies in console.h).

#include <cstdint>
#include <cstdio>
#include <optional>

#include "xsflib/testing/check.h"

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/units.h"
#include "ntdlib/sound_build.h"

using xsflib::testing::failures;

namespace
{

using ntdlib::commands::Fixed;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Unit;
using ntdlib::nwsnd::FromUnit;
using ntdlib::nwsnd::Grammar;
using ntdlib::nwsnd::ScaleOf;
using ntdlib::nwsnd::ToUnit;

const Grammar kSwitch = ntdlib::nwsnd::GrammarOf<ntdlib::nwsnd::NxPolicy>();

void TestNames()
{
    for (int u = 0; u < ntdlib::commands::kUnitCount; ++u)
    {
        const auto unit = static_cast<Unit>(u);
        XSFLIB_CHECK(ntdlib::commands::UnitNamed(ntdlib::commands::Name(unit)) == unit);
    }

    XSFLIB_CHECK(ntdlib::commands::Name(Unit::kCents) == "cents" && !ntdlib::commands::UnitNamed("semitones"));
    XSFLIB_CHECK(ntdlib::commands::UnitOf(Kind::kVolume) == Unit::kLevel);
    XSFLIB_CHECK(ntdlib::commands::UnitOf(Kind::kSend) == Unit::kSend);
    XSFLIB_CHECK(ntdlib::commands::UnitOf(Kind::kNote) == Unit::kTicks);
    XSFLIB_CHECK(ntdlib::commands::UnitOf(Kind::kPitchBend) == Unit::kBend);
    XSFLIB_CHECK(ntdlib::commands::UnitOf(Kind::kLfoSpeed) == Unit::kHz);
    XSFLIB_CHECK(ntdlib::commands::UnitOf(Kind::kPriority) == Unit::kNumber);
    XSFLIB_CHECK(!ntdlib::commands::UnitOf(Kind::kJump) && !ntdlib::commands::UnitOf(Kind::kData));
}

// Values that the player gives a meaning, in their units.
void TestValues()
{
    const auto unit = [](Kind kind, int64_t value)
    {
        return ToUnit(value, ScaleOf(kind, kSwitch));
    };

    XSFLIB_CHECK(unit(Kind::kVolume, 100) == 100 * kOne && unit(Kind::kSend, 127) == 127 * kOne);
    XSFLIB_CHECK(unit(Kind::kPan, 0) == -64 * kOne && unit(Kind::kPan, 127) == 63 * kOne);
    XSFLIB_CHECK(unit(Kind::kInitialPan, 64) == 0 && unit(Kind::kSurroundPan, 63) == 63 * kOne);
    XSFLIB_CHECK(unit(Kind::kTranspose, -1) == -100 * kOne && unit(Kind::kBendRange, 2) == 200 * kOne);
    XSFLIB_CHECK(unit(Kind::kSweepPitch, 64) == 100 * kOne && unit(Kind::kSweepPitch, 1) == 102400);
    XSFLIB_CHECK(unit(Kind::kPitchBend, 64) == kOne / 2 && unit(Kind::kPitchBend, -128) == -kOne);
    XSFLIB_CHECK(unit(Kind::kLowPass, 64) == 0 && unit(Kind::kLowPass, 0) == -kOne);
    XSFLIB_CHECK(unit(Kind::kLfoDepth, 128) == kOne && unit(Kind::kLfoSpeed, 128) == 50 * kOne);
    XSFLIB_CHECK(unit(Kind::kLfoDelay, 3) == 15 * kOne && unit(Kind::kLfoPeriod, 3) == 30 * kOne);
    XSFLIB_CHECK(unit(Kind::kTempo, 120) == 120 * kOne && unit(Kind::kWait, 48) == 48 * kOne);

    // The Wii's sweep before 2008 counts in 32nds of a semitone.
    ntdlib::SndBuild build;
    build.date = 20070101;
    const Grammar wii = ntdlib::nwsnd::GrammarOf<ntdlib::nwsnd::RevolutionPolicy>(build);
    XSFLIB_CHECK(wii.sweep_steps == 32 && ToUnit(32, ScaleOf(Kind::kSweepPitch, wii)) == 100 * kOne);
    XSFLIB_CHECK(kSwitch.sweep_steps == 64);
}

// Every value that a command can hold comes back: bytes, 16-bit values and random bounds, and a variable-length
// value's range at its edges.
void TestRoundTrips()
{
    int failed = 0;
    for (int k = 0; k < ntdlib::commands::kKindCount; ++k)
    {
        const ntdlib::nwsnd::Scale scale = ScaleOf(static_cast<Kind>(k), kSwitch);
        for (int64_t value = -65536; value <= 65536; ++value)
        {
            failed += FromUnit(ToUnit(value, scale), scale) != value;
        }
        for (const int64_t value : {int64_t{0xFFFFFFF}, int64_t{0xFFFFFFFF}, int64_t{-0x80000000LL}})
        {
            failed += FromUnit(ToUnit(value, scale), scale) != value;
        }
    }
    XSFLIB_CHECK(failed == 0);
}

// A unit between two console values gives the nearest, and a halfway value goes away from zero.
void TestNearest()
{
    const ntdlib::nwsnd::Scale bend = ScaleOf(Kind::kPitchBend, kSwitch);
    XSFLIB_CHECK(FromUnit(255, bend) == 0 && FromUnit(256, bend) == 1 && FromUnit(-256, bend) == -1);
    XSFLIB_CHECK(FromUnit(-255, bend) == 0 && FromUnit(767, bend) == 1 && FromUnit(768, bend) == 2);

    const ntdlib::nwsnd::Scale pan = ScaleOf(Kind::kPan, kSwitch);
    XSFLIB_CHECK(FromUnit(-kOne / 2, pan) == 63 && FromUnit(-kOne / 2 + 1, pan) == 64);
}

} // namespace

int main()
{
    TestNames();
    TestValues();
    TestRoundTrips();
    TestNearest();

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::printf("Unit tests passed\n");

    return 0;
}
