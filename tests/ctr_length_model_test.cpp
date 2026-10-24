// SPDX-License-Identifier: MIT

// Tests the length analysis of a sequence sound (ntdlib/nwsnd/length_model.h) on the length cases
// (ntdlib/testing/length_cases.h), on the 3DS's model: archive mode on the model of the DSP's audio firmware. The
// analysis waits for the loops of the tracks other than the main track. A tie gives its channel the velocity's volume
// without the region's. On the 3DS as on the Wii U, a counted loop that runs past the limit plays to the limit: the
// 3DS's engine can't make a loop endless.

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
#include "ntdlib/testing/sar_files.h"

namespace
{

namespace length_cases = ntdlib::testing::length_cases;
namespace sar_files = ntdlib::testing::sar_files;
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

// An archive with one sequence: a loop of 64 passes, each C4 for 250 ms and a rest, 10 s in all. The passes take
// 640 s, past the limit of 600 s. As in the length cases, program 0 plays a wave of 1 s that ends.
std::vector<uint8_t> CountedLoopArchive()
{
    constexpr ntdlib::sar::Flavor kCtr = ntdlib::sar::Flavor::kCtr;
    length_cases::Code code(length_cases::BigEndianArguments(kCtr));
    code.LoopStart(64).Note(60, 127, 24).Wait(936).LoopEnd().End();

    sar_files::ArchiveSpec spec;
    spec.banks = {{"BANK", 0}};
    spec.wave_archives = {{"WARC", 1}};
    spec.players = {{"PLAYER", 1}};
    const std::vector<uint8_t> wave = sar_files::Wave(kCtr, sar_files::ConstantWave(false));
    spec.files = {{sar_files::Bank(kCtr, sar_files::OneRegionBank()), "", false},
                  {sar_files::WaveArchive(kCtr, {wave}), "", false},
                  {sar_files::Sequence(kCtr, code.Bytes(), {{"Start", 0}}), "", false}};
    sar_files::SoundSpec sound;
    sound.name = "SEQ_COUNTED_LOOP";
    sound.volume = 127;
    sound.allocate_track_flags = 1;
    sound.banks = {0};
    sound.file = 2;
    spec.sounds = {sound};

    return sar_files::Archive(kCtr, spec);
}

void TestCases()
{
    Check(std::abs(LengthConsole::kFrameMs - 4.888706) < 0.000001, "the 3DS's frames of 160 samples last 4.888706 ms");

    const std::vector<uint8_t> bytes = length_cases::Archive(ntdlib::sar::Flavor::kCtr);
    const auto archive = ntdlib::sar::SoundArchive::Load(bytes, ntdlib::ctr::kSarOptions);

    length_cases::CheckCases([&archive](uint32_t index) { return Measure(archive, index); }, true, true);
}

// The counted loop plays to the limit, and fades there.
void TestCountedLoop()
{
    const std::vector<uint8_t> bytes = CountedLoopArchive();
    const auto archive = ntdlib::sar::SoundArchive::Load(bytes, ntdlib::ctr::kSarOptions);

    const std::optional<xsflib::rip::Timing> timing = Measure(archive, 0);

    Check(timing && timing->length_ms >= 599995 && timing->length_ms <= 600000 && timing->fade_ms == 10000,
          "on the 3DS, a counted loop that runs past the limit plays to the limit, " +
              std::to_string(timing ? timing->length_ms : -1) + " ms");
}

} // namespace

int main()
{
    TestCases();
    TestCountedLoop();

    return xsflib::testing::Summary("ctr_length_model");
}
