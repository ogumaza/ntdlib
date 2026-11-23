// SPDX-License-Identifier: MIT

// Tests the length analysis of a sequence sound (ntdlib/nwsnd/length_model.h) on the length cases
// (ntdlib/testing/length_cases.h), on the Switch's model and the Wii U's. The analysis waits for the loops of the
// tracks other than the main track. Counted loops that run past the limit follow. The analysis then takes each track's
// outermost counted loop as endless. Then a step of the game's code still to come (a console's Pending) holds the
// sequence back, and a console's Sounded replaces the test of the output. Last, the overload of MeasureSequence that
// takes a start and the rules gives the timings of the overload that takes a sound, a pass longer than the limit plays
// twice under the long limit, and a console's Tracks leaves tracks out of the wait for the other tracks' loops. The
// timing's first sample that isn't 0 comes from the console's output, and a loop's timing has its pass.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xsflib/rip/length.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/timing.h"

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
using xsflib::testing::CheckTiming;

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

// The timing of sound `sound` of `archive` on the console's model, measured by the overload of MeasureSequence that
// takes a start and the rules, or nothing.
template <class Console>
std::optional<xsflib::rip::Timing> MeasureWithStart(const ntdlib::sar::SoundArchive& archive, uint32_t sound)
{
    const auto make_model = [&archive]
    {
        return std::make_unique<typename Console::Model>(archive);
    };
    const auto start = [sound](typename Console::Model& model)
    {
        return model.engine->StartSequence(sound);
    };
    const auto measure = [&make_model, &start]
    {
        return ntdlib::nwsnd::MeasureSequence<Console>(make_model, start,
                                                       ntdlib::nwsnd::LengthRulesFor(Console::kFrameMs));
    };

    return xsflib::rip::TryMeasure(measure);
}

// Whether two measures give the same timing, or both give nothing.
bool SameTiming(const std::optional<xsflib::rip::Timing>& a, const std::optional<xsflib::rip::Timing>& b)
{
    if (!a || !b)
    {
        return !a && !b;
    }

    return a->length_ms == b->length_ms && a->fade_ms == b->fade_ms && a->silent == b->silent;
}

// The timing of sound 0 of the archive `bytes` on the console's model, or nothing.
template <class Console>
std::optional<xsflib::rip::Timing> MeasureFirst(const std::vector<uint8_t>& bytes, bool big_endian)
{
    return Measure<Console>(LoadArchive(bytes, big_endian), 0);
}

void TestSwitch()
{
    using ntdlib::nx::atk::LengthConsole;
    Check(LengthConsole::kFrameMs == 5.0, "the Switch's frames last 5 ms");
    const ntdlib::sar::SoundArchive archive = LoadArchive(length_cases::Archive(false), false);
    length_cases::CheckCases([&archive](uint32_t index) { return Measure<LengthConsole>(archive, index); }, true);

    // SEQ_END ends at the same frame as SEQ_REST_LOOP and SEQ_LONG_REST. A different rule ends each one. A model whose
    // output runs a frame behind its engine can end SEQ_END sooner.
    const auto measure = [&archive](uint32_t index)
    {
        return Measure<LengthConsole>(archive, index);
    };
    const long long end = length_cases::LengthOf(measure, "SEQ_END");
    Check(end > 0 && end == length_cases::LengthOf(measure, "SEQ_REST_LOOP"),
          "SEQ_END ends at the same frame as SEQ_REST_LOOP");

    const auto measure_first = [](const std::vector<uint8_t>& bytes)
    {
        return MeasureFirst<LengthConsole>(bytes, false);
    };
    length_cases::CheckLongLoops(ntdlib::sar::Flavor::kNx, measure_first, "the Switch");
}

void TestWiiU()
{
    using ntdlib::cafe::snd::LengthConsole;
    Check(LengthConsole::kFrameMs == 3.0, "the Wii U's frames last 3 ms");
    const ntdlib::sar::SoundArchive archive = LoadArchive(length_cases::Archive(true), true);
    length_cases::CheckCases([&archive](uint32_t index) { return Measure<LengthConsole>(archive, index); }, true);

    const auto measure_first = [](const std::vector<uint8_t>& bytes)
    {
        return MeasureFirst<LengthConsole>(bytes, true);
    };
    length_cases::CheckLongLoops(ntdlib::sar::Flavor::kCafe, measure_first, "the Wii U");
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
    const uint32_t index = length_cases::CaseIndex("SEQ_HELD_LOOP");
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
    const uint32_t index = length_cases::CaseIndex("SEQ_HELD_LOOP");
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

// With the engine's StartSequence as the start and the console's rules, the overload that takes a start gives each
// case and each sequence of length_cases::LongLoopArchives the timing of the overload that takes a sound.
template <class Console>
void CheckSameTimings(bool big_endian, const std::string& console)
{
    const ntdlib::sar::SoundArchive archive = LoadArchive(length_cases::Archive(big_endian), big_endian);
    int differing = 0;
    for (uint32_t i = 0; i < length_cases::Cases().size(); ++i)
    {
        if (!SameTiming(MeasureWithStart<Console>(archive, i), Measure<Console>(archive, i)))
        {
            ++differing;
        }
    }
    Check(differing == 0, "on " + console + ", the overload that takes a start gives every case the same timing, " +
                              std::to_string(differing) + " differ");

    for (const std::vector<uint8_t>& bytes : length_cases::LongLoopArchives(sar_files::FlavorOf(big_endian)))
    {
        const ntdlib::sar::SoundArchive loops = LoadArchive(bytes, big_endian);
        Check(SameTiming(MeasureWithStart<Console>(loops, 0), Measure<Console>(loops, 0)),
              "on " + console + ", the overload that takes a start gives a long loop the same timing");
    }
}

// A model that doesn't start and a sound that doesn't start each give nothing. A measure that reaches the limit inside
// a counted loop makes the model and starts the sound twice. A measure that ends before the limit does each once.
void TestStart()
{
    using ntdlib::nx::atk::LengthConsole;
    using Model = LengthConsole::Model;
    const ntdlib::sar::SoundArchive archive = LoadArchive(length_cases::Archive(false), false);
    const xsflib::rip::LengthRules rules = ntdlib::nwsnd::LengthRulesFor(LengthConsole::kFrameMs);
    int models = 0;
    int starts = 0;
    const auto make_model = [&archive, &models]
    {
        ++models;
        return std::make_unique<Model>(archive);
    };
    const auto start = [&starts](Model& model)
    {
        ++starts;
        return model.engine->StartSequence(length_cases::CaseIndex("SEQ_JUMP"));
    };

    const auto no_model = []
    {
        return std::unique_ptr<Model>();
    };
    Check(!ntdlib::nwsnd::MeasureSequence<LengthConsole>(no_model, start, rules) && starts == 0,
          "a model that doesn't start gives nothing, and nothing starts the sound");
    const auto no_player = [](Model&)
    {
        return static_cast<ntdlib::nx::atk::SequenceSoundPlayer*>(nullptr);
    };
    Check(!ntdlib::nwsnd::MeasureSequence<LengthConsole>(make_model, no_player, rules) && models == 1,
          "a sound that doesn't start gives nothing");

    models = 0;
    const std::optional<xsflib::rip::Timing> jump =
        ntdlib::nwsnd::MeasureSequence<LengthConsole>(make_model, start, rules);
    Check(jump && models == 1 && starts == 1, "a measure that ends before the limit runs once");

    const ntdlib::sar::SoundArchive counted =
        LoadArchive(length_cases::CountedLoopArchive(ntdlib::sar::Flavor::kNx), false);
    int counted_models = 0;
    int counted_starts = 0;
    const auto make_counted = [&counted, &counted_models]
    {
        ++counted_models;
        return std::make_unique<Model>(counted);
    };
    const auto start_counted = [&counted_starts](Model& model)
    {
        ++counted_starts;
        return model.engine->StartSequence(0);
    };
    const std::optional<xsflib::rip::Timing> timing =
        ntdlib::nwsnd::MeasureSequence<LengthConsole>(make_counted, start_counted, rules);
    Check(timing && counted_models == 2 && counted_starts == 2,
          "a measure that reaches the limit inside a counted loop makes the model and starts the sound twice");
}

// A loop whose pass takes 640 s with no counted loop: 64 times C4 for 250 ms and a rest, 10 s each, then a jump back.
// The measure under the limit of 600 s finds no loop: the limit's length, the fade and `at_limit`, from one model. With
// the long limit, a second model plays two passes: the fade starts at 1,280 s.
void TestLongLimit()
{
    using ntdlib::nx::atk::LengthConsole;
    using Model = LengthConsole::Model;
    length_cases::Code code(false);
    for (int i = 0; i < 64; i++)
    {
        code.Note(60, 127, 24).Wait(936);
    }

    code.Jump(0);
    const ntdlib::sar::SoundArchive archive =
        LoadArchive(length_cases::SequenceArchive(false, "SEQ_TEST", code), false);
    const xsflib::rip::LengthRules rules = ntdlib::nwsnd::LengthRulesFor(LengthConsole::kFrameMs);
    int models = 0;
    const auto make_model = [&archive, &models]
    {
        ++models;
        return std::make_unique<Model>(archive);
    };
    const auto start = [](Model& model)
    {
        return model.engine->StartSequence(0);
    };

    const std::optional<xsflib::rip::Timing> first =
        ntdlib::nwsnd::MeasureSequence<LengthConsole>(make_model, start, rules);
    const int first_models = models;
    const std::optional<xsflib::rip::Timing> long_limit =
        ntdlib::nwsnd::MeasureSequence<LengthConsole>(make_model, start, rules, {}, xsflib::rip::Limit::kLong);

    Check(first && first->length_ms == 600000 && first->fade_ms == 7000 && first->at_limit && first_models == 1,
          "a pass of 640 s reaches the limit of 600 s");
    Check(long_limit && long_limit->length_ms >= 1279995 && long_limit->length_ms <= 1280015 &&
              long_limit->fade_ms == 7000 && !long_limit->at_limit && models == 3,
          "with the long limit, a pass of 640 s plays twice, " +
              std::to_string(long_limit ? long_limit->length_ms : -1) + " ms");
}

// SEQ_JUMP's first sample that isn't 0 is the first one in the console's output, played frame by frame on the same
// model. A frame's pairs of samples share the frame equally. The timing has it rounded to the millisecond. The note
// loops each 24 ticks, 250 ms: the timing has that pass, and two passes end at its length.
template <class Console>
void CheckFirstSampleAndPass(bool big_endian, const std::string& console)
{
    const ntdlib::sar::SoundArchive archive = LoadArchive(length_cases::Archive(big_endian), big_endian);
    const uint32_t index = length_cases::CaseIndex("SEQ_JUMP");
    typename Console::Model model(archive);
    model.engine->StartSequence(index);
    std::optional<double> first_ms;
    for (int frame = 0; frame < 100 && !first_ms; ++frame)
    {
        model.engine->RunFrame();
        const std::span<const int16_t> output = Console::Output(model);
        for (std::size_t i = 0; i < output.size() && !first_ms; ++i)
        {
            if (output[i] != 0)
            {
                const double pair_ms = 2.0 * Console::kFrameMs / static_cast<double>(output.size());
                first_ms = frame * Console::kFrameMs + static_cast<double>(i / 2) * pair_ms;
            }
        }
    }

    const std::optional<xsflib::rip::Timing> timing = Measure<Console>(archive, index);

    Check(first_ms && timing && timing->first_sample_ms == static_cast<long long>(*first_ms + 0.5),
          "on " + console + ", the timing's first sample is the output's, " +
              std::to_string(timing && timing->first_sample_ms ? *timing->first_sample_ms : -1) + " ms");
    Check(timing && timing->loop && timing->loop->length_ms >= 245 && timing->loop->length_ms <= 255 &&
              timing->loop->start_ms + 2 * timing->loop->length_ms == timing->length_ms,
          "on " + console + ", SEQ_JUMP's pass is 250 ms, " +
              std::to_string(timing && timing->loop ? timing->loop->length_ms : -1) + " ms");
}

// The tracks that TracksConsole gives, a bit for each.
uint32_t g_tracks = ~0u;

// The Switch's console, with the tracks that play from g_tracks.
struct TracksConsole : ntdlib::nx::atk::LengthConsole
{
    static uint32_t Tracks(const Model&)
    {
        return g_tracks;
    }
};

// In SEQ_OTHER_LOOP, track 0 loops every 24 ticks and track 1 every 60 ticks. With both tracks, the fade waits for
// track 1's second loop at 120 ticks. A console's Tracks that leaves track 1 out gives the fade after track 0's second
// loop at 48 ticks, as in SEQ_JUMP. 48 ticks take 500 ms. The bounds are the case's.
void TestTracks()
{
    const ntdlib::sar::SoundArchive archive = LoadArchive(length_cases::Archive(false), false);
    const uint32_t index = length_cases::CaseIndex("SEQ_OTHER_LOOP");
    if (!Check(index < length_cases::Cases().size(), "SEQ_OTHER_LOOP is a case"))
    {
        return;
    }

    const length_cases::Case& other_loop = length_cases::Cases()[index];
    g_tracks = 0b11;
    const std::optional<xsflib::rip::Timing> both = MeasureWithStart<TracksConsole>(archive, index);
    g_tracks = 0b01;
    const std::optional<xsflib::rip::Timing> main_only = MeasureWithStart<TracksConsole>(archive, index);
    g_tracks = ~0u;

    CheckTiming("with both tracks, the fade waits for track 1's loops", both, other_loop.others_min_ms,
                other_loop.others_max_ms, other_loop.fade_ms);
    CheckTiming("a console's Tracks that leaves track 1 out gives the fade after track 0's loops", main_only,
                other_loop.min_ms, other_loop.max_ms, other_loop.fade_ms);
}

} // namespace

int main()
{
    TestSwitch();
    TestWiiU();
    TestPending();
    TestSounded();
    CheckFirstSampleAndPass<ntdlib::nx::atk::LengthConsole>(false, "the Switch");
    CheckFirstSampleAndPass<ntdlib::cafe::snd::LengthConsole>(true, "the Wii U");
    CheckSameTimings<ntdlib::nx::atk::LengthConsole>(false, "the Switch");
    CheckSameTimings<ntdlib::cafe::snd::LengthConsole>(true, "the Wii U");
    TestStart();
    TestLongLimit();
    TestTracks();

    return xsflib::testing::Summary("nwsnd_length_model");
}
