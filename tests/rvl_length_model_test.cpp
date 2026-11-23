// SPDX-License-Identifier: MIT

// Tests the length analysis of a sequence sound (ntdlib/nwsnd/length_model.h) on the Wii's model
// (ntdlib/rvl/snd/length_model.h), with the renderer in place of the DSP. rvl_length_cases.h has the tests. The
// tests of ArrangedLengthConsole, without the DSP and with only some tracks, are here.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "xsflib/rip/length.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/timing.h"

#include "ntdlib/nwsnd/length_model.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/snd/length_model.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sound_build.h"
#include "ntdlib/testing/rsar_files.h"
#include "rvl_length_cases.h"

namespace
{

using ntdlib::testing::rsar_files::Bytes;
using ntdlib::testing::rsar_files::MakeAudibleArchive;
using xsflib::testing::Check;
using xsflib::testing::CheckTiming;
using ArrangedLengthConsole = ntdlib::rvl::snd::ArrangedLengthConsole;
using LengthConsole = ntdlib::rvl::snd::LengthConsole;

// The timing of the sequence of `commands` with the tracks in the mask `tracks`, on the model of Wii Sports' build and
// the struct `Console`. `start(model, archive)` starts the model on the archive.
template <class Console, class Start>
std::optional<xsflib::rip::Timing> MeasureOn(const Bytes& commands, uint32_t tracks, const Start& start)
{
    const ntdlib::rsar::SoundArchive archive =
        ntdlib::rsar::SoundArchive::Load(MakeAudibleArchive(commands, tracks), ntdlib::rvl::snd::kRsarOptions);
    const auto make_model = [&archive, &start]
    {
        auto model = std::make_unique<typename Console::Model>();
        model->build = ntdlib::SndBuild{20070227, false};
        start(*model, archive);
        return model;
    };
    const auto measure = [&make_model]
    {
        return ntdlib::nwsnd::MeasureSequence<Console>(make_model, 0);
    };

    return xsflib::rip::TryMeasure(measure);
}

// The timing of the sequence of `commands` with the tracks in the mask `tracks`, with the renderer in place of the DSP.
std::optional<xsflib::rip::Timing> Measure(const Bytes& commands, uint32_t tracks)
{
    const auto start = [](LengthConsole::Model& model, const ntdlib::rsar::SoundArchive& archive)
    {
        model.StartWithRenderer(archive);
    };

    return MeasureOn<LengthConsole>(commands, tracks, start);
}

// The timing of the sequence of `commands` with the tracks in the mask `tracks`, on ArrangedLengthConsole. The model
// runs without the DSP when `without_dsp` is set, and with the renderer otherwise. The tracks in the mask `playing`
// play (ArrangedLengthModel::tracks).
std::optional<xsflib::rip::Timing> MeasureArranged(const Bytes& commands, uint32_t tracks, bool without_dsp,
                                                   uint32_t playing = ~0u)
{
    const auto start =
        [without_dsp, playing](ArrangedLengthConsole::Model& model, const ntdlib::rsar::SoundArchive& archive)
    {
        if (without_dsp)
        {
            model.StartWithoutDsp(archive);
        }
        else
        {
            model.StartWithRenderer(archive);
        }

        model.without_dsp = without_dsp;
        model.tracks = playing;
    };

    return MeasureOn<ArrangedLengthConsole>(commands, tracks, start);
}

// Tests the length analysis on ArrangedLengthConsole: with the renderer as on LengthConsole, without the DSP, and with
// only some tracks playing.
void TestArranged()
{
    // With the renderer, the struct times each sequence as LengthConsole does, its first sample and loop included.
    const Bytes kLoops = {0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 60, 127, 24, 0x89, 0x00, 0x00, 6, 0xFF};
    const Bytes kEnds = {0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 60, 127, 24, 0xFF};
    for (const Bytes& commands : {kLoops, kEnds})
    {
        const auto plain = Measure(commands, 1);
        const auto arranged = MeasureArranged(commands, 1, false);
        const bool same = plain && arranged && plain->length_ms == arranged->length_ms &&
                          plain->fade_ms == arranged->fade_ms && plain->first_sample_ms == arranged->first_sample_ms &&
                          plain->first_sound_ms == arranged->first_sound_ms &&
                          plain->loop.has_value() == arranged->loop.has_value();
        Check(same, "with the renderer, ArrangedLengthConsole times a sequence as LengthConsole does");
    }

    // Without the DSP, a frame sounds while a note plays. The loops and the end come at the same ticks.
    CheckTiming("two loops of 24 ticks without the DSP", MeasureArranged(kLoops, 1, true), 490, 510, 7000);
    CheckTiming("a note that ends without the DSP", MeasureArranged(kEnds, 1, true), 245, 275, 0);

    // Track 0 waits 12 ticks (125 ms) and plays a note of 24 ticks. Without the DSP, the first frame in which the note
    // plays gives the first sample, from the frame's start: 125 ms, within a frame.
    const auto waits = MeasureArranged({0x80, 12, 0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 60, 127, 24, 0xFF}, 1, true);
    Check(waits && waits->first_sample_ms && *waits->first_sample_ms >= 122 && *waits->first_sample_ms <= 131,
          "without the DSP, the first frame in which a note plays gives the first sample (" +
              std::to_string(waits && waits->first_sample_ms ? *waits->first_sample_ms : -1) + " ms)");

    // Track 0 opens track 1 at 19, then plays a note of 24 ticks over and over (from 11). Track 1 plays a note of 60
    // ticks over and over (from 25). With both tracks, the fade waits for track 1's second loop, at 120 ticks
    // (1,250 ms). With track 1 out of the tracks that play, the fade starts after track 0's second loop, at 48 ticks
    // (500 ms).
    const Bytes kOtherLoop = {0x88, 1,    0x00, 0x00, 19,   0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F,
                              60,   127,  24,   0x89, 0x00, 0x00, 11,   0xFF, 0xD1, 0x7F, 0xD2,
                              0x7F, 0xD3, 0x7F, 62,   127,  60,   0x89, 0x00, 0x00, 25,   0xFF};
    CheckTiming("with both tracks, the fade waits for track 1's loops", MeasureArranged(kOtherLoop, 3, false), 1240,
                1260, 7000);
    CheckTiming("with track 1 out, the fade starts after track 0's loops", MeasureArranged(kOtherLoop, 3, false, 1),
                490, 510, 7000);
}

} // namespace

int main()
{
    Check(LengthConsole::kFrameMs == 3.0, "the Wii's frames last 3 ms");
    rvl_length_cases::RunAll(Measure);
    TestArranged();

    return xsflib::testing::Summary("rvl_length_model");
}
