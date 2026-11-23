// SPDX-License-Identifier: MIT

// Tests of sequence_export on synthetic archives: the reading of the commands, and the notes, legatos, ties and sweeps
// with their MIDI parts. Each test runs on every console whose engine it describes, with the settings of that console
// (see sequence_export_test.h).

#include "sequence_export_test.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "seqlib/midi/encoder.h"
#include "seqlib/midi/performance.h"
#include "xsflib/binary.h"
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

using namespace ntdlib::sequence_export;
using namespace ntdlib::sequence_export::tests;
using ntdlib::sar::SoundInfo;

namespace
{

void TestBrokenBankAndWaveReferencesAreReported()
{
    // Banks 0-4 all play their wave entry 0. Test these references: bank 0 has no entry; bank 1 selects missing wave 1
    // in WARC_0; bank 2 selects absent wave archive 9; bank 3 uses the valid no-wave sentinel 0xFFFFFFFF; bank 4
    // references item 0xFFFFFFFF, not a wave archive. Sound 0 uses banks 0-2 and missing bank 99; sound 1 uses banks
    // 3-4.
    std::vector<ArchiveFile> files(6);
    files[0].data = OneRegionFbnk({});
    files[1].data = OneRegionFbnk({{0x05000000, 1}});
    files[2].data = OneRegionFbnk({{0x05000009, 0}});
    files[3].data = OneRegionFbnk({{0x05000000, 0xffffffff}});
    files[4].data = OneRegionFbnk({{0xffffffff, 0}});
    files[5].data = Fwar({Pcm16Fwav(1, {0})});
    const SoundArchive archive =
        SoundArchive::Load(Fsar(files, {.banks = {0, 1, 2, 3, 4}, .wave_archives = {5}}), console->settings.archive);
    SoundInfo first;
    first.banks = {0x03000000, 0x03000001, 0x03000002, 0x03000063};
    SoundInfo second;
    second.banks = {0x03000003, 0x03000004};

    BankSet banks(archive, first, console->settings);
    BankSet more_banks(archive, second, console->settings);
    const bool no_waves = !banks.GetWave(0, 0) && !banks.GetWave(1, 0) && !banks.GetWave(2, 0) &&
                          !more_banks.GetWave(0, 0) && !more_banks.GetWave(1, 0);
    banks.GetWave(0, 0); // repeat the lookup without reporting the error again

    XSFLIB_CHECK(no_waves && banks.FindRegion(0, 0, 60, 100) && more_banks.NoWave(0, 0) && !more_banks.NoWave(1, 0));
    const std::vector<std::string> kErrors = {"bank slot 3 has 0x03000063: invalid bank ID",
                                              "bank BANK_0 has no wave 0", "wave archive WARC_0 has no wave 1",
                                              "wave archive 9 doesn't exist (the archive has 1)"};
    const std::vector<std::string> kMoreErrors = {"bank BANK_4's wave 0 is in 0xffffffff: invalid wave archive ID"};
    XSFLIB_CHECK(banks.Errors() == kErrors && more_banks.Errors() == kMoreErrors);
}

void TestLittleEndianFilesAreRead()
{
    // A file's byte order mark gives its byte order. A little-endian PCM16 wave in the Switch's format decodes the same
    // as a big-endian one.
    const std::vector<int16_t> kSamples = {1, -2, 300, -32768};

    const Pcm big = DecodeWave(Pcm16Fwav(4, kSamples));
    const Pcm little = DecodeWave(Pcm16Fwav(4, kSamples, 1, false, 32000, ntdlib::sar::Flavor::kNx));

    XSFLIB_CHECK(big.channels.size() == 1 && big.channels[0] == kSamples);
    XSFLIB_CHECK(little.channels.size() == 1 && little.channels[0] == kSamples && little.sample_rate == 32000);
}

void TestCallsNestTenDeep()
{
    // Levels 0 to 10 each call the next and then play a note a semitone above the last level's note. Level 11 plays key
    // 71. On the Wii U and the Switch, tracks nest ten calls. On the 3DS they nest three. Level 10's call is the first
    // to be skipped.
    std::vector<uint8_t> commands;
    for (int level = 0; level <= 10; level++)
    {
        const uint32_t next = static_cast<uint32_t>(8 * (level + 1));
        Append(commands, {0x8a, U24(next)});
        commands.insert(commands.end(), {static_cast<uint8_t>(60 + level), 0x64, 0x01, 0xfd});
    }
    commands.insert(commands.end(), {71, 0x64, 0x01, 0xfd});
    commands[7] = 0xff; // level 0 finishes instead of returning
    const Performance p = PerformOnBank(commands, OneRegionFbnk({{0x05000000, 0}}));

    const std::vector<int> kKeys = {70, 69, 68, 67, 66, 65, 64, 63, 62, 61, 60};
    XSFLIB_CHECK(NoteKeys(p) == kKeys);
}

void TestNotesKeepTheirCommands()
{
    // With note wait off, key 60 plays at tick 0. The track waits 24 ticks and calls the subroutine at 24 twice. Key 64
    // plays there for 6 ticks. A loop of two passes then plays key 62 for 12 ticks in each. Each command's offset
    // comes first in its comment.
    std::vector<uint8_t> commands;
    Append(commands, {0xc7, 0x00});       // 0: note wait off
    Append(commands, {0x3c, 0x64, 0x18}); // 2: key 60 for 24 ticks
    Append(commands, {0x80, 0x18});       // 5: wait 24 ticks
    Append(commands, {0x8a, U24(24)});    // 7: call 24
    Append(commands, {0x8a, U24(24)});    // 11: call 24
    Append(commands, {0xd4, 0x02});       // 15: loop twice
    Append(commands, {0x3e, 0x64, 0x0c}); // 17: key 62 for 12 ticks
    Append(commands, {0x80, 0x0c});       // 20: wait 12 ticks
    Append(commands, {0xfc});             // 22: the loop's end
    Append(commands, {0xff});             // 23: the end
    Append(commands, {0x40, 0x64, 0x06}); // 24: key 64 for 6 ticks
    Append(commands, {0x80, 0x06});       // 27: wait 6 ticks
    Append(commands, {0xfd});             // 29: return

    const Performance p = PerformOnBank(commands, OneRegionFbnk({{0x05000000, 0}}));

    // The calls, the returns and the loop's end begin the stretches of commands. The note commands are in them.
    const std::vector<Stretch> kStretches = {
        {.tick = 0, .first = 0, .last = 7, .entry = Entry::kOpen, .depth = 0, .note_wait = true},
        {.tick = 24, .first = 24, .last = 29, .entry = Entry::kCall, .depth = 1, .note_wait = false},
        {.tick = 30, .first = 11, .last = 11, .entry = Entry::kReturn, .depth = 0, .note_wait = false},
        {.tick = 30, .first = 24, .last = 29, .entry = Entry::kCall, .depth = 1, .note_wait = false},
        {.tick = 36, .first = 15, .last = 22, .entry = Entry::kReturn, .depth = 0, .note_wait = false},
        {.tick = 48, .first = 17, .last = 23, .entry = Entry::kLoop, .depth = 1, .note_wait = false},
    };
    const std::vector<NoteCommand> kCommands = {{2, 0}, {24, 1}, {24, 3}, {17, 4}, {17, 5}};
    XSFLIB_CHECK((NoteKeys(p) == std::vector<int>{60, 64, 64, 62, 62}));
    XSFLIB_CHECK(p.stretches[0] == kStretches && p.note_commands == kCommands);
}

void TestMixChangesAreRecorded()
{
    // The track sets its volume, its pan and send A before a note of 24 ticks, then sends B and C. Its volume then
    // falls to 50 over 4 ticks (a time prefix on 0xc1) during a note of 8 ticks. A command that gives a setting the
    // value that it has records nothing.
    const std::vector<uint8_t> commands = Code({
        0xc1, 0x64,               // volume 100
        0xc0, 0x20,               // pan 32, or -32 from the centre
        0xd9, 0x28,               // send A 40
        0xc1, 0x64,               // volume 100 again
        0x3c, 0x64, 0x18,         // key 60 for 24 ticks
        0xda, 0x0a,               // send B 10
        0xde, 0x05,               // send C 5
        0xa3, 0xc1, 0x32, U16(4), // volume 50 over 4 ticks
        0x3e, 0x64, 0x08,         // key 62 for 8 ticks
        0xff,
    });

    const Performance p = PerformOnBank(commands, OneRegionFbnk({{0x05000000, 0}}));

    // The ramp moves the volume down in each of the 4 ticks after its command.
    const std::vector<MixChange> kChanges = {
        {0, Mix::kVolume, 100}, {0, Mix::kPan, -32}, {0, Mix::kSendA, 40}, {24, Mix::kSendB, 10}, {24, Mix::kSendC, 5},
    };
    const std::vector<MixChange>& mix = p.mix[0];
    XSFLIB_CHECK(mix.size() == kChanges.size() + 4 && std::equal(kChanges.begin(), kChanges.end(), mix.begin()));
    for (std::size_t i = kChanges.size(); i < mix.size(); i++)
    {
        const int16_t before = i == kChanges.size() ? 100 : mix[i - 1].value;
        XSFLIB_CHECK(mix[i].setting == Mix::kVolume && mix[i].tick == 25 + i - kChanges.size() &&
                     mix[i].value < before);
    }
    XSFLIB_CHECK(mix.back().value == 50);
    XSFLIB_CHECK(std::all_of(p.mix.begin() + 1, p.mix.end(), [](const auto& changes) { return changes.empty(); }));
}

void TestWiiUCommandsAreRead()
{
    // First: the main volume falls to 64 over 48 ticks (a time prefix on 0xc2) during a 96-tick note. Second: the
    // second LFO gets a depth (0xf0 0xa2, one byte) before a note. The note still plays. Third: 0xe4 sets the first
    // LFO's period to 100 ms. Its speed is then 10 Hz.
    std::vector<ArchiveFile> files(5);
    files[0].data = Fseq(Code({0xa3, 0xc2, 0x40, U16(0x0030), 0x3c, 0x64, 0x60, 0xff}));
    files[1].data = Fseq({0xf0, 0xa2, 0x40, 0x3c, 0x64, 0x30, 0xff});
    files[2].data = Fseq(Code({0xca, 0x40, 0xe4, U16(0x000a), 0x3c, 0x64, 0x30, 0xff}));
    files[3].data = OneRegionFbnk({{0x05000000, 0}});
    files[4].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const std::vector<uint32_t> kBanks = {0x03000000};
    const SoundArchive archive = SoundArchive::Load(
        Fsar(files,
             {.sequences = {{0, 0, kBanks}, {1, 0, kBanks}, {2, 0, kBanks}}, .banks = {3}, .wave_archives = {4}}),
        console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance fade = Perform(archive, 0, banks, {});
    const Performance second_lfo = Perform(archive, 1, banks, {});
    const Performance period = Perform(archive, 2, banks, {});

    std::vector<std::pair<uint32_t, int>> expression;
    for (const TrackEvent& e : fade.tracks[0])
    {
        if (e.kind == EventKind::kExpression)
        {
            expression.emplace_back(e.tick, e.value);
        }
    }

    XSFLIB_CHECK(expression.size() > 10 && expression.front() == (std::pair<uint32_t, int>{0, 127}));
    XSFLIB_CHECK(!expression.empty() && expression.back() == (std::pair<uint32_t, int>{48, 64}));
    XSFLIB_CHECK(NoteKeys(second_lfo) == std::vector<int>{60});
    XSFLIB_CHECK(second_lfo.approximations.contains("second to fourth LFOs (omitted)"));
    XSFLIB_CHECK(period.presets.size() == 1 && period.presets[0].lfo_speed == 10.0f);
}

void TestVariableShiftsAreThePowerPcs()
{
    // Set track variable 0 to 1 and shift it left by 64. PowerPC's slw takes that shift as 0. Then play key 60 if the
    // variable is still 1. Shift it by 40. That leaves nothing. Play key 62 if the variable is 0.
    const Performance p = PerformOnBank(
        {0xf0, 0x80, 0x20, 0x00, 0x01, 0xf0, 0x85, 0x20, 0x00, 0x40, 0xf0, 0x90, 0x20, 0x00, 0x01, 0xa2, 0x3c,
         0x64, 0x01, 0xf0, 0x85, 0x20, 0x00, 0x28, 0xf0, 0x90, 0x20, 0x00, 0x00, 0xa2, 0x3e, 0x64, 0x01, 0xff},
        OneRegionFbnk({{0x05000000, 0}}));

    XSFLIB_CHECK(NoteKeys(p) == (std::vector<int>{60, 62}));
}

void TestSequenceStartingPastItsEndPlaysNothing()
{
    // Start track 0 at 2^32 - 2, beyond the bytecode. The track then finishes immediately. Checking for alloctrack must
    // not wrap to the command at the start of the data.
    std::vector<ArchiveFile> files(1);
    files[0].data = Fseq(Code({0xfe, U16(0x0003), 0xff}));
    const SoundArchive archive =
        SoundArchive::Load(Fsar(files, {.sequences = {{0, 0xfffffffe}}}), console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance p = Perform(archive, 0, banks, {});

    XSFLIB_CHECK(p.end_tick == 0 && p.presets.empty() && p.warnings.empty());
}

void TestBankSlotPastTheFourthHasNoBank()
{
    // Select a bank slot and play key 60 for 48 ticks. All four valid slots contain the same bank. Higher slots are
    // silent. There nw::snd and nn::atk read past the end of the bank table.
    std::vector<ArchiveFile> files(4);
    files[0].data = Fseq({0xb6, 0x03, 0x3c, 0x64, 0x30, 0xff});
    files[1].data = Fseq({0xb6, 0x05, 0x3c, 0x64, 0x30, 0xff});
    files[2].data = OneRegionFbnk({{0x05000000, 0}});
    files[3].data = Fwar({Pcm16Fwav(100, std::vector<int16_t>(100, 1000))});
    const std::vector<uint32_t> kBanks(4, 0x03000000);
    const SoundArchive archive = SoundArchive::Load(
        Fsar(files, {.sequences = {{0, 0, kBanks}, {1, 0, kBanks}}, .banks = {2}, .wave_archives = {3}}),
        console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance fourth = Perform(archive, 0, banks, {});
    const Performance fifth = Perform(archive, 1, banks, {});

    XSFLIB_CHECK(NoteCount(fourth) == 1 && NoteCount(fifth) == 0 && fifth.presets.empty());
}

void TestOnlyAJumpBackToPlayedCommandsIsALoop()
{
    // First sequence: skip a FIN, wait one tick and jump back to the unvisited FIN. Second: play key 60 for a tick and
    // loop to the start until the performance reaches 10 s. Its 960th tick ends a pass at 10 s by the tempo, but on the
    // Wii U and the Switch ticks come a little early: at 9,999.99 ms. That pass is the last. On the 3DS no tick comes
    // early, and the performance ends at the first tick past 10 s.
    std::vector<ArchiveFile> files(4);
    files[0].data = Fseq(Code({0x89, U24(0x000005), 0xff, 0x80, 0x01, 0x89, U24(0x000004)}));
    files[1].data = Fseq({0x3c, 0x64, 0x01, 0x89, 0x00, 0x00, 0x00});
    files[2].data = OneRegionFbnk({{0x05000000, 0}});
    files[3].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive = SoundArchive::Load(
        Fsar(files, {.sequences = {{0, 0, {0x03000000}}, {1, 0, {0x03000000}}}, .banks = {2}, .wave_archives = {3}}),
        console->settings.archive);
    BankSet once_banks(archive, archive.Sounds()[0], console->settings);
    BankSet looping_banks(archive, archive.Sounds()[1], console->settings);

    const Performance once = Perform(archive, 0, once_banks, {});
    const Performance looping = Perform(archive, 1, looping_banks, {});

    XSFLIB_CHECK(!once.loop && once.loops == 0);
    XSFLIB_CHECK(looping.loop && looping.loop->start == 0 && looping.loop->end == 1);
    if (console->settings.early_ticks_ms > 0.0)
    {
        XSFLIB_CHECK(looping.seconds > 9.999 && looping.seconds < 10.0);
    }
    else
    {
        XSFLIB_CHECK(looping.seconds >= 10.0 && looping.seconds < 10.1);
    }
}

void TestMainLoopWaitsForAFadeTooLoudForMidi()
{
    // Track 0 starts a looping note at volume 255, fades to 128 over 2000 ticks, then waits forever. Track 1 loops
    // every 48 ticks. Track 2 plays at tick 1500 with volume and volume2 both 255. That sets the peak above MIDI's
    // limit. Perform must scale CC11 on a second pass and wait for track 0's fade before marking the main loop on both
    // passes, even when CC11 clips on the first.
    const Performance p = PerformOnBank(
        Code({0xfe, U16(0x0007), 0x88,        0x01, U24(0x000018), 0x88, 0x02, U24(0x00001e), 0xc1, 0xff, 0xa3,
              0xc1, 0x80,        U16(0x07d0), 0x3c, 0x7f,          0x00, 0xff, 0x80,          0x30, 0x89, U24(0x000018),
              0x80, 0x8b,        0x5c,        0xc1, 0xff,          0xd5, 0xff, 0x3c,          0x7f, 0x30, 0xff}),
        OneRegionFbnk({{0x05000000, 0}}));

    XSFLIB_CHECK(p.loop && p.loop->start > 2000 && p.loop->end - p.loop->start == 48);
    XSFLIB_CHECK(std::fabs(p.level_cut_db - 40.0 * std::log10(512.0 / 127.0)) < 0.001);
}

void TestMainLoopWaitsForALegatosFade()
{
    // The regions split at velocity 63. Track 0 ties key 60 at velocity 40, with volume2 32 and volume 128. At tick 1
    // it fades to 250 over 1220 ticks, plays key 60 legato at velocity 100, then waits forever. The MIDI note can start
    // no higher than 63. CC11 rises by 100 / 63 for the legato. That gives the legato finer steps than the underlying
    // level. Track 1 loops a 12-tick key 72 and 12-tick wait. Its main loop must start after track 0's fade.
    const Performance p =
        PerformOnBank(Code({0xfe, U16(0x0003), 0x88, 0x01, U24(0x00001a), 0xc8, 0x01, 0xd5,        0x20,         0xc1,
                            0x80, 0x3c,        0x28, 0x01, 0xa3,          0xc1, 0xfa, U16(0x04c4), 0x3c,         0x64,
                            0x00, 0xff,        0x48, 0x64, 0x0c,          0x80, 0x0c, 0x89,        U24(0x00001a)}),
                      VelocitySplitFbnk({{0x05000000, 0}}, {63, 127}, {64, 64}));

    XSFLIB_CHECK(p.loop && p.loop->start == 1224 && p.loop->end == 1248);
}

void TestLegatoKeepsTheRegionsVolume()
{
    // With volume2 64 and tie enabled, play key 60 at velocity 100 on a volume-64 region, then key 62 at 100 and key 64
    // at 127. nw::snd and nn::atk continue the channel with the new velocity and the region's volume. The MIDI note
    // starts at 127, the highest velocity the legatos reach. CC11 takes it down to 100. The note continues through both
    // legatos. Like the game, the zone applies the region's volume.
    const Performance p =
        PerformOnBank({0xd5, 0x40, 0xc8, 0x01, 0x3c, 0x64, 0x30, 0x3e, 0x64, 0x30, 0x40, 0x7f, 0x30, 0xff},
                      OneRegionFbnk({{0x05000000, 0}}, 64));

    const LegatoOutput out = ReadLegatoOutput(p);
    const std::vector<std::pair<uint32_t, int>> kExpression = {{0, 50}, {96, 64}};
    const std::vector<std::tuple<uint32_t, int, int>> kNotes = {{0, 60, 127}};
    XSFLIB_CHECK(out.expression == kExpression && out.notes == kNotes && p.level_cut_db == 0.0);
}

void TestLegatoOnAnotherRegionKeepsTheFirstsVolume()
{
    // The regions split at velocity 63, the lower at volume 64. With tie on and note wait off, play key 60 at velocity
    // 40, on the lower region. At tick 10, change the release and raise the note to 100. The new release requires
    // another preset. The legato therefore starts a new MIDI note, on the upper region's zone at volume 127. In nw::snd
    // and nn::atk, the channel keeps the lower region's volume. CC11 takes the note down by 64 / 127 in power.
    const Performance p = PerformOnBank(
        {0xc8, 0x01, 0xc7, 0x00, 0x3c, 0x28, 0x00, 0x80, 0x0a, 0xd3, 0x40, 0x3c, 0x64, 0x00, 0x80, 0x0a, 0xff},
        VelocitySplitFbnk({{0x05000000, 0}}, {63, 127}, {64, 64}, {64, 127}));

    const LegatoOutput out = ReadLegatoOutput(p);
    const std::vector<std::tuple<uint32_t, int, int>> kNotes = {{0, 60, 40}, {10, 60, 100}};
    const std::vector<std::pair<uint32_t, int>> kExpression = {{0, 127}, {10, 90}};
    XSFLIB_CHECK(out.notes == kNotes && out.expression == kExpression);
}

void TestRisingLegatoCarriesItsNoteOn()
{
    // With tie on and note wait off, play key 60 at velocity 40, then key 62 at 100 at tick 10 and key 60 at 70 at tick
    // 20, and finish at tick 30. nw::snd and nn::atk play one channel throughout. Its sample runs on, and so does the
    // MIDI note. The note starts at velocity 100, the highest the legatos reach. Until then, CC11 takes it down to 40.
    const Performance p = PerformOnBank({0xc8, 0x01, 0xc7, 0x00, 0x3c, 0x28, 0x00, 0x80, 0x0a, 0x3e,
                                         0x64, 0x00, 0x80, 0x0a, 0x3c, 0x46, 0x00, 0x80, 0x0a, 0xff},
                                        OneRegionFbnk({{0x05000000, 0}}));

    const LegatoOutput out = ReadLegatoOutput(p);
    const std::vector<std::tuple<uint32_t, int, int>> kNotes = {{0, 60, 100}};
    const std::vector<std::pair<uint32_t, int>> kExpression = {{0, 51}, {10, 127}, {20, 89}};
    XSFLIB_CHECK(out.notes == kNotes && out.expression == kExpression);
    XSFLIB_CHECK(p.level_cut_db == 0.0 && !p.approximations.contains("legato (tie or mono) played as a new note"));
}

void TestRisingLegatoBesideATailTakesAnotherPart()
{
    // With note wait off and release 20, play key 60 for 5 ticks, then turn tie on. That detaches 60 while its release
    // sounds. Play key 62 at velocity 40 and raise it to 100 at tick 10. The track goes on to another part. There 62
    // starts at 100 with CC11 taking it down to 40, and 60's release keeps the first part's CC11.
    const Performance p = PerformOnBank({0xc7, 0x00, 0xd3, 0x14, 0x3c, 0x64, 0x05, 0x80, 0x05, 0xc8, 0x01,
                                         0x3e, 0x28, 0x00, 0x80, 0x05, 0x3e, 0x64, 0x00, 0x80, 0x05, 0xff},
                                        OneRegionFbnk({{0x05000000, 0}}));

    const std::vector<std::tuple<uint32_t, int, int>> kNotes = {{0, 60, 100}, {5, 62, 100}};
    const std::vector<int> kParts = {0, 1};
    const std::vector<std::pair<uint32_t, int>> kFirstPart = {{0, 127}};
    const std::vector<std::pair<uint32_t, int>> kSecondPart = {{5, 51}, {10, 127}};
    XSFLIB_CHECK(ReadLegatoOutput(p).notes == kNotes && NoteParts(p) == kParts);
    XSFLIB_CHECK(PartControls(p, EventKind::kExpression, 0) == kFirstPart &&
                 PartControls(p, EventKind::kExpression, 1) == kSecondPart);
    XSFLIB_CHECK(!p.approximations.contains("legato (tie or mono) played as a new note"));
}

void TestDetachedTailKeepsItsSettings()
{
    // With note wait off and release 64, play key 60 for 96 ticks and turn tie on at tick 1. That detaches the note
    // while its release sounds. Set the volume to 64 at tick 2. In the game the release keeps its volume. The track
    // therefore goes on to another part, and that part gets the new volume. The first part keeps 127.
    const Performance p = PerformOnBank(
        {0xc7, 0x00, 0xd3, 0x40, 0x3c, 0x64, 0x60, 0x80, 0x01, 0xc8, 0x01, 0x80, 0x01, 0xc1, 0x40, 0x80, 0x30, 0xff},
        OneRegionFbnk({{0x05000000, 0}}));

    const std::vector<std::pair<uint32_t, int>> kFirstPart = {{0, 127}};
    const std::vector<std::pair<uint32_t, int>> kSecondPart = {{1, 127}, {2, 64}};
    XSFLIB_CHECK(PartControls(p, EventKind::kVolume, 0) == kFirstPart &&
                 PartControls(p, EventKind::kVolume, 1) == kSecondPart);
    XSFLIB_CHECK(!p.approximations.contains("detached notes (still receive track controller changes in MIDI)"));
}

void TestSweepLeavesTheOlderNoteItsPitch()
{
    // With note wait off and release 20, play key 60 for 5 ticks, then key 64 for 10 with a sweep from 4 semitones up
    // (sweep pitch 256, in 64ths). 60's release still sounds. 64 therefore goes on another part, and its sweep bends
    // that part alone.
    const Performance p = PerformOnBank(Code({0xc7, 0x00, 0xd3, 0x14, 0x3c, 0x64, 0x05, 0x80, 0x05, 0xe3, U16(0x0100),
                                              0x40, 0x64, 0x0a, 0x80, 0x0a, 0xff}),
                                        OneRegionFbnk({{0x05000000, 0}}));

    bool first_part_bends = false;
    float second_part_start = 0.0f;
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kPitch && e.part == 0)
        {
            first_part_bends = true;
        }
        if (e.kind == EventKind::kPitch && e.part == 1 && e.tick == 5)
        {
            second_part_start = e.semitones;
        }
    }

    const std::vector<int> kParts = {0, 1};
    XSFLIB_CHECK(NoteParts(p) == kParts && !first_part_bends && second_part_start == 4.0f);
}

void TestPartLeftTakesTheBendInItsTick()
{
    // With note wait off and release 20, play key 60 for 5 ticks, then bend the track a semitone up and play key 64
    // with a sweep from 4 semitones up. 60's release still sounds. 64 therefore goes on another part. The track still
    // holds 60, and that note's channel takes the bend in the same frame as 64's channel. So the first part bends at
    // tick 5 too, without the sweep.
    const Performance p = PerformOnBank(Code({0xc7, 0x00, 0xd3, 0x14, 0x3c, 0x64, 0x05, 0x80, 0x05, 0xc4, 0x40, 0xe3,
                                              U16(0x0100), 0x40, 0x64, 0x0a, 0x80, 0x0a, 0xff}),
                                        OneRegionFbnk({{0x05000000, 0}}));

    std::vector<std::pair<uint32_t, float>> first_part;
    float second_part_start = 0.0f;
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kPitch && e.part == 0)
        {
            first_part.emplace_back(e.tick, e.semitones);
        }
        if (e.kind == EventKind::kPitch && e.part == 1 && e.tick == 5)
        {
            second_part_start = e.semitones;
        }
    }

    const std::vector<std::pair<uint32_t, float>> kFirstPart = {{5, 1.0f}};
    const std::vector<int> kParts = {0, 1};
    XSFLIB_CHECK(NoteParts(p) == kParts && first_part == kFirstPart && second_part_start == 5.0f);
}

void TestBiquadSwitchedLaterInTheFrameReachesItsNotes()
{
    // At timebase 96 and tempo 999 a tick lasts 0.63 ms. Ticks 0 and 1 then share a sound frame. A frame lasts 3 ms on
    // the Wii U, 5 ms on the Switch and 4.9 ms on the 3DS. With note wait off, play key 60 at tick 0 and switch the
    // biquad on (low-pass, 64) at tick 1. Or start with it on and switch it off at tick 1. The game gives the note the
    // biquad that the frame ends with. The SFZ file selects the note's region at note-on. That value must come before
    // the note. Switched on 10 ticks later, in another frame, the biquad doesn't reach the note.
    auto biquad_at_note_on = [](const std::vector<uint8_t>& switching)
    {
        std::vector<uint8_t> commands = Code({0xb0, 0x60, 0xe1, U16(0x03e7), 0xc7, 0x00});
        commands.insert(commands.end(), switching.begin(), switching.end());
        commands.insert(commands.end(), {0x80, 0x60, 0xff});
        const Performance p = PerformOnBank(commands, OneRegionFbnk({{0x05000000, 0}}));

        int value = 0;
        for (const TrackEvent& e : p.tracks[0])
        {
            if (e.kind == EventKind::kNoteOn)
            {
                return value;
            }

            if (e.kind == kSfzBiquadEvent)
            {
                value = e.value;
            }
        }

        return -1;
    };

    const int on = biquad_at_note_on({0x3c, 0x64, 0x30, 0x80, 0x01, 0xb4, 0x01, 0xb5, 0x40});
    const int off = biquad_at_note_on({0xb4, 0x01, 0xb5, 0x40, 0x3c, 0x64, 0x30, 0x80, 0x01, 0xb5, 0x00});
    const int later = biquad_at_note_on({0x3c, 0x64, 0x30, 0x80, 0x0a, 0xb4, 0x01, 0xb5, 0x40});

    XSFLIB_CHECK(on == SfzBiquadValue(1, 64) && off == 0 && later == 0);
}

void TestLegatoAfterAnEnvelopeResetCarriesOn()
{
    // With tie on and attack 100, play key 60, reset the envelope settings and tie key 62 on. The reset leaves the
    // channel's attack as it was. The MIDI note therefore continues.
    const Performance p = PerformOnBank({0xc8, 0x01, 0xd0, 0x64, 0x3c, 0x64, 0x18, 0xfb, 0x3e, 0x64, 0x18, 0xff},
                                        OneRegionFbnk({{0x05000000, 0}}));

    const auto [note_ons, restarted] = NoteOnsAndRestarts(p);
    XSFLIB_CHECK(note_ons == 1 && !restarted);
}

void TestLegatoOnADecayedNoteStaysSilent()
{
    // With tie on, decay 127 and sustain 0, play key 60 on a region that ignores note-off, and tie key 62 on 480 ticks
    // (5 s) later. The bank's default hold, 127, keeps the note at full level for 4.1 s, and then it decays at once. A
    // legato doesn't start the envelope again. No MIDI note starts.
    const Performance p =
        PerformOnBank({0xc8, 0x01, 0xd1, 0x7f, 0xd2, 0x00, 0x3c, 0x64, 0x83, 0x60, 0x3e, 0x64, 0x30, 0xff},
                      OneRegionFbnk({{0x05000000, 0}}, 127, 64, true));

    const auto [note_ons, restarted] = NoteOnsAndRestarts(p);
    XSFLIB_CHECK(note_ons == 1 && !restarted);
}

void TestMidiNoteOfAnEndlessNoteEndsBelowHearing()
{
    // With sustain 0, decay 100 and hold 0, play key 60 with note wait off and no length. Then wait 192 ticks (2 s) and
    // end. With no length, only the track's end releases the note. The MIDI note and the performance end where the
    // channel has faded to kFadedAttenuation. With sustain 5, the level stays above that, and the MIDI note and the
    // performance end below hearing (kInaudibleSustain). The same note with a length of 96 ticks keeps its MIDI note
    // until that length runs out.
    const std::vector<uint8_t> bank = OneRegionFbnk({{0x05000000, 0}});
    const Performance faded =
        PerformOnBank({0xd2, 0x00, 0xd1, 0x64, 0xb1, 0x00, 0xc7, 0x00, 0x3c, 0x64, 0x00, 0x80, 0x81, 0x40, 0xff}, bank);
    const Performance shallow =
        PerformOnBank({0xd2, 0x05, 0xd1, 0x64, 0xb1, 0x00, 0xc7, 0x00, 0x3c, 0x64, 0x00, 0x80, 0x81, 0x40, 0xff}, bank);
    const Performance timed = PerformOnBank({0xd2, 0x00, 0xd1, 0x64, 0xb1, 0x00, 0x3c, 0x64, 0x60, 0xff}, bank);

    auto at_ms = [](int attenuation, int sustain)
    {
        const EnvelopeValues v{127, 100, sustain, 0, 127};
        return static_cast<double>(FramesToLevel(v, -static_cast<float>(attenuation)) + 1) * console->settings.frame_ms;
    };
    auto off_tick = [](const Performance& p)
    {
        const auto off = std::ranges::find(p.tracks[0], EventKind::kNoteOff, &TrackEvent::kind);
        return off == p.tracks[0].end() ? 0u : off->tick;
    };
    auto ends_at = [&](const Performance& p, double ms)
    {
        const double off_ms = off_tick(p) * kTickMs;
        return NoteCount(p) == 1 && off_ms >= ms && off_ms < ms + kTickMs + console->settings.frame_ms &&
               p.seconds * 1000.0 >= ms && p.seconds * 1000.0 < ms + kTickMs + console->settings.frame_ms;
    };
    const double faded_ms = at_ms(kFadedAttenuation, 0), quiet_ms = at_ms(kInaudibleSustain, 5);
    XSFLIB_CHECK(quiet_ms < faded_ms && faded_ms < 1900.0);
    XSFLIB_CHECK(ends_at(faded, faded_ms));
    XSFLIB_CHECK(ends_at(shallow, quiet_ms));
    XSFLIB_CHECK(off_tick(timed) == 96);
}

void TestSilentLegatoIsNotReported()
{
    // With tie on, play key 60 at velocity 0, then tie key 62 on at velocity 0. Neither sounds, and the legato isn't
    // reported as a new note.
    const Performance p =
        PerformOnBank({0xc8, 0x01, 0x3c, 0x00, 0x18, 0x3e, 0x00, 0x18, 0xff}, OneRegionFbnk({{0x05000000, 0}}));

    const auto [note_ons, restarted] = NoteOnsAndRestarts(p);
    XSFLIB_CHECK(note_ons == 0 && !restarted);
}

void TestSilentNoteLeavesTheTailItsPitch()
{
    // With note wait off and release 64, play key 60 for 48 ticks and turn tie on a tick later. That detaches the note
    // while its release sounds, and the track goes on to another part. Then play key 60 at velocity 0 with a sweep down
    // from 18 semitones up. The game plays that note silently, and the tail keeps its pitch. No part's pitch moves.
    const Performance p = PerformOnBank(Code({0xc7, 0x00, 0xd3, 0x40, 0x3c, 0x64, 0x30, 0x80, 0x01, 0xc8, 0x01, 0xe3,
                                              U16(0x0480), 0x3c, 0x00, 0x30, 0x80, 0x30, 0xff}),
                                        OneRegionFbnk({{0x05000000, 0}}));

    const auto moved = [](const TrackEvent& e)
    {
        return e.kind == EventKind::kPitch && e.semitones != 0.0f;
    };
    XSFLIB_CHECK(NoteCount(p) == 1 && std::ranges::none_of(p.tracks[0], moved));
}

void TestSweepBelowHearingLeavesTheNewerNoteItsPitch()
{
    // With note wait off, release 96 and portamento from key 48 at time 64, key 60 plays for one tick. Portamento then
    // goes off. Key 64 plays at tick 144 (1.5 s). The sweep of key 60 starts 12 semitones down and lasts 7.68 s
    // (SequenceTrack::NoteOn, 0x022C9A4C). The game gives key 64 no sweep. The release of key 60 falls 40 dB in about
    // 1 s. The channel of key 60 ends about 2.3 s in (CalcRelease, 0x022B5F34). At 1.5 s, that channel still sweeps,
    // about 60 dB down (GetSweepValue, 0x022C4204). The channel of key 64 plays at its key. The part of key 64 must
    // therefore stay at pitch 0 while key 64 sounds.
    const Performance p = PerformOnBank(Code({0xc7, 0x00, 0xd3, 0x60, 0xc9, 0x30, 0xcf, 0x40, 0x3c, 0x64, 0x01,
                                              0xce, 0x00, 0x80, 0x81, 0x10, 0x40, 0x64, 0x30, 0x80, 0x30, 0xff}),
                                        OneRegionFbnk({{0x05000000, 0}}));

    // The pitch of the part of key 64 at its note-on, and each pitch the part takes up to its note-off.
    std::array<float, 16> pitches{};
    std::vector<float> while_sounding;
    uint32_t note = 0;
    uint8_t part = 0;
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kPitch)
        {
            pitches[e.part] = e.semitones;
            if (note != 0 && e.part == part)
            {
                while_sounding.push_back(e.semitones);
            }
        }
        else if (e.kind == EventKind::kNoteOn && e.key == 64)
        {
            note = e.note;
            part = e.part;
            while_sounding.push_back(pitches[part]);
        }
        else if (e.kind == EventKind::kNoteOff && note != 0 && e.note == note)
        {
            break;
        }
    }

    const auto at_key = [](float semitones)
    {
        return semitones == 0.0f;
    };
    XSFLIB_CHECK(NoteCount(p) == 2 && !while_sounding.empty() && std::ranges::all_of(while_sounding, at_key));
}

void TestTieGivesTheTailTheTicksVolume()
{
    // With note wait off and release 20, hold key 60, and 5 ticks later set the track's volume to 64 and turn tie on.
    // Tie releases the note and detaches it, but the game first gives its channel the track's settings, the volume
    // among them. So the note's part takes CC7 64 at tick 5, before the track moves to another part.
    const Performance p =
        PerformOnBank({0xc7, 0x00, 0xd3, 0x14, 0x3c, 0x64, 0x00, 0x80, 0x05, 0xc1, 0x40, 0xc8, 0x01, 0x80, 0x30, 0xff},
                      OneRegionFbnk({{0x05000000, 0}}));

    std::vector<std::pair<uint32_t, int>> first_part;
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kVolume && e.part == 0)
        {
            first_part.emplace_back(e.tick, e.value);
        }
    }

    const std::vector<std::pair<uint32_t, int>> kFirstPart = {{0, 127}, {5, 64}};
    XSFLIB_CHECK(first_part == kFirstPart);
}

void TestNoteAboveVelocity127TakesItsLevel()
{
    // Velocity range 255 takes velocity 100 to 200. A region whose range goes up to 255 plays that velocity. The MIDI
    // note plays at 127, and CC11 the rest of the way. That takes the sequence down by 40 log10(200 / 127) dB.
    const Performance p =
        PerformOnBank({0xb3, 0xff, 0x3c, 0x64, 0x30, 0xff}, VelocitySplitFbnk({{0x05000000, 0}}, {255}, {64}));

    const std::vector<std::tuple<uint32_t, int, int>> kNotes = {{0, 60, 127}};
    XSFLIB_CHECK(ReadLegatoOutput(p).notes == kNotes &&
                 std::fabs(p.level_cut_db - 40.0 * std::log10(200.0 / 127.0)) < 1e-9);
    XSFLIB_CHECK(!p.approximations.contains("velocity above 127 (played at 127)"));
}

void TestMuteStopLeavesDetachedTails()
{
    // With note wait off and release 64, play key 60 for 96 ticks and turn tie on at tick 1. That detaches the note
    // while its release sounds. Then play key 62. Mute mode 3 at tick 2 stops 62 at once, but not 60's release: the
    // track detached 60. CC120 goes to 62's part alone.
    const Performance p = PerformOnBank({0xc7, 0x00, 0xd3, 0x40, 0x3c, 0x64, 0x60, 0x80, 0x01, 0xc8, 0x01,
                                         0x3e, 0x64, 0x00, 0x80, 0x01, 0xdd, 0x03, 0x80, 0x30, 0xff},
                                        OneRegionFbnk({{0x05000000, 0}}));

    std::vector<std::pair<uint32_t, int>> sound_offs; // tick, part
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kSoundOff)
        {
            sound_offs.emplace_back(e.tick, e.part);
        }
    }

    const std::vector<int> kParts = {0, 1};
    const std::vector<std::pair<uint32_t, int>> kSoundOffs = {{2, 1}};
    XSFLIB_CHECK(NoteParts(p) == kParts && sound_offs == kSoundOffs);
}

void TestRisingLegatoKeepsToItsRegion()
{
    // The regions split at velocity 63. With note wait off, tie key 60 at velocity 40, and raise it to 100 at tick 10.
    // The MIDI note can start no higher than 63. Above that, the synth would select the other region's zone. CC11
    // therefore has to rise above 127 for the legato, and the whole sequence comes down by 40 log10(100 / 63) dB.
    const Performance p =
        PerformOnBank({0xc8, 0x01, 0xc7, 0x00, 0x3c, 0x28, 0x00, 0x80, 0x0a, 0x3c, 0x64, 0x00, 0x80, 0x0a, 0xff},
                      VelocitySplitFbnk({{0x05000000, 0}}, {63, 127}, {64, 32}));

    const LegatoOutput out = ReadLegatoOutput(p);
    const double kCutDb = 40.0 * std::log10(100.0 / 63.0);
    const std::vector<std::tuple<uint32_t, int, int>> kNotes = {{0, 60, 63}};
    const std::vector<std::pair<uint32_t, int>> kExpression = {{0, 51}, {10, 127}};
    XSFLIB_CHECK(out.notes == kNotes && out.expression == kExpression);
    XSFLIB_CHECK(std::fabs(p.level_cut_db - kCutDb) < 1e-9);
}

void TestLevelCutFitsTheRunThatsKept()
{
    // The regions split at velocity 63, the lower at volume 32. Tie key 60 at velocity 127, on the upper region. At
    // tick 24, change the filter and play key 62 at velocity 40. The first pass starts a new preset and note on the
    // lower region's zone. CC11 goes to 253 to make up the upper region's volume (127 / 32 in power). In nw::snd and
    // nn::atk, the channel keeps that volume. With a filter controller on the later passes, the MIDI note continues at
    // 40 / 127 of its velocity in the upper region's zone. CC11 stays within 127, and the sequence keeps its level.
    const Performance p = PerformOnBank({0xc8, 0x01, 0x3c, 0x7f, 0x18, 0xd8, 0x20, 0x3e, 0x28, 0x18, 0x80, 0x30, 0xff},
                                        VelocitySplitFbnk({{0x05000000, 0}}, {63, 127}, {64, 64}, {32, 127}));

    const std::vector<std::pair<uint32_t, int>> kExpression = {{0, 127}, {24, 40}};
    XSFLIB_CHECK(ReadLegatoOutput(p).expression == kExpression && p.level_cut_db == 0.0);
}

void TestTiedNoteTakesPartInTheMainLoop()
{
    // Track 0 opens track 1, ties key 72 for 30 ticks, then indefinitely, and waits for it. Track 1 loops a 26-tick key
    // 60. The tie at tick 30 emits no MIDI change but still contributes to track 1's pass starting at tick 26. Mark the
    // following pass as the main loop.
    const Performance p = PerformOnBank(Code({0xfe, U16(0x0003), 0x88, 0x01, U24(0x000010), 0xc8, 0x01, 0x48, 0x78,
                                              0x1e, 0x48, 0x78, 0x00, 0x3c, 0x64, 0x1a, 0x89, U24(0x000010)}),
                                        OneRegionFbnk({{0x05000000, 0}}));

    XSFLIB_CHECK(p.loop && p.loop->start == 52 && p.loop->end == 78);
}

void TestSilentLegatoCarriesItsNoteOn()
{
    // With mono enabled and notewait off, play key 60 for 30 ticks. Change release at tick 10. That makes a different
    // preset. Then play velocity 0 at tick 20 and finish at tick 50. Keep the MIDI note alive but silent through CC11;
    // note-off would leave a release tail. Do not create the unused preset.
    const Performance p = PerformOnBank({0xb2, 0x01, 0xc7, 0x00, 0x3c, 0x64, 0x1e, 0x80, 0x0a, 0xd3, 0x40, 0x80, 0x0a,
                                         0x3c, 0x00, 0x3c, 0x80, 0x1e, 0xff},
                                        OneRegionFbnk({{0x05000000, 0}}));

    std::vector<std::tuple<uint32_t, EventKind, int>> events; // tick, kind and CC11 value
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kNoteOn || e.kind == EventKind::kNoteOff || e.kind == EventKind::kExpression)
        {
            events.emplace_back(e.tick, e.kind, e.kind == EventKind::kExpression ? e.value : 0);
        }
    }

    const std::vector<std::tuple<uint32_t, EventKind, int>> kEvents = {{0, EventKind::kExpression, 127},
                                                                       {0, EventKind::kNoteOn, 0},
                                                                       {20, EventKind::kExpression, 0},
                                                                       {50, EventKind::kNoteOff, 0}};
    XSFLIB_CHECK(events == kEvents && p.presets.size() == 1);
}

void TestLegatosChangeOfKeyIsNoChangeOfTheTracks()
{
    // Track 0 opens track 1, sets tempo 1023, enables tie/portamento (time 100), and holds key 60. At tick 1, change
    // the low-pass during the note and play key 62 legato. The first pass creates a new MIDI note; the second uses a
    // filter controller and a 2-semitone bend on the existing note. Track 1 loops every 6 ticks. Sweep completion
    // counts as a track-0 change on both passes, even though only the first MIDI bend returns to 0. Both passes must
    // select the same main loop.
    const Performance p = PerformOnBank(
        Code({0xfe, U16(0x0003), 0x88, 0x01, U24(0x000020), 0xe1, U16(0x03ff), 0xc8, 0x01, 0xce,         0x01,
              0xcf, 0x64,        0xc7, 0x00, 0x3c,          0x64, 0x00,        0x80, 0x01, 0xd8,         0x10,
              0xc7, 0x01,        0x3e, 0x64, 0x00,          0xff, 0x80,        0x06, 0x89, U24(0x000020)}),
        OneRegionFbnk({{0x05000000, 0}}));

    // The passes select the loop from tick 2562 to 2568, or from 2502 to 2508 on the 3DS.
    const uint32_t start = console->settings.engine == Engine::kCtr ? 2502 : 2562;
    XSFLIB_CHECK(p.loop && p.loop->start == start && p.loop->end == start + 6);
}

void TestMonoLegatoTakesItsLengthWithTieOn()
{
    // With mono and tie enabled, play key 60 for 48 ticks, then key 62 for 24 ticks and wait. Mono updates the
    // channel's length even with tie enabled. The note therefore ends at tick 72.
    const Performance p = PerformOnBank({0xb2, 0x01, 0xc8, 0x01, 0x3c, 0x64, 0x30, 0x3e, 0x64, 0x18, 0x80, 0x60, 0xff},
                                        OneRegionFbnk({{0x05000000, 0}}));

    std::vector<std::pair<uint32_t, EventKind>> notes;
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kNoteOn || e.kind == EventKind::kNoteOff)
        {
            notes.emplace_back(e.tick, e.kind);
        }
    }

    const std::vector<std::pair<uint32_t, EventKind>> kNotes = {{0, EventKind::kNoteOn}, {72, EventKind::kNoteOff}};
    XSFLIB_CHECK(notes == kNotes);
}

void TestNoteReleasedBeforeItsFirstFrameIsSilent()
{
    // With notewait off, play key 60 and enable mono to release it. An immediate release precedes the first envelope
    // update and stays silent; a release one tick later is audible.
    std::vector<ArchiveFile> files(4);
    files[0].data = Fseq({0xc7, 0x00, 0x3c, 0x64, 0x30, 0xb2, 0x01, 0x80, 0x30, 0xff});
    files[1].data = Fseq({0xc7, 0x00, 0x3c, 0x64, 0x30, 0x80, 0x01, 0xb2, 0x01, 0x80, 0x30, 0xff});
    files[2].data = OneRegionFbnk({{0x05000000, 0}});
    files[3].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const std::vector<uint32_t> kBanks = {0x03000000};
    const SoundArchive archive = SoundArchive::Load(
        Fsar(files, {.sequences = {{0, 0, kBanks}, {1, 0, kBanks}}, .banks = {2}, .wave_archives = {3}}),
        console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance at_once = Perform(archive, 0, banks, {});
    const Performance later = Perform(archive, 1, banks, {});

    std::vector<std::pair<uint32_t, EventKind>> notes;
    for (const TrackEvent& e : later.tracks[0])
    {
        if (e.kind == EventKind::kNoteOn || e.kind == EventKind::kNoteOff)
        {
            notes.emplace_back(e.tick, e.kind);
        }
    }

    const std::vector<std::pair<uint32_t, EventKind>> kNotes = {{0, EventKind::kNoteOn}, {1, EventKind::kNoteOff}};
    XSFLIB_CHECK(NoteCount(at_once) == 0 && notes == kNotes);
}

void TestWhatMidiCantSeparateIsReported()
{
    // First sequence: all 16 tracks release a slow-release key 60 by enabling tie one tick later, then change volume
    // during its detached release tail. Every channel goes to a track. None is left for another part, and MIDI can't
    // keep the volume from the releases. Second: combine left track pan with right region pan.
    std::vector<uint8_t> all_tracks;
    for (uint8_t t = 1; t < 16; t++)
    {
        Append(all_tracks, {0x88, t, U24(15 * 5)});
    }
    all_tracks.insert(all_tracks.end(), {0xc7, 0x00, 0xd3, 0x40, 0x3c, 0x64, 0x60, 0x80, 0x01, 0xc8, 0x01, 0x80, 0x01,
                                         0xc1, 0x40, 0x80, 0x30, 0xff});
    std::vector<ArchiveFile> files(4);
    files[0].data = Fseq(all_tracks);
    files[1].data = Fseq({0xc0, 0x20, 0x3c, 0x64, 0x30, 0xff});
    files[2].data = OneRegionFbnk({{0x05000000, 0}}, 127, 100);
    files[3].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const std::vector<uint32_t> kBanks = {0x03000000};
    const SoundArchive archive = SoundArchive::Load(
        Fsar(files, {.sequences = {{0, 0, kBanks, 0, 0, 0xffff}, {1, 0, kBanks}}, .banks = {2}, .wave_archives = {3}}),
        console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance detached = Perform(archive, 0, banks, {});
    const Performance panned = Perform(archive, 1, banks, {});

    const std::string kDetached = "detached notes (still receive track controller changes in MIDI)";
    const std::string kPans = "combined region and track pan (using the synth pan law)";
    XSFLIB_CHECK(detached.approximations.contains(kDetached) && !detached.approximations.contains(kPans));
    XSFLIB_CHECK(panned.approximations.contains(kPans) && !panned.approximations.contains(kDetached));
}

void TestOneShotWaveEndsAsAxPlaysIt()
{
    // Play a 6399-sample one-shot at four times AX's rate (384 samples a frame), then wait for it to finish. AX plays
    // the voice from frame 2 and reads its last sample in the 17th frame, frame 18. nw::snd reads that state in frame
    // 19 (57 ms) and frees the channel before the frame's ticks. Execution resumes at tick 6 (62.5 ms), the first tick
    // after that.
    std::vector<ArchiveFile> files(3);
    files[0].data = Fseq({0x3c, 0x7f, 0x00, 0xff});
    files[1].data = OneRegionFbnk({{0x05000000, 0}});
    files[2].data = Fwar({Pcm16Fwav(6399, std::vector<int16_t>(6399, 1000), 1, false, 4 * 32000)});
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

    XSFLIB_CHECK(note_offs == std::vector<uint32_t>{6} && p.end_tick == 6);
}

void TestTicksFallInTheGamesSoundFrames()
{
    // Wait 39 ticks at tempo 65. Scheduling in whole 1/65536 ms makes each tick a little short. Tick 39 then falls at
    // the end of the frame before 750 ms: frame 249 of 3 ms, or frame 149 of 5 ms. Exact arithmetic would put it at the
    // start of the next frame (750 ms). On the 3DS, wait 755 ticks at tempo 229 instead. Single-precision scheduling in
    // thousandths of ARM cycles puts tick 755 at the end of frame 842. Exact arithmetic puts it at the start of frame
    // 843. MIDI events must use the fractional tick position of the actual frame start.
    const bool ctr = console->settings.engine == Engine::kCtr;
    const int tempo = ctr ? 229 : 65;
    const std::size_t ticks = ctr ? 755 : 39;
    std::vector<ArchiveFile> files(1);
    files[0].data =
        Fseq(ctr ? Code({0xe1, U16(229), 0x80, 0x85, 0x73, 0xff}) : Code({0xe1, U16(65), 0x80, 0x27, 0xff}));
    const SoundArchive archive = SoundArchive::Load(Fsar(files, {.sequences = {{0, 0}}}), console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance p = Perform(archive, 0, banks, {});

    const double frame = ctr ? 842.0 : std::round(750.0 / console->settings.frame_ms) - 1.0;
    const double frame_start = frame * console->settings.frame_ms / (60000.0 / (48 * tempo));
    XSFLIB_CHECK(p.frame_ticks.size() == ticks + 1 && std::fabs(p.frame_ticks[ticks] - frame_start) < 0.01);
}

void TestOddArgumentsAreReadAsTheGameReadsThem()
{
    // Test a volume ramp over 0x8000 ticks (negative in the engine's 16-bit counter: it applies at once), a 128-tick
    // wait encoded in seven bytes (five would leave bytes interpreted as a program change), and timebase 0 stopping
    // sequence timing.
    std::vector<ArchiveFile> files(5);
    files[0].data = Fseq(Code({0xa3, 0xc1, 0x40, U16(0x8000), 0x3c, 0x64, 0x30, 0xff}));
    files[1].data = Fseq({0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x81, 0x00, 0x3c, 0x64, 0x30, 0xff});
    files[2].data = Fseq({0xb0, 0x00, 0x80, 0x30, 0xff});
    files[3].data = OneRegionFbnk({{0x05000000, 0}});
    files[4].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const std::vector<uint32_t> kBanks = {0x03000000};
    const SoundArchive archive = SoundArchive::Load(
        Fsar(files,
             {.sequences = {{0, 0, kBanks}, {1, 0, kBanks}, {2, 0, kBanks}}, .banks = {3}, .wave_archives = {4}}),
        console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance ramp = Perform(archive, 0, banks, {});
    const Performance wait = Perform(archive, 1, banks, {});
    const Performance timebase = Perform(archive, 2, banks, {});

    std::vector<int> volumes;
    for (const TrackEvent& e : ramp.tracks[0])
    {
        if (e.kind == EventKind::kVolume)
        {
            volumes.push_back(e.value);
        }
    }

    auto note_on = [](const TrackEvent& e)
    {
        return e.kind == EventKind::kNoteOn;
    };
    const auto waited = std::ranges::find_if(wait.tracks[0], note_on);

    XSFLIB_CHECK(volumes == std::vector<int>{64});
    XSFLIB_CHECK(waited != wait.tracks[0].end() && waited->tick == 128);
    const std::vector<std::string> kStopped = {"timebase 0: the sequence stops advancing"};
    XSFLIB_CHECK(timebase.warnings == kStopped && timebase.end_tick == 0 && timebase.timebase == 48);
}

void TestSequenceThatStopsAdvancingPlaysOut()
{
    // The first three sequences disable notewait, play key 60, wait and set tempo 0. A 96-tick note then holds
    // indefinitely and is capped at 10 s using MIDI's 120 BPM. A one-tick note at release 90 has already been released
    // and finishes normally. The third sequence also ends on the stopping tick. The latter two match a fourth sequence
    // with the same notes and no tempo 0.
    std::vector<ArchiveFile> files(6);
    files[0].data = Fseq({0xc7, 0x00, 0x3c, 0x64, 0x60, 0x80, 0x01, 0xe1, 0x00, 0x00, 0x80, 0x30, 0xff});
    files[1].data = Fseq({0xc7, 0x00, 0xd3, 0x5a, 0x3c, 0x64, 0x01, 0x80, 0x02, 0xe1, 0x00, 0x00, 0x80, 0x30, 0xff});
    files[2].data = Fseq({0xc7, 0x00, 0xd3, 0x5a, 0x3c, 0x64, 0x01, 0x80, 0x02, 0xe1, 0x00, 0x00, 0xff});
    files[3].data = Fseq({0xc7, 0x00, 0xd3, 0x5a, 0x3c, 0x64, 0x01, 0x80, 0x02, 0xff});
    files[4].data = OneRegionFbnk({{0x05000000, 0}});
    files[5].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const std::vector<uint32_t> kBanks = {0x03000000};
    const std::vector<SequenceSound> kSounds = {{0, 0, kBanks}, {1, 0, kBanks}, {2, 0, kBanks}, {3, 0, kBanks}};
    const SoundArchive archive = SoundArchive::Load(
        Fsar(files, {.sequences = kSounds, .banks = {4}, .wave_archives = {5}}), console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance held = Perform(archive, 0, banks, {});
    const Performance released = Perform(archive, 1, banks, {});
    const Performance ending = Perform(archive, 2, banks, {});
    const Performance unstopped = Perform(archive, 3, banks, {});

    const std::vector<std::string> kStopped = {"tempo 0: the sequence stops advancing"};
    XSFLIB_CHECK(held.warnings == kStopped && held.holds && held.end_tick == 961 && held.tracks[0].back().tick == 961);
    XSFLIB_CHECK(released.warnings == kStopped && !released.holds && unstopped.end_tick > 200 &&
                 released.end_tick == unstopped.end_tick && ending.end_tick == unstopped.end_tick);
}

void TestTieLoopThatHoldsEndsAsAHeldSound()
{
    // The first sequence turns tie on, plays key 60 for 2400 ticks (25 s) and jumps back to the note. The note
    // continues: the sound holds forever. The performance therefore ends 10 s after the note starts, without loop
    // markers. The second sequence does the same without tie. Each pass then releases the note and strikes it again: a
    // 25 s loop.
    std::vector<ArchiveFile> files(4);
    files[0].data = Fseq(Code({0xc8, 0x01, 0x3c, 0x64, 0x92, 0x60, 0x89, U24(0x000002)}));
    files[1].data = Fseq({0x3c, 0x64, 0x92, 0x60, 0x89, 0x00, 0x00, 0x00});
    files[2].data = OneRegionFbnk({{0x05000000, 0}});
    files[3].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const std::vector<uint32_t> kBanks = {0x03000000};
    const SoundArchive archive = SoundArchive::Load(
        Fsar(files, {.sequences = {{0, 0, kBanks}, {1, 0, kBanks}}, .banks = {2}, .wave_archives = {3}}),
        console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance tied = Perform(archive, 0, banks, {});
    const Performance struck = Perform(archive, 1, banks, {});

    XSFLIB_CHECK(tied.holds && !tied.loop && std::fabs(tied.seconds - 10.0) < 0.02);
    XSFLIB_CHECK(!struck.holds && struck.loop && struck.loop->start == 0 && struck.loop->end == 2400);
}

void TestTieLoopOverAOneShotWaveStaysALoop()
{
    // The tied note of the test above, on a 30 s one-shot wave: it's still playing when the 25 s pass ends, but it ends
    // by itself. The pass therefore doesn't hold it forever.
    std::vector<ArchiveFile> files(3);
    files[0].data = Fseq(Code({0xc8, 0x01, 0x3c, 0x64, 0x92, 0x60, 0x89, U24(0x000002)}));
    files[1].data = OneRegionFbnk({{0x05000000, 0}});
    files[2].data = Fwar({Pcm16Fwav(30 * 32000, std::vector<int16_t>(30 * 32000, 1000))});
    const std::vector<uint32_t> kBanks = {0x03000000};
    const SoundArchive archive = SoundArchive::Load(
        Fsar(files, {.sequences = {{0, 0, kBanks}}, .banks = {1}, .wave_archives = {2}}), console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance p = Perform(archive, 0, banks, {});

    XSFLIB_CHECK(!p.holds && p.loop && p.loop->end == 2400);
}

// The tests that hold on the Wii U, the Switch and the 3DS.
const Test kWiiUSwitchAndCtrTests[] = {
    TestBrokenBankAndWaveReferencesAreReported,
    TestNotesKeepTheirCommands,
    TestSequenceStartingPastItsEndPlaysNothing,
    TestBankSlotPastTheFourthHasNoBank,
    TestOnlyAJumpBackToPlayedCommandsIsALoop,
    TestMainLoopWaitsForAFadeTooLoudForMidi,
    TestMainLoopWaitsForALegatosFade,
    TestRisingLegatoCarriesItsNoteOn,
    TestRisingLegatoBesideATailTakesAnotherPart,
    TestDetachedTailKeepsItsSettings,
    TestSweepLeavesTheOlderNoteItsPitch,
    TestPartLeftTakesTheBendInItsTick,
    TestBiquadSwitchedLaterInTheFrameReachesItsNotes,
    TestLegatoAfterAnEnvelopeResetCarriesOn,
    TestLegatoOnADecayedNoteStaysSilent,
    TestMidiNoteOfAnEndlessNoteEndsBelowHearing,
    TestSilentLegatoIsNotReported,
    TestSilentNoteLeavesTheTailItsPitch,
    TestTieGivesTheTailTheTicksVolume,
    TestNoteAboveVelocity127TakesItsLevel,
    TestMuteStopLeavesDetachedTails,
    TestRisingLegatoKeepsToItsRegion,
    TestLevelCutFitsTheRunThatsKept,
    TestTiedNoteTakesPartInTheMainLoop,
    TestSilentLegatoCarriesItsNoteOn,
    TestLegatosChangeOfKeyIsNoChangeOfTheTracks,
    TestMonoLegatoTakesItsLengthWithTieOn,
    TestNoteReleasedBeforeItsFirstFrameIsSilent,
    TestWhatMidiCantSeparateIsReported,
    TestTicksFallInTheGamesSoundFrames,
    TestOddArgumentsAreReadAsTheGameReadsThem,
    TestSequenceThatStopsAdvancingPlaysOut,
    TestTieLoopThatHoldsEndsAsAHeldSound,
    TestTieLoopOverAOneShotWaveStaysALoop,
};

// The tests that hold on the Wii U and the Switch.
const Test kWiiUAndSwitchTests[] = {
    TestCallsNestTenDeep,
    TestMixChangesAreRecorded,
    TestWiiUCommandsAreRead,
    TestLegatoKeepsTheRegionsVolume,
    TestLegatoOnAnotherRegionKeepsTheFirstsVolume,
    TestSweepBelowHearingLeavesTheNewerNoteItsPitch,
};

// The tests of the Wii U alone.
const Test kWiiUOnlyTests[] = {
    TestLittleEndianFilesAreRead,
    TestVariableShiftsAreThePowerPcs,
    TestOneShotWaveEndsAsAxPlaysIt,
};

// The tests of `lists`, in order.
std::vector<Test> Join(std::initializer_list<std::span<const Test>> lists)
{
    std::vector<Test> tests;
    for (const std::span<const Test> list : lists)
    {
        tests.insert(tests.end(), list.begin(), list.end());
    }

    return tests;
}

} // namespace

int main()
{
    RunOn(WiiU(), Join({kWiiUSwitchAndCtrTests, kWiiUAndSwitchTests, kWiiUOnlyTests}));
    RunOn(Switch(), Join({kWiiUSwitchAndCtrTests, kWiiUAndSwitchTests}));
    RunOn(Ctr(), kWiiUSwitchAndCtrTests);

    return xsflib::testing::Summary("sequence_export");
}
