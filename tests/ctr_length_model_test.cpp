// SPDX-License-Identifier: MIT

// Tests the length analysis of a sequence sound (ntdlib/nwsnd/length_model.h) on the length cases
// (ntdlib/testing/length_cases.h), on the 3DS's model: archive mode on the model of the DSP's audio firmware. The
// analysis waits for the loops of the tracks other than the main track. A tie gives its channel the velocity's volume
// without the region's. The cases take the lengths that the Wii U and the Switch give them: a pass that only drops the
// region's volume isn't a loop. Counted loops that run past the limit follow. The analysis then takes each track's
// outermost counted loop as endless.

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/rip/length.h"
#include "xsflib/testing/check.h"

#include "ntdlib/ctr/snd/engine.h"
#include "ntdlib/ctr/snd/length_model.h"
#include "ntdlib/ctr/snd/sar_options.h"
#include "ntdlib/nwsnd/length_model.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/testing/length_cases.h"

namespace
{

namespace length_cases = ntdlib::testing::length_cases;
using ntdlib::ctr::snd::ArchiveModel;
using ntdlib::ctr::snd::LengthConsole;
using xsflib::testing::Check;

// The timing of sound `sound` of `archive` on the 3DS's model, or nothing.
std::optional<xsflib::rip::Timing> Measure(const ntdlib::sar::SoundArchive& archive, uint32_t sound)
{
    const auto make_model = [&archive]
    {
        auto model = std::make_unique<ArchiveModel>();
        Check(model->Start(archive), "the model starts");

        return model;
    };
    const auto measure = [&make_model, sound]
    {
        return ntdlib::nwsnd::MeasureSequence<LengthConsole>(make_model, sound);
    };

    return xsflib::rip::TryMeasure(measure);
}

void TestCases()
{
    Check(std::abs(LengthConsole::kFrameMs - 4.888706) < 0.000001, "the 3DS's frames of 160 samples last 4.888706 ms");

    const std::vector<uint8_t> bytes = length_cases::Archive(ntdlib::sar::Flavor::kCtr);
    const auto archive = ntdlib::sar::SoundArchive::Load(bytes, ntdlib::ctr::kSarOptions);

    length_cases::CheckCases([&archive](uint32_t index) { return Measure(archive, index); }, true);
}

// The counted loops that run past the limit (length_cases::CheckLongLoops), on the 3DS's model.
void TestLongLoops()
{
    const auto measure_first = [](const std::vector<uint8_t>& bytes)
    {
        return Measure(ntdlib::sar::SoundArchive::Load(bytes, ntdlib::ctr::kSarOptions), 0);
    };
    length_cases::CheckLongLoops(ntdlib::sar::Flavor::kCtr, measure_first, "the 3DS");
}

} // namespace

int main()
{
    TestCases();
    TestLongLoops();

    return xsflib::testing::Summary("ctr_length_model");
}
