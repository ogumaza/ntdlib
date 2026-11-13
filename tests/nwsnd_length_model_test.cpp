// SPDX-License-Identifier: MIT

// Tests the length analysis of a sequence sound (ntdlib/nwsnd/length_model.h) on the length cases
// (ntdlib/testing/length_cases.h), on the Switch's model and the Wii U's. The analysis waits for the loops of the
// tracks other than the main track. Counted loops that run past the limit follow. The analysis then takes each track's
// outermost counted loop as endless. Then a step of the game's code still to come (a console's Pending) holds the
// sequence back. Last, a console's Sounded replaces the test of the output.

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

// An archive with one sequence of `code`'s commands, on the tracks of `tracks`. Program 0 plays a wave of 1 s that
// ends, as in the length cases.
std::vector<uint8_t> SequenceArchive(bool big_endian, const length_cases::Code& code, uint16_t tracks = 1)
{
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
    sound.allocate_track_flags = tracks;
    sound.banks = {0};
    sound.file = 2;
    spec.sounds = {sound};

    return sar_files::Archive(big_endian, spec);
}

// A loop of 64 passes, each C4 for 250 ms and a rest, 10 s in all. The passes take 640 s, past the limit of 600 s.
std::vector<uint8_t> CountedLoopArchive(bool big_endian)
{
    length_cases::Code code(big_endian);
    code.LoopStart(64).Note(60, 127, 24).Wait(936).LoopEnd().End();

    return SequenceArchive(big_endian, code);
}

// A loop of 127 passes of 10.5 s: C4 for 0.5 s, then D4 for 5 s twice in an inner loop. The passes take 1,333.5 s. The
// limit comes 1.5 s into the 58th pass, inside the inner loop.
std::vector<uint8_t> NestedLoopsArchive(bool big_endian)
{
    length_cases::Code code(big_endian);
    code.LoopStart(127).Note(60, 127, 48);
    code.LoopStart(2).Note(62, 127, 480).LoopEnd();
    code.LoopEnd().End();

    return SequenceArchive(big_endian, code);
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
std::vector<uint8_t> CountedInEndlessArchive(bool big_endian)
{
    length_cases::Code track_0(big_endian);
    track_0.OpenTrack(1, 0);
    EndlessPart(track_0);
    const auto track_1 = static_cast<uint32_t>(track_0.Bytes().size());

    length_cases::Code code(big_endian);
    code.OpenTrack(1, track_1);
    EndlessPart(code);
    code.LoopStart(64).Note(64, 127, 24).Wait(936).LoopEnd().End();

    return SequenceArchive(big_endian, code, 3);
}

// The loops that run past the limit loop for ever, and the fade starts after two passes. Of the nested loops, only the
// outer one does: its passes take 10.5 s. Were the inner loop endless too, the track would repeat D4 for ever, and the
// fade would start after 10.5 s. A track whose outermost loop is endless keeps its counted loop: the fade starts once
// track 0 has looped twice, at 21 s. Were track 0's counted loop endless too, track 0 would repeat D4 for ever, and the
// fade would start once track 1 had looped twice, at 20 s.
template <class Console>
void CheckLongLoops(bool big_endian, const std::string& console)
{
    const ntdlib::sar::SoundArchive counted = LoadArchive(CountedLoopArchive(big_endian), big_endian);
    const ntdlib::sar::SoundArchive nested = LoadArchive(NestedLoopsArchive(big_endian), big_endian);
    const ntdlib::sar::SoundArchive in_endless = LoadArchive(CountedInEndlessArchive(big_endian), big_endian);

    const std::optional<xsflib::rip::Timing> counted_timing = Measure<Console>(counted, 0);
    const std::optional<xsflib::rip::Timing> nested_timing = Measure<Console>(nested, 0);
    const std::optional<xsflib::rip::Timing> in_endless_timing = Measure<Console>(in_endless, 0);

    Check(counted_timing && counted_timing->length_ms >= 19995 && counted_timing->length_ms <= 20015 &&
              counted_timing->fade_ms == 10000,
          "on " + console + ", a counted loop that runs past the limit loops after two passes, " +
              std::to_string(counted_timing ? counted_timing->length_ms : -1) + " ms");
    Check(nested_timing && nested_timing->length_ms >= 20995 && nested_timing->length_ms <= 21015 &&
              nested_timing->fade_ms == 10000,
          "on " + console + ", only the outer of two counted loops loops for ever, " +
              std::to_string(nested_timing ? nested_timing->length_ms : -1) + " ms");
    Check(in_endless_timing && in_endless_timing->length_ms >= 20995 && in_endless_timing->length_ms <= 21015 &&
              in_endless_timing->fade_ms == 10000,
          "on " + console + ", a counted loop inside an endless loop still ends, " +
              std::to_string(in_endless_timing ? in_endless_timing->length_ms : -1) + " ms");
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

    CheckLongLoops<LengthConsole>(false, "the Switch");
}

void TestWiiU()
{
    using ntdlib::cafe::snd::LengthConsole;
    Check(LengthConsole::kFrameMs == 3.0, "the Wii U's frames last 3 ms");
    const ntdlib::sar::SoundArchive archive = LoadArchive(length_cases::Archive(true), true);
    length_cases::CheckCases([&archive](uint32_t index) { return Measure<LengthConsole>(archive, index); }, true);

    CheckLongLoops<LengthConsole>(true, "the Wii U");
}

// Whether PendingConsole has a step of the game's code to come.
bool g_pending = false;

// The Switch's console, with a step of the game's code to come while g_pending is true.
struct PendingConsole : ntdlib::nx::atk::LengthConsole
{
    static bool Pending(const Model&)
    {
        return g_pending;
    }
};

// SEQ_HELD_LOOP's held note settles the sequence at once. A step to come keeps it from being settled or stuck, and
// keeps the model busy. Without one, the model follows the player and the engine.
void TestPending()
{
    const ntdlib::sar::SoundArchive archive = LoadArchive(length_cases::Archive(false), false);
    uint32_t index = 0;
    while (index < length_cases::Cases().size() && std::string(length_cases::Cases()[index].name) != "SEQ_HELD_LOOP")
    {
        ++index;
    }
    ntdlib::nx::atk::ArchiveModel model(archive);
    ntdlib::nx::atk::SequenceSoundPlayer* player = model.engine->StartSequence(index);
    if (!Check(player != nullptr, "SEQ_HELD_LOOP starts"))
    {
        return;
    }

    ntdlib::nwsnd::LengthModel<PendingConsole> length_model(model, *player);
    for (int frame = 0; frame < 20 && !player->IsSettled(*model.engine); ++frame)
    {
        length_model.RunFrame();
    }

    g_pending = false;
    Check(length_model.Settled() && length_model.Stuck() == player->IsStuck(*model.engine) &&
              length_model.Busy() == model.engine->IsBusy(),
          "without a step to come, the model follows the player and the engine");
    g_pending = true;
    Check(!length_model.Settled() && !length_model.Stuck() && length_model.Busy(),
          "a step to come keeps the sequence from being settled or stuck, and keeps the model busy");
    g_pending = false;
}

// What SoundedConsole says about each frame's sound.
bool g_sounded = false;

// The Switch's console, with a frame's sound from g_sounded in place of the output's samples.
struct SoundedConsole : ntdlib::nx::atk::LengthConsole
{
    static bool Sounded(const Model&)
    {
        return g_sounded;
    }
};

// SEQ_HELD_LOOP's held note sounds from its first frames. A struct's Sounded replaces the test of the output. Without
// one, the model tests the output's samples.
void TestSounded()
{
    const ntdlib::sar::SoundArchive archive = LoadArchive(length_cases::Archive(false), false);
    uint32_t index = 0;
    while (index < length_cases::Cases().size() && std::string(length_cases::Cases()[index].name) != "SEQ_HELD_LOOP")
    {
        ++index;
    }
    ntdlib::nx::atk::ArchiveModel model(archive);
    ntdlib::nx::atk::SequenceSoundPlayer* player = model.engine->StartSequence(index);
    if (!Check(player != nullptr, "SEQ_HELD_LOOP starts"))
    {
        return;
    }

    ntdlib::nwsnd::LengthModel<ntdlib::nx::atk::LengthConsole> plain_model(model, *player);
    ntdlib::nwsnd::LengthModel<SoundedConsole> sounded_model(model, *player);
    for (int frame = 0; frame < 20 && !plain_model.Sounded(); ++frame)
    {
        plain_model.RunFrame();
    }

    Check(plain_model.Sounded(), "without Sounded, the model hears the held note in the output");
    g_sounded = false;
    Check(!sounded_model.Sounded(), "a struct's Sounded replaces the test of the output");
    g_sounded = true;
    Check(sounded_model.Sounded(), "a struct's Sounded gives the frame's sound");
    g_sounded = false;
}

} // namespace

int main()
{
    TestSwitch();
    TestWiiU();
    TestPending();
    TestSounded();

    return xsflib::testing::Summary("nwsnd_length_model");
}
