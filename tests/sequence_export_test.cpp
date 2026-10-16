// SPDX-License-Identifier: MIT

// Tests of sequence_export on synthetic archives: the performer, the envelope, pitch and filter models, the banks, and
// the MIDI, SoundFont and SFZ conversion. Each test runs on every console whose engine it describes, with that
// console's settings (see sequence_export_test.h).

#include "sequence_export_test.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <map>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/nwsnd/envelope.h"
#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/reference.h"
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
#include "seqlib/file_name.h"
#include "seqlib/loop.h"
#include "seqlib/midi/encoder.h"
#include "seqlib/midi/performance_writer.h"
#include "seqlib/midi/reader.h"
#include "seqlib/read_error.h"
#include "seqlib/sf2/reader.h"
#include "seqlib/sf2/units.h"
#include "seqlib/sf2/writer.h"
#include "seqlib/wav.h"
#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

using namespace ntdlib::sequence_export;
using namespace ntdlib::sequence_export::tests;
using ntdlib::nwsnd::DecibelSquare;
using ntdlib::nwsnd::HoldMs;
using ntdlib::sar::Bank;
using ntdlib::sar::Instrument;
using ntdlib::sar::KeyRegion;
using ntdlib::sar::Sequence;
using ntdlib::sar::SoundInfo;
using ntdlib::sar::SoundType;
using ntdlib::sar::VelocityRegion;
using ntdlib::sar::WaveArchive;
using xsflib::FormatError;

namespace sf2 = seqlib::sf2;

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
    std::vector<ArchiveFile> files(3);
    files[0].data = Fseq(commands);
    files[1].data = OneRegionFbnk({{0x05000000, 0}});
    files[2].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive =
        SoundArchive::Load(Fsar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}),
                           console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance p = Perform(archive, 0, banks, {});

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

    // The calls, the returns and the loop's end begin the stretches of commands. The note commands lie in them.
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
    std::vector<ArchiveFile> files(3);
    files[0].data =
        Fseq({0xf0, 0x80, 0x20, 0x00, 0x01, 0xf0, 0x85, 0x20, 0x00, 0x40, 0xf0, 0x90, 0x20, 0x00, 0x01, 0xa2, 0x3c,
              0x64, 0x01, 0xf0, 0x85, 0x20, 0x00, 0x28, 0xf0, 0x90, 0x20, 0x00, 0x00, 0xa2, 0x3e, 0x64, 0x01, 0xff});
    files[1].data = OneRegionFbnk({{0x05000000, 0}});
    files[2].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive =
        SoundArchive::Load(Fsar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}),
                           console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance p = Perform(archive, 0, banks, {});

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
    std::vector<ArchiveFile> files(3);
    files[0].data = Fseq(
        Code({0xfe, U16(0x0007), 0x88,        0x01, U24(0x000018), 0x88, 0x02, U24(0x00001e), 0xc1, 0xff, 0xa3,
              0xc1, 0x80,        U16(0x07d0), 0x3c, 0x7f,          0x00, 0xff, 0x80,          0x30, 0x89, U24(0x000018),
              0x80, 0x8b,        0x5c,        0xc1, 0xff,          0xd5, 0xff, 0x3c,          0x7f, 0x30, 0xff}));
    files[1].data = OneRegionFbnk({{0x05000000, 0}});
    files[2].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive =
        SoundArchive::Load(Fsar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}),
                           console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance p = Perform(archive, 0, banks, {});

    XSFLIB_CHECK(p.loop && p.loop->start > 2000 && p.loop->end - p.loop->start == 48);
    XSFLIB_CHECK(std::fabs(p.level_cut_db - 40.0 * std::log10(512.0 / 127.0)) < 0.001);
}

void TestMainLoopWaitsForALegatosFade()
{
    // The regions split at velocity 63. Track 0 ties key 60 at velocity 40, with volume2 32 and volume 128. At tick 1
    // it fades to 250 over 1220 ticks, plays key 60 legato at velocity 100, then waits forever. The MIDI note can start
    // no higher than 63. CC11 rises by 100 / 63 for the legato. That gives the legato finer steps than the underlying
    // level. Track 1 loops a 12-tick key 72 and 12-tick wait. Its main loop must start after track 0's fade.
    std::vector<ArchiveFile> files(3);
    files[0].data = Fseq(Code({0xfe, U16(0x0003), 0x88, 0x01, U24(0x00001a), 0xc8, 0x01,         0xd5, 0x20, 0xc1, 0x80,
                               0x3c, 0x28,        0x01, 0xa3, 0xc1,          0xfa, U16(0x04c4),  0x3c, 0x64, 0x00, 0xff,
                               0x48, 0x64,        0x0c, 0x80, 0x0c,          0x89, U24(0x00001a)}));
    files[1].data = VelocitySplitFbnk({{0x05000000, 0}}, {63, 127}, {64, 64});
    files[2].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive =
        SoundArchive::Load(Fsar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}),
                           console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance p = Perform(archive, 0, banks, {});

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
    std::vector<ArchiveFile> files(3);
    files[0].data = Fseq(Code({0xfe, U16(0x0003), 0x88, 0x01, U24(0x000010), 0xc8, 0x01, 0x48, 0x78, 0x1e, 0x48, 0x78,
                               0x00, 0x3c, 0x64, 0x1a, 0x89, U24(0x000010)}));
    files[1].data = OneRegionFbnk({{0x05000000, 0}});
    files[2].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive =
        SoundArchive::Load(Fsar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}),
                           console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance p = Perform(archive, 0, banks, {});

    XSFLIB_CHECK(p.loop && p.loop->start == 52 && p.loop->end == 78);
}

void TestSilentLegatoCarriesItsNoteOn()
{
    // With mono enabled and notewait off, play key 60 for 30 ticks. Change release at tick 10. That makes a different
    // preset. Then play velocity 0 at tick 20 and finish at tick 50. Keep the MIDI note alive but silent through CC11;
    // note-off would leave a release tail. Do not create the unused preset.
    std::vector<ArchiveFile> files(3);
    files[0].data = Fseq({0xb2, 0x01, 0xc7, 0x00, 0x3c, 0x64, 0x1e, 0x80, 0x0a, 0xd3, 0x40, 0x80, 0x0a, 0x3c, 0x00,
                          0x3c, 0x80, 0x1e, 0xff});
    files[1].data = OneRegionFbnk({{0x05000000, 0}});
    files[2].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive =
        SoundArchive::Load(Fsar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}),
                           console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance p = Perform(archive, 0, banks, {});

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
    std::vector<ArchiveFile> files(3);
    files[0].data =
        Fseq(Code({0xfe, U16(0x0003), 0x88, 0x01, U24(0x000020), 0xe1, U16(0x03ff), 0xc8, 0x01, 0xce,         0x01,
                   0xcf, 0x64,        0xc7, 0x00, 0x3c,          0x64, 0x00,        0x80, 0x01, 0xd8,         0x10,
                   0xc7, 0x01,        0x3e, 0x64, 0x00,          0xff, 0x80,        0x06, 0x89, U24(0x000020)}));
    files[1].data = OneRegionFbnk({{0x05000000, 0}});
    files[2].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive =
        SoundArchive::Load(Fsar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}),
                           console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance p = Perform(archive, 0, banks, {});

    // The passes select the loop from tick 2562 to 2568, or from 2502 to 2508 on the 3DS.
    const uint32_t start = console->settings.engine == Engine::kCtr ? 2502 : 2562;
    XSFLIB_CHECK(p.loop && p.loop->start == start && p.loop->end == start + 6);
}

void TestMonoLegatoTakesItsLengthWithTieOn()
{
    // With mono and tie enabled, play key 60 for 48 ticks, then key 62 for 24 ticks and wait. Mono updates the
    // channel's length even with tie enabled. The note therefore ends at tick 72.
    std::vector<ArchiveFile> files(3);
    files[0].data = Fseq({0xb2, 0x01, 0xc8, 0x01, 0x3c, 0x64, 0x30, 0x3e, 0x64, 0x18, 0x80, 0x60, 0xff});
    files[1].data = OneRegionFbnk({{0x05000000, 0}});
    files[2].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive =
        SoundArchive::Load(Fsar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}),
                           console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance p = Perform(archive, 0, banks, {});

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

void TestClosingTickReachesTheReleaseTail()
{
    // Release 100 and key 60 for one tick, then volume 0 and FIN in the next. Like the end of a tick, closing the track
    // gives the releasing note the track's settings. The tail is therefore silent in the game and requires CC7 0 in
    // MIDI.
    std::vector<ArchiveFile> files(3);
    files[0].data = Fseq({0xd3, 0x64, 0x3c, 0x64, 0x01, 0xc1, 0x00, 0xff});
    files[1].data = OneRegionFbnk({{0x05000000, 0}});
    files[2].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const std::vector<uint32_t> kBanks = {0x03000000};
    const SoundArchive archive = SoundArchive::Load(
        Fsar(files, {.sequences = {{0, 0, kBanks}}, .banks = {1}, .wave_archives = {2}}), console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance p = Perform(archive, 0, banks, {});

    const auto silenced = [](const TrackEvent& e)
    {
        return e.kind == EventKind::kVolume && e.value == 0 && e.tick == 1;
    };
    XSFLIB_CHECK(std::ranges::any_of(p.tracks[0], silenced));
}

void TestSilenceAtTheTimeLimitEndsWithTheLastSound()
{
    // Key 60 plays for 48 ticks (0.5 s), then the track waits 100,000 ticks (17 minutes), past the 15-minute limit. The
    // track has been silent since the note. The performance therefore treats the sequence as finished and ends with the
    // note.
    std::vector<ArchiveFile> files(3);
    files[0].data = Fseq({0x3c, 0x64, 0x30, 0x80, 0x86, 0x8d, 0x20, 0xff});
    files[1].data = OneRegionFbnk({{0x05000000, 0}});
    files[2].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const std::vector<uint32_t> kBanks = {0x03000000};
    const SoundArchive archive = SoundArchive::Load(
        Fsar(files, {.sequences = {{0, 0, kBanks}}, .banks = {1}, .wave_archives = {2}}), console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance p = Perform(archive, 0, banks, {});

    XSFLIB_CHECK(!p.truncated && !p.holds && p.seconds < 1.0);
}

void TestOneShotWavePlaysOutAtItsPitch()
{
    // Bend key 60 up by 127/128 of a 12-semitone range. That shortens a 4 s one-shot to about 2 s. Compare waiting for
    // completion with disabling notewait and setting tempo 0 immediately. The wave must finish at the same time.
    std::vector<ArchiveFile> files(4);
    files[0].data = Fseq({0xc5, 0x0c, 0xc4, 0x7f, 0x3c, 0x64, 0x00, 0xff});
    files[1].data = Fseq({0xc7, 0x00, 0xc5, 0x0c, 0xc4, 0x7f, 0x3c, 0x64, 0x00, 0xe1, 0x00, 0x00, 0x80, 0x30, 0xff});
    files[2].data = OneRegionFbnk({{0x05000000, 0}});
    files[3].data = Fwar({Pcm16Fwav(4 * 32000, std::vector<int16_t>(4 * 32000, 1000))});
    const std::vector<uint32_t> kBanks = {0x03000000};
    const SoundArchive archive = SoundArchive::Load(
        Fsar(files, {.sequences = {{0, 0, kBanks}, {1, 0, kBanks}}, .banks = {2}, .wave_archives = {3}}),
        console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance waited = Perform(archive, 0, banks, {});
    const Performance stopped = Perform(archive, 1, banks, {});

    XSFLIB_CHECK(waited.seconds > 2.0 && waited.seconds < 2.1 && !stopped.holds &&
                 std::fabs(stopped.seconds - waited.seconds) < 0.011);
}

void TestTimedSweepGlidesOnWhenTheSequenceStops()
{
    // With notewait off, hold key 60 with about 1 s of portamento from key 48, then set tempo 0. The millisecond sweep
    // must continue. In the second sequence, hold key 60, play key 72 for one tick at release 127 with about 2 s of
    // portamento from key 48, then stop timing at tick 1. Key 72 has ended. Its sweep must stop too.
    std::vector<ArchiveFile> files(4);
    files[0].data = Fseq({0xc7, 0x00, 0xc9, 0x30, 0xcf, 0x17, 0x3c, 0x64, 0x00, 0xe1, 0x00, 0x00, 0x80, 0x30, 0xff});
    files[1].data = Fseq({0xc7, 0x00, 0x3c, 0x64, 0x00, 0xd3, 0x7f, 0xc9, 0x30, 0xcf, 0x17,
                          0x48, 0x64, 0x01, 0x80, 0x01, 0xe1, 0x00, 0x00, 0x80, 0x30, 0xff});
    files[2].data = OneRegionFbnk({{0x05000000, 0}});
    files[3].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const std::vector<uint32_t> kBanks = {0x03000000};
    const SoundArchive archive = SoundArchive::Load(
        Fsar(files, {.sequences = {{0, 0, kBanks}, {1, 0, kBanks}}, .banks = {2}, .wave_archives = {3}}),
        console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance gliding = Perform(archive, 0, banks, {});
    const Performance ended = Perform(archive, 1, banks, {});

    auto pitch = [](const Performance& p)
    {
        std::vector<std::pair<uint32_t, float>> events;
        for (const TrackEvent& e : p.tracks[0])
        {
            if (e.kind == EventKind::kPitch)
            {
                events.emplace_back(e.tick, e.semitones);
            }
        }

        return events;
    };
    const auto glide = pitch(gliding);
    const auto after_end = pitch(ended);

    XSFLIB_CHECK(gliding.holds && glide.size() > 50 && glide.front().second == -12.0f && glide.back().second == 0.0f &&
                 glide.back().first < 100);
    XSFLIB_CHECK(ended.holds && !after_end.empty() && after_end.back().second == 0.0f && after_end.back().first <= 2);
}

void TestTimedSweepMovesEveryFrame()
{
    // With note wait off, hold key 60 with portamento from key 48 over 120 ms (time 8). Each sound frame moves the
    // sweep on by an envelope step. A step is 3 ms on the Wii U and 5 ms on the Switch and the 3DS. The glide therefore
    // takes 40 frames on the Wii U and 24 on the others. A tick at tempo 120 takes 10.4 ms, over several of the game's
    // frames, and the sweep moves on in each. Every frame from the note's frame to the one in which it reaches the key
    // sends a pitch, placed where the frame starts.
    const Performance p = PerformOnBank({0xc7, 0x00, 0xc9, 0x30, 0xcf, 0x08, 0x3c, 0x64, 0x00, 0x80, 0x30, 0xff},
                                        OneRegionFbnk({{0x05000000, 0}}));

    const std::vector<std::pair<double, float>> pitches = PitchPlaces(p);

    XSFLIB_CHECK(GlidesEveryFrame(pitches) && !pitches.empty() && pitches[0].first == 0.0);
}

void TestTimedSweepGoesOnAfterItsTrackCloses()
{
    // Track 0 opens track 1 and waits 48 ticks. Track 1 holds key 60 at release 100 with portamento from key 48 over
    // 120 ms, and closes 2 ticks later. Closing releases the note. Like the game's channels, the note's tail goes on
    // gliding to the key. The second sequence has track 1 alone. Closing that track ends the sequence, and the tail's
    // pitches go on after the last tick.
    const std::vector<uint8_t> kGlide = {0xc7, 0x00, 0xc9, 0x30, 0xcf, 0x08, 0xd3,
                                         0x64, 0x3c, 0x64, 0x00, 0x80, 0x02, 0xff};
    std::vector<uint8_t> with_track_0 = Code({0xfe, U16(0x0003), 0x88, 0x01, U24(0x00000b), 0x80, 0x30, 0xff});
    with_track_0.insert(with_track_0.end(), kGlide.begin(), kGlide.end());
    std::vector<ArchiveFile> files(4);
    files[0].data = Fseq(with_track_0);
    files[1].data = Fseq(kGlide);
    files[2].data = OneRegionFbnk({{0x05000000, 0}});
    files[3].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const std::vector<uint32_t> kBanks = {0x03000000};
    const SoundArchive archive = SoundArchive::Load(
        Fsar(files, {.sequences = {{0, 0, kBanks}, {1, 0, kBanks}}, .banks = {2}, .wave_archives = {3}}),
        console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance closed = Perform(archive, 0, banks, {});
    const Performance ended = Perform(archive, 1, banks, {});

    XSFLIB_CHECK(GlidesEveryFrame(PitchPlaces(closed, 1)));
    const std::vector<std::pair<double, float>> after_end = PitchPlaces(ended);
    XSFLIB_CHECK(GlidesEveryFrame(after_end) && after_end.back().first > 2.0 && ended.end_tick >= 12);
}

void TestHeldNoteHoldsUntilStopped()
{
    // Key 60 for 10,000 ticks with note wait on, 104 s at tempo 120. sequence_export counts a note of 9,600 ticks or
    // more as one that the game stops itself. The sequence therefore holds and ends 10 s after it last changes. A note
    // of 9,599 ticks plays out.
    const Performance held = PerformOnBank({0x3c, 0x64, 0xce, 0x10, 0xff}, OneRegionFbnk({{0x05000000, 0}}));
    const Performance played = PerformOnBank({0x3c, 0x64, 0xca, 0x7f, 0xff}, OneRegionFbnk({{0x05000000, 0}}));

    XSFLIB_CHECK(held.holds && !held.truncated && held.seconds >= 10.0 && held.seconds < 10.1);
    XSFLIB_CHECK(!played.holds && played.seconds > 99.0);
}

void TestNegativeWaitBlocksTheTrackForEver()
{
    // Track 0 opens track 1, then waits as many ticks as track variable 0 holds (index 32). Track variables start at -1
    // (SequenceTrack::InitParam, 0x022CABAC, and MmlParser::GetVariablePtr, 0x022CB600). A track reads commands only
    // while its wait is 0. A negative wait never counts down (SequenceTrack::ParseNextTick, 0x022B6F14). Track 0
    // therefore never reads on. Track 1 plays key 60 for 24 ticks with note wait on and jumps back. The sound repeats
    // each pass of track 1, every 24 ticks. In the second sequence, track 0 holds key 60 with note wait off and no
    // length, then waits the same way. The sound never changes again. The sound holds until the game stops it.
    const std::vector<uint8_t> bank = OneRegionFbnk({{0x05000000, 0}});
    const Performance looping = PerformOnBank(Code({0xfe, U16(0x0003), 0x88, 0x01, U24(0x00000c), 0xa1, 0x80, 0x20,
                                                    0xff, 0x3c, 0x64, 0x18, 0x89, U24(0x00000c)}),
                                              bank);
    const Performance held = PerformOnBank({0xc7, 0x00, 0x3c, 0x64, 0x00, 0xa1, 0x80, 0x20, 0xff}, bank);

    XSFLIB_CHECK(looping.loop && looping.loop->end - looping.loop->start == 24 && looping.loop->start % 24 == 0 &&
                 !looping.truncated);
    XSFLIB_CHECK(held.holds && !held.loop && !held.truncated);
}

void TestHoldStartsOnceTheVolumesStopMoving()
{
    // A time prefix makes volume2 (0xd5) fall from 127 to 32 over 1920 ticks (20 s). Then key 60 plays for 10,000
    // ticks with note wait on. The track waits that note out. The sound holds until the game stops it. But the ramp
    // moves volume2 a step each tick up to tick 1920 (MmlParser::CommandProc, 0x022CBF1C, and
    // SequenceTrack::ParseNextTick, 0x022B6F14). The sound changes until then. CC11 therefore reaches 32 at tick 1920.
    // The main volume (0xc2) ramps the same way, a step each tick (SequenceSoundPlayer::ParseNextTick, 0x022B70AC).
    const std::vector<uint8_t> bank = OneRegionFbnk({{0x05000000, 0}});
    const Performance volume2 =
        PerformOnBank(Code({0xa3, 0xd5, 0x20, U16(0x0780), 0x3c, 0x64, 0xce, 0x10, 0xff}), bank);
    const Performance main_volume =
        PerformOnBank(Code({0xa3, 0xc2, 0x20, U16(0x0780), 0x3c, 0x64, 0xce, 0x10, 0xff}), bank);

    const auto last_expression = [](const Performance& p)
    {
        const std::vector<std::pair<uint32_t, int>> values = PartControls(p, EventKind::kExpression, 0);
        return values.empty() ? std::pair<uint32_t, int>{} : values.back();
    };
    const std::pair<uint32_t, int> kRampEnd = {1920, 32};
    XSFLIB_CHECK(volume2.holds && last_expression(volume2) == kRampEnd);
    XSFLIB_CHECK(main_volume.holds && last_expression(main_volume) == kRampEnd);
}

void TestLfoSetAfterANoteInItsTickReachesItsPreset()
{
    // With note wait off and LFO depth 40, play key 60, then set the LFO speed to 10 in the same tick. The game gives
    // the note the track's LFO settings at the end of the sound frame. The note's preset therefore has speed 10
    // (3.90625 Hz), and nothing is reported. The 3DS's presets keep the speed as the sequence gives it. The other
    // consoles' presets keep it in hertz.
    const Performance p = PerformOnBank({0xc7, 0x00, 0xca, 0x28, 0x3c, 0x64, 0x60, 0xcb, 0x0a, 0x80, 0x60, 0xff},
                                        OneRegionFbnk({{0x05000000, 0}}));

    const PresetKey* preset = FirstNotePreset(p);
    const float speed = console->settings.engine == Engine::kCtr ? 10.0f : 3.90625f;

    XSFLIB_CHECK(preset && preset->lfo_speed == speed && p.approximations.empty());
}

void TestVolumeLfoChangedAtDepth0IsReportedWhenHeard()
{
    // With note wait off, a volume LFO and LFO depth 0, play key 60 for 192 ticks. 48 ticks in, set the LFO speed to
    // 10, and 48 ticks later raise the depth to 40. The note's preset keeps speed 16 (6.25 Hz), and the game plays its
    // LFO at speed 10. That is reported once. The 3DS's presets keep the speed as the sequence gives it. The other
    // consoles' presets keep it in hertz.
    const Performance p = PerformOnBank({0xc7, 0x00, 0xcc, 0x01, 0xca, 0x00, 0x3c, 0x64, 0x81, 0x40, 0x80,
                                         0x30, 0xcb, 0x0a, 0x80, 0x30, 0xca, 0x28, 0x80, 0x60, 0xff},
                                        OneRegionFbnk({{0x05000000, 0}}));

    const PresetKey* preset = FirstNotePreset(p);
    const auto moved = p.approximations.find("LFO parameters changed during a note (using its note-on settings)");
    const float speed = console->settings.engine == Engine::kCtr ? 16.0f : 6.25f;

    XSFLIB_CHECK(preset && preset->lfo_speed == speed && moved != p.approximations.end() && moved->second == 1);
}

void TestPitchLfoChangedDuringANoteGoesOutAsPitchBends()
{
    // As in TestVolumeLfoChangedAtDepth0IsReportedWhenHeard, with a pitch LFO. The note plays on a part of its own,
    // with a preset without an LFO. The part's pitch bends follow the game's LFO, and nothing is reported. The game's
    // LFO peaks at depth / 128 * range semitones: 40 / 128 at range 1. Taken once a sound frame, the 3.90625 Hz sine
    // comes within 1% of its peak. The bends start once the depth is up, 96 ticks in.
    const Performance p = PerformOnBank({0xc7, 0x00, 0xca, 0x00, 0x3c, 0x64, 0x81, 0x40, 0x80, 0x30, 0xcb, 0x0a, 0x80,
                                         0x30, 0xca, 0x28, 0x80, 0x60, 0xff},
                                        OneRegionFbnk({{0x05000000, 0}}));

    const PresetKey* preset = FirstNotePreset(p);
    // The pitch bends of the note's part, and the first tick at which they leave 0.
    int part = -1;
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            part = e.part;
            break;
        }
    }
    float high = 0.0f;
    float low = 0.0f;
    uint32_t first = 0;
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kPitch && e.part == part && e.semitones != 0.0f)
        {
            first = first == 0 ? e.tick : first;
            high = std::max(high, e.semitones);
            low = std::min(low, e.semitones);
        }
    }
    const bool reported = std::ranges::any_of(
        p.approximations, [](const auto& approximation) { return approximation.first.starts_with("LFO parameters"); });
    const float peak = 40.0f / 128.0f;

    XSFLIB_CHECK(preset && preset->lfo_type == 3 && !reported);
    XSFLIB_CHECK(first >= 96 && high > peak * 0.99f && high < peak * 1.001f && low < -peak * 0.99f &&
                 low > -peak * 1.001f);
}

void TestRandomLfoCurveInPitchBendsIsReportedOnce()
{
    // A pitch LFO with the random curve (0xBE 4) at depth 40. The LFO's speed changes while the note sounds (0xCB 10).
    // The note plays on a part of its own, as in TestPitchLfoChangedDuringANoteGoesOutAsPitchBends. The part's pitch
    // bends follow the curve. CurveRandom (0x022BB610) gives a number from 0 to 1, times the depth and the range: from
    // 0 to 40 / 128 semitones here. The numbers come from a copy of the random generator. The note reports the curve
    // once. The report of a curve other than a sine is for an LFO in a preset, and doesn't apply.
    const Performance p = PerformOnBank(
        {0xbe, 0x04, 0xc7, 0x00, 0xca, 0x28, 0x3c, 0x64, 0x81, 0x40, 0x80, 0x30, 0xcb, 0x0a, 0x80, 0x81, 0x10, 0xff},
        OneRegionFbnk({{0x05000000, 0}}));

    const PresetKey* preset = FirstNotePreset(p);
    int part = -1;
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            part = e.part;
            break;
        }
    }
    float high = 0.0f;
    float low = 0.0f;
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kPitch && e.part == part)
        {
            high = std::max(high, e.semitones);
            low = std::min(low, e.semitones);
        }
    }
    const auto random =
        p.approximations.find("random LFO curve in pitch bends (its numbers can differ from those in the game)");
    const bool other_curve = std::ranges::any_of(
        p.approximations, [](const auto& approximation) { return approximation.first.starts_with("LFO curve"); });

    XSFLIB_CHECK(preset && preset->lfo_type == 3 && random != p.approximations.end() && random->second == 1 &&
                 !other_curve);
    XSFLIB_CHECK(low == 0.0f && high > 0.0f && high < 40.0f / 128.0f * 1.001f);
}

void TestDeepTremoloIsKeptTo6dB()
{
    // A volume LFO (type 1) at depth 127 swings 6 dB per unit of range. The game can't raise a note more than 6 dB. At
    // range 2, CC1 drops from 64 to 32. That swings 6 dB in the SoundFont, and the change is reported. At range 1, CC1
    // stays at 64. A pitch LFO at range 2 keeps 64 too.
    const std::vector<uint8_t> kBank = OneRegionFbnk({{0x05000000, 0}});
    const Performance deep = PerformOnBank({0xcc, 0x01, 0xcd, 0x02, 0xca, 0x7f, 0x3c, 0x64, 0x30, 0xff}, kBank);
    const Performance shallow = PerformOnBank({0xcc, 0x01, 0xcd, 0x01, 0xca, 0x7f, 0x3c, 0x64, 0x30, 0xff}, kBank);
    const Performance vibrato = PerformOnBank({0xcc, 0x00, 0xcd, 0x02, 0xca, 0x7f, 0x3c, 0x64, 0x30, 0xff}, kBank);

    auto modulation = [](const Performance& p)
    {
        std::vector<int> values;
        for (const TrackEvent& e : p.tracks[0])
        {
            if (e.kind == EventKind::kModulation)
            {
                values.push_back(e.value);
            }
        }

        return values;
    };
    const char* kReport = "volume LFO deeper than 6 dB (limited to 6 dB: it dips less than in the game)";
    XSFLIB_CHECK(modulation(deep) == std::vector<int>{32} && deep.approximations.count(kReport) == 1);
    XSFLIB_CHECK(modulation(shallow) == std::vector<int>{64} && shallow.approximations.count(kReport) == 0);
    XSFLIB_CHECK(modulation(vibrato) == std::vector<int>{64} && vibrato.approximations.count(kReport) == 0);
}

void TestNoteReleasedInItsAttackKeepsTheGamesLevel()
{
    // With attack 20, key 60 plays for 5 ticks, 52.1 ms. Its release comes after the envelope updates of the whole
    // frames in that time: 17 of 3 ms, or 10 of 5 ms. The game is below -30 dB then. The SoundFont attack, about 330 ms
    // long, has reached -16 dB. A second sequence plays the note for 200 ticks, past the attack. A SoundFont attack
    // that ramps up over t * 10^(-L / 20) reaches the game's level L dB at the release, t into the note. Only the first
    // note gets one.
    const std::vector<uint8_t> kBank = OneRegionFbnk({{0x05000000, 0}});
    const Performance early = PerformOnBank({0xd0, 0x14, 0x3c, 0x64, 0x05, 0xff}, kBank);
    const Performance late = PerformOnBank({0xd0, 0x14, 0x3c, 0x64, 0x81, 0x48, 0xff}, kBank);

    EnvelopeValues env;
    env.attack = 20;
    const auto frames = static_cast<int64_t>(5 * kTickMs / console->settings.frame_ms);
    const double level_db = HeldLevel(env, frames) / 10.0;
    const double attack_ms =
        static_cast<double>(frames) * console->settings.frame_ms * std::pow(10.0, -level_db / 20.0);
    XSFLIB_CHECK(level_db < -30.0 && attack_ms > Sf2AttackMs(env));
    XSFLIB_CHECK(early.presets.size() == 1 && early.presets[0].attack_timecents &&
                 *early.presets[0].attack_timecents == ToTimecents(attack_ms, -12000, 8000));
    XSFLIB_CHECK(late.presets.size() == 1 && !late.presets[0].attack_timecents);
}

void TestTrackWaitingForAVariableGoesOn()
{
    // Track 0 opens track 1, waits 96 ticks and sets player variable 0 to 1. Track 1 polls without waits, then plays
    // key 60. The 10,000-command limit must yield each tick so the note can start at tick 96.
    std::vector<ArchiveFile> files(3);
    files[0].data = Fseq(
        Code({0xfe, U16(0x0003), 0x88, 0x01, U24(0x000010), 0x80, 0x60, 0xf0, 0x80, 0x00, U16(0x0001), 0xff, 0xf0, 0x95,
              0x00, U16(0x0001), 0xa2, 0x89, U24(0x000010), 0x3c, 0x64, 0x30, 0xff}));
    files[1].data = OneRegionFbnk({{0x05000000, 0}});
    files[2].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive =
        SoundArchive::Load(Fsar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}),
                           console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance p = Perform(archive, 0, banks, {});

    std::vector<uint32_t> note_ons;
    for (const TrackEvent& e : p.tracks[1])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            note_ons.push_back(e.tick);
        }
    }

    XSFLIB_CHECK(note_ons == std::vector<uint32_t>{96} && p.warnings.empty());
}

void TestLoopStartSendsTheVolumeTheGameRestores()
{
    // With note wait off, set volume 100, play key 60 for 6 ticks and wait 12. The loop then sets volume 100 again,
    // plays key 60, and after half its length sets volume 50. The first pass requires no volume event at its start, but
    // the game goes back to 100 there. The loop start therefore sends it for a MIDI player that loops on the markers.
    // The first loop is 48 ticks long and passes on to the next pass; the second is 10 s long, and the performance
    // stops at its end.
    const std::vector<uint8_t> bank = OneRegionFbnk({{0x05000000, 0}});
    const Performance short_loop =
        PerformOnBank(Code({0xc7, 0x00, 0xc1, 0x64, 0x3c, 0x64, 0x06, 0x80, 0x0c, 0xc1, 0x64,
                            0x3c, 0x64, 0x18, 0x80, 0x18, 0xc1, 0x32, 0x80, 0x18, 0x89, U24(0x000009)}),
                      bank);
    const Performance long_loop =
        PerformOnBank(Code({0xc7, 0x00, 0xc1, 0x64, 0x3c, 0x64, 0x06, 0x80, 0x0c, 0xc1, 0x64, 0x3c,
                            0x64, 0x18, 0x80, 0x83, 0x60, 0xc1, 0x32, 0x80, 0x83, 0x60, 0x89, U24(0x000009)}),
                      bank);

    auto sent_at_start = [](const Performance& p)
    {
        std::vector<uint8_t> at_zero;
        std::vector<uint8_t> at_start;
        for (const TrackEvent& e : p.tracks[0])
        {
            if (e.kind == EventKind::kVolume && e.tick == 0)
            {
                at_zero.push_back(e.value);
            }
            else if (e.kind == EventKind::kVolume && p.loop && e.tick == p.loop->start)
            {
                at_start.push_back(e.value);
            }
        }

        return p.loop && p.loop->start == 12 && at_zero.size() == 1 && at_start == at_zero;
    };
    XSFLIB_CHECK(sent_at_start(short_loop));
    XSFLIB_CHECK(sent_at_start(long_loop) && long_loop.loop->end == 972);
}

void TestLoopMarksAPassThatPlaysAsTheNext()
{
    // With note wait off, set volume 127 before the loop. The loop plays key 60, sets volume 64 halfway through and
    // jumps back. Its first pass plays the note at 127 and later passes at 64. The first pass sends 127 at its start. A
    // MIDI player that loops on that pass therefore plays every repeat too loud. The second pass plays as the next
    // does. In the first sequence a pass is 48 ticks, and the performance runs on to 10 s anyway. In the second a pass
    // is 10 s. The second pass plays as the third. The performance therefore plays the second pass there too. The
    // second pass ends a little before 20 s. On the Wii U and the Switch, ticks come early (UpdateTick).
    const std::vector<uint8_t> bank = OneRegionFbnk({{0x05000000, 0}});
    const Performance short_loop = PerformOnBank(
        Code({0xc7, 0x00, 0xc1, 0x7f, 0x3c, 0x64, 0x18, 0x80, 0x18, 0xc1, 0x40, 0x80, 0x18, 0x89, U24(0x000004)}),
        bank);
    const Performance long_loop = PerformOnBank(Code({0xc7, 0x00, 0xc1, 0x7f, 0x3c, 0x64, 0x18, 0x80, 0x83, 0x60, 0xc1,
                                                      0x40, 0x80, 0x83, 0x60, 0x89, U24(0x000004)}),
                                                bank);

    XSFLIB_CHECK(short_loop.loop && short_loop.loop->start == 48 && short_loop.loop->end == 96);
    XSFLIB_CHECK(long_loop.loop && long_loop.loop->start == 960 && long_loop.loop->end == 1920 &&
                 long_loop.loops == 1 && long_loop.seconds > 19.99 && long_loop.seconds < 21.0);
}

void TestLoopDoesNotStrikeATiedNoteAgain()
{
    // Turn tie on. The loop ties key 60 for a tick, pans left, ties it again for a tick, pans right and jumps back. The
    // first pass strikes the note, and later passes only tie it again. The loop is therefore the second pass. A MIDI
    // player repeats that pass without striking the note every 2 ticks.
    const Performance p = PerformOnBank(
        Code({0xc8, 0x01, 0x3c, 0x64, 0x01, 0xc0, 0x20, 0x3c, 0x64, 0x01, 0xc0, 0x60, 0x89, U24(0x000002)}),
        OneRegionFbnk({{0x05000000, 0}}));

    const auto note_ons =
        std::ranges::count_if(p.tracks[0], [](const TrackEvent& e) { return e.kind == EventKind::kNoteOn; });

    XSFLIB_CHECK(note_ons == 1 && p.loop && p.loop->start == 2 && p.loop->end == 4);
}

void TestLoopThatNoPassRepeatsKeepsItsFirstPass()
{
    // Track 0 opens track 1, then plays key 60 every 960 ticks (10 s); track 1 plays key 64 every 959. Track 1's notes
    // come a tick earlier in each of track 0's passes. No pass therefore plays as the next. The loop stays on the first
    // pass, and the performance doesn't play another pass to find one.
    const Performance p = PerformOnBank(
        Code({0x88,          0x01, U24(0x000011), 0xc7, 0x00, 0x3c, 0x64, 0x18, 0x80, 0x87, 0x40,         0x89,
              U24(0x000007), 0xc7, 0x00,          0x40, 0x64, 0x18, 0x80, 0x87, 0x3f, 0x89, U24(0x000013)}),
        OneRegionFbnk({{0x05000000, 0}}), 3);

    XSFLIB_CHECK(p.loop && p.loop->start == 0 && p.loop->end == 960 && p.seconds < 12.0);
}

void TestPassOnAnotherPartPlaysAsTheNext()
{
    // With attack and release 122, glide from key 67 to key 83 over 30 ticks, wait 10 and jump back. Each note glides
    // while the last one's release still sounds. The notes therefore take turns on two parts, and a part that a note
    // moves to takes every setting again. The note of a pass and the note of the next pass are on different parts with
    // the same settings. So the first pass plays as the next.
    const Performance p =
        PerformOnBank({0xd0, 0x7a, 0xd3, 0x7a, 0xc9, 0x43, 0x53, 0x5a, 0x1e, 0x80, 0x0a, 0x89, 0x00, 0x00, 0x00},
                      OneRegionFbnk({{0x05000000, 0}}));

    const std::vector<int> parts = NoteParts(p);
    XSFLIB_CHECK(parts.size() > 3 && parts[0] == 0 && parts[1] == 1 && parts[2] == 0);
    XSFLIB_CHECK(p.loop && p.loop->start == 0 && p.loop->end == 40);
}

void TestSequenceThatFinishesHasNoLoop()
{
    // Set variable 0 to 0. Play key 60 for 24 ticks, add 1 to the variable, and jump back while it's below 3, then end.
    // The passes count as a loop on the way, but the sequence ends after three notes. It therefore has no loop.
    const Performance p =
        PerformOnBank(Code({0xf0, 0x80,        0x00, 0x00, 0x00, 0x3c,        0x64, 0x18, 0xf0,          0x81,
                            0x00, U16(0x0001), 0xf0, 0x94, 0x00, U16(0x0003), 0xa2, 0x89, U24(0x000005), 0xff}),
                      OneRegionFbnk({{0x05000000, 0}}));

    const auto note_ons =
        std::ranges::count_if(p.tracks[0], [](const TrackEvent& e) { return e.kind == EventKind::kNoteOn; });

    XSFLIB_CHECK(note_ons == 3 && !p.loop && p.loops == 0);
}

void TestVariableOptionSetsAVariableBeforeTheStart()
{
    // Wait a tick and check again while player variable 0 isn't 1, then play key 60 and end. Variables start at -1. The
    // sequence therefore plays nothing until the game sets the variable. PerformOptions::variables sets it first.
    std::vector<ArchiveFile> files(3);
    files[0].data = Fseq(
        Code({0xf0, 0x95, 0x00, U16(0x0001), 0xa2, 0x80, 0x01, 0xa2, 0x89, 0x00, 0x00, 0x00, 0x3c, 0x64, 0x18, 0xff}));
    files[1].data = OneRegionFbnk({{0x05000000, 0}});
    files[2].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive =
        SoundArchive::Load(Fsar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}),
                           console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);
    PerformOptions options;
    options.variables[0] = 1;

    const Performance unset = Perform(archive, 0, banks, {});
    const Performance set = Perform(archive, 0, banks, options);

    auto note_ons = [](const Performance& p)
    {
        return std::ranges::count_if(p.tracks[0], [](const TrackEvent& e) { return e.kind == EventKind::kNoteOn; });
    };
    XSFLIB_CHECK(note_ons(unset) == 0 && note_ons(set) == 1);
}

void TestVariableChangeComesBeforeTheTicksOfItsFrame()
{
    // Wait a tick and check again while variable `variable` isn't 1, then play key 60 and end. A tick lasts 10.4 ms at
    // 120 BPM. A change in the frame that holds 216 ms comes after tick 20, which lies in an earlier frame. Tick 21
    // sees it. A change in frame 0 comes before the first tick. The idle passes before a change at 15.005 s don't end
    // the performance. Tick 1441 sees that one.
    const auto first_note = [](uint8_t variable, double ms)
    {
        PerformOptions options;
        options.changes = {{static_cast<uint64_t>(ms / console->settings.frame_ms), variable, 1}};
        const Performance p = PerformOnBank(Code({0xf0, 0x95, variable, U16(0x0001), 0xa2, 0x80, 0x01, 0xa2, 0x89,
                                                  U24(0x000000), 0x3c, 0x64, 0x18, 0xff}),
                                            OneRegionFbnk({{0x05000000, 0}}), 1, options);
        const auto note = std::ranges::find(p.tracks[0], EventKind::kNoteOn, &TrackEvent::kind);
        return note != p.tracks[0].end() ? std::optional<uint32_t>(note->tick) : std::nullopt;
    };

    XSFLIB_CHECK(first_note(0, 216.0) == 21u);
    XSFLIB_CHECK(first_note(16, 216.0) == 21u);
    XSFLIB_CHECK(first_note(0, 0.0) == 0u);
    XSFLIB_CHECK(first_note(0, 15005.0) == 1441u);
}

void TestMainLoopStartsAfterTheLastVariableChange()
{
    // Play key 72 for 48 ticks while variable 0 is 1, else key 60, and jump back. The game sets the variable at
    // 20.25 s, in the middle of the pass from tick 1920. That pass plays key 60 to its end. The next pass plays key 72,
    // and it's the main loop. Without the change, the first pass is.
    const std::vector<uint8_t> code = Code({0xf0, 0x90, 0x00, U16(0x0001), 0xa2, 0x89, U24(0x000011), 0x3c, 0x64, 0x30,
                                            0x89, U24(0x000000), 0x48, 0x64, 0x30, 0x89, U24(0x000000)});
    PerformOptions options;
    options.changes = {{static_cast<uint64_t>(20250.0 / console->settings.frame_ms), 0, 1}};

    const Performance unchanged = PerformOnBank(code, OneRegionFbnk({{0x05000000, 0}}));
    const Performance changed = PerformOnBank(code, OneRegionFbnk({{0x05000000, 0}}), 1, options);

    const std::vector<int> keys = NoteKeys(changed);
    XSFLIB_CHECK(unchanged.loop && unchanged.loop->start == 0 && unchanged.loop->end == 48);
    XSFLIB_CHECK(changed.loop && changed.loop->start == 1968 && changed.loop->end == 2016 && changed.loops == 1);
    XSFLIB_CHECK(keys.size() == 42 && keys[40] == 60 && keys[41] == 72);
}

void TestWaitingForAVariableIsNoLoop()
{
    // Track 1 waits 96 ticks and sets variable 0. Track 0 repeatedly sets volume and polls the variable without waits.
    // The 10,000-command limit stops partway through a pass. Later ticks then jump back to commands visited on the
    // previous tick. These are polling passes, not the main loop. After the variable is set, track 0 loops a 48-tick
    // key 60; that is the main loop.
    std::vector<ArchiveFile> files(3);
    files[0].data = Fseq(Code({0xfe,          U16(0x0003), 0x88, 0x01, U24(0x00001b), 0xc1, 0x7f,        0xf0, 0x95,
                               0x00,          U16(0x0001), 0xa2, 0x89, U24(0x000008), 0x3c, 0x64,        0x30, 0x89,
                               U24(0x000014), 0x80,        0x60, 0xf0, 0x80,          0x00, U16(0x0001), 0xff}));
    files[1].data = OneRegionFbnk({{0x05000000, 0}});
    files[2].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive =
        SoundArchive::Load(Fsar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}),
                           console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance p = Perform(archive, 0, banks, {});

    XSFLIB_CHECK(p.loop && p.loop->start == 97 && p.loop->end == 145 && p.warnings.empty());
}

void TestPollingInASubroutineAfterAOneShotIsNoLoop()
{
    // Call a subroutine that plays key 60 for 12 ticks, then checks each tick whether variable 0 is 1, and returns when
    // it is. Nothing sets the variable. The subroutine therefore polls for ever. The conditional jump could leave that
    // loop, and its passes play no note. The loop therefore doesn't count. The performance ends with the note and has
    // no loop markers. The same loop counts in three other subroutines: one that holds a note of 9,600 ticks on a
    // looping wave with note wait off, one that ties keys 60 and 62 in each pass, and one whose only conditional
    // command clamps variable 0 at 0 and can't leave the loop. That one holds a note of 2,000 ticks with note wait off.
    // A loop that sounds nothing doesn't count either (SilentLoop).
    const std::vector<uint8_t> bank = OneRegionFbnk({{0x05000000, 0}});

    const Performance one_shot =
        PerformOnBank(Code({0x8a, U24(0x000005), 0xff, 0x3c, 0x64, 0x0c, 0xf0, 0x90, 0x00, U16(0x0001), 0xa2, 0x89,
                            U24(0x000018), 0x80, 0x01, 0x89, U24(0x000008), 0xfd}),
                      bank);
    const Performance held = PerformOnBank(
        Code({0xc7, 0x00,        0x8a, U24(0x000007), 0xff,          0x3c, 0x64, 0xcb, 0x00,          0xf0, 0x90,
              0x00, U16(0x0001), 0xa2, 0x89,          U24(0x00001b), 0x80, 0x01, 0x89, U24(0x00000b), 0xfd}),
        bank);
    const Performance melody =
        PerformOnBank(Code({0xc8, 0x01, 0xc7,        0x00, 0x8a, U24(0x000009), 0xff, 0x3c,          0x64,
                            0x07, 0x80, 0x07,        0x3e, 0x64, 0x07,          0x80, 0x07,          0xf0,
                            0x90, 0x00, U16(0x0001), 0xa2, 0x89, U24(0x000021), 0x89, U24(0x000009), 0xfd}),
                      bank);
    const Performance clamp = PerformOnBank(
        Code({0x8a, U24(0x000005), 0xff, 0xc7, 0x00, 0x3c, 0x64, 0x8f, 0x50, 0x80, 0x01,          0xf0, 0x94,
              0x00, 0x00,          0x00, 0xa2, 0xf0, 0x80, 0x00, 0x00, 0x00, 0x89, U24(0x00000b), 0xfd}),
        bank);

    XSFLIB_CHECK(!one_shot.loop && one_shot.loops == 0 && one_shot.seconds < 1.0);
    XSFLIB_CHECK(held.loop && held.loop->end == held.loop->start + 1 && held.seconds > 9.0);
    XSFLIB_CHECK(melody.loop && melody.loop->end == melody.loop->start + 14 && melody.seconds < 20.0);
    XSFLIB_CHECK(clamp.loop && clamp.loop->end == clamp.loop->start + 1 && clamp.seconds < 20.0);
}

void TestLoopLeftBeforeASilentTimeLimitIsDropped()
{
    // After key 60, count variable 1 up from -1 to 8 in passes of a tick. Those passes count as a loop. Then call a
    // subroutine that polls variable 0 for ever (see TestPollingInASubroutineAfterAOneShotIsNoLoop). The performance
    // reaches the time limit in silence. That ends it as if it had finished, and a sequence that finishes has no main
    // loop.
    const Performance p =
        PerformOnBank(Code({0x3c, 0x64,          0x0c, 0xf0, 0x81,        0x01,          U16(0x0001), 0x80,
                            0x01, 0xf0,          0x94, 0x01, U16(0x0008), 0xa2,          0x89,        U24(0x000003),
                            0x8a, U24(0x000019), 0xff, 0xf0, 0x90,        0x00,          U16(0x0001), 0xa2,
                            0x89, U24(0x000029), 0x80, 0x01, 0x89,        U24(0x000019), 0xfd}),
                      OneRegionFbnk({{0x05000000, 0}}));

    XSFLIB_CHECK(!p.loop && p.loops == 0 && !p.truncated && p.seconds < 1.0);
}

void TestCountedLoopPastTheTimeLimitPlaysAsAnEndlessLoop()
{
    // Play key 60 for 1920 ticks (20 s) in a loop of 64 passes, then end. The passes take 21 minutes and 20 s, past the
    // time limit. The performance reaches the limit inside the counted loop. It runs again with the loop endless and
    // plays as the same loop with a count of 0: the first pass is the main loop. A loop of 2 passes ends before the
    // limit, and the sequence ends after its passes.
    const auto loop = [](uint8_t count)
    {
        return PerformOnBank(Code({0xd4, count, 0x3c, 0x64, 0x8f, 0x00, 0xfc, 0xff}), OneRegionFbnk({{0x05000000, 0}}));
    };
    const Performance counted = loop(64);
    const Performance endless = loop(0);
    const Performance twice = loop(2);

    MidiReport report;
    XSFLIB_CHECK(endless.loop && endless.loop->start == 0 && endless.loop->end == 1920 && endless.loops == 1 &&
                 !endless.truncated && endless.seconds < 20.1);
    XSFLIB_CHECK(!counted.truncated && counted.loops == 1 && counted.seconds == endless.seconds &&
                 WriteMidi(counted, "test", report) == WriteMidi(endless, "test", report));
    XSFLIB_CHECK(!twice.loop && twice.loops == 0 && !twice.truncated && NoteCount(twice) == 2 &&
                 twice.seconds >= 40.0 && twice.seconds < 41.0);
}

void TestTiedMelodyBelowHearingIsNoLoop()
{
    // With sustain 0, decay 100 and hold 0, call a subroutine that ties keys 60 and 62 on for 7 ticks each, and returns
    // once variable 0 is 1 (see TestPollingInASubroutineAfterAOneShotIsNoLoop). Nothing sets the variable. The legatos
    // don't start the envelope again, and the channel sinks below hearing (kInaudibleSustain). From then on, the passes
    // play no note that can be heard. The conditional jump could leave the loop. The passes therefore don't count as a
    // loop. The MIDI note and the performance end where the channel has faded to kFadedAttenuation. The performance has
    // no loop markers.
    const Performance p = PerformOnBank(
        Code({0xd2,          0x00, 0xd1, 0x64, 0xb1,        0x00, 0xc8, 0x01,          0xc7, 0x00,          0x8a,
              U24(0x00000f), 0xff, 0x3c, 0x64, 0x07,        0x80, 0x07, 0x3e,          0x64, 0x07,          0x80,
              0x07,          0xf0, 0x90, 0x00, U16(0x0001), 0xa2, 0x89, U24(0x000027), 0x89, U24(0x00000f), 0xfd}),
        OneRegionFbnk({{0x05000000, 0}}));

    const EnvelopeValues kEnvelope{127, 100, 0, 0, 127};
    const double faded_ms = static_cast<double>(FramesToLevel(kEnvelope, -static_cast<float>(kFadedAttenuation)) + 1) *
                            console->settings.frame_ms;
    const auto off = std::ranges::find(p.tracks[0], EventKind::kNoteOff, &TrackEvent::kind);
    const double off_ms = off == p.tracks[0].end() ? 0.0 : off->tick * kTickMs;
    XSFLIB_CHECK(!p.loop && p.loops == 0 && !p.truncated && NoteCount(p) == 1);
    XSFLIB_CHECK(off_ms >= faded_ms && off_ms < faded_ms + kTickMs + console->settings.frame_ms);
    XSFLIB_CHECK(p.seconds * 1000.0 >= faded_ms &&
                 p.seconds * 1000.0 < faded_ms + kTickMs + console->settings.frame_ms);
}

void TestLegatoBelowHearingKeepsItsMidiNote()
{
    // With tie on, sustain 0, decay 100 and hold 0, play key 60 with note wait off. After 100 ticks the channel has
    // sunk below hearing (kInaudibleSustain) but hasn't faded to kFadedAttenuation. Its MIDI note still sounds. Then
    // tie key 62 on, once after setting release 64 and once at velocity 127. The game's legato doesn't start the
    // envelope again. Neither legato starts a new MIDI note, and velocity 127 doesn't start the note higher on a later
    // pass.
    const std::vector<uint8_t> bank = OneRegionFbnk({{0x05000000, 0}});
    const EnvelopeValues kEnvelope{127, 100, 0, 0, 127};
    auto at_ms = [&](int attenuation)
    {
        return static_cast<double>(FramesToLevel(kEnvelope, -static_cast<float>(attenuation)) + 1) *
               console->settings.frame_ms;
    };

    const Performance released = PerformOnBank({0xc8, 0x01, 0xd2, 0x00, 0xd1, 0x64, 0xb1, 0x00, 0xc7, 0x00, 0x3c, 0x64,
                                                0x01, 0x80, 0x64, 0xd3, 0x40, 0x3e, 0x64, 0x01, 0x80, 0x30, 0xff},
                                               bank);
    const Performance louder = PerformOnBank({0xc8, 0x01, 0xd2, 0x00, 0xd1, 0x64, 0xb1, 0x00, 0xc7, 0x00, 0x3c,
                                              0x64, 0x01, 0x80, 0x64, 0x3e, 0x7f, 0x01, 0x80, 0x30, 0xff},
                                             bank);

    const auto on = std::ranges::find(louder.tracks[0], EventKind::kNoteOn, &TrackEvent::kind);
    XSFLIB_CHECK(at_ms(kInaudibleSustain) < 100 * kTickMs && 100 * kTickMs < at_ms(kFadedAttenuation));
    XSFLIB_CHECK(NoteCount(released) == 1 &&
                 !released.approximations.contains("legato (tie or mono) played as a new note"));
    XSFLIB_CHECK(NoteCount(louder) == 1 && on != louder.tracks[0].end() && on->value == 100);
}

void TestPollAfterTheSoundIsNoLoop()
{
    // Track 0 opens track 1, then checks each tick whether variable 0 is 1, and leaves the loop when it is. Nothing
    // sets the variable. Track 1 plays key 60 for 12 ticks and ends. Once the note has ended, track 0's passes count as
    // the main loop. They sound nothing (SilentLoop). The performance therefore ends with the note and has no loop
    // markers. When track 1 holds a note of 9,600 ticks instead, the same passes are the main loop under the note.
    const std::vector<uint8_t> bank = OneRegionFbnk({{0x05000000, 0}});
    const std::vector<uint8_t> root = Code({0xfe, U16(0x0003), 0x88, 0x01, U24(0x000019), 0x80, 0x01, 0xf0, 0x90, 0x00,
                                            U16(0x0001), 0xa2, 0x89, U24(0x000018), 0x89, U24(0x000008), 0xff});
    std::vector<uint8_t> one_shot = root;
    one_shot.insert(one_shot.end(), {0x3c, 0x64, 0x0c, 0xff});
    std::vector<uint8_t> held = root;
    held.insert(held.end(), {0x3c, 0x64, 0xcb, 0x00, 0xff});

    const Performance after = PerformOnBank(one_shot, bank);
    const Performance under = PerformOnBank(held, bank);

    XSFLIB_CHECK(!after.loop && after.loops == 0 && !after.truncated && NoteCount(after) == 1);
    XSFLIB_CHECK(after.seconds < 1.0);
    XSFLIB_CHECK(under.loop && under.loop->end == under.loop->start + 1 && under.seconds > 9.0);
}

void TestSilentPassesThatChangeSettingsEndWithTheSound()
{
    // Play key 60 for 12 ticks, then set the volume to 127 and to 64 in passes of 48 ticks. The passes change the track
    // and sound nothing (SilentLoop). The performance ends with the note. The volume changes after it are left out.
    const Performance p =
        PerformOnBank(Code({0x3c, 0x64, 0x0c, 0xc1, 0x7f, 0x80, 0x18, 0xc1, 0x40, 0x80, 0x18, 0x89, U24(0x000003)}),
                      OneRegionFbnk({{0x05000000, 0}}));

    const auto after_end = [&p](const TrackEvent& e)
    {
        return e.tick > p.end_tick;
    };
    XSFLIB_CHECK(!p.loop && p.loops == 0 && NoteCount(p) == 1 && p.seconds < 1.0);
    XSFLIB_CHECK(std::ranges::none_of(p.tracks[0], after_end));
}

void TestSequenceWithoutNotesEndsAtItsStart()
{
    // A sequence that only waits a tick and jumps back sounds nothing (SilentLoop). It ends at tick 0 with no loop
    // markers. Its MIDI file and SoundFont are still valid.
    std::vector<ArchiveFile> files(1);
    files[0].data = Fseq({0x80, 0x01, 0x89, 0x00, 0x00, 0x00});
    const SoundArchive archive = SoundArchive::Load(Fsar(files, {.sequences = {{0, 0}}}), console->settings.archive);

    const Conversion c = ConvertSequence(archive, 0);

    const MidiView m = ParseMidi(c.midi);
    const auto at_start = [](uint64_t end)
    {
        return end == 0;
    };
    XSFLIB_CHECK(!c.performance.loop && c.performance.loops == 0 && c.performance.end_tick == 0);
    XSFLIB_CHECK(c.performance.seconds == 0.0 && !c.performance.truncated);
    XSFLIB_CHECK(m.ok && !m.track_ends.empty() && std::ranges::all_of(m.track_ends, at_start));
    XSFLIB_CHECK(Sf2Problem(c.sf2).empty());
}

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
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
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
    // notes at once, kMaxVoiceLimit play, and the performer omits six as an approximation. With tie enabled, two notes
    // more than the voices, ending on key 72, use one channel; the final legato requires no approximation. As many
    // notes as the voices on missing program 5 allocate nothing. A following held note on program 0 must therefore
    // play. Finally, one tied note more than the voices at velocity 1 is followed by one at 127 with a new release.
    // That note would require another preset. The note must keep the existing MIDI note, in its preset and at its
    // level, without reducing the sequence's level.
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

    const std::string kLegatosPastCap = "more than " + std::to_string(voices) +
                                        " legatos in a tick (keeping the MIDI preset and limiting gain to the starting "
                                        "level)";
    const std::string kPastCap = " new channels on a track in one tick (excess omitted; the game keeps the newest)";
    auto past_cap = [&kPastCap](const Performance& p)
    {
        return std::ranges::count_if(p.approximations,
                                     [&kPastCap](const auto& a) { return a.first.ends_with(kPastCap); });
    };
    const auto left_out = capped.approximations.find("more than " + std::to_string(voices) + kPastCap);
    const auto past_most = most.approximations.find("more than " + std::to_string(kMaxVoiceLimit) + kPastCap);
    const bool option = console->settings.voice_limit > 0;
    const bool drops = option && console->settings.engine != Engine::kCtr;
    XSFLIB_CHECK(note_ons(flood) == voices + 4 && past_cap(flood) == 0);
    XSFLIB_CHECK(note_ons(capped) == (option ? voices : voices + 4));
    XSFLIB_CHECK(option && !drops ? left_out != capped.approximations.end() && left_out->second == 4
                                  : past_cap(capped) == 0);
    XSFLIB_CHECK(note_ons(most) == static_cast<int>(kMaxVoiceLimit) && past_most != most.approximations.end() &&
                 past_most->second == 6);
    XSFLIB_CHECK(note_ons(tied) == 1 && tied_pitch == 12.0f && !tied.approximations.contains(kLegatosPastCap));
    XSFLIB_CHECK(note_ons(held) == 1 && held.holds);
    XSFLIB_CHECK(note_ons(loud) == 1 && loud.level_cut_db == 0.0 && loud.approximations.contains(kLegatosPastCap));
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
    std::vector<ArchiveFile> files(3);
    files[0].data =
        Fseq(Code({0xfe,        U16(0x0003), 0x88,          0x01,          U24(0x000013), 0x80, 0x32, 0xf0, 0x80, 0x00,
                   U16(0x0001), 0x89,        U24(0x000008), 0x80,          0x2d,          0xc7, 0x00, 0xf0, 0x95, 0x00,
                   U16(0x0001), 0xa2,        0x89,          U24(0x000017), 0xf0,          0x80, 0x00, 0x00, 0x00, 0x3c,
                   0x64,        0x18,        0x89,          U24(0x000017)}));
    files[1].data = OneRegionFbnk({{0x05000000, 0}});
    files[2].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive =
        SoundArchive::Load(Fsar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}),
                           console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance p = Perform(archive, 0, banks, {});

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
    std::vector<ArchiveFile> files(3);
    files[0].data = Fseq(Code({0xc7, 0x00, 0x3c, 0x64, 0x01, 0x89, U24(0x000002)}));
    files[1].data = OneRegionFbnk({{0x05000000, 0}});
    files[2].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive =
        SoundArchive::Load(Fsar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}),
                           console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance p = Perform(archive, 0, banks, {.voice_limit = console->settings.voice_limit});

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
    std::vector<ArchiveFile> files(3);
    files[0].data = Fseq(Code(
        {0xc7, 0x00, 0x3c, 0x64, 0x60, 0x80, 0x01, 0x3e, 0x64, 0x60, 0x80, 0x01, 0x40, 0x64, 0x60, 0x80, 0x60, 0xff}));
    files[1].data = OneRegionFbnk({{0x05000000, 0}});
    files[2].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive =
        SoundArchive::Load(Fsar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}),
                           console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance two = Perform(archive, 0, banks, {.voice_limit = 2});
    const Performance off = Perform(archive, 0, banks, {.voice_limit = 0});
    const Performance usual = Perform(archive, 0, banks, {});
    const Performance game = Perform(archive, 0, banks, {.voice_limit = console->settings.voice_limit});

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
    // sfizz falls by e^9 (78.17 dB) over its decay time. Use the game's time to fall 78.17 dB, based on its 1000 / rate
    // envelope milliseconds per 100 dB, in real time.
    const double decay_ms = 1000.0 / DecayRate(100) * console->settings.env_time_scale;

    const SfzEnvelope d = ConvertEnvelopeForSfz(EnvelopeValues{127, 100, 64, 0, 100});

    const double kSfizzFallDb = 9.0 * 20.0 / std::log(10.0);
    XSFLIB_CHECK(std::abs(d.decay * 1000.0 - decay_ms * kSfizzFallDb / 100.0) < 1e-9 * decay_ms);
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

    std::string message;
    try
    {
        DecodeWave(wave);
    }
    catch (const xsflib::FormatError& e)
    {
        message = e.what();
    }

    XSFLIB_CHECK(message == "bad " + magic + ": no DATA block");
}

void TestShortFileIsReported()
{
    std::string message;
    try
    {
        DecodeWave(std::vector<uint8_t>{'C', 'W', 'A', 'V'});
    }
    catch (const xsflib::FormatError& e)
    {
        message = e.what();
    }

    XSFLIB_CHECK(message == ByConsole<std::string>("bad FWAV: too short to be an FWAV file",
                                                   "bad FWAV: too short to be an FWAV file",
                                                   "bad CWAV: too short to be a CWAV file"));
}

void TestSoundFontWriterOutputIsValid()
{
    sf2::SoundFont sf;
    sf.info = {{"INAM", "test"}, {"ISFT", console->program}};
    for (int c = 0; c < 3; c++)
    {
        sf2::Sample s;
        s.name = "s" + std::to_string(c);
        s.data.assign(100, static_cast<int16_t>(c * 1000));
        s.loop_start = 10;
        s.loop_end = 90;
        sf.samples.push_back(s);
    }

    sf.samples[1].type = sf2::kLeft;
    sf.samples[1].link = 2;
    sf.samples[2].type = sf2::kRight;
    sf.samples[2].link = 1;

    sf2::Instrument inst;
    inst.name = "i";
    sf2::Zone global;
    global.modulators.push_back({0x0081, sf2::kVibLfoToPitch, 100, 0});
    inst.zones.push_back(global);
    for (uint16_t id = 0; id < 3; id++)
    {
        sf2::Zone z;
        z.generators.push_back(sf2::Generator::Value(sf2::kSampleId, id)); // written last whatever the order
        z.generators.push_back(sf2::Generator::Value(sf2::kSampleModes, 1));
        z.generators.push_back(sf2::Generator::Range(sf2::kVelRange, 0, 127));
        z.generators.push_back(sf2::Generator::Range(sf2::kKeyRange, 0, 127));
        inst.zones.push_back(z);
    }
    sf.instruments.push_back(inst);

    sf2::Preset p;
    p.name = "p";
    sf2::Zone pz;
    pz.generators.push_back(sf2::Generator::Value(sf2::kInstrument, 0));
    p.zones.push_back(pz);
    sf.presets.push_back(p);

    const auto bytes = sf2::Write(sf);

    const std::string problem = Sf2Problem(bytes);
    XSFLIB_CHECK(problem.empty());
    if (!problem.empty())
    {
        std::fprintf(stderr, "  SoundFont writer: %s\n", problem.c_str());
    }

    const seqlib::sf2::Records v = seqlib::sf2::Read(bytes);
    XSFLIB_CHECK(v.chunks.at("phdr").size() == 38 * 2 && v.chunks.at("inst").size() == 22 * 2);
    XSFLIB_CHECK(v.chunks.at("smpl").size() == 2 * 3 * (100 + 46));
    XSFLIB_CHECK(v.chunks.at("imod").size() == 10 * 2);
}

void TestSoundFontWithoutSamplesHasSampleData()
{
    // Even a silent sequence requires sample padding: FluidSynth rejects empty sample data.
    sf2::SoundFont sf;
    sf.info = {{"INAM", ""}, {"ISFT", console->program}};
    const auto bytes = sf2::Write(sf);

    const seqlib::sf2::Records v = seqlib::sf2::Read(bytes);

    XSFLIB_CHECK(Sf2Problem(bytes).empty() && v.chunks.at("smpl").size() == 2 * 46);
}

void TestMidiWriterLayersMarkersAndBendRange()
{
    Performance p;
    p.timebase = 48;
    p.tempo = {{0, 120.0}};
    p.loop = Performance::Loop{48, 96};
    p.end_tick = 96;
    p.presets.push_back({});

    auto add = [&](int t, uint32_t tick, EventKind k, uint8_t key, uint8_t value, uint32_t note)
    {
        TrackEvent e;
        e.tick = tick;
        e.kind = k;
        e.key = key;
        e.value = value;
        e.note = note;
        p.tracks[t].push_back(e);
    };

    add(0, 0, EventKind::kPreset, 0, 0, 0);
    add(0, 0, EventKind::kVolume, 0, 100, 0);
    add(0, 0, EventKind::kNoteOn, 60, 100, 1);
    add(0, 10, EventKind::kNoteOn, 60, 90, 2); // the same key again: another channel
    add(0, 20, EventKind::kNoteOff, 60, 0, 1);
    add(0, 30, EventKind::kNoteOff, 60, 0, 2);
    add(3, 5, EventKind::kNoteOn, 64, 80, 3);
    add(3, 50, EventKind::kNoteOff, 64, 0, 3);
    TrackEvent bend;
    bend.tick = 5;
    bend.kind = EventKind::kPitch;
    bend.semitones = -7.5f;
    p.tracks[3].insert(p.tracks[3].begin(), bend);

    MidiReport rep;
    const auto bytes = WriteMidi(p, "test", rep);

    // Timebase 48 gives 160 MIDI ticks for each of the performance's ticks.
    const MidiView v = ParseMidi(bytes);
    XSFLIB_CHECK(v.ok);
    XSFLIB_CHECK(v.division == 7680);
    XSFLIB_CHECK(rep.channels == 3 && rep.cut_notes == 0);
    XSFLIB_CHECK(v.track_names.size() == 4 && v.track_names[0] == "test" && v.track_names[2] == "Track 0 (layer 2)");
    XSFLIB_CHECK(v.notes.size() == 3);

    // Track 0 gets channel 0, and channel 2 for its layer (track 3 has channel 1).
    int channels_for_key60 = 0;
    for (const MidiNote& n : v.notes)
    {
        if (n.key == 60)
        {
            channels_for_key60 |= 1 << n.channel;
        }
    }

    XSFLIB_CHECK(channels_for_key60 == 0b101);

    XSFLIB_CHECK(v.markers.size() == 2 && v.markers[0].first == 7680 && v.markers[1].second == "loopEnd");

    // Pitch bend range 8 (RPN 0) for a 7.5 semitone bend on track 3's channel (1).
    const std::vector<uint8_t> kRpn = {0xb1, 6, 8};
    XSFLIB_CHECK(std::search(bytes.begin(), bytes.end(), kRpn.begin(), kRpn.end()) != bytes.end());
}

void TestMidiWriterGivesPartsChannelsOfTheirOwn()
{
    // Track 0 plays key 60 on its first part and key 62 on its second part. Only the second part sets volume 50.
    Performance p;
    p.timebase = 48;
    p.tempo = {{0, 120.0}};
    p.end_tick = 48;
    p.presets.push_back({});

    auto add = [&](uint32_t tick, EventKind k, uint8_t key, uint8_t value, uint32_t note, uint8_t part)
    {
        TrackEvent e;
        e.tick = tick;
        e.kind = k;
        e.key = key;
        e.value = value;
        e.note = note;
        e.part = part;
        p.tracks[0].push_back(e);
    };
    add(0, EventKind::kPreset, 0, 0, 0, 0);
    add(0, EventKind::kNoteOn, 60, 100, 1, 0);
    add(10, EventKind::kPreset, 0, 0, 0, 1);
    add(10, EventKind::kVolume, 0, 50, 0, 1);
    add(10, EventKind::kNoteOn, 62, 100, 2, 1);
    add(20, EventKind::kNoteOff, 60, 0, 1, 0);
    add(30, EventKind::kNoteOff, 62, 0, 2, 1);

    MidiReport rep;
    const auto bytes = WriteMidi(p, "test", rep);

    const MidiView v = ParseMidi(bytes);
    const std::vector<uint8_t> kSecondVolume = {0xb1, 7, 50}, kFirstVolume = {0xb0, 7, 50};
    XSFLIB_CHECK(v.ok && rep.channels == 2 && v.track_names.size() == 3 && v.track_names[2] == "Track 0 (layer 2)");
    XSFLIB_CHECK(v.notes.size() == 2 && v.notes[0].channel != v.notes[1].channel);
    XSFLIB_CHECK(std::search(bytes.begin(), bytes.end(), kSecondVolume.begin(), kSecondVolume.end()) != bytes.end() &&
                 std::search(bytes.begin(), bytes.end(), kFirstVolume.begin(), kFirstVolume.end()) == bytes.end());
}

void TestMidiWriterHasAtLeast7680TicksPerQuarterNote()
{
    // Use at least 7680 MIDI ticks per quarter note, with an integer ratio to performance ticks. Without frame
    // timestamps, preserve note positions.
    const std::pair<uint32_t, uint16_t> kCases[] = {{48, 7680}, {96, 7680}, {100, 7700}, {255, 7905}, {1, 7680}};
    for (const auto& [timebase, division] : kCases)
    {
        Performance p;
        p.timebase = timebase;
        p.tempo = {{0, 120.0}};
        p.end_tick = 8;
        p.presets.push_back({});
        TrackEvent on;
        on.tick = 3;
        on.key = 60;
        on.value = 100;
        on.note = 1;
        TrackEvent off = on;
        off.tick = 7;
        off.kind = EventKind::kNoteOff;
        p.tracks[0] = {on, off};

        MidiReport rep;
        const MidiView v = ParseMidi(WriteMidi(p, "test", rep));
        const uint32_t scale = division / timebase;
        XSFLIB_CHECK(v.ok && v.division == division);
        XSFLIB_CHECK(v.notes.size() == 1 && v.notes[0].on == 3 * scale && v.notes[0].off == 7 * scale);
    }
}

void TestMidiEventsGoOnSoundFrames()
{
    // At timebase 48, use 160 MIDI ticks per performance tick. Align events to fractional frame starts without crossing
    // the loop start at tick 2. Keys 60, 62 and 64 span ticks 1-2, 2-3 and 3-4, and a sweep bends key 64 up a semitone
    // in a frame between ticks 3 and 4, half a tick after tick 3.
    Performance p;
    p.tempo = {{0, 120.0}};
    p.end_tick = 8;
    p.presets.push_back({});
    p.frame_ticks = {0.0, 0.5, 1.75, 2.75, 4.0};
    p.loop = Performance::Loop{2, 6};
    const std::pair<uint8_t, uint32_t> kNotes[] = {{60, 1}, {62, 2}, {64, 3}}; // key, tick
    uint32_t id = 1;
    for (const auto& [key, tick] : kNotes)
    {
        TrackEvent on;
        on.tick = tick;
        on.key = key;
        on.value = 100;
        on.note = id++;
        TrackEvent off = on;
        off.tick = tick + 1;
        off.kind = EventKind::kNoteOff;
        p.tracks[0].push_back(on);
        p.tracks[0].push_back(off);
    }

    TrackEvent bend;
    bend.tick = 3;
    bend.kind = EventKind::kPitch;
    bend.semitones = 1.0f;
    bend.after = 0.5;
    p.tracks[0].insert(p.tracks[0].end() - 1, bend);

    MidiReport rep;
    const MidiView m = ParseMidi(WriteMidi(p, "test", rep));

    XSFLIB_CHECK(m.ok && m.notes.size() == 3);
    XSFLIB_CHECK(m.bends == (std::vector<std::pair<uint32_t, int>>{{560, 12288}}));
    if (m.notes.size() == 3)
    {
        XSFLIB_CHECK(m.notes[0].key == 60 && m.notes[0].on == 80 && m.notes[0].off == 320);
        XSFLIB_CHECK(m.notes[1].key == 62 && m.notes[1].on == 320 && m.notes[1].off == 440);
        XSFLIB_CHECK(m.notes[2].key == 64 && m.notes[2].on == 440 && m.notes[2].off == 640);
    }
    XSFLIB_CHECK(m.markers == (std::vector<std::pair<uint32_t, std::string>>{{320, "loopStart"}, {960, "loopEnd"}}));
}

void TestMidiWriterSplitsLongDeltaTimes()
{
    // At timebase 1, each performance tick is 7680 MIDI ticks. A 600000-tick note exceeds both 32-bit MIDI positions
    // and the 28-bit delta limit.
    Performance p;
    p.timebase = 1;
    p.tempo = {{0, 120.0}};
    p.end_tick = 600000;
    p.presets.push_back({});
    TrackEvent on;
    on.key = 60;
    on.value = 100;
    on.note = 1;
    TrackEvent off = on;
    off.tick = 600000;
    off.kind = EventKind::kNoteOff;
    p.tracks[0] = {on, off};

    MidiReport rep;
    const MidiView v = ParseMidi(WriteMidi(p, "test", rep));

    XSFLIB_CHECK(v.ok && v.track_ends == std::vector<uint64_t>(2, uint64_t{600000} * 7680));
}

void TestMidiBendUpByAWholeRangeIsExact()
{
    // A +7-semitone bend requires range 8 because the pitch wheel stops at +8191, one step short of full range. With
    // range 8, +7 is 7168 steps: wheel value 15360 (0x3c00, bytes 0 and 0x78).
    Performance p;
    p.tempo = {{0, 120.0}};
    p.end_tick = 48;
    p.presets.push_back({});
    TrackEvent bend;
    bend.kind = EventKind::kPitch;
    bend.semitones = 7.0f;
    p.tracks[0].push_back(bend);
    TrackEvent on;
    on.key = 60;
    on.value = 100;
    on.note = 1;
    p.tracks[0].push_back(on);
    TrackEvent off = on;
    off.tick = 24;
    off.kind = EventKind::kNoteOff;
    p.tracks[0].push_back(off);

    MidiReport rep;
    const auto bytes = WriteMidi(p, "test", rep);

    const std::vector<uint8_t> kRange = {0xb0, 6, 8};
    const std::vector<uint8_t> kBend = {0xe0, 0x00, 0x78};
    XSFLIB_CHECK(std::search(bytes.begin(), bytes.end(), kRange.begin(), kRange.end()) != bytes.end());
    XSFLIB_CHECK(std::search(bytes.begin(), bytes.end(), kBend.begin(), kBend.end()) != bytes.end());
}

void TestMidiWriterSendsTheFilterToEveryLayer()
{
    // Overlapping strikes of one key require a second channel. Send filter controllers to both.
    Performance p;
    p.tempo = {{0, 120.0}};
    p.end_tick = 48;
    p.presets.push_back({});
    TrackEvent filter;
    filter.kind = kFilterEvent;
    filter.value = 40;
    TrackEvent on;
    on.key = 60;
    on.value = 100;
    on.note = 1;
    TrackEvent again = on;
    again.tick = 12;
    again.note = 2;
    TrackEvent off = on;
    off.tick = 24;
    off.kind = EventKind::kNoteOff;
    TrackEvent off_again = again;
    off_again.tick = 36;
    off_again.kind = EventKind::kNoteOff;
    p.tracks[0] = {filter, on, again, off, off_again};

    MidiReport rep;
    const auto bytes = WriteMidi(p, "test", rep);

    XSFLIB_CHECK(rep.channels == 2);
    for (const int channel : {0, 1})
    {
        const std::vector<uint8_t> kFilter = {static_cast<uint8_t>(0xb0 | channel), kFilterController, 40};
        XSFLIB_CHECK(std::search(bytes.begin(), bytes.end(), kFilter.begin(), kFilter.end()) != bytes.end());
    }
}

void TestMidiWriterListsChannel10Presets()
{
    // Track t uses preset t. With 16 tracks, the last gets channel 10 (index 9). Copy its preset to bank 128. With 15
    // tracks, leave channel 10 unused.
    for (int tracks : {15, 16})
    {
        Performance p;
        p.tempo = {{0, 120.0}};
        p.end_tick = 48;
        for (int t = 0; t < tracks; t++)
        {
            TrackEvent preset;
            preset.kind = EventKind::kPreset;
            preset.preset = static_cast<uint32_t>(t);
            TrackEvent on;
            on.key = 60;
            on.value = 100;
            on.note = static_cast<uint32_t>(t + 1);
            TrackEvent off = on;
            off.tick = 24;
            off.kind = EventKind::kNoteOff;
            p.presets.push_back({});
            p.tracks[t] = {preset, on, off};
        }

        MidiReport rep;
        WriteMidi(p, "test", rep);

        XSFLIB_CHECK(rep.channels == tracks);
        XSFLIB_CHECK(rep.channel_10_presets == (tracks == 16 ? std::set<uint32_t>{15} : std::set<uint32_t>{}));
    }
}

void TestSfzFileHasTheTracksPresetsAndFilters()
{
    // Track 2 uses presets 0 and 1 with nominal 80 Hz low-pass (0xd8 argument 4) and biquad high-pass (type 2). Other
    // tracks are silent and get no MIDI channel or SFZ file. With no archive banks, the presets have no regions.
    const SoundArchive archive = SoundArchive::Load(Fsar({}), console->settings.archive);
    BankSet banks(archive, SoundInfo{}, console->settings);
    Performance p;
    p.presets.resize(2);
    TrackEvent low_pass;
    low_pass.kind = kSfzLowPassEvent;
    low_pass.value = SfzLowPassValue(4);
    TrackEvent biquad;
    biquad.kind = kSfzBiquadEvent;
    biquad.value = SfzBiquadValue(2, 64);
    TrackEvent first;
    first.kind = EventKind::kPreset;
    TrackEvent second = first;
    second.preset = 1;
    p.tracks[2] = {low_pass, biquad, first, second};
    p.biquad_types[2] = 2;
    p.level_cut_db = 2.5;
    MidiReport midi;
    midi.bend_ranges[2] = 2;

    const SfzInstruments sfz = BuildSfz(p, midi, banks, "TITLE", {"first", "second"});

    XSFLIB_CHECK(sfz.files.size() == 1 && sfz.samples.empty());
    if (sfz.files.size() != 1)
    {
        return;
    }

    const SfzFile& f = sfz.files[0];
    auto has = [&](const std::string& s)
    {
        return f.text.find(s) != std::string::npos;
    };
    XSFLIB_CHECK(f.name == "Track 2.sfz");
    XSFLIB_CHECK(has("MIDI gain was reduced by 2.5 dB. Region volumes restore that gain.\n"));
    XSFLIB_CHECK(has("<control>\ndefault_path=../samples/\n"));
    XSFLIB_CHECK(has("bend_up=199.975586 bend_down=-199.975586\n"));

    // CC7, CC10 and CC11 act at once, with CC7 and CC11 squared.
    XSFLIB_CHECK(has("pan_oncc10=100 pan_curvecc10=7 pan_smoothcc10=0\n"));
    XSFLIB_CHECK(
        has("amplitude_oncc7=100 amplitude_curvecc7=4 amplitude_smoothcc7=0 amplitude_oncc11=100 "
            "amplitude_curvecc11=4 amplitude_smoothcc11=0\n"));

    // Low-pass uses filter 1. Controller value 60 selects nominal 80 Hz: 9558 cents below 20 kHz, 8936 on the Switch,
    // the 115 Hz of nn::atk's table, or 9131 on the 3DS. 0 leaves it open.
    XSFLIB_CHECK(low_pass.value == 60);
    XSFLIB_CHECK(has("fil_type=lpf_1p cutoff=20000 cutoff_oncc103=12000 cutoff_curvecc103=8\n"));
    const std::string v060 = ByConsole<std::string>(" v060=-0.7965 ", " v060=-0.744667 ", " v060=-0.760917 ");
    XSFLIB_CHECK(has("<curve> curve_index=8\nv000=0 ") && has(v060));

    // Each preset gets a master. Notes starting with the biquad controller above 0 use high-pass in filter slot 2.
    XSFLIB_CHECK(has("// first\n<master> loprog=0 hiprog=0\n") && has("// second\n<master> loprog=1 hiprog=1\n"));
    XSFLIB_CHECK(has("<group> hicc104=0\n<group> locc104=1 fil2_type=hpf_2p cutoff2=1000 "));
}

// The archive of the tests of a bank's conversion. Its sequence plays program 0 at key 60, then program 2 at key 64.
// Program 1 of the bank holds no instrument. Program 0 plays a mono wave. Program 2 plays a stereo wave, at its
// original key, volume, pan and envelope. `volume` is program 2's volume.
SoundArchive BankTestArchive(uint8_t volume = 100)
{
    const auto table = [](const files::Region& region)
    {
        const files::VelocityTable velocities{files::VelocityTable::kDirect, 0, {{127, region}}};
        return files::KeyTable{files::KeyTable::kDirect, 0, {{127, velocities}}};
    };
    files::Region second;
    second.wave = 1;
    second.original_key = 64;
    second.volume = volume;
    second.pan = 40;
    second.adshr = std::array<uint8_t, 5>{120, 100, 90, 0, 110};

    std::vector<ArchiveFile> archive_files(3);
    archive_files[0].data = Fseq({0x81, 0x00, 0x3c, 0x64, 0x30, 0x81, 0x02, 0x40, 0x64, 0x30, 0xff});
    archive_files[1].data = files::Bank(console->settings.archive.flavor,
                                        {{{0x05000000, 0}, {0x05000000, 1}}, {table({}), std::nullopt, table(second)}});
    archive_files[2].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true),
                                  Pcm16Fwav(32, std::vector<int16_t>(32, -2000), 2, true)});

    return SoundArchive::Load(
        Fsar(archive_files, {.sequences = {{.banks = {0x03000000}}}, .banks = {1}, .wave_archives = {2}}),
        console->settings.archive);
}

// A bank's SoundFont holds each instrument at its program. Each instrument's zones and samples are those of a
// sequence that plays the instrument with a track's starting settings.
void TestBankSoundFontHoldsEachInstrumentAtItsProgram()
{
    const SoundArchive archive = BankTestArchive();
    const auto u16 = [](const uint8_t* p)
    {
        return p[0] | p[1] << 8;
    };

    const BankConversion bank = ConvertBank(archive, 0, console->settings, console->program);
    const Conversion sequence = ConvertSequence(archive, 0);

    const seqlib::sf2::Records v = seqlib::sf2::Read(bank.sf2);
    const seqlib::sf2::Records w = seqlib::sf2::Read(sequence.sf2);
    XSFLIB_CHECK(Sf2Problem(bank.sf2).empty() && bank.warnings.empty() && bank.samples == 3);
    XSFLIB_CHECK(v.Count("phdr", 38) == 3);
    XSFLIB_CHECK(std::string(reinterpret_cast<const char*>(v.Record("phdr", 38, 0))) == "Program 0");
    XSFLIB_CHECK(u16(v.Record("phdr", 38, 0) + 20) == 0 && u16(v.Record("phdr", 38, 0) + 22) == 0);
    XSFLIB_CHECK(std::string(reinterpret_cast<const char*>(v.Record("phdr", 38, 1))) == "Program 2");
    XSFLIB_CHECK(u16(v.Record("phdr", 38, 1) + 20) == 2 && u16(v.Record("phdr", 38, 1) + 22) == 0);
    for (const char* chunk : {"ibag", "igen", "imod", "shdr", "smpl"})
    {
        XSFLIB_CHECK(v.chunks.at(chunk) == w.chunks.at(chunk));
    }
}

// A bank's SFZ files play each instrument by itself, under the bank's name and the program. Each file's regions and
// samples are those of a sequence's track file that plays the instrument with a track's starting settings.
void TestBankSfzFilesPlayEachInstrumentByItself()
{
    const SoundArchive archive = BankTestArchive();
    const std::string& name = archive.Banks()[0].name;

    // A preset's text in a track's file runs from its <master> line to the next preset's comment. In an instrument's
    // file, it follows <global>. A track's file numbers the presets in the order that the sequence plays them. Program
    // 2 of the bank is preset 1 there.
    const auto preset = [](const std::string& text, const std::string& after)
    {
        const std::size_t found = text.find(after);
        if (found == std::string::npos)
        {
            return std::string("no preset");
        }

        const std::size_t start = found + after.size();
        const std::size_t end = text.find("\n// ", start);
        return text.substr(start, end == std::string::npos ? end : end - start);
    };

    const BankConversion bank = ConvertBank(archive, 0, console->settings, console->program);
    const Conversion sequence = ConvertSequence(archive, 0);

    XSFLIB_CHECK(bank.sfz.files.size() == 2 && sequence.sfz.files.size() == 1);
    if (bank.sfz.files.size() != 2 || sequence.sfz.files.size() != 1)
    {
        return;
    }
    const std::string& track = sequence.sfz.files[0].text;
    const std::string& first = bank.sfz.files[0].text;
    const std::string& second = bank.sfz.files[1].text;
    const std::string global = "amplitude_smoothcc11=0\n\n";
    XSFLIB_CHECK(bank.sfz.files[0].name == seqlib::SafeFileName(name) + "_0.sfz");
    XSFLIB_CHECK(bank.sfz.files[1].name == seqlib::SafeFileName(name) + "_2.sfz");
    XSFLIB_CHECK(first.starts_with("// Program 0 of " + name + ", converted by " + console->program + ".\n\n"));
    XSFLIB_CHECK(first.find("<control>\ndefault_path=../samples/\n") != std::string::npos);
    XSFLIB_CHECK(first.find("bend_up=199.975586 bend_down=-199.975586\n") != std::string::npos);
    XSFLIB_CHECK(first.find("<master>") == std::string::npos && first.find("<region> ") != std::string::npos);
    XSFLIB_CHECK(preset(first, global) == preset(track, "<master> loprog=0 hiprog=0\n"));
    XSFLIB_CHECK(preset(second, global) == preset(track, "<master> loprog=1 hiprog=1\n"));
    XSFLIB_CHECK(bank.sfz.samples.size() == 3 && sequence.sfz.samples.size() == 3);
    for (std::size_t i = 0; i < bank.sfz.samples.size() && i < sequence.sfz.samples.size(); i++)
    {
        XSFLIB_CHECK(bank.sfz.samples[i].name == sequence.sfz.samples[i].name);
        XSFLIB_CHECK(bank.sfz.samples[i].wav == sequence.sfz.samples[i].wav);
    }
}

// Programs above 127 take the SoundFont's next banks: program 130 is program 2 of bank 1. The SFZ files' names pad the
// programs to the digits of the last one.
void TestBankProgramsAbove127TakeTheNextBank()
{
    const files::VelocityTable velocities{files::VelocityTable::kDirect, 0, {{127, files::Region{}}}};
    const files::KeyTable keys{files::KeyTable::kDirect, 0, {{127, velocities}}};
    std::vector<std::optional<files::KeyTable>> instruments(131);
    instruments[0] = keys;
    instruments[130] = keys;
    std::vector<ArchiveFile> archive_files(2);
    archive_files[0].data = files::Bank(console->settings.archive.flavor, {{{0x05000000, 0}}, instruments});
    archive_files[1].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive =
        SoundArchive::Load(Fsar(archive_files, {.banks = {0}, .wave_archives = {1}}), console->settings.archive);
    const auto u16 = [](const uint8_t* p)
    {
        return p[0] | p[1] << 8;
    };

    const BankConversion bank = ConvertBank(archive, 0, console->settings, console->program);

    const seqlib::sf2::Records v = seqlib::sf2::Read(bank.sf2);
    XSFLIB_CHECK(Sf2Problem(bank.sf2).empty() && v.Count("phdr", 38) == 3);
    XSFLIB_CHECK(u16(v.Record("phdr", 38, 1) + 20) == 2 && u16(v.Record("phdr", 38, 1) + 22) == 1);
    XSFLIB_CHECK(bank.sfz.files.size() == 2 && bank.sfz.files.front().name == "BANK_0_000.sfz" &&
                 bank.sfz.files.back().name == "BANK_0_130.sfz");
}

// A region above volume 127 attenuates every zone of the SoundFont, as in a sequence's SoundFont, and the conversion
// says so. A bank that the archive lacks throws.
void TestLoudBankIsAttenuatedAndAMissingBankThrows()
{
    const SoundArchive archive = BankTestArchive(254);
    bool threw = false;

    const BankConversion bank = ConvertBank(archive, 0, console->settings, console->program);
    const Conversion sequence = ConvertSequence(archive, 0);
    try
    {
        ConvertBank(archive, 1, console->settings, console->program);
    }
    catch (const std::out_of_range&)
    {
        threw = true;
    }

    XSFLIB_CHECK(bank.warnings ==
                 std::vector<std::string>{
                     "region volume exceeds 127 (SoundFont attenuated by 6.0 dB; SFZ keeps the original volume)"});
    XSFLIB_CHECK(seqlib::sf2::Read(bank.sf2).chunks.at("igen") == seqlib::sf2::Read(sequence.sf2).chunks.at("igen"));
    XSFLIB_CHECK(threw);
}

void TestDirectVelocityTableTakesVelocitiesAbove127()
{
    // With velocity range 255 (0xb3), key 60 at velocity 127 plays at velocity 255, and at velocity 255 at 512. Like
    // the game's GetRegion (0x022BBA30), the bank's direct velocity table gives its region for any velocity. Both notes
    // play.
    auto note_ons = [](uint8_t velocity)
    {
        const Performance p = PerformOnBank({0xb3, 0xff, 0x3c, velocity, 0x18, 0xff}, OneRegionFbnk({{0x05000000, 0}}));
        return std::ranges::count_if(p.tracks[0], [](const TrackEvent& e) { return e.kind == EventKind::kNoteOn; });
    };

    XSFLIB_CHECK(note_ons(0x7f) == 1 && note_ons(0xff) == 1);
}

void TestSfzRegionFollowsLinearInterpolation()
{
    // A region that selects linear interpolation (1) on AX gets sfizz's linear one. The SoundFont can't follow it. The
    // performance therefore reports it. A region that selects the 4-tap filters (0) keeps sfizz's default.
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

        const bool reported =
            p.approximations.contains("linear sample interpolation (SoundFont uses the synth's interpolation)");
        return std::pair{sfz.files.size() == 1 && sfz.files[0].text.find(" sample_quality=1") != std::string::npos,
                         reported};
    };

    XSFLIB_CHECK(convert(1) == std::pair(true, true) && convert(0) == std::pair(false, false));
}

void TestSfzCommentKeepsTheNameOnItsLine()
{
    // A newline in a sequence name could end its comment and inject SFZ opcodes. Test without banks. Any region would
    // then have come from the name.
    const SoundArchive archive = SoundArchive::Load(Fsar({}), console->settings.archive);
    BankSet banks(archive, SoundInfo{}, console->settings);
    Performance p;
    p.presets.resize(1);
    TrackEvent preset;
    preset.kind = EventKind::kPreset;
    p.tracks[0] = {preset};
    MidiReport midi;
    midi.bend_ranges[0] = 2;

    const SfzInstruments sfz = BuildSfz(p, midi, banks, "A\r\n<region> sample=x.wav", {"b0 p0"});

    XSFLIB_CHECK(sfz.files.size() == 1);
    for (const SfzFile& f : sfz.files)
    {
        XSFLIB_CHECK(f.text.starts_with("// Track 0 of A  <region> sample=x.wav, converted by " + console->program));
        XSFLIB_CHECK(f.text.find("\n<region>") == std::string::npos);
    }
}

// The tests that run on the Wii U: those of its engine, and those that hold on several consoles.
const Test kWiiUTests[] = {
    TestEnvelopeValuesMatchTheGame,
    TestInstantEnvelopeIsAnOrgan,
    TestDecayTakesTheGamesTime,
    TestSlowDecaySplitsItsError,
    TestSfzDecayTakesTheGamesTime,
    TestEnvelopeSimulationSustainsAndReleases,
    TestNoteIsQuietOnlyAfterItsAttack,
    TestTrackFiltersFollowTheGame,
    TestSoundFontFilterStandsInForTheGames,
    TestSfzFilterFollowsTheGame,
    TestMissingBlockIsReported,
    TestShortFileIsReported,
    TestBrokenBankAndWaveReferencesAreReported,
    TestLittleEndianFilesAreRead,
    TestCallsNestTenDeep,
    TestNotesKeepTheirCommands,
    TestMixChangesAreRecorded,
    TestWiiUCommandsAreRead,
    TestVariableShiftsAreThePowerPcs,
    TestSequenceStartingPastItsEndPlaysNothing,
    TestBankSlotPastTheFourthHasNoBank,
    TestOnlyAJumpBackToPlayedCommandsIsALoop,
    TestMainLoopWaitsForAFadeTooLoudForMidi,
    TestMainLoopWaitsForALegatosFade,
    TestLegatoKeepsTheRegionsVolume,
    TestLegatoOnAnotherRegionKeepsTheFirstsVolume,
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
    TestSweepBelowHearingLeavesTheNewerNoteItsPitch,
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
    TestOneShotWaveEndsAsAxPlaysIt,
    TestTicksFallInTheGamesSoundFrames,
    TestOddArgumentsAreReadAsTheGameReadsThem,
    TestSequenceThatStopsAdvancingPlaysOut,
    TestTieLoopThatHoldsEndsAsAHeldSound,
    TestTieLoopOverAOneShotWaveStaysALoop,
    TestClosingTickReachesTheReleaseTail,
    TestSilenceAtTheTimeLimitEndsWithTheLastSound,
    TestOneShotWavePlaysOutAtItsPitch,
    TestTimedSweepGlidesOnWhenTheSequenceStops,
    TestTimedSweepMovesEveryFrame,
    TestTimedSweepGoesOnAfterItsTrackCloses,
    TestHeldNoteHoldsUntilStopped,
    TestNegativeWaitBlocksTheTrackForEver,
    TestHoldStartsOnceTheVolumesStopMoving,
    TestLfoSetAfterANoteInItsTickReachesItsPreset,
    TestVolumeLfoChangedAtDepth0IsReportedWhenHeard,
    TestPitchLfoChangedDuringANoteGoesOutAsPitchBends,
    TestRandomLfoCurveInPitchBendsIsReportedOnce,
    TestDeepTremoloIsKeptTo6dB,
    TestNoteReleasedInItsAttackKeepsTheGamesLevel,
    TestTrackWaitingForAVariableGoesOn,
    TestLoopStartSendsTheVolumeTheGameRestores,
    TestLoopMarksAPassThatPlaysAsTheNext,
    TestLoopDoesNotStrikeATiedNoteAgain,
    TestLoopThatNoPassRepeatsKeepsItsFirstPass,
    TestPassOnAnotherPartPlaysAsTheNext,
    TestSequenceThatFinishesHasNoLoop,
    TestVariableOptionSetsAVariableBeforeTheStart,
    TestVariableChangeComesBeforeTheTicksOfItsFrame,
    TestMainLoopStartsAfterTheLastVariableChange,
    TestWaitingForAVariableIsNoLoop,
    TestPollingInASubroutineAfterAOneShotIsNoLoop,
    TestLoopLeftBeforeASilentTimeLimitIsDropped,
    TestCountedLoopPastTheTimeLimitPlaysAsAnEndlessLoop,
    TestTiedMelodyBelowHearingIsNoLoop,
    TestLegatoBelowHearingKeepsItsMidiNote,
    TestPollAfterTheSoundIsNoLoop,
    TestSilentPassesThatChangeSettingsEndWithTheSound,
    TestSequenceWithoutNotesEndsAtItsStart,
    TestTrackStartsNoMoreNotesATickThanTheGameHasVoices,
    TestIdleTrackDoesNotOwnTheLoopWhileAnotherPlays,
    TestTrackPlayingANoteEachTimeAVariableIsSetGoesOn,
    TestVariableSetAfterAJumpBackBelongsToTheNewPass,
    TestCheckArchivePassesAValidArchive,
    TestRunArchiveCheckGivesMainsExitCode,
    TestSfzProblemFindsABrokenWav,
    TestCheckConversionFindsAFrameAfterItsTick,
    TestTrackWaitingInSilenceDoesNotHoldOffTheLoop,
    TestCountdownInsideALoopIsAnInnerLoop,
    TestTrackThatLeavesItsLoopForAnIdleLoopEnds,
    TestLoopOfAnEndedTrackIsNoLoop,
    TestTrackCountingDownInASubroutinePlaysOn,
    TestJumpBackIntoAFinishedCallEndsNoPass,
    TestInnerLoopCountsAgainOnceTheOuterLoopTakesLong,
    TestTrackPlayingNotesWithoutWaitingIsStopped,
    TestNoteWithoutAFreeChannelIsDropped,
    TestSoundsOwnPanCurveIsReported,
    TestSoundFontWriterOutputIsValid,
    TestSoundFontWithoutSamplesHasSampleData,
    TestMidiWriterLayersMarkersAndBendRange,
    TestMidiWriterGivesPartsChannelsOfTheirOwn,
    TestMidiWriterHasAtLeast7680TicksPerQuarterNote,
    TestMidiWriterSplitsLongDeltaTimes,
    TestMidiBendUpByAWholeRangeIsExact,
    TestMidiEventsGoOnSoundFrames,
    TestMidiWriterSendsTheFilterToEveryLayer,
    TestMidiWriterListsChannel10Presets,
    TestSfzFileHasTheTracksPresetsAndFilters,
    TestDirectVelocityTableTakesVelocitiesAbove127,
    TestSfzRegionFollowsLinearInterpolation,
    TestSfzCommentKeepsTheNameOnItsLine,
    TestBankSoundFontHoldsEachInstrumentAtItsProgram,
    TestBankSfzFilesPlayEachInstrumentByItself,
    TestBankProgramsAbove127TakeTheNextBank,
    TestLoudBankIsAttenuatedAndAMissingBankThrows,
};

// The tests that run on the Switch.
const Test kSwitchTests[] = {
    TestEnvelopeValuesMatchTheGame,
    TestInstantEnvelopeIsAnOrgan,
    TestDecayTakesTheGamesTime,
    TestSlowDecaySplitsItsError,
    TestSfzDecayTakesTheGamesTime,
    TestEnvelopeSimulationSustainsAndReleases,
    TestNoteIsQuietOnlyAfterItsAttack,
    TestTrackFiltersFollowTheGame,
    TestSoundFontFilterStandsInForTheGames,
    TestSfzFilterFollowsTheGame,
    TestMissingBlockIsReported,
    TestShortFileIsReported,
    TestBrokenBankAndWaveReferencesAreReported,
    TestCallsNestTenDeep,
    TestNotesKeepTheirCommands,
    TestMixChangesAreRecorded,
    TestWiiUCommandsAreRead,
    TestSequenceStartingPastItsEndPlaysNothing,
    TestBankSlotPastTheFourthHasNoBank,
    TestOnlyAJumpBackToPlayedCommandsIsALoop,
    TestMainLoopWaitsForAFadeTooLoudForMidi,
    TestMainLoopWaitsForALegatosFade,
    TestLegatoKeepsTheRegionsVolume,
    TestLegatoOnAnotherRegionKeepsTheFirstsVolume,
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
    TestSweepBelowHearingLeavesTheNewerNoteItsPitch,
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
    TestClosingTickReachesTheReleaseTail,
    TestSilenceAtTheTimeLimitEndsWithTheLastSound,
    TestOneShotWavePlaysOutAtItsPitch,
    TestTimedSweepGlidesOnWhenTheSequenceStops,
    TestTimedSweepMovesEveryFrame,
    TestTimedSweepGoesOnAfterItsTrackCloses,
    TestHeldNoteHoldsUntilStopped,
    TestNegativeWaitBlocksTheTrackForEver,
    TestHoldStartsOnceTheVolumesStopMoving,
    TestLfoSetAfterANoteInItsTickReachesItsPreset,
    TestVolumeLfoChangedAtDepth0IsReportedWhenHeard,
    TestPitchLfoChangedDuringANoteGoesOutAsPitchBends,
    TestRandomLfoCurveInPitchBendsIsReportedOnce,
    TestDeepTremoloIsKeptTo6dB,
    TestNoteReleasedInItsAttackKeepsTheGamesLevel,
    TestTrackWaitingForAVariableGoesOn,
    TestLoopStartSendsTheVolumeTheGameRestores,
    TestLoopMarksAPassThatPlaysAsTheNext,
    TestLoopDoesNotStrikeATiedNoteAgain,
    TestLoopThatNoPassRepeatsKeepsItsFirstPass,
    TestPassOnAnotherPartPlaysAsTheNext,
    TestSequenceThatFinishesHasNoLoop,
    TestVariableOptionSetsAVariableBeforeTheStart,
    TestVariableChangeComesBeforeTheTicksOfItsFrame,
    TestMainLoopStartsAfterTheLastVariableChange,
    TestWaitingForAVariableIsNoLoop,
    TestPollingInASubroutineAfterAOneShotIsNoLoop,
    TestLoopLeftBeforeASilentTimeLimitIsDropped,
    TestCountedLoopPastTheTimeLimitPlaysAsAnEndlessLoop,
    TestTiedMelodyBelowHearingIsNoLoop,
    TestLegatoBelowHearingKeepsItsMidiNote,
    TestPollAfterTheSoundIsNoLoop,
    TestSilentPassesThatChangeSettingsEndWithTheSound,
    TestSequenceWithoutNotesEndsAtItsStart,
    TestTrackStartsNoMoreNotesATickThanTheGameHasVoices,
    TestIdleTrackDoesNotOwnTheLoopWhileAnotherPlays,
    TestTrackPlayingANoteEachTimeAVariableIsSetGoesOn,
    TestVariableSetAfterAJumpBackBelongsToTheNewPass,
    TestCheckArchivePassesAValidArchive,
    TestRunArchiveCheckGivesMainsExitCode,
    TestSfzProblemFindsABrokenWav,
    TestCheckConversionFindsAFrameAfterItsTick,
    TestTrackWaitingInSilenceDoesNotHoldOffTheLoop,
    TestCountdownInsideALoopIsAnInnerLoop,
    TestTrackThatLeavesItsLoopForAnIdleLoopEnds,
    TestLoopOfAnEndedTrackIsNoLoop,
    TestTrackCountingDownInASubroutinePlaysOn,
    TestJumpBackIntoAFinishedCallEndsNoPass,
    TestInnerLoopCountsAgainOnceTheOuterLoopTakesLong,
    TestSoundsOwnPanCurveIsReported,
    TestSoundFontWriterOutputIsValid,
    TestSoundFontWithoutSamplesHasSampleData,
    TestMidiWriterLayersMarkersAndBendRange,
    TestMidiWriterGivesPartsChannelsOfTheirOwn,
    TestMidiWriterHasAtLeast7680TicksPerQuarterNote,
    TestMidiWriterSplitsLongDeltaTimes,
    TestMidiBendUpByAWholeRangeIsExact,
    TestMidiEventsGoOnSoundFrames,
    TestMidiWriterSendsTheFilterToEveryLayer,
    TestMidiWriterListsChannel10Presets,
    TestSfzFileHasTheTracksPresetsAndFilters,
    TestDirectVelocityTableTakesVelocitiesAbove127,
    TestSfzCommentKeepsTheNameOnItsLine,
    TestBankSoundFontHoldsEachInstrumentAtItsProgram,
    TestBankSfzFilesPlayEachInstrumentByItself,
    TestBankProgramsAbove127TakeTheNextBank,
    TestLoudBankIsAttenuatedAndAMissingBankThrows,
};

// The tests that run on the 3DS.
const Test kCtrTests[] = {
    TestEnvelopeValuesMatchTheGame,
    TestNotesKeepTheirCommands,
    TestInstantEnvelopeIsAnOrgan,
    TestTrackFiltersFollowTheGame,
    TestSoundFontFilterStandsInForTheGames,
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
    TestTimedSweepMovesEveryFrame,
    TestHeldNoteHoldsUntilStopped,
    TestDeepTremoloIsKeptTo6dB,
    TestLoopStartSendsTheVolumeTheGameRestores,
    TestLoopDoesNotStrikeATiedNoteAgain,
    TestLoopThatNoPassRepeatsKeepsItsFirstPass,
    TestPassOnAnotherPartPlaysAsTheNext,
    TestSequenceThatFinishesHasNoLoop,
    TestPollingInASubroutineAfterAOneShotIsNoLoop,
    TestLoopLeftBeforeASilentTimeLimitIsDropped,
    TestCountedLoopPastTheTimeLimitPlaysAsAnEndlessLoop,
    TestTiedMelodyBelowHearingIsNoLoop,
    TestPollAfterTheSoundIsNoLoop,
    TestSilentPassesThatChangeSettingsEndWithTheSound,
    TestIdleTrackDoesNotOwnTheLoopWhileAnotherPlays,
    TestVariableSetAfterAJumpBackBelongsToTheNewPass,
    TestTrackWaitingInSilenceDoesNotHoldOffTheLoop,
    TestCountdownInsideALoopIsAnInnerLoop,
    TestTrackThatLeavesItsLoopForAnIdleLoopEnds,
    TestLoopOfAnEndedTrackIsNoLoop,
    TestTrackCountingDownInASubroutinePlaysOn,
    TestJumpBackIntoAFinishedCallEndsNoPass,
    TestInnerLoopCountsAgainOnceTheOuterLoopTakesLong,
    TestMidiBendUpByAWholeRangeIsExact,
    TestMidiWriterSendsTheFilterToEveryLayer,
    TestMidiWriterListsChannel10Presets,
    TestDirectVelocityTableTakesVelocitiesAbove127,
    TestBrokenBankAndWaveReferencesAreReported,
    TestSequenceStartingPastItsEndPlaysNothing,
    TestBankSlotPastTheFourthHasNoBank,
    TestOnlyAJumpBackToPlayedCommandsIsALoop,
    TestMainLoopWaitsForAFadeTooLoudForMidi,
    TestMainLoopWaitsForALegatosFade,
    TestLegatoBelowHearingKeepsItsMidiNote,
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
    TestClosingTickReachesTheReleaseTail,
    TestSilenceAtTheTimeLimitEndsWithTheLastSound,
    TestOneShotWavePlaysOutAtItsPitch,
    TestTimedSweepGlidesOnWhenTheSequenceStops,
    TestTimedSweepGoesOnAfterItsTrackCloses,
    TestLfoSetAfterANoteInItsTickReachesItsPreset,
    TestVolumeLfoChangedAtDepth0IsReportedWhenHeard,
    TestPitchLfoChangedDuringANoteGoesOutAsPitchBends,
    TestNoteReleasedInItsAttackKeepsTheGamesLevel,
    TestTrackWaitingForAVariableGoesOn,
    TestLoopMarksAPassThatPlaysAsTheNext,
    TestVariableOptionSetsAVariableBeforeTheStart,
    TestVariableChangeComesBeforeTheTicksOfItsFrame,
    TestMainLoopStartsAfterTheLastVariableChange,
    TestWaitingForAVariableIsNoLoop,
    TestSequenceWithoutNotesEndsAtItsStart,
    TestTrackStartsNoMoreNotesATickThanTheGameHasVoices,
    TestTrackPlayingANoteEachTimeAVariableIsSetGoesOn,
    TestTrackPlayingNotesWithoutWaitingIsStopped,
    TestSoundsOwnPanCurveIsReported,
    TestDecayTakesTheGamesTime,
    TestSlowDecaySplitsItsError,
    TestSfzDecayTakesTheGamesTime,
    TestEnvelopeSimulationSustainsAndReleases,
    TestNoteIsQuietOnlyAfterItsAttack,
    TestSfzFilterFollowsTheGame,
    TestMissingBlockIsReported,
    TestShortFileIsReported,
    TestSoundFontWriterOutputIsValid,
    TestSoundFontWithoutSamplesHasSampleData,
    TestMidiWriterLayersMarkersAndBendRange,
    TestMidiWriterGivesPartsChannelsOfTheirOwn,
    TestMidiWriterHasAtLeast7680TicksPerQuarterNote,
    TestMidiEventsGoOnSoundFrames,
    TestMidiWriterSplitsLongDeltaTimes,
    TestSfzFileHasTheTracksPresetsAndFilters,
    TestSfzCommentKeepsTheNameOnItsLine,
    TestBankSoundFontHoldsEachInstrumentAtItsProgram,
    TestBankSfzFilesPlayEachInstrumentByItself,
    TestBankProgramsAbove127TakeTheNextBank,
    TestLoudBankIsAttenuatedAndAMissingBankThrows,
};

// The tests that run on the Wii as well: those that build no archive. sequence_export_revolution_test.cpp builds the
// Wii's RSAR archives for its tests.
const Test kWiiTests[] = {
    TestInstantEnvelopeIsAnOrgan,  TestDecayTakesTheGamesTime,        TestSlowDecaySplitsItsError,
    TestSfzDecayTakesTheGamesTime, TestNoteIsQuietOnlyAfterItsAttack,
};

// length_cases.h's Code writes a sequence's variable-length numbers in MIDI's format. seqlib's AppendVlq writes the
// same format. length_cases.h also serves tests that don't link seqlib. The two writers therefore stay apart. A fix to
// one of them fails this check until the other matches.
void TestLengthCasesWriteVariableLengthNumbersAsSeqlib()
{
    using ntdlib::testing::length_cases::Code;

    // A wait is its command byte, then the number of ticks.
    const auto prologue = static_cast<std::ptrdiff_t>(Code(true).Bytes().size()) + 1;
    for (const uint32_t value :
         {0x0u, 0x7Fu, 0x80u, 0x3FFFu, 0x4000u, 0x1FFFFFu, 0x200000u, 0xFFFFFFFu, 0x10000000u, 0xFFFFFFFFu})
    {
        Code code(true);
        code.Wait(value);
        const std::vector<uint8_t> ours(code.Bytes().begin() + prologue, code.Bytes().end());
        std::vector<uint8_t> midi;
        seqlib::midi::AppendVlq(midi, value);
        XSFLIB_CHECK(ours == midi);
    }
}

} // namespace

int main()
{
    RunOn(WiiU(), kWiiUTests);
    RunOn(Switch(), kSwitchTests);
    RunOn(Ctr(), kCtrTests);
    RunOn(Wii(), kWiiTests);
    TestLengthCasesWriteVariableLengthNumbersAsSeqlib();

    return Report();
}
