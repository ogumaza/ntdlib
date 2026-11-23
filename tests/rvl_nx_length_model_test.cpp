// SPDX-License-Identifier: MIT

// Tests the length analysis of a sequence sound (ntdlib/nwsnd/length_model.h) on Skyward Sword HD's model
// (ntdlib/rvl/nx/length_model.h): AX on the game's layer, mixed by the Switch renderer. rvl_length_cases.h has the
// tests.

#include <cstdint>
#include <memory>
#include <optional>

#include "xsflib/rip/length.h"
#include "xsflib/testing/check.h"

#include "ntdlib/nwsnd/length_model.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/nx/length_model.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/testing/rsar_files.h"
#include "rvl_length_cases.h"

namespace
{

using ntdlib::testing::rsar_files::Bytes;
using ntdlib::testing::rsar_files::MakeAudibleArchive;
using xsflib::testing::Check;
using LengthConsole = ntdlib::rvl::nx::LengthConsole;

// The timing of the sequence of `commands` with the tracks in the mask `tracks`, on the model of the game's build.
std::optional<xsflib::rip::Timing> Measure(const Bytes& commands, uint32_t tracks)
{
    const ntdlib::rsar::SoundArchive archive =
        ntdlib::rsar::SoundArchive::Load(MakeAudibleArchive(commands, tracks), ntdlib::rvl::snd::kRsarOptions);
    const auto make_model = [&archive]
    {
        auto model = std::make_unique<LengthConsole::Model>();
        model->Start(archive);
        return model;
    };
    const auto measure = [&make_model]
    {
        return ntdlib::nwsnd::MeasureSequence<LengthConsole>(make_model, 0);
    };

    return xsflib::rip::TryMeasure(measure);
}

} // namespace

int main()
{
    Check(LengthConsole::kFrameMs == 5.0, "the renderer's frames last 5 ms");
    rvl_length_cases::RunAll(Measure);

    return xsflib::testing::Summary("rvl_nx_length_model");
}
