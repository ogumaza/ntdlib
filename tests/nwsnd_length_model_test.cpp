// SPDX-License-Identifier: MIT

// Tests the length analysis of a sequence sound (ntdlib/nwsnd/length_model.h) on the length cases
// (ntdlib/testing/length_cases.h), on the Switch's model and the Wii U's. The analysis waits for the loops of the
// tracks other than the main track. A counted loop that runs past the limit follows. On the Switch, the analysis then
// takes the loop as endless. The Wii U's engine can't make a loop endless, and the sound plays to the limit.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/rip/length.h"
#include "xsflib/testing/check.h"

#include "ntdlib/cafe/snd/length_model.h"
#include "ntdlib/nwsnd/length_model.h"
#include "ntdlib/nx/atk/length_model.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/testing/length_cases.h"
#include "ntdlib/testing/sar_files.h"

namespace
{

namespace length_cases = ntdlib::testing::length_cases;
namespace sar_files = ntdlib::testing::sar_files;
using xsflib::testing::Check;

ntdlib::sar::SoundArchive LoadArchive(const std::vector<uint8_t>& bytes, bool big_endian)
{
    ntdlib::sar::Options options;
    options.flavor = big_endian ? ntdlib::sar::Flavor::kCafe : ntdlib::sar::Flavor::kNx;
    options.rules = ntdlib::sar::Rules::kStopAtFirstProblem;

    return ntdlib::sar::SoundArchive::Load(bytes, options);
}

// The timing of sound `sound` of `archive` on the console's model, or nothing.
template <class Console>
std::optional<xsflib::rip::Timing> Measure(const ntdlib::sar::SoundArchive& archive, uint32_t sound)
{
    const auto make_model = [&archive]
    {
        return std::make_unique<typename Console::Model>(archive);
    };
    const auto measure = [&make_model, sound]
    {
        return ntdlib::nwsnd::MeasureSequence<Console>(make_model, sound);
    };

    return xsflib::rip::TryMeasure(measure);
}

// The length of the case `name` on the console's model, or -1.
template <class Console>
long long LengthOf(const ntdlib::sar::SoundArchive& archive, const std::string& name)
{
    for (std::size_t i = 0; i < length_cases::Cases().size(); ++i)
    {
        if (name == length_cases::Cases()[i].name)
        {
            const std::optional<xsflib::rip::Timing> timing = Measure<Console>(archive, static_cast<uint32_t>(i));
            return timing ? timing->length_ms : -1;
        }
    }

    return -1;
}

// An archive with one sequence: a loop of 64 passes, each C4 for 250 ms and a rest, 10 s in all. The passes take
// 640 s, past the limit of 600 s. Program 0 plays a wave of 1 s that ends, as in the length cases.
std::vector<uint8_t> CountedLoopArchive(bool big_endian)
{
    length_cases::Code code(big_endian);
    code.LoopStart(64).Note(60, 127, 24).Wait(936).LoopEnd().End();

    sar_files::ArchiveSpec spec;
    spec.banks = {{"BANK", 0}};
    spec.wave_archives = {{"WARC", 1}};
    spec.players = {{"PLAYER", 1}};
    const std::vector<uint8_t> wave = sar_files::Wave(big_endian, sar_files::ConstantWave(big_endian));
    spec.files = {{sar_files::Bank(big_endian, sar_files::OneRegionBank()), "", false},
                  {sar_files::WaveArchive(big_endian, {wave}), "", false},
                  {sar_files::Sequence(big_endian, code.Bytes(), {{"Start", 0}}), "", false}};
    sar_files::SoundSpec sound;
    sound.name = "SEQ_COUNTED_LOOP";
    sound.volume = 127;
    sound.allocate_track_flags = 1;
    sound.banks = {0};
    sound.file = 2;
    spec.sounds = {sound};

    return sar_files::Archive(big_endian, spec);
}

void TestSwitch()
{
    using ntdlib::nx::atk::LengthConsole;
    Check(LengthConsole::kFrameMs == 5.0, "the Switch's frames last 5 ms");
    const ntdlib::sar::SoundArchive archive = LoadArchive(length_cases::Archive(false), false);
    length_cases::CheckCases([&archive](uint32_t index) { return Measure<LengthConsole>(archive, index); }, true);

    // SEQ_END ends at the same frame as SEQ_REST_LOOP and SEQ_LONG_REST. A different rule ends each one. A model whose
    // output runs a frame behind its engine can end SEQ_END sooner.
    const long long end = LengthOf<LengthConsole>(archive, "SEQ_END");
    Check(end > 0 && end == LengthOf<LengthConsole>(archive, "SEQ_REST_LOOP"),
          "SEQ_END ends at the same frame as SEQ_REST_LOOP");

    // The counted loop reaches the limit. The analysis then takes it as endless: two passes of 10 s, then the fade.
    const ntdlib::sar::SoundArchive counted = LoadArchive(CountedLoopArchive(false), false);
    const std::optional<xsflib::rip::Timing> timing = Measure<LengthConsole>(counted, 0);
    Check(timing && timing->length_ms >= 19995 && timing->length_ms <= 20015 && timing->fade_ms == 10000,
          "on the Switch, a counted loop that runs past the limit loops after two passes, " +
              std::to_string(timing ? timing->length_ms : -1) + " ms");
}

void TestWiiU()
{
    using ntdlib::cafe::snd::LengthConsole;
    Check(LengthConsole::kFrameMs == 3.0, "the Wii U's frames last 3 ms");
    const ntdlib::sar::SoundArchive archive = LoadArchive(length_cases::Archive(true), true);
    length_cases::CheckCases([&archive](uint32_t index) { return Measure<LengthConsole>(archive, index); }, true);

    // The counted loop plays to the limit, and fades there.
    const ntdlib::sar::SoundArchive counted = LoadArchive(CountedLoopArchive(true), true);
    const std::optional<xsflib::rip::Timing> timing = Measure<LengthConsole>(counted, 0);
    Check(timing && timing->length_ms == 600000 && timing->fade_ms == 10000,
          "on the Wii U, a counted loop that runs past the limit plays to the limit, " +
              std::to_string(timing ? timing->length_ms : -1) + " ms");
}

} // namespace

int main()
{
    TestSwitch();
    TestWiiU();

    return xsflib::testing::Summary("nwsnd_length_model");
}
