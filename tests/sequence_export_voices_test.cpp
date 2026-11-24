// SPDX-License-Identifier: MIT

// Tests of sequence_export on synthetic archives: the archive checks, inner loops, the voice limit, envelopes, filters
// and the reports of broken files. Each test runs on every console whose engine it describes, with the settings of that
// console (see sequence_export_test.h).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/midi/encoder.h"
#include "seqlib/midi/performance.h"
#include "seqlib/sf2/soundfont.h"
#include "xsflib/binary.h"
#include "xsflib/files.h"
#include "xsflib/testing/check.h"

#include "ntdlib/nwsnd/envelope.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/envelope.h"
#include "ntdlib/sequence_export/filter.h"
#include "ntdlib/sequence_export/instruments.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/performer.h"
#include "ntdlib/sequence_export/settings.h"
#include "ntdlib/sequence_export/sfz.h"
#include "ntdlib/testing/length_cases.h"
#include "ntdlib/testing/sar_files.h"
#include "ntdlib/testing/sequence_export_check.h"
#include "sequence_export_test.h"

using namespace ntdlib::sequence_export;
using namespace ntdlib::sequence_export::tests;
using ntdlib::nwsnd::DecibelSquare;
using ntdlib::nwsnd::HoldMs;
using xsflib::testing::ErrorOf;

namespace
{

void TestCheckArchivePassesAValidArchive()
{
    // The sequence of TestSequenceWithoutNotesEndsAtItsStart. Its MIDI file, SoundFont and frames are valid.
    std::vector<ArchiveFile> files(1);
    files[0].data = Fseq({0x80, 0x01, 0x89, 0x00, 0x00, 0x00});
    const SoundArchive archive = SoundArchive::Load(Fsar(files, {.sequences = {{0, 0}}}), console->settings.archive);

    const int failures = ntdlib::testing::CheckArchive(archive, console->settings, console->program);

    XSFLIB_CHECK(failures == 0);
}

void TestRunArchiveCheckGivesMainsExitCode()
{
    // The archive of TestCheckArchivePassesAValidArchive, in a file. Without the one argument, the check gives 2. A
    // file that can't be read gives 1, and the valid archive 0. The exit codes are fixed.
    std::vector<ArchiveFile> files(1);
    files[0].data = Fseq({0x80, 0x01, 0x89, 0x00, 0x00, 0x00});
    const std::vector<uint8_t> bytes = Fsar(files, {.sequences = {{0, 0}}});
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                       ("sequence_export_test_" + std::to_string(std::random_device{}()) + ".bin");
    XSFLIB_CHECK(!xsflib::WriteFile(path, bytes));
    std::string test = "xyz2midi_test", archive = path.string(), missing = path.string() + ".missing";
    char* without_argument[] = {test.data()};
    char* unreadable[] = {test.data(), missing.data()};
    char* valid[] = {test.data(), archive.data()};

    const int usage =
        ntdlib::testing::RunArchiveCheck(1, without_argument, console->settings, console->program, ".bin");
    const int broken = ntdlib::testing::RunArchiveCheck(2, unreadable, console->settings, console->program, ".bin");
    const int passed = ntdlib::testing::RunArchiveCheck(2, valid, console->settings, console->program, ".bin");
    std::filesystem::remove(path);

    XSFLIB_CHECK(usage == 2 && broken == 1 && passed == 0);
}

void TestSfzProblemFindsABrokenWav()
{
    SfzInstruments sfz;
    sfz.samples.push_back({"w0_1.wav", WavFile(std::vector<int16_t>(8, 0), 32000)});
    sfz.samples.push_back({"w0_2.wav", {'R', 'I', 'F', 'F'}});

    const std::string problem = ntdlib::testing::SfzProblem(sfz, {"Conductor"});

    XSFLIB_CHECK(problem == "broken WAV file w0_2.wav");
}

void TestCheckConversionFindsAFrameAfterItsTick()
{
    std::vector<ArchiveFile> files(1);
    files[0].data = Fseq({0x80, 0x01, 0x89, 0x00, 0x00, 0x00});
    const SoundArchive archive = SoundArchive::Load(Fsar(files, {.sequences = {{0, 0}}}), console->settings.archive);
    Conversion c = ConvertSequence(archive, 0);
    c.performance.frame_ticks = {0.0, 1.5};

    const int failures = ntdlib::testing::CheckConversion(c, "frames");

    XSFLIB_CHECK(failures == 1);
}

void TestTrackWaitingInSilenceDoesNotHoldOffTheLoop()
{
    // Track 0 opens track 1, holds key 60 for 2,000 ticks with note wait off, and then only waits 2 ticks and jumps
    // back. Track 1 plays key 60 for 24 ticks, then waits 3,000 ticks (31 s) in silence and ends. A track that has
    // sounded nothing and changed nothing for 10 s doesn't count as playing on. Track 0's idle passes therefore count
    // from then: the sequence stops about 10 s after track 1's note, and its loop is one of those passes. Track 0's
    // note still sounds there. The loop therefore isn't silent (SilentLoop).
    const Performance p = PerformOnBank(
        Code({0xfe, U16(0x0003), 0x88,          0x01, U24(0x000014), 0xc7, 0x00, 0x3c, 0x64, 0x8f, 0x50, 0x80,
              0x02, 0x89,        U24(0x00000e), 0xc7, 0x00,          0x3c, 0x64, 0x18, 0x80, 0x97, 0x38, 0xff}),
        OneRegionFbnk({{0x05000000, 0}}));

    XSFLIB_CHECK(p.seconds < 15.0 && p.loop && p.loop->start > 24 && p.loop->end - p.loop->start == 2);
}

// The ticks of the note-ons on track 0.
std::vector<uint32_t> NoteOnTicks(const Performance& p)
{
    std::vector<uint32_t> ticks;
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            ticks.push_back(e.tick);
        }
    }

    return ticks;
}

void TestCountdownInsideALoopIsAnInnerLoop()
{
    // Play key 60 for 12 ticks, set variable 32 to 40 and add 10, then wait a tick and subtract 1 until the variable
    // reaches 0, and jump back to the note. The countdown is an inner loop once the first pass of the outer loop has
    // ended. The marked loop therefore runs from one note to the next, 62 ticks, not over a silent tick of the
    // countdown. With a random draw of 0-40 in place of the 40, the passes differ. The marked loop is then the first.
    std::vector<uint8_t> commands =
        Code({0x3c, 0x64,        0x0c,        0xf0, 0x80, 0x20,          U16(0x0028), 0xf0,         0x81,
              0x20, U16(0x000a), 0x80,        0x01, 0xf0, 0x82,          0x20,        U16(0x0001),  0xf0,
              0x92, 0x20,        U16(0x0000), 0xa2, 0x89, U24(0x00000d), 0x89,        U24(0x000000)});
    const Performance counted = PerformOnBank(commands, OneRegionFbnk({{0x05000000, 0}}));
    commands[4] = 0x86;
    const Performance drawn = PerformOnBank(commands, OneRegionFbnk({{0x05000000, 0}}));

    const std::vector<uint32_t> counted_notes = NoteOnTicks(counted);
    const std::vector<uint32_t> drawn_notes = NoteOnTicks(drawn);
    XSFLIB_CHECK(counted.loop && counted.loop->end - counted.loop->start == 62);
    XSFLIB_CHECK(counted.loop && std::ranges::count(counted_notes, counted.loop->start) == 1);
    XSFLIB_CHECK(drawn.loop && drawn.loop->start == 0 && drawn_notes.size() > 1 && drawn.loop->end == drawn_notes[1]);
}

void TestTrackThatLeavesItsLoopForAnIdleLoopEnds()
{
    // Play key 60 for 12 ticks and wait 50 more, three times, then only wait a tick and jump back. The idle loop is on
    // the track of the marked loop. The track has left that loop. The idle loop therefore becomes the main loop. It
    // plays no note (SilentLoop). The sequence ends with its last note.
    const Performance p =
        PerformOnBank(Code({0xf0,        0x80, 0x20, U16(0x0000),   0x3c,        0x64, 0x0c, 0x80,
                            0x32,        0xf0, 0x81, 0x20,          U16(0x0001), 0xf0, 0x94, 0x20,
                            U16(0x0003), 0xa2, 0x89, U24(0x000005), 0x80,        0x01, 0x89, U24(0x000019)}),
                      OneRegionFbnk({{0x05000000, 0}}));

    XSFLIB_CHECK(!p.loop && p.loops == 0 && NoteOnTicks(p).size() == 3 && p.seconds < 2.0);
}

void TestLoopOfAnEndedTrackIsNoLoop()
{
    // As in TestTrackThatLeavesItsLoopForAnIdleLoopEnds, but track 0 ends after its three notes. Track 1 only waits a
    // tick and jumps back, from the start. Once track 0 has ended, its loop no longer plays. Track 1's idle loop then
    // becomes the main loop. The sequence ends with the last note.
    const Performance p = PerformOnBank(
        Code({0xfe, U16(0x0003), 0x88, 0x01, U24(0x000022), 0xf0, 0x80, 0x20,        U16(0x0000), 0x3c,
              0x64, 0x0c,        0x80, 0x32, 0xf0,          0x81, 0x20, U16(0x0001), 0xf0,        0x94,
              0x20, U16(0x0003), 0xa2, 0x89, U24(0x00000d), 0xff, 0x80, 0x01,        0x89,        U24(0x000022)}),
        OneRegionFbnk({{0x05000000, 0}}));

    XSFLIB_CHECK(!p.loop && p.loops == 0 && NoteOnTicks(p).size() == 3 && p.seconds < 2.0);
}

void TestTrackCountingDownInASubroutinePlaysOn()
{
    // Track 0 opens track 1, then plays key 60 twelve times (a loop with a count). After each note, a subroutine counts
    // down 96 one-tick waits and returns. Track 1 plays key 60 every 24 ticks. The countdown passes can leave their
    // loop and don't count. Track 0 therefore still plays on during them. Track 1's loop can't become the main loop
    // before track 0 ends, after 12 s. The sequence plays all twelve notes of track 0.
    const Performance p =
        PerformOnBank(Code({0xfe, U16(0x0003), 0x88,        0x01, U24(0x00002c), 0xc7, 0x00,          0xd4,        0x0c,
                            0x3c, 0x64,        0x18,        0x8a, U24(0x000015), 0xfc, 0xff,          0xf0,        0x80,
                            0x20, U16(0x0000), 0x80,        0x01, 0xf0,          0x81, 0x20,          U16(0x0001), 0xf0,
                            0x91, 0x20,        U16(0x0060), 0xa2, 0xfd,          0x89, U24(0x00001a), 0xc7,        0x00,
                            0x3c, 0x64,        0x0c,        0x80, 0x18,          0x89, U24(0x00002e)}),
                      OneRegionFbnk({{0x05000000, 0}}));

    XSFLIB_CHECK(NoteOnTicks(p).size() == 12 && p.loop && p.loop->start > 11 * 96 && p.loop->end - p.loop->start == 24);
}

void TestTrackPollingInASubroutineLetsALaterTrackLoop()
{
    // Track 0 opens track 1, then calls a subroutine that waits a tick, ends the track if variable 0 holds 1, and jumps
    // back, for ever. Nothing sets the variable. The poll's passes can leave their loop, and they don't count. They
    // change nothing, no variable included, and draw no random number. Track 0 is therefore idle, and track 1's loop is
    // the main loop. Track 1 plays key 60 for 24 ticks every 48 ticks. The rhythm pieces of Super Mario Party and of
    // Jamboree poll the game's variables in such a subroutine on track 0.
    const Performance p = PerformOnBank(
        Code({0xfe, U16(0x0003), 0x88, 0x01,        U24(0x00001b), 0x8a, U24(0x00000d), 0xff,          0x80, 0x01,
              0xf0, 0x90,        0x00, U16(0x0001), 0xa2,          0xff, 0x89,          U24(0x00000d), 0xfd, 0xc7,
              0x00, 0x3c,        0x64, 0x18,        0x80,          0x30, 0x89,          U24(0x00001d)}),
        OneRegionFbnk({{0x05000000, 0}}), 3);

    XSFLIB_CHECK(p.seconds < 15.0 && p.loop && p.loop->end - p.loop->start == 48);
}

void TestJumpBackIntoAFinishedCallEndsNoPass()
{
    // Set variable 32 to 0. Then call subroutine A, play key 60 for 12 ticks, call subroutine B and jump back, for
    // ever. B waits a tick and returns. A adds 1 to the variable and, from the second pass on, jumps back into B's
    // wait. That wait last ran in B's call of the pass before. That call has returned. The jump therefore ends no pass
    // and makes no loop inner. Otherwise the outer loop and B's wait would each make the other inner. No pass would
    // count, and the sequence would run to the time limit. From the second pass on, a pass of the outer loop takes 14
    // ticks.
    const Performance p = PerformOnBank(Code({0xf0, 0x80, 0x20,        U16(0x0000),   0x8a, U24(0x000017), 0x3c,
                                              0x64, 0x0c, 0x8a,        U24(0x000014), 0x89, U24(0x000005), 0x80,
                                              0x01, 0xfd, 0xf0,        0x81,          0x20, U16(0x0001),   0xf0,
                                              0x91, 0x20, U16(0x0002), 0xa2,          0x89, U24(0x000014), 0xfd}),
                                        OneRegionFbnk({{0x05000000, 0}}));

    XSFLIB_CHECK(p.seconds < 15.0 && p.loop && p.loop->end - p.loop->start == 14);
}

void TestInnerLoopCountsAgainOnceTheOuterLoopTakesLong()
{
    // Set variable 33 to 0, turn note wait off and hold key 60. Then set the volume to 80 and count variable 32 down, a
    // tick at a time: from 10 in the first pass and from 2,000 in the later ones. Set the volume to 100 and variable 33
    // to 1, and jump back to the volume of 80. The countdown is an inner loop once the first pass of the outer loop has
    // ended. The second pass would end after about 21 s. The countdown's passes count again 10 s after the first pass.
    // The performance then stops after about 10 s, on a tick of the countdown. The held note sounds through it.
    const Performance p = PerformOnBank(
        Code({0xf0,          0x80, 0x21,        U16(0x0000), 0xc7, 0x00,        0x3c,        0x64, 0x00,
              0xc1,          0x50, 0xf0,        0x80,        0x20, U16(0x000a), 0xf0,        0x90, 0x21,
              U16(0x0001),   0xa2, 0xf0,        0x80,        0x20, U16(0x07d0), 0x80,        0x01, 0xf0,
              0x82,          0x20, U16(0x0001), 0xf0,        0x92, 0x20,        U16(0x0000), 0xa2, 0x89,
              U24(0x00001c), 0xc1, 0x64,        0xf0,        0x80, 0x21,        U16(0x0001), 0x89, U24(0x00000a)}),
        OneRegionFbnk({{0x05000000, 0}}));

    XSFLIB_CHECK(p.seconds < 11.0 && p.loop && p.loop->end - p.loop->start == 1 && NoteOnTicks(p).size() == 1);
}

void TestTrackStartsNoMoreNotesATickThanTheGameHasVoices()
{
    // Test per-tick voice limits with notewait off, with the game's voices: 96, or 24 on the 3DS. Four notes more than
    // that on key 60 at once all play without --voices. With --voices and the game's count (Settings::voice_limit),
    // they yield as many voices as the game has. On the Wii U, the four find no free channel and play nothing, as in
    // the game. On the 3DS, the game gives the four newest notes the voices of the four oldest. The performer omits the
    // four newest as an approximation. The Switch has no --voices. Without --voices, the cap is kMaxVoiceLimit: of 1030
    // notes at once, kMaxVoiceLimit play, and the performer omits six as an approximation. The report says that the
    // game keeps the first notes on the Wii U, and the newest on the other consoles. With tie enabled, two notes more
    // than the voices, ending on key 72, use one channel; the final legato requires no approximation. As many notes as
    // the voices on missing program 5 allocate nothing. A following held note on program 0 must therefore play.
    // Finally, one tied note more than the voices at velocity 1 is followed by one at 127 with a new release. That note
    // would require another preset. The note must keep the existing MIDI note, in its preset and at its level, without
    // reducing the sequence's level.
    const int voices = console->settings.voices;
    std::vector<ArchiveFile> files(7);
    files[0].data = Fseq(Code({0xc7, 0x00, 0xd4, voices + 4, 0x3c, 0x64, 0x30, 0xfc, 0x80, 0x30, 0xff}));
    files[1].data = Fseq(
        Code({0xc7, 0x00, 0xc8, 0x01, 0xd4, voices + 1, 0x3c, 0x64, 0x30, 0xfc, 0x48, 0x64, 0x30, 0x80, 0x30, 0xff}));
    files[2].data = Fseq(Code({0xc7, 0x00, 0x81, 0x05, 0xd4, voices, 0x3c, 0x64, 0x30, 0xfc, 0x81, 0x00, 0xc7, 0x01,
                               0x3c, 0x64, 0x00, 0xff}));
    files[3].data = Fseq(Code({0xc7, 0x00, 0xc8, 0x01, 0x3c, 0x01, 0x30, 0xd4, voices, 0x3c, 0x01,
                               0x30, 0xfc, 0xd3, 0x40, 0x3c, 0x7f, 0x30, 0x80, 0x30,   0xff}));
    files[4].data = Fseq(Code({0xc7, 0x00, 0xd4, 0x05, 0xd4, 0xce, 0x3c, 0x64, 0x30, 0xfc, 0xfc, 0x80, 0x30, 0xff}));
    files[5].data = OneRegionFbnk({{0x05000000, 0}});
    files[6].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const std::vector<uint32_t> kBanks = {0x03000000};
    const std::vector<SequenceSound> kSounds = {
        {0, 0, kBanks}, {1, 0, kBanks}, {2, 0, kBanks}, {3, 0, kBanks}, {4, 0, kBanks}};
    const SoundArchive archive = SoundArchive::Load(
        Fsar(files, {.sequences = kSounds, .banks = {5}, .wave_archives = {6}}), console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance flood = Perform(archive, 0, banks, {});
    const Performance capped = Perform(archive, 0, banks, {.voice_limit = console->settings.voice_limit});
    const Performance tied = Perform(archive, 1, banks, {});
    const Performance held = Perform(archive, 2, banks, {});
    const Performance loud = Perform(archive, 3, banks, {});
    const Performance most = Perform(archive, 4, banks, {});

    auto note_ons = [](const Performance& p)
    {
        return std::ranges::count_if(p.tracks[0], [](const TrackEvent& e) { return e.kind == EventKind::kNoteOn; });
    };

    float tied_pitch = 0.0f;
    for (const TrackEvent& e : tied.tracks[0])
    {
        if (e.kind == EventKind::kPitch)
        {
            tied_pitch = e.semitones;
        }
    }

    const std::string legatos_past_cap =
        "more than " + std::to_string(voices) +
        " legatos in a tick (keeping the MIDI preset and limiting gain to the starting "
        "level)";
    const std::string past_cap_ending =
        console->settings.engine == Engine::kCafe
            ? " new channels on a track in one tick (excess omitted; the game keeps the first)"
            : " new channels on a track in one tick (excess omitted; the game keeps the newest)";
    auto past_cap = [&past_cap_ending](const Performance& p)
    {
        return std::ranges::count_if(p.approximations,
                                     [&past_cap_ending](const auto& a) { return a.first.ends_with(past_cap_ending); });
    };
    const auto left_out = capped.approximations.find("more than " + std::to_string(voices) + past_cap_ending);
    const auto past_most = most.approximations.find("more than " + std::to_string(kMaxVoiceLimit) + past_cap_ending);
    const bool option = console->settings.voice_limit > 0;
    const bool drops = option && console->settings.engine != Engine::kCtr;
    XSFLIB_CHECK(note_ons(flood) == voices + 4 && past_cap(flood) == 0);
    XSFLIB_CHECK(note_ons(capped) == (option ? voices : voices + 4));
    XSFLIB_CHECK(option && !drops ? left_out != capped.approximations.end() && left_out->second == 4
                                  : past_cap(capped) == 0);
    XSFLIB_CHECK(note_ons(most) == static_cast<int>(kMaxVoiceLimit) && past_most != most.approximations.end() &&
                 past_most->second == 6);
    XSFLIB_CHECK(note_ons(tied) == 1 && tied_pitch == 12.0f && !tied.approximations.contains(legatos_past_cap));
    XSFLIB_CHECK(note_ons(held) == 1 && held.holds);
    XSFLIB_CHECK(note_ons(loud) == 1 && loud.level_cut_db == 0.0 && loud.approximations.contains(legatos_past_cap));
}

void TestIdleTrackDoesNotOwnTheLoopWhileAnotherPlays()
{
    // Track 0 opens track 1 and then only waits 2 ticks and jumps back. Track 1 plays key 60 every 48 ticks, 30 times,
    // for 15 s, and ends. Track 0's passes change nothing while track 1 plays. They therefore aren't passes of the main
    // loop until track 1 has ended. The sequence plays all of track 1. The idle loop after it sounds nothing
    // (SilentLoop). The sequence therefore ends with track 1's last note and has no loop markers.
    const Performance p =
        PerformOnBank(Code({0xfe, U16(0x0003), 0x88, 0x01, U24(0x00000e), 0x80, 0x02, 0x89, U24(0x000008), 0xc7,
                            0x00, 0xd4,        0x1e, 0x3c, 0x64,          0x18, 0x80, 0x30, 0xfc,          0xff}),
                      OneRegionFbnk({{0x05000000, 0}}));

    const auto notes = std::ranges::count(p.tracks[1], EventKind::kNoteOn, &TrackEvent::kind);
    XSFLIB_CHECK(notes == 30 && !p.loop && p.loops == 0 && p.seconds > 14.75 && p.seconds < 15.0);
}

void TestTrackPlayingANoteEachTimeAVariableIsSetGoesOn()
{
    // Track 0 sets variable 0 to 1 every 50 ticks. Track 1 waits 45 ticks, then polls without waits. When the variable
    // is 1, track 1 clears it, plays key 60 and goes on polling in the same tick. Track 1 should play once per update
    // from track 0. The main loop's pass that ends at tick 1000, the first past 10 s, ends the performance. That's
    // before track 1 has run without waits for 10 s. Track 0 changes no sound. The variable it sets is one that track 1
    // reads, though. Track 0's 50-tick passes are therefore passes of the main loop.
    const Performance p = PerformOnBank(
        Code({0xfe, U16(0x0003), 0x88,        0x01,          U24(0x000013), 0x80,          0x32,         0xf0, 0x80,
              0x00, U16(0x0001), 0x89,        U24(0x000008), 0x80,          0x2d,          0xc7,         0x00, 0xf0,
              0x95, 0x00,        U16(0x0001), 0xa2,          0x89,          U24(0x000017), 0xf0,         0x80, 0x00,
              0x00, 0x00,        0x3c,        0x64,          0x18,          0x89,          U24(0x000017)}),
        OneRegionFbnk({{0x05000000, 0}}));

    std::vector<uint32_t> note_ons;
    for (const TrackEvent& e : p.tracks[1])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            note_ons.push_back(e.tick);
        }
    }

    const bool every_50 = std::ranges::all_of(note_ons, [](uint32_t tick) { return tick % 50 == 0; });
    XSFLIB_CHECK(p.warnings.empty() && note_ons.size() == 19 && note_ons.front() == 50 && every_50);
    XSFLIB_CHECK(p.loop && p.loop->end - p.loop->start == 50);
}

void TestVariableSetAfterAJumpBackBelongsToTheNewPass()
{
    // As in TestTrackPlayingANoteEachTimeAVariableIsSetGoesOn, but track 0 sets the variable first in each pass, in the
    // tick it jumps back, and track 1 polls once a tick. That change belongs to the pass it starts. Track 0's 48-tick
    // passes therefore still own the main loop, not track 1's one-tick polls.
    const Performance p = PerformOnBank(
        Code({0xfe,         U16(0x0003),   0x88, 0x01, U24(0x000013), 0xf0, 0x80, 0x00, U16(0x0001), 0x80,        0x30,
              0x89,         U24(0x000008), 0xc7, 0x00, 0x80,          0x01, 0xf0, 0x95, 0x00,        U16(0x0001), 0xa2,
              0x89,         U24(0x000015), 0xf0, 0x80, 0x00,          0x00, 0x00, 0x3c, 0x64,        0x18,        0x89,
              U24(0x000015)}),
        OneRegionFbnk({{0x05000000, 0}}));

    XSFLIB_CHECK(p.loop && p.loop->end - p.loop->start == 48);
}

void TestTrackPlayingNotesWithoutWaitingIsStopped()
{
    // Loop a one-tick key 60 with notewait off: 5000 note commands per 10,000-command tick. With --voices, keep the
    // game's voices per tick, 96 or 24 on the 3DS, then stop the track after 10 s without a wait. With the game's voice
    // drops, a tick's notes still hold every channel in the next tick, and that tick's notes find none. The notes then
    // come every second tick. On the 3DS, a tick's notes take the voices of the last tick's notes instead. Every tick
    // then has notes. The Switch has no --voices. Without --voices, a track keeps kMaxVoiceLimit notes a tick, and 10 s
    // of them take a minute and a half to perform. The test therefore runs on the Wii U and the 3DS only.
    // TestTrackStartsNoMoreNotesATickThanTheGameHasVoices checks the cap without --voices.
    const Performance p =
        PerformOnBank(Code({0xc7, 0x00, 0x3c, 0x64, 0x01, 0x89, U24(0x000002)}), OneRegionFbnk({{0x05000000, 0}}), 1,
                      {.voice_limit = console->settings.voice_limit});

    std::map<uint32_t, int> note_ons; // by tick
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            note_ons[e.tick]++;
        }
    }

    const std::vector<std::string> kStopped = {"track 0 stopped after running without waits"};
    const int voices = console->settings.voices;
    const bool every_tick_full =
        std::ranges::all_of(note_ons, [voices](const auto& tick) { return tick.second == voices; });
    const bool drops = console->settings.voice_limit > 0 && console->settings.engine != Engine::kCtr;
    const bool even_ticks = std::ranges::all_of(note_ons, [](const auto& tick) { return tick.first % 2 == 0; });
    const std::size_t ticks = drops ? 2 * note_ons.size() : note_ons.size();
    XSFLIB_CHECK(p.warnings == kStopped && every_tick_full && ticks > 900 && ticks < 1000);
    XSFLIB_CHECK(!drops || even_ticks);
}

void TestNoteWithoutAFreeChannelIsDropped()
{
    // With notewait off, keys 60, 62 and 64 start a tick apart and hold for 96 ticks. With two voices, the third note
    // finds no free channel and plays nothing, as in the game. --voices off keeps it. So does the default. The
    // console's count has room for all three.
    const std::vector<uint8_t> code = Code(
        {0xc7, 0x00, 0x3c, 0x64, 0x60, 0x80, 0x01, 0x3e, 0x64, 0x60, 0x80, 0x01, 0x40, 0x64, 0x60, 0x80, 0x60, 0xff});
    const std::vector<uint8_t> bank = OneRegionFbnk({{0x05000000, 0}});

    const Performance two = PerformOnBank(code, bank, 1, {.voice_limit = 2});
    const Performance off = PerformOnBank(code, bank, 1, {.voice_limit = 0});
    const Performance usual = PerformOnBank(code, bank);
    const Performance game = PerformOnBank(code, bank, 1, {.voice_limit = console->settings.voice_limit});

    auto keys = [](const Performance& p)
    {
        std::vector<int> played;
        for (const TrackEvent& e : p.tracks[0])
        {
            if (e.kind == EventKind::kNoteOn)
            {
                played.push_back(e.key);
            }
        }

        return played;
    };
    const std::vector<int> kFirstTwo = {60, 62};
    const std::vector<int> kAll = {60, 62, 64};
    XSFLIB_CHECK(keys(two) == kFirstTwo);
    XSFLIB_CHECK(keys(off) == kAll && keys(usual) == kAll && keys(game) == kAll);
}

void TestSoundsOwnPanCurveIsReported()
{
    // Play key 60 with four sound settings: pan curve 2, balance mode on mono (no effect), balance mode on stereo, and
    // the default curve/mode.
    std::vector<ArchiveFile> files(4);
    files[0].data = Fseq({0x3c, 0x64, 0x30, 0xff});
    files[1].data = OneRegionFbnk({{0x05000000, 0}});
    files[2].data = OneRegionFbnk({{0x05000000, 1}});
    const std::vector<int16_t> kSamples(64, 1000);
    files[3].data = Fwar({Pcm16Fwav(64, kSamples, 1, true), Pcm16Fwav(64, kSamples, 2, true)});
    const std::vector<uint32_t> kMono = {0x03000000}, kStereo = {0x03000001};
    const std::vector<SequenceSound> kSounds = {{.banks = kMono, .pan_curve = 2},
                                                {.banks = kMono, .pan_mode = 1},
                                                {.banks = kStereo, .pan_mode = 1},
                                                {.banks = kMono}};
    const SoundArchive archive = SoundArchive::Load(
        Fsar(files, {.sequences = kSounds, .banks = {1, 2}, .wave_archives = {3}}), console->settings.archive);
    BankSet mono(archive, archive.Sounds()[0], console->settings);
    BankSet stereo(archive, archive.Sounds()[2], console->settings);

    const Performance curve = Perform(archive, 0, mono, {});
    const Performance mono_balance = Perform(archive, 1, mono, {});
    const Performance stereo_balance = Perform(archive, 2, stereo, {});
    const Performance usual = Perform(archive, 3, mono, {});

    const std::string kPan = "custom pan curve or mode (using defaults)";
    XSFLIB_CHECK(archive.Sounds()[0].pan_curve == 2 && archive.Sounds()[2].pan_mode == 1);
    XSFLIB_CHECK(curve.approximations.contains(kPan) && stereo_balance.approximations.contains(kPan));
    XSFLIB_CHECK(!mono_balance.approximations.contains(kPan) && !usual.approximations.contains(kPan));
}

void TestEnvelopeValuesMatchTheGame()
{
    XSFLIB_CHECK(AttackMs(127) == 1.0);
    XSFLIB_CHECK(AttackMs(0) > 13000.0 && AttackMs(0) < 13300.0); // 13.1 s: the slowest attack
    XSFLIB_CHECK(DecayRate(127) == 65535.0 && DecayRate(126) == 24.0);
    XSFLIB_CHECK(DecibelSquare(127) == 0.0f && DecibelSquare(0) == -723.0f);
    XSFLIB_CHECK(HoldMs(0) == 0 && HoldMs(1) == 1);
}

void TestInstantEnvelopeIsAnOrgan()
{
    // Attack, decay, sustain and release 127, hold 0: immediate full level, then immediate release.
    const Sf2Envelope e = ConvertEnvelope(EnvelopeValues{127, 127, 127, 0, 127});

    XSFLIB_CHECK(e.attack == -12000 && e.hold == -12000 && e.sustain == 0 && e.release == -12000);
}

void TestDecayTakesTheGamesTime()
{
    // A decay of 100 dB takes 1000 / rate envelope milliseconds. The 3DS's envelope steps 5 ms each 4.89 ms frame.
    // env_time_scale turns envelope time into real time.
    const double decay_ms = 1000.0 / DecayRate(100) * console->settings.env_time_scale;

    const Sf2Envelope d = ConvertEnvelope(EnvelopeValues{127, 100, 64, 0, 100});

    XSFLIB_CHECK(std::abs(d.decay - 1200.0 * std::log2(decay_ms / 1000.0)) <= 1.0);
    XSFLIB_CHECK(d.sustain == -DecibelSquare(64));
}

void TestSlowDecaySplitsItsError()
{
    // Decay 2 is slower than SoundFont's slowest decay, 8000 timecents for 100 dB. The hold makes up half the
    // difference. At the hold's end, the note is as far above the game's level as the note is below that level when the
    // decay reaches sustain 100 (-4.2 dB).
    const double game_ms = 1000.0 / DecayRate(2) * console->settings.env_time_scale;
    const double sustain_db = -DecibelSquare(100) / 10.0;

    const Sf2Envelope d = ConvertEnvelope(EnvelopeValues{127, 2, 100, 0, 127});

    const double hold_ms = 1000.0 * std::exp2(d.hold / 1200.0);
    const double reached_ms = hold_ms + sustain_db / 100.0 * 1000.0 * std::exp2(d.decay / 1200.0);
    const double loud_db = 100.0 * hold_ms / game_ms;
    const double quiet_db = sustain_db - 100.0 * reached_ms / game_ms;
    XSFLIB_CHECK(d.decay == 8000);
    XSFLIB_CHECK(loud_db > 0.1 && std::abs(loud_db - quiet_db) < 0.01);
}

void TestSfzDecayTakesTheGamesTime()
{
    // SFZ's decay falls by a factor of e^-8 (69.49 dB) over its time (sfzformat.com's ampeg_attack page). The test
    // expects the game's time to fall 69.49 dB. The game's envelope falls 100 dB in 1000 / rate of its milliseconds.
    // env_time_scale turns them into real milliseconds.
    const double decay_ms = 1000.0 / DecayRate(100) * console->settings.env_time_scale;

    const SfzEnvelope d = ConvertEnvelopeForSfz(EnvelopeValues{127, 100, 64, 0, 100});

    const double kSfzFallDb = 8.0 * 20.0 / std::log(10.0);
    XSFLIB_CHECK(std::abs(d.decay * 1000.0 - decay_ms * kSfzFallDb / 100.0) < 1e-9 * decay_ms);
    XSFLIB_CHECK(d.release == d.decay && d.attack == 0.0 && d.hold == 0.0);
    XSFLIB_CHECK(std::abs(d.sustain - 100.0 * std::pow(10.0, DecibelSquare(64) / 200.0)) < 1e-9);

    // An attack of 127 is instant.
    const SfzEnvelope organ = ConvertEnvelopeForSfz(EnvelopeValues{127, 127, 127, 0, 127});
    XSFLIB_CHECK(organ.attack == 0.0 && organ.hold == 0.0 && organ.sustain == 100.0 && organ.release < 0.001);
}

void TestEnvelopeSimulationSustainsAndReleases()
{
    // Reach sustain, then release below -90.4 dB. Before the first update, release starts at -90.4 dB even for instant
    // attack.
    XSFLIB_CHECK(HeldLevel(EnvelopeValues{127, 127, 64, 0, 127}, 10) == DecibelSquare(64));
    XSFLIB_CHECK(HeldLevel(EnvelopeValues{127, 127, 64, 0, 127}, 0) == -904.0f);
    XSFLIB_CHECK(ReleaseFrames(0.0f, DecayRate(127)) == 1);
    XSFLIB_CHECK(FramesToLevel(EnvelopeValues{127, 127, 127, 0, 127}, -400.0f) == -1);
    XSFLIB_CHECK(FramesToLevel(EnvelopeValues{127, 100, 0, 0, 127}, -400.0f) > 0);

    // A release of 0.1 dB a millisecond falls 0.3 dB a step on the Wii U and 0.5 dB on the Switch. From -50 dB, 134
    // steps of 0.3 dB end at -90.2 dB, or 80 of 0.5 dB at -90 dB. The next step goes below -90.4 dB. From -49.9 dB, 135
    // steps of 0.3 dB or 81 of 0.5 dB reach -90.4 dB exactly. That doesn't stop the channel. Only the next step falls
    // below the stop threshold.
    const int step = console->settings.env_step_ms;
    XSFLIB_CHECK(ReleaseFrames(-500.0f, 1.0f) == 404 / step + 1);
    XSFLIB_CHECK(ReleasedLevel(-499.0f, 1.0f, 405 / step) == -904.0f && ReleaseFrames(-499.0f, 1.0f) == 405 / step + 1);

    // Release 2 falls exactly 2^-7 x 0.1 dB a millisecond: 3 x 2^-7 x 0.1 dB a step on the Wii U, and 5 x 2^-7 x 0.1 dB
    // on the Switch. From sustain 2 (-72.1 dB), 7808 steps on the Wii U reach -90.4 dB, and only the 7809th falls below
    // the stop threshold. On the Switch, 4684 steps leave it just above -90.4 dB, and the 4685th falls below.
    const float kSustain2 = DecibelSquare(2);
    const int64_t steps = 183 * 128 / step;
    const float level = kSustain2 - static_cast<float>(steps * step) / 128.0f;
    XSFLIB_CHECK(ReleasedLevel(kSustain2, DecayRate(2), steps) == level && level >= -904.0f);
    XSFLIB_CHECK(ReleaseFrames(kSustain2, DecayRate(2)) == steps + 1);
}

void TestNoteIsQuietOnlyAfterItsAttack()
{
    // A slow attack starts below -40 dB, but has not yet decayed to that level.
    const EnvelopeValues kSlowAttack{60, 100, 0, 0, 127};

    const int64_t frames = FramesToLevel(kSlowAttack, -400.0f);

    XSFLIB_CHECK(frames * console->settings.env_step_ms > AttackMs(kSlowAttack.attack));
}

// The first biquad type that has no preset on the console. The presets of the Wii U and the 3DS are types 1 to 5. The
// Switch adds types 6 to 10, the same presets made for 48 kHz.
int FirstTypeWithoutPreset()
{
    return console->settings.engine == Engine::kSwitch ? 11 : 6;
}

void TestTrackFiltersFollowTheGame()
{
    // Low-pass arguments 0-10 select kLpfFreq[0] (80 Hz); 11-57 advance through the table: 32 -> 800 Hz, 40 -> 2000, 48
    // -> 5120, 54 -> 10240. Arguments >= 58, including 0x40, disable it.
    const std::pair<int, int> kLowPass[] = {{0, 0},   {10, 0},  {11, 1},  {32, 10},   {40, 14}, {48, 18},
                                            {54, 21}, {57, 22}, {58, -1}, {0x40, -1}, {255, -1}};
    for (const auto& [arg, index] : kLowPass)
    {
        XSFLIB_CHECK(TrackFilters(arg, 0, 0).low_pass == index);
    }

    // A valid biquad type and positive value enable the filter. Value 64 selects low-pass step 55; values >= 127 select
    // the final step.
    XSFLIB_CHECK(TrackFilters(0x40, 1, 64).biquad_type == 1 && TrackFilters(0x40, 1, 64).biquad_step == 55);
    XSFLIB_CHECK(TrackFilters(0x40, 1, 255).biquad_step == 111 && TrackFilters(0x40, 2, 127).biquad_step == 96);
    XSFLIB_CHECK(TrackFilters(0x40, 3, 127).biquad_step == 121 && TrackFilters(0x40, 5, 127).biquad_step == 92);
    XSFLIB_CHECK(TrackFilters(0x40, 1, 0).biquad_type == 0 && TrackFilters(0x40, 0, 64).biquad_type == 0 &&
                 TrackFilters(0x40, FirstTypeWithoutPreset(), 64).biquad_type == 0);
}

void TestSoundFontFilterStandsInForTheGames()
{
    // No active filter leaves SoundFont open. So does an unsupported high-pass.
    XSFLIB_CHECK(ToSoundFont(TrackFilters(0x40, 0, 0)) == SoundFontFilter{});
    XSFLIB_CHECK(ToSoundFont(TrackFilters(0x40, 2, 64)) == SoundFontFilter{});

    // Biquad low-pass Q is 0.864 (17 cB for FluidSynth). With both low-passes enabled, select the lower cutoff.
    const SoundFontFilter biquad = ToSoundFont(TrackFilters(0x40, 1, 64));
    const SoundFontFilter low_pass = ToSoundFont(TrackFilters(32, 0, 0));
    XSFLIB_CHECK(biquad.cutoff < kFilterOpen && biquad.resonance == 17 && low_pass.resonance == 0);
    XSFLIB_CHECK(ToSoundFont(TrackFilters(32, 1, 64)) == (low_pass.cutoff < biquad.cutoff ? low_pass : biquad));

    // Lower settings give lower cutoffs.
    int rising = 0;
    for (int arg = 1; arg < 58; arg++)
    {
        rising += ToSoundFont(TrackFilters(arg, 0, 0)).cutoff < ToSoundFont(TrackFilters(arg - 1, 0, 0)).cutoff;
    }
    for (int value = 1; value < 128; value++)
    {
        rising +=
            ToSoundFont(TrackFilters(0x40, 1, value)).cutoff > ToSoundFont(TrackFilters(0x40, 1, value - 1)).cutoff;
    }

    XSFLIB_CHECK(rising == 0);

    // Each controller step lowers cutoff by 75 cents.
    XSFLIB_CHECK(FilterControllerValue(kFilterOpen) == 0 && FilterControllerValue(kFilterOpen - 75) == 1);
    XSFLIB_CHECK(FilterControllerValue(kFilterOpen - 75 * 111) == 111 && FilterControllerValue(0) == 127);
}

void TestSfzFilterFollowsTheGame()
{
    // SFZ controllers: 64 minus the low-pass argument (<= 6 means off), and the biquad value (0 without a preset).
    XSFLIB_CHECK(SfzLowPassValue(0x40) == 0 && SfzLowPassValue(200) == 0 && SfzLowPassValue(58) == 6);
    XSFLIB_CHECK(SfzLowPassValue(57) == 7 && SfzLowPassValue(0) == 64 && SfzLowPassValue(-100) == 127);
    XSFLIB_CHECK(SfzBiquadValue(1, 64) == 64 && SfzBiquadValue(2, 300) == 127 && SfzBiquadValue(0, 64) == 0);
    XSFLIB_CHECK(SfzBiquadValue(FirstTypeWithoutPreset(), 64) == 0 && SfzBiquadValue(3, -5) == 0);

    // AX's one-pole filter puts the nominal 80 Hz cutoff at 80 Hz, 9558 cents below 20 kHz. The Switch's renderer, from
    // nn::atk's table for 48 kHz, puts it at 115 Hz, 8936 cents below 20 kHz. On the 3DS, the cosine table shifts it to
    // a one-pole filter at 102 Hz, about 9131 cents below 20 kHz. Lower settings must give lower cutoffs.
    XSFLIB_CHECK(SfzLowPassCutoff(0) == ByConsole(-9558, -8936, -9131));
    int falling = 0;
    for (int i = 1; i < 23; i++)
    {
        falling += SfzLowPassCutoff(i) <= SfzLowPassCutoff(i - 1);
    }

    XSFLIB_CHECK(falling == 0);

    // Biquad low-pass value 64 (step 55) matches a cookbook filter at about 1.8 kHz, Q 0.864 (-1.27 dB), gain -0.5 dB.
    // The Switch's renderer runs that table, made for 32 kHz, at 48 kHz: about 2.7 kHz. Band-pass gain is 1.
    const SfzBiquad step55 = SfzBiquadStep(1, 55);
    const int lowest = ByConsole(900, 1600, 900);
    XSFLIB_CHECK(step55.cutoff > lowest && step55.cutoff < lowest + 200 && step55.resonance == -127 &&
                 step55.gain == -50);
    XSFLIB_CHECK(SfzBiquadStep(3, 60).gain == 0 && SfzBiquadStep(5, 92).gain == 0);
    XSFLIB_CHECK(SfzBiquadStep(1, 500).cutoff == SfzBiquadStep(1, 111).cutoff);
}

void TestMissingBlockIsReported()
{
    // A wave with INFO (0x7000) but no DATA block: an FWAV, big-endian, or on the 3DS a CWAV, little-endian.
    const bool ctr = console->settings.engine == Engine::kCtr;
    const std::string magic = ctr ? "CWAV" : "FWAV";
    std::vector<uint8_t> wave(0x20, 0);
    std::memcpy(wave.data(), magic.data(), 4);
    wave[ctr ? 4 : 5] = 0xff;
    wave[ctr ? 5 : 4] = 0xfe;
    wave[ctr ? 0x10 : 0x11] = 1;
    wave[ctr ? 0x15 : 0x14] = 0x70;

    const std::string message = ErrorOf<xsflib::FormatError>([&] { DecodeWave(wave); });

    XSFLIB_CHECK(message == "bad " + magic + ": no DATA block");
}

void TestShortFileIsReported()
{
    const std::string message =
        ErrorOf<xsflib::FormatError>([&] { DecodeWave(std::vector<uint8_t>{'C', 'W', 'A', 'V'}); });

    XSFLIB_CHECK(message == ByConsole<std::string>("bad FWAV: too short to be an FWAV file",
                                                   "bad FWAV: too short to be an FWAV file",
                                                   "bad CWAV: too short to be a CWAV file"));
}

// The tests that hold on every console, the Wii included.
const Test kEveryConsoleTests[] = {
    TestInstantEnvelopeIsAnOrgan,
    TestDecayTakesTheGamesTime,
    TestSlowDecaySplitsItsError,
    TestSfzDecayTakesTheGamesTime,
    TestNoteIsQuietOnlyAfterItsAttack,
    TestEnvelopeValuesMatchTheGame,
    TestSoundFontFilterStandsInForTheGames,
    TestIdleTrackDoesNotOwnTheLoopWhileAnotherPlays,
    TestVariableSetAfterAJumpBackBelongsToTheNewPass,
    TestTrackWaitingInSilenceDoesNotHoldOffTheLoop,
};

// The tests that hold on the Wii U, the Switch and the 3DS.
const Test kWiiUSwitchAndCtrTests[] = {
    TestEnvelopeSimulationSustainsAndReleases,
    TestTrackFiltersFollowTheGame,
    TestSfzFilterFollowsTheGame,
    TestMissingBlockIsReported,
    TestShortFileIsReported,
    TestTrackStartsNoMoreNotesATickThanTheGameHasVoices,
    TestTrackPlayingANoteEachTimeAVariableIsSetGoesOn,
    TestCountdownInsideALoopIsAnInnerLoop,
    TestTrackThatLeavesItsLoopForAnIdleLoopEnds,
    TestLoopOfAnEndedTrackIsNoLoop,
    TestTrackCountingDownInASubroutinePlaysOn,
    TestTrackPollingInASubroutineLetsALaterTrackLoop,
    TestJumpBackIntoAFinishedCallEndsNoPass,
    TestInnerLoopCountsAgainOnceTheOuterLoopTakesLong,
    TestSoundsOwnPanCurveIsReported,
};

// The tests that hold on the Wii U and the Switch.
const Test kWiiUAndSwitchTests[] = {
    TestCheckArchivePassesAValidArchive,
    TestRunArchiveCheckGivesMainsExitCode,
    TestSfzProblemFindsABrokenWav,
    TestCheckConversionFindsAFrameAfterItsTick,
};

// The tests that hold on the Wii U and the 3DS.
const Test kWiiUAndCtrTests[] = {
    TestTrackPlayingNotesWithoutWaitingIsStopped,
};

// The tests of the Wii U alone.
const Test kWiiUOnlyTests[] = {
    TestNoteWithoutAFreeChannelIsDropped,
};

} // namespace

int main()
{
    RunOn(WiiU(),
          Join({kEveryConsoleTests, kWiiUSwitchAndCtrTests, kWiiUAndSwitchTests, kWiiUAndCtrTests, kWiiUOnlyTests}));
    RunOn(Switch(), Join({kEveryConsoleTests, kWiiUSwitchAndCtrTests, kWiiUAndSwitchTests}));
    RunOn(Ctr(), Join({kEveryConsoleTests, kWiiUSwitchAndCtrTests, kWiiUAndCtrTests}));
    RunOn(Wii(), kEveryConsoleTests);

    return xsflib::testing::Summary("sequence_export_voices");
}
