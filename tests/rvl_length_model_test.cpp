// SPDX-License-Identifier: MIT

// Tests the length analysis of a sequence sound (ntdlib/nwsnd/length_model.h) on the Wii's model
// (ntdlib/rvl/snd/length_model.h), with the renderer in place of the DSP. Each archive holds one sequence sound, from
// ntdlib/testing/rsar_files.h, whose waves sound here. The sequences play at the default timebase and tempo: 96 ticks a
// second. A loop's length runs to the end of its second pass, and an end gets 0.5 s after the last sound. A track that
// another track opens again during a wait starts its part again only when the wait runs out. A counted loop that
// outlasts the limit counts as endless, and only the outermost counted loop of a track goes endless.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/rip/length.h"
#include "xsflib/testing/check.h"

#include "ntdlib/nwsnd/length_model.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/snd/length_model.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sound_build.h"
#include "ntdlib/testing/rsar_files.h"

namespace
{

using ntdlib::testing::rsar_files::Bytes;
using xsflib::testing::Check;
using LengthConsole = ntdlib::rvl::snd::LengthConsole;

// The archive of MakeArchive with a sequence of `commands` and the tracks in the mask `tracks`. MakeArchive's waves are
// silent: each frame of their DSP-ADPCM holds only its header. Here each frame gives the samples of its nibbles at a
// scale of 2^11.
Bytes AudibleArchive(const Bytes& commands, uint32_t tracks)
{
    namespace rsar_files = ntdlib::testing::rsar_files;
    Bytes bytes = rsar_files::MakeArchive(rsar_files::MakeSequence(commands), 0, tracks);
    constexpr uint8_t kNibbles[] = {0x7F, 0x19, 0xE3, 0x5A, 0xC6, 0x2D, 0x91};
    for (std::size_t at = 0; at + 4 <= bytes.size(); at++)
    {
        if (std::memcmp(&bytes[at], "RWAV", 4) != 0)
        {
            continue;
        }

        // The wave's DATA block holds 0x40 bytes of samples after its header of 8 bytes.
        const uint32_t data = xsflib::LoadBe32(&bytes[at + 0x18]);
        for (std::size_t frame = 0; frame < 0x40; frame += 8)
        {
            uint8_t* samples = &bytes[at + data + 8 + frame];
            samples[0] = 0x0B;
            std::memcpy(samples + 1, kNibbles, sizeof(kNibbles));
        }
    }

    return bytes;
}

// The timing of the sequence of `commands` with the tracks in the mask `tracks`, on the model of Wii Sports' build.
std::optional<xsflib::rip::Timing> Measure(const Bytes& commands, uint32_t tracks)
{
    const ntdlib::rsar::SoundArchive archive =
        ntdlib::rsar::SoundArchive::Load(AudibleArchive(commands, tracks), ntdlib::rvl::snd::kRsarOptions);
    const auto make_model = [&archive]
    {
        auto model = std::make_unique<LengthConsole::Model>();
        model->build = ntdlib::SndBuild{20070227, false};
        model->StartWithRenderer(archive);
        return model;
    };
    const auto measure = [&make_model]
    {
        return ntdlib::nwsnd::MeasureSequence<LengthConsole>(make_model, 0);
    };

    return xsflib::rip::TryMeasure(measure);
}

// Checks that a sequence's length lies from `min_ms` to `max_ms` and that its fade and silence are as given.
void CheckTiming(const std::string& name, const std::optional<xsflib::rip::Timing>& timing, long long min_ms,
                 long long max_ms, long long fade_ms, bool silent = false)
{
    const std::string got = timing ? std::to_string(timing->length_ms) + " ms, fade " +
                                         std::to_string(timing->fade_ms) + " ms" + (timing->silent ? ", silent" : "")
                                   : "none";
    Check(timing && timing->length_ms >= min_ms && timing->length_ms <= max_ms && timing->fade_ms == fade_ms &&
              timing->silent == silent,
          name + " (" + got + ")");
}

// Each track that plays sets its notes' envelope first (D1 7F D2 7F D3 7F, 6 bytes): a full sustain at once and the
// fastest release. The test bank's regions leave both slow.
void TestLoopsAndEnds()
{
    Check(LengthConsole::kFrameMs == 3.0, "the Wii's frames last 3 ms");

    // Track 0 plays a note of 24 ticks over and over (from 6). Two loops take 500 ms.
    CheckTiming("two loops of 24 ticks",
                Measure({0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 60, 127, 24, 0x89, 0x00, 0x00, 6, 0xFF}, 1), 490, 510,
                10000);

    // Track 0 plays a note of 24 ticks and ends. The note sounds for 250 ms and releases at once. 500 ms of silence
    // follow.
    CheckTiming("a note that ends", Measure({0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 60, 127, 24, 0xFF}, 1), 745, 775, 0);

    // Track 0 waits a tick and jumps back, over and over, and plays nothing. The sequence gets the silence alone.
    CheckTiming("a sequence that never sounds is silent", Measure({0x80, 1, 0x89, 0x00, 0x00, 0, 0xFF}, 1), 500, 500, 0,
                true);
}

// Tests the length analysis on sequences whose track 0 opens track 1 again in each of its loops. Track 1 never loops.
void TestReopen()
{
    // Track 0 opens track 1 at 12, waits 48 ticks and jumps back to the open, over and over. Track 1 plays a note of 48
    // ticks, waits 96 ticks and ends. In the second sequence, track 1's note lasts 24 ticks. Track 1 then still waits
    // when track 0 opens it again.
    const Bytes kNoteEnds = {0x88, 1,    0x00, 0x00, 12,   0x80, 48, 0x89, 0x00, 0x00, 0,  0xFF,
                             0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 60, 127,  48,   0x80, 96, 0xFF};
    const Bytes kWaiting = {0x88, 1,    0x00, 0x00, 12,   0x80, 48, 0x89, 0x00, 0x00, 0,  0xFF,
                            0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 60, 127,  24,   0x80, 96, 0xFF};

    // Track 0's loops end at 48 and 96 ticks. Track 1's note ends at both, and track 1 starts its part again from the
    // open (1,000 ms).
    CheckTiming("a track opened again makes a pass as it starts again", Measure(kNoteEnds, 3), 990, 1010, 10000);

    // The opens keep track 1's wait. Track 1 starts again at 120 ticks, where its wait runs out, and at 144 ticks,
    // where its note ends as track 0 opens it again (1,500 ms). Were the passes counted at the opens, at 48 and 96
    // ticks, both loops would come while track 1 still waits. Nothing sounds then. The sequence would seem to end after
    // its first note, at about 750 ms.
    CheckTiming("a track opened again during a wait makes a pass when the wait runs out", Measure(kWaiting, 3), 1490,
                1510, 10000);
}

// Tests the length analysis on sequences whose tracks repeat their parts in counted loops that outlast the limit.
void TestLongCountedLoops()
{
    // Track 0 opens track 1 at 20, then plays a note of 480 ticks over and over (from 11). Track 1 plays a note of 480
    // ticks in a loop of 127 passes (from 26) and then starts the loop again. The 127 passes take 635 s.
    const auto counted = Measure({0x88, 1,    0x00, 0x00, 20,   0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 60,   127,
                                  0x83, 0x60, 0x89, 0x00, 0x00, 11,   0xFF, 0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F,
                                  0xD4, 127,  62,   127,  0x83, 0x60, 0xFC, 0x89, 0x00, 0x00, 26,   0xFF},
                                 3);

    // The analysis reaches its limit inside track 1's loop. It then takes that loop as endless. Track 1's passes then
    // count, and the fade starts after two loops of 480 ticks (5 s each).
    CheckTiming("a counted loop that outlasts the limit counts as endless", counted, 9990, 10010, 10000);

    // Track 0 repeats a part of 1,008 ticks in a loop of 127 passes (from 6) and then starts the loop again. The part
    // plays a note of 48 ticks, then a note of 480 ticks twice in an inner loop (from 11). A pass takes 10.5 s, and
    // the 127 passes 1,333.5 s. The limit comes 1.5 s into the 58th pass, inside the inner loop.
    const auto nested = Measure({0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 0xD4, 127,  60,   127,  48, 0xD4,
                                 2,    62,   127,  0x83, 0x60, 0xFC, 0xFC, 0x89, 0x00, 0x00, 6,  0xFF},
                                1);

    // Only the outer loop goes endless. Its passes count, and the fade starts after two of them (21 s). Were the inner
    // loop endless too, the track would repeat the note of 480 ticks for ever, and fade after 10.5 s.
    CheckTiming("only the outermost counted loop goes endless", nested, 20990, 21010, 10000);
}

} // namespace

int main()
{
    TestLoopsAndEnds();
    TestReopen();
    TestLongCountedLoops();

    return xsflib::testing::Summary("rvl_length_model");
}
