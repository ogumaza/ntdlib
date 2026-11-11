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

// An archive with one sequence of `code`'s commands, on the tracks of `tracks`. As in the length cases, program 0 plays
// a wave of 1 s that ends.
std::vector<uint8_t> SequenceArchive(const length_cases::Code& code, uint16_t tracks = 1)
{
    constexpr ntdlib::sar::Flavor kCtr = ntdlib::sar::Flavor::kCtr;
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
    sound.allocate_track_flags = tracks;
    sound.banks = {0};
    sound.file = 2;
    spec.sounds = {sound};

    return sar_files::Archive(kCtr, spec);
}

// A loop of 64 passes, each C4 for 250 ms and a rest, 10 s in all. The passes take 640 s, past the limit of 600 s.
std::vector<uint8_t> CountedLoopArchive()
{
    length_cases::Code code(length_cases::BigEndianArguments(ntdlib::sar::Flavor::kCtr));
    code.LoopStart(64).Note(60, 127, 24).Wait(936).LoopEnd().End();

    return SequenceArchive(code);
}

// A loop of 127 passes of 10.5 s: C4 for 0.5 s, then D4 for 5 s twice in an inner loop. The passes take 1,333.5 s. The
// limit comes 1.5 s into the 58th pass, inside the inner loop.
std::vector<uint8_t> NestedLoopsArchive()
{
    length_cases::Code code(length_cases::BigEndianArguments(ntdlib::sar::Flavor::kCtr));
    code.LoopStart(127).Note(60, 127, 48);
    code.LoopStart(2).Note(62, 127, 480).LoopEnd();
    code.LoopEnd().End();

    return SequenceArchive(code);
}

// Track 0's part of 10.5 s for ever: C4 for 0.5 s, then D4 for 5 s twice in a counted loop.
void EndlessPart(length_cases::Code& code)
{
    code.LoopStart(0).Note(60, 127, 48);
    code.LoopStart(2).Note(62, 127, 480).LoopEnd();
    code.LoopEnd().End();
}

// Track 0 plays its part for ever, and track 1 the loop of 64 passes of 10 s. The limit comes 1.5 s into track 0's
// 58th pass, inside its counted loop.
std::vector<uint8_t> CountedInEndlessArchive()
{
    const bool big_endian = length_cases::BigEndianArguments(ntdlib::sar::Flavor::kCtr);
    length_cases::Code track_0(big_endian);
    track_0.OpenTrack(1, 0);
    EndlessPart(track_0);
    const auto track_1 = static_cast<uint32_t>(track_0.Bytes().size());

    length_cases::Code code(big_endian);
    code.OpenTrack(1, track_1);
    EndlessPart(code);
    code.LoopStart(64).Note(64, 127, 24).Wait(936).LoopEnd().End();

    return SequenceArchive(code, 3);
}

void TestCases()
{
    Check(std::abs(LengthConsole::kFrameMs - 4.888706) < 0.000001, "the 3DS's frames of 160 samples last 4.888706 ms");

    const std::vector<uint8_t> bytes = length_cases::Archive(ntdlib::sar::Flavor::kCtr);
    const auto archive = ntdlib::sar::SoundArchive::Load(bytes, ntdlib::ctr::kSarOptions);

    length_cases::CheckCases([&archive](uint32_t index) { return Measure(archive, index); }, true);
}

// The loops that run past the limit loop for ever, and the fade starts after two passes. Of the nested loops, only the
// outer one does: its passes take 10.5 s. Were the inner loop endless too, the track would repeat D4 for ever, and the
// fade would start after 10.5 s. A track whose outermost loop is endless keeps its counted loop: the fade starts once
// track 0 has looped twice, at 21 s. Were track 0's counted loop endless too, track 0 would repeat D4 for ever, and the
// fade would start once track 1 had looped twice, at 20 s.
void TestLongLoops()
{
    const std::vector<uint8_t> counted_bytes = CountedLoopArchive();
    const std::vector<uint8_t> nested_bytes = NestedLoopsArchive();
    const std::vector<uint8_t> in_endless_bytes = CountedInEndlessArchive();
    const auto counted = ntdlib::sar::SoundArchive::Load(counted_bytes, ntdlib::ctr::kSarOptions);
    const auto nested = ntdlib::sar::SoundArchive::Load(nested_bytes, ntdlib::ctr::kSarOptions);
    const auto in_endless = ntdlib::sar::SoundArchive::Load(in_endless_bytes, ntdlib::ctr::kSarOptions);

    const std::optional<xsflib::rip::Timing> counted_timing = Measure(counted, 0);
    const std::optional<xsflib::rip::Timing> nested_timing = Measure(nested, 0);
    const std::optional<xsflib::rip::Timing> in_endless_timing = Measure(in_endless, 0);

    Check(counted_timing && counted_timing->length_ms >= 19995 && counted_timing->length_ms <= 20015 &&
              counted_timing->fade_ms == 10000,
          "on the 3DS, a counted loop that runs past the limit loops after two passes, " +
              std::to_string(counted_timing ? counted_timing->length_ms : -1) + " ms");
    Check(nested_timing && nested_timing->length_ms >= 20995 && nested_timing->length_ms <= 21015 &&
              nested_timing->fade_ms == 10000,
          "on the 3DS, only the outer of two counted loops loops for ever, " +
              std::to_string(nested_timing ? nested_timing->length_ms : -1) + " ms");
    Check(in_endless_timing && in_endless_timing->length_ms >= 20995 && in_endless_timing->length_ms <= 21015 &&
              in_endless_timing->fade_ms == 10000,
          "on the 3DS, a counted loop inside an endless loop still ends, " +
              std::to_string(in_endless_timing ? in_endless_timing->length_ms : -1) + " ms");
}

} // namespace

int main()
{
    TestCases();
    TestLongLoops();

    return xsflib::testing::Summary("ctr_length_model");
}
