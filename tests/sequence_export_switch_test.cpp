// SPDX-License-Identifier: MIT

// The conversion tests of what only the Switch does: nn::atk's variable shifts on AArch64, its note-offs that wait for
// a channel's first update, the effect sends that reach the mix dry, the audio renderer's one-shot ends and
// interpolation, and biquad types 6 to 10. sequence_export_test.cpp runs the tests that hold on the Wii U as well.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/filter.h"
#include "ntdlib/sequence_export/instruments.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/sfz.h"
#include "seqlib/midi/performance_writer.h"
#include "sequence_export_test.h"
#include "xsflib/testing/check.h"

using namespace ntdlib::sequence_export;
using namespace ntdlib::sequence_export::tests;

namespace
{

void TestVariableShiftsAreAArch64s()
{
    // Set track variable 0 to 1 and shift it left by 64. AArch64's lsl takes that shift as 0. Then play key 60 if the
    // variable is still 1. Shift it by 40. AArch64 takes that as 8. The Wii U's PowerPC would leave nothing. Play key
    // 62 if the variable is 256.
    const Performance p =
        PerformOnBank(Code({0xf0, 0x80,        0x20, U16(0x0001), 0xf0, 0x85, 0x20, U16(0x0040), 0xf0, 0x90,
                            0x20, U16(0x0001), 0xa2, 0x3c,        0x64, 0x01, 0xf0, 0x85,        0x20, U16(0x0028),
                            0xf0, 0x90,        0x20, U16(0x0100), 0xa2, 0x3e, 0x64, 0x01,        0xff}),
                      OneRegionFbnk({{0x05000000, 0}}));

    XSFLIB_CHECK(NoteKeys(p) == (std::vector<int>{60, 62}));
}

void TestNoteOffBeforeTheFirstUpdateWaitsForIt()
{
    // At timebase 96 and tempo 999 a tick lasts 0.63 ms. A 5 ms frame holds 8 ticks. Key 60 plays for one tick. The
    // note-off comes before the channel's first update. The game makes it in that update, after the envelope's first
    // step: the note sounds for frame 0, and its release starts in frame 1. The MIDI note-off goes where frame 1
    // starts. A tie-off releases at once instead, and that note stays unheard.
    const Performance off = PerformOnBank(Code({0xb0, 0x60, 0xe1, U16(0x03e7), 0x3c, 0x64, 0x01, 0x80, 0x60, 0xff}),
                                          OneRegionFbnk({{0x05000000, 0}}));
    const Performance tie_off =
        PerformOnBank(Code({0xb0, 0x60, 0xe1, U16(0x03e7), 0xc7, 0x00, 0x3c, 0x64, 0x30, 0xc8, 0x00, 0x80, 0x60, 0xff}),
                      OneRegionFbnk({{0x05000000, 0}}));

    double note_off = -1.0;
    for (const TrackEvent& e : off.tracks[0])
    {
        if (e.kind == EventKind::kNoteOff)
        {
            note_off = e.tick + e.after;
        }
    }

    const double frame_1 = console->settings.frame_ms / (60000.0 / (96 * 999));
    XSFLIB_CHECK(NoteCount(off) == 1 && std::fabs(note_off - frame_1) < 1e-6);
    XSFLIB_CHECK(NoteCount(tie_off) == 0);
}

void TestSweepGoesOnThroughAWaitingNoteOff()
{
    // At timebase 96 and tempo 999, a tick lasts 0.63 ms. Ticks 0 and 1 fall in sound frame 0. With note wait off,
    // release 64 and portamento from key 48 at time 8, key 60 plays for one tick. The sweep of key 60 starts 12
    // semitones down and lasts 120 ms (SequenceTrack::NoteOn, 0x022C9A4C on the Wii U). At tick 1, key 64 glides from
    // key 60 with another sweep. The track therefore moves to another part for key 64. The note-off of key 60 comes
    // before the channel's first update. nn::atk makes the note-off in that update. The release then starts in frame 1
    // (Channel::NoteOff, 0xc23a70). Tempo 30 puts tick 2 in frame 4. Each frame moves the sweep on by 5 ms
    // (Channel::Update, 0xc23120, and GetSweepValue, 0x022C4204 on the Wii U). The part of key 60 therefore takes a
    // pitch in frames 1, 2 and 3, between ticks 1 and 2: 11.5, 11 and 10.5 semitones down.
    const Performance p =
        PerformOnBank(Code({0xb0, 0x60, 0xe1, U16(0x03e7), 0xc7, 0x00, 0xd3, 0x40, 0xc9,        0x30, 0xcf, 0x08, 0x3c,
                            0x64, 0x01, 0x80, 0x01,        0x40, 0x64, 0x30, 0xe1, U16(0x001e), 0x80, 0x0a, 0xff}),
                      OneRegionFbnk({{0x05000000, 0}}));

    std::vector<float> between_ticks;
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kPitch && e.part == 0 && e.tick == 1 && e.after > 0.0)
        {
            between_ticks.push_back(e.semitones);
        }
    }

    const std::vector<int> kParts = {0, 1};
    const std::vector<float> kBetweenTicks = {-11.5f, -11.0f, -10.5f};
    XSFLIB_CHECK(NoteParts(p) == kParts && between_ticks == kBetweenTicks);
}

void TestDrySendsAddToTheLevel()
{
    // Effect sends B and C (0xda, 0xde) reach the Switch's mix dry, beside the main bus. The main send (0xdb) sets the
    // main bus's level. A send of 127 to B doubles the note's amplitude. CC11 then goes past 127. Every CC11 is scaled
    // down, and the sequence 6 dB with it. A main send of 0 and no other sends leave the note silent. A send to A goes
    // to the synth's reverb (CC91) instead.
    const Performance send_b = PerformOnBank({0xda, 0x7f, 0x3c, 0x64, 0x30, 0xff}, OneRegionFbnk({{0x05000000, 0}}));
    const Performance no_main = PerformOnBank({0xdb, 0x00, 0x3c, 0x64, 0x30, 0xff}, OneRegionFbnk({{0x05000000, 0}}));
    const Performance send_a = PerformOnBank({0xd9, 0x7f, 0x3c, 0x64, 0x30, 0xff}, OneRegionFbnk({{0x05000000, 0}}));

    auto values = [](const Performance& p, EventKind kind)
    {
        std::vector<int> found;
        for (const TrackEvent& e : p.tracks[0])
        {
            if (e.kind == kind)
            {
                found.push_back(e.value);
            }
        }

        return found;
    };
    XSFLIB_CHECK(values(send_b, EventKind::kExpression) == std::vector<int>{127} &&
                 std::fabs(send_b.level_cut_db - 20.0 * std::log10(2.0)) < 0.001);
    XSFLIB_CHECK(values(no_main, EventKind::kExpression) == std::vector<int>{0});
    XSFLIB_CHECK(values(send_a, EventKind::kExpression) == std::vector<int>{127} &&
                 values(send_a, EventKind::kReverb) == std::vector<int>{127} && send_a.level_cut_db == 0.0);
}

void TestOneShotWaveEndsAsTheRendererPlaysIt()
{
    // Play a 6399-sample one-shot at four times the renderer's rate (960 samples a frame), then wait for it to finish.
    // The renderer plays the voice from its note-on frame, frame 0, and reads its last sample in the 7th frame, frame
    // 6. It reports that two frames later. nn::atk frees the channel in frame 9 (45 ms), before the frame's ticks.
    // Execution resumes at tick 5 (52.1 ms), the first tick after that.
    std::vector<ArchiveFile> files(3);
    files[0].data = Fseq({0x3c, 0x7f, 0x00, 0xff});
    files[1].data = OneRegionFbnk({{0x05000000, 0}});
    files[2].data = Fwar({Pcm16Fwav(6399, std::vector<int16_t>(6399, 1000), 1, false, 4 * 48000)});
    const SoundArchive archive =
        SoundArchive::Load(Fsar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}),
                           console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance p = Perform(archive, 0, banks, {});

    std::vector<uint32_t> note_offs;
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kNoteOff)
        {
            note_offs.push_back(e.tick);
        }
    }

    XSFLIB_CHECK(note_offs == std::vector<uint32_t>{5} && p.end_tick == 5);
}

void TestBiquadTypes6To10AreThe48kHzPresets()
{
    // Types 6 to 10 are the presets of types 1 to 5, made for the renderer's 48 kHz. They take the same steps, and the
    // SFZ controller takes their values. Value 64 of type 6 (step 55) keeps the low-pass at about 1.8 kHz. Type 1's
    // table, made for 32 kHz, gives about 2.7 kHz at 48 kHz.
    XSFLIB_CHECK(TrackFilters(0x40, 6, 64).biquad_type == 6 && TrackFilters(0x40, 6, 64).biquad_step == 55);
    XSFLIB_CHECK(TrackFilters(0x40, 8, 127).biquad_step == 121 && TrackFilters(0x40, 10, 127).biquad_step == 92);
    XSFLIB_CHECK(BiquadShape(1) == 1 && BiquadShape(6) == 1 && BiquadShape(7) == 2 && BiquadShape(10) == 5 &&
                 BiquadShape(0) == 0 && BiquadShape(11) == 0);
    XSFLIB_CHECK(SfzBiquadValue(6, 64) == 64);
    const SfzBiquad step55_48k = SfzBiquadStep(6, 55);
    XSFLIB_CHECK(step55_48k.cutoff > 900 && step55_48k.cutoff < 1100 && step55_48k.gain == -50);
}

void TestRegionInterpolationReachesNoRenderer()
{
    // A region can select linear interpolation (1). nn::atk keeps the request in the voice's record
    // (MultiVoice::SetInterpolationType, 0xc263b0) and never passes it to the Switch's audio renderer. Every voice
    // plays at the renderer's quality. The SFZ file doesn't follow the request, and nothing is reported.
    auto convert = [](uint8_t interpolation)
    {
        std::vector<ArchiveFile> files(3);
        files[0].data = Fseq({0x3c, 0x64, 0x18, 0xff});
        files[1].data = OneRegionFbnk({{0x05000000, 0}}, 127, 64, false, interpolation);
        files[2].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
        const SoundArchive archive =
            SoundArchive::Load(Fsar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}),
                               console->settings.archive);
        BankSet banks(archive, archive.Sounds()[0], console->settings);
        MidiReport midi;
        midi.bend_ranges[0] = 2;

        const Performance p = Perform(archive, 0, banks, {});
        const SfzInstruments sfz =
            BuildSfz(p, midi, banks, "TITLE", std::vector<std::string>(p.presets.size(), "preset"));

        auto mentions_interpolation = [](const auto& entry)
        {
            return entry.first.find("interpolation") != std::string::npos;
        };
        const bool reported = std::ranges::any_of(p.approximations, mentions_interpolation);
        return std::pair{sfz.files.size() == 1 && sfz.files[0].text.find(" sample_quality=") != std::string::npos,
                         reported};
    };

    XSFLIB_CHECK(convert(1) == std::pair(false, false) && convert(0) == std::pair(false, false));
}

const Test kTests[] = {
    TestVariableShiftsAreAArch64s,
    TestNoteOffBeforeTheFirstUpdateWaitsForIt,
    TestSweepGoesOnThroughAWaitingNoteOff,
    TestDrySendsAddToTheLevel,
    TestOneShotWaveEndsAsTheRendererPlaysIt,
    TestBiquadTypes6To10AreThe48kHzPresets,
    TestRegionInterpolationReachesNoRenderer,
};

} // namespace

int main()
{
    RunOn(Switch(), kTests);

    return xsflib::testing::Summary("sequence_export_switch");
}
