// SPDX-License-Identifier: MIT

// Tests of sequence_export on the Wii's engine (Engine::kRevolution), with the Wii's settings (Wii() in
// sequence_export_test.h) and synthetic RSAR archives (sequence_export_revolution_test.h): the archive and bank readers
// as the conversion uses them, the notes, legatos, ties and sweeps.

#include "sequence_export_revolution_test.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <numbers>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "seqlib/midi/performance.h"
#include "seqlib/midi/reader.h"
#include "seqlib/pcm.h"
#include "seqlib/sf2/reader.h"
#include "seqlib/sf2/soundfont.h"
#include "seqlib/sf2/units.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/rsar/decode.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/envelope.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/performer.h"
#include "ntdlib/sequence_export/recording_engine.h"
#include "ntdlib/sequence_export/rsar_instruments.h"
#include "ntdlib/sequence_export/settings.h"
#include "ntdlib/sequence_export/sfz.h"
#include "sequence_export/wii_pan.h"
#include "sequence_export_test.h"

using namespace ntdlib::sequence_export::wii_tests;

namespace
{

void TestAdpcmLoopIsInSamples()
{
    // A DSP-ADPCM wave gives its loop as nibble addresses, two header nibbles to each 8-byte frame of 14 samples: the
    // loop start's address 18 is sample 14, the second frame's first, and the end's address 47 the last sample of a
    // 42-sample wave.
    Writer w = FileHeader("RWAV", 0x0102, 2);
    const std::size_t info = StartBlock(w, "INFO");
    w.Add(WaveInfoSize(1) + 0x30);
    WaveInfo(w, info + 8, 1, 1, true, 32000, 0, 0);
    w.U8(info + 8, 2);
    w.U32(info + 8 + 8, 18);
    w.U32(info + 8 + 0xC, 47);
    w.U32(info + 8 + 0x1C + 4 + 4, static_cast<uint32_t>(WaveInfoSize(1))); // the ADPCM info, after the wave info
    Block(w, 0, info);
    const std::size_t data = StartBlock(w, "DATA");
    w.Add(24);
    Block(w, 1, data);

    const Pcm pcm = ToPcm(DecodeWave(Wave::ParseFile(w.bytes)));

    XSFLIB_CHECK(pcm.loop && pcm.loop_start == 14 && pcm.loop_end == 42 && pcm.channels[0].size() == 42);
}

void TestVersionOneBankDescribesItsWaves()
{
    // An RBNK 1.1 bank's WAVE block describes its waves: two four-sample waves. Their samples are the raw wave data.
    std::vector<ArchiveFile> files(2);
    files[0].data = Rseq({0xff});
    files[1].data = Rbnk({TestInstrument{{{.wave_index = 1}}}}, 0x0101, 2, 4);
    Writer samples;
    AddSamples(samples, {1, 2, 3, 4, -5, -6, -7, -8});
    files[1].wave_data = samples.bytes;
    const SoundArchive archive = SoundArchive::Load(Rsar(files, {.version = 0x0101, .sequences = {{}}, .banks = {1}}));
    BankSet banks(archive, archive.Sounds()[0]);

    const auto* second = banks.GetWave(1);

    XSFLIB_CHECK(banks.GetBank() && banks.GetBank()->HasWaveBlock() && banks.Errors().empty());
    XSFLIB_CHECK(second && second->channels[0] == std::vector<int16_t>({-5, -6, -7, -8}));
}

void TestBankSelectsRegionsAsTheGameDoes()
{
    // Program 0 splits at key 59 into regions playing waves 0 and 1. Program 1's only region has no wave (a negative
    // index) and plays nothing. Program 2 doesn't exist and plays nothing either.
    const std::vector<uint8_t> rbnk =
        Rbnk({TestInstrument{{{.wave_index = 0}, {.wave_index = 1}}, {59, 127}}, TestInstrument{{{.wave_index = -1}}}});

    const Bank bank = Bank::Parse(rbnk);

    XSFLIB_CHECK(bank.InstrumentCount() == 2);
    XSFLIB_CHECK(bank.Find(0, 59, 100) && bank.Find(0, 59, 100)->wave_index == 0);
    XSFLIB_CHECK(bank.Find(0, 60, 1) && bank.Find(0, 60, 1)->wave_index == 1);
    XSFLIB_CHECK(!bank.Find(1, 60, 100) && !bank.Find(2, 60, 100));

    // The SoundFont gets a zone for each region, with the keys and velocities that play it.
    const std::vector<PlayedRanges> ranges = RegionRanges(bank, 0);
    const std::vector<std::pair<int, int>> kAll = {{0, 127}};
    XSFLIB_CHECK(ranges.size() == 2 && ranges[0].keys == (std::vector<std::pair<int, int>>{{0, 59}}) &&
                 ranges[1].keys == (std::vector<std::pair<int, int>>{{60, 127}}) && ranges[1].velocities == kAll);
    XSFLIB_CHECK(RegionRanges(bank, 1).empty());
}

void TestOldBankHasNoVolumeOrTune()
{
    // BankFileReader::ReadInstInfo gives a region of a bank older than version 1.1 volume 127 and tune 1.
    const TestRegion kRegion{.volume = 64, .tune = 2.0f};

    const Bank old = Bank::Parse(Rbnk({TestInstrument{{kRegion}}}, 0x0100));
    const Bank current = Bank::Parse(Rbnk({TestInstrument{{kRegion}}}, 0x0101));

    XSFLIB_CHECK(old.Find(0, 60, 100)->volume == 127 && old.Find(0, 60, 100)->tune == 1.0f);
    XSFLIB_CHECK(current.Find(0, 60, 100)->volume == 64 && current.Find(0, 60, 100)->tune == 2.0f);
}

void TestFilesAreFoundInGroups()
{
    // Group 0 holds files 0 and 1, group 1 files 1 and 2; file 3 is in no group. Each file's first copy is used.
    std::vector<ArchiveFile> files(4);
    files[0].data = Bytes("zero");
    files[1].data = Bytes("one");
    files[1].wave_data = Bytes("waves");
    files[2].data = Bytes("two");
    files[3].data = Bytes("three");

    const SoundArchive a = SoundArchive::Load(Rsar(files, {.groups = {{0, 1}, {1, 2}}}));

    XSFLIB_CHECK(Text(a.FileData(0)) == "zero" && Text(a.FileData(1)) == "one" && Text(a.FileData(2)) == "two");
    XSFLIB_CHECK(Text(a.WaveData(1)) == "waves" && a.WaveData(0).empty());
    XSFLIB_CHECK(a.FileData(3).empty() && !a.FileMissing(3) && !a.Truncated());
}

void TestDamagedNameTableLeavesSoundsNumbered()
{
    // Point the one name of an archive past its end. The archive still loads, and its sound goes by number.
    const std::vector<ArchiveFile> files = {
        {Rseq({0x3c, 0x64, 0x18, 0xff})},
        {Rbnk({TestInstrument{}}), Rwar({Rwav(64, std::vector<int16_t>(64, 1000), 1, true)})}};
    std::vector<uint8_t> bytes = Rsar(files, {.sequences = {{.file_id = 0}}, .banks = {1}, .names = {"SEQ_A"}});
    const std::size_t symb_base = xsflib::LoadBe32(&bytes[0x10]) + 8;
    const std::size_t name_offset = symb_base + xsflib::LoadBe32(&bytes[symb_base]) + 4;
    std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(name_offset), 4, uint8_t{0xff});

    const SoundArchive archive = SoundArchive::Load(bytes);
    const Performance p = PerformSound(archive, 0);

    const auto note_ons =
        std::ranges::count_if(p.tracks[0], [](const TrackEvent& e) { return e.kind == EventKind::kNoteOn; });
    XSFLIB_CHECK(archive.NamesDamaged() && archive.Sounds()[0].name == "SEQ_0" && note_ons == 1);
}

void TestTruncatedArchiveKeepsWhatItHolds()
{
    // Cut the archive in the second group's wave data: the first group's files stay whole, and so do the second group's
    // files, but not their wave data.
    std::vector<ArchiveFile> files(2);
    files[0].data = Bytes("first");
    files[0].wave_data = Bytes("first waves");
    files[1].data = Bytes("second");
    files[1].wave_data = Bytes("second waves");
    const std::vector<uint8_t> bytes = Rsar(files, {.groups = {{0}, {1}}});

    const SoundArchive a = SoundArchive::Load(std::vector<uint8_t>(bytes.begin(), bytes.end() - 4));

    XSFLIB_CHECK(a.Truncated());
    XSFLIB_CHECK(Text(a.FileData(0)) == "first" && Text(a.WaveData(0)) == "first waves");
    XSFLIB_CHECK(Text(a.FileData(1)) == "second" && a.WaveData(1).empty() && a.WaveDataMissing(1));
    XSFLIB_CHECK(!a.FileMissing(1) && !a.WaveDataMissing(0));
}

void TestCopyWithWaveDataIsPreferred()
{
    // Group 0 holds the file whole but its wave data past the end; group 1 holds both. Use group 1's copy.
    std::vector<ArchiveFile> files(1);
    files[0].data = Bytes("bank");
    files[0].wave_data = Bytes("waves");
    std::vector<uint8_t> bytes = Rsar(files, {.groups = {{0}, {0}}});
    const SoundArchive whole = SoundArchive::Load(bytes);

    XSFLIB_CHECK(Text(whole.FileData(0)) == "bank" && Text(whole.WaveData(0)) == "waves");
}

void TestTruncatedArchiveWhoseTablesCantBeReadIsReported()
{
    // Cut the archive within the INFO block's tables.
    std::vector<ArchiveFile> files(1);
    files[0].data = Bytes("data");
    const std::vector<uint8_t> bytes = Rsar(files);
    const std::size_t info = xsflib::LoadBe32(&bytes[0x18]);
    const std::size_t cut = info + 0x20;

    const auto load_cut = [&]
    {
        SoundArchive::Load(std::vector<uint8_t>(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(cut)));
    };
    const std::string message = ErrorOf<xsflib::FormatError>(load_cut);

    XSFLIB_CHECK(message.starts_with("truncated archive: " + std::to_string(cut) + " of " +
                                     std::to_string(bytes.size()) + " bytes; cannot read tables (read out of bounds"));
}

void TestOtherPlatformsArchivesAreReported()
{
    auto message_for = [](const char* magic)
    {
        std::vector<uint8_t> bytes(0x40, 0);
        std::memcpy(bytes.data(), magic, 4);

        return ErrorOf<xsflib::FormatError>([&] { SoundArchive::Load(bytes); });
    };

    XSFLIB_CHECK(message_for("CSAR") == "unsupported CSAR archive (3DS); expected RSAR (Wii)");
    XSFLIB_CHECK(message_for("FSAR") == "unsupported FSAR archive (Wii U/Switch); expected RSAR (Wii)");
    XSFLIB_CHECK(message_for("SDAT") == "unsupported SDAT archive (DS); expected RSAR (Wii)");
    XSFLIB_CHECK(message_for("RIFF") == "bad magic, expected RSAR");
}

void TestNamesHaveNoControlCharacters()
{
    // Replace unsafe name characters with '?': newline, terminal escape, DEL, C1 NEL, Unicode line/paragraph separators
    // and invalid UTF-8 (lone byte, overlong encoding, surrogate and truncated character). Preserve valid é and emoji.
    const std::vector<std::pair<std::string, std::string>> kPieces = {
        {"Seq", "Seq"},           {"\n", "?"},
        {"\x1b[2J", "?[2J"},      {"\x7f", "?"},
        {"\xc2\x85", "?"},        {"\xe2\x80\xa8", "?"},
        {"\xe2\x80\xa9", "?"},    {"\xff", "?"},
        {"\xc0\xaf", "??"},       {"\xed\xa0\x80", "???"},
        {"\xc3\xa9", "\xc3\xa9"}, {"\xf0\x9f\x98\x80", "\xf0\x9f\x98\x80"},
        {"\xe2\x80", "??"}};
    std::string name, printable;
    for (const auto& [piece, shown] : kPieces)
    {
        name += piece;
        printable += shown;
    }

    std::vector<ArchiveFile> files(1);
    files[0].data = Rseq({0xff});

    const SoundArchive archive = SoundArchive::Load(Rsar(files, {.sequences = {{}, {}}, .names = {name}}));

    XSFLIB_CHECK(archive.Sounds().size() == 2 && archive.Sounds()[0].name == printable);
    XSFLIB_CHECK(archive.Sounds()[1].name == "SEQ_1");
}

void TestBrokenBankReferencesAreReported()
{
    // Sound 0 names bank 5. No such bank exists. Sound 1 names bank 0. No group holds bank 0's file. Sound 2 names bank
    // 1. Bank 1's region plays wave 3 of an RWAR with one wave.
    std::vector<ArchiveFile> files(3);
    files[0].data = Rseq({0xff});
    files[1].data = Rbnk({TestInstrument{}});
    files[2].data = Rbnk({TestInstrument{{{.wave_index = 3}}}});
    files[2].wave_data = Rwar({Rwav(4, {1, 2, 3, 4})});
    const SoundArchive archive = SoundArchive::Load(
        Rsar(files, {.sequences = {{0, 0, 5}, {0, 0, 0}, {0, 0, 1}}, .banks = {1, 2}, .groups = {{0, 2}}}));
    BankSet none(archive, archive.Sounds()[0]);
    BankSet unstored(archive, archive.Sounds()[1]);
    BankSet missing_wave(archive, archive.Sounds()[2]);

    const auto* wave = missing_wave.GetWave(3);
    missing_wave.GetWave(3); // repeat the lookup without reporting the error again

    XSFLIB_CHECK(none.Errors() == std::vector<std::string>{"bank 5 doesn't exist (the archive has 2)"});
    XSFLIB_CHECK(unstored.Errors() == std::vector<std::string>{"bank BANK_0 is not stored in the archive"});
    XSFLIB_CHECK(!wave && missing_wave.Errors() ==
                              std::vector<std::string>{"bank BANK_1's wave archive has no wave 3 (it has 1)"});
}

void TestWaveDataPastTheEndIsReported()
{
    // The bank's file is whole, but its wave data is past the end of the truncated archive.
    std::vector<ArchiveFile> files = LoopingWaveFiles({0x3c, 0x64, 0x30, 0xff});
    const std::vector<uint8_t> bytes = Rsar(files, {.sequences = {{0, 0, 0}}, .banks = {1}});
    const SoundArchive archive = SoundArchive::Load(std::vector<uint8_t>(bytes.begin(), bytes.end() - 8));
    BankSet banks(archive, archive.Sounds()[0]);

    const Performance p = Perform(archive, 0, banks, {});

    XSFLIB_CHECK(archive.Truncated() && NoteCount(p) == 0);
    XSFLIB_CHECK(banks.Errors() == std::vector<std::string>{"bank BANK_0's waves extend past the truncated archive"});
}

void TestSequenceStartingPastItsEndPlaysNothing()
{
    // Start track 0 at 2^32 - 2, beyond the bytecode. The track finishes immediately.
    std::vector<ArchiveFile> files(1);
    files[0].data = Rseq({0x3c, 0x64, 0x30, 0xff});
    const SoundArchive archive = SoundArchive::Load(Rsar(files, {.sequences = {{0, 0xfffffffe}}}));

    const Performance p = PerformSound(archive, 0);

    XSFLIB_CHECK(p.end_tick == 0 && p.presets.empty() && p.warnings.empty());
}

void TestOnlyAJumpBackToPlayedCommandsIsALoop()
{
    // The first sequence skips a FIN, waits a tick and jumps back to the FIN it skipped. The second plays key 60 for a
    // tick and jumps back to its start, a loop it repeats until the performance reaches 10 s.
    const SoundArchive archive = SequenceArchive({{0x89, 0x00, 0x00, 0x05, 0xff, 0x80, 0x01, 0x89, 0x00, 0x00, 0x04},
                                                  {0x3c, 0x64, 0x01, 0x89, 0x00, 0x00, 0x00}});

    const Performance once = PerformSound(archive, 0);
    const Performance looping = PerformSound(archive, 1);

    XSFLIB_CHECK(!once.loop && once.loops == 0);
    XSFLIB_CHECK(looping.loop && looping.loop->start == 0 && looping.loop->end == 1);
    XSFLIB_CHECK(looping.seconds >= 10.0 && looping.seconds < 10.1);
}

void TestMainLoopWaitsForAFade()
{
    // Track 0 opens track 1, fades from volume 127 to 64 over 2000 ticks and then holds a looping note. The fade uses a
    // time prefix, a command of the builds from 2008. Track 1 loops every 48 ticks. The main loop starts once the fade
    // has ended.
    const Performance p = PerformCommands({0x88, 0x01, 0x00, 0x00, 0x0e, 0xa3, 0xc1, 0x40, 0x07, 0xd0,
                                           0x3c, 0x7f, 0x00, 0xff, 0x80, 0x30, 0x89, 0x00, 0x00, 0x0e},
                                          {.track_flags = 3});

    XSFLIB_CHECK(p.loop && p.loop->start >= 2000 && p.loop->end - p.loop->start == 48);
}

void TestLevelTooLoudForMidiIsCut()
{
    // Volume 64 and volume2 255 play a note at full level. The voice holds it there. With CC7 at 64, that level
    // requires CC11 252: more than MIDI has. Every CC11 is scaled down to fit, and the SFZ files add the cut back.
    const Performance p = PerformCommands({0xc1, 0x40, 0xd5, 0xff, 0x3c, 0x7f, 0x30, 0xff});

    const std::vector<std::pair<uint32_t, int>> kVolume = {{0, 64}};
    const std::vector<std::pair<uint32_t, int>> kExpression = {{0, 127}};
    XSFLIB_CHECK(Controls(p, EventKind::kVolume) == kVolume && Controls(p, EventKind::kExpression) == kExpression);
    XSFLIB_CHECK(std::fabs(p.level_cut_db - 40.0 * std::log10(127.0 / 64.0)) < 1e-9);
}

void TestFullLevelStopsTheTrackVolumes()
{
    // Volume 255 would play a note at four times full level, but the voice stops it at full level. The fade from 255 to
    // 128 that follows makes no difference and requires no level cut.
    const Performance p = PerformCommands({0xc1, 0xff, 0x3c, 0x7f, 0x18, 0xc1, 0x80, 0x3c, 0x7f, 0x18, 0xff});

    const std::vector<std::pair<uint32_t, int>> kVolume = {{0, 127}};
    const std::vector<std::pair<uint32_t, int>> kExpression = {{0, 127}};
    XSFLIB_CHECK(Controls(p, EventKind::kVolume) == kVolume && Controls(p, EventKind::kExpression) == kExpression);
    XSFLIB_CHECK(p.level_cut_db == 0.0);
}

void TestTrackThatTheGameSilencesKeepsItsNotes()
{
    // Track 0 opens track 1, and each plays a note at full level. A game's code can set a track's volume to 0 through
    // the sound's handle (PerformOptions::tracks). The silenced track's note stays in the file, and CC11 holds it at 0.
    // Track 1 plays on at full level.
    const std::vector<uint8_t> kCommands = {0x88, 0x01, 0x00, 0x00, 0x09, 0x3c, 0x7f,
                                            0x18, 0xff, 0x40, 0x7f, 0x18, 0xff};
    PerformOptions silenced;
    silenced.tracks = 0xfffe;

    const Performance all = PerformCommandsWith(kCommands, {}, {.track_flags = 3});
    const Performance some = PerformCommandsWith(kCommands, silenced, {.track_flags = 3});

    const std::vector<std::tuple<uint32_t, int, int>> kFirst = {{0, 60, 127}};
    const std::vector<std::tuple<uint32_t, int, int>> kSecond = {{0, 64, 127}};
    const std::vector<std::pair<uint32_t, int>> kFullLevel = {{0, 127}};
    const std::vector<std::pair<uint32_t, int>> kSilent = {{0, 0}};
    XSFLIB_CHECK(NoteOns(all, 0) == kFirst && NoteOns(all, 1) == kSecond &&
                 Controls(all, EventKind::kExpression, 0) == kFullLevel &&
                 Controls(all, EventKind::kExpression, 1) == kFullLevel);
    XSFLIB_CHECK(NoteOns(some, 0) == kFirst && NoteOns(some, 1) == kSecond &&
                 Controls(some, EventKind::kExpression, 0) == kSilent &&
                 Controls(some, EventKind::kExpression, 1) == kFullLevel);
    XSFLIB_CHECK(some.end_tick == all.end_tick && some.level_cut_db == 0.0);
}

void TestSoundsSendsAddToTheTracks()
{
    // A game's code can give a sound effect sends through its handle (PerformOptions::fx_sends). The voice adds each to
    // the track's send on its bus, and holds the sum at 1 at most. The track's send A of 100 and the sound's 0.5 reach
    // the full send: CC91 127. Its send B of 16 and the sound's 0.25 make 16 + 31.75: CC93 48. Without the sound's
    // sends, CC91 and CC93 take the track's sends as they are.
    const std::vector<uint8_t> kCommands = {0xd9, 0x64, 0xda, 0x10, 0x3c, 0x7f, 0x18, 0xff};
    PerformOptions sends;
    sends.fx_sends = {0.5f, 0.25f, 0.0f};

    const Performance plain = PerformCommandsWith(kCommands, {});
    const Performance sent = PerformCommandsWith(kCommands, sends);

    const std::vector<std::pair<uint32_t, int>> kPlainA = {{0, 100}};
    const std::vector<std::pair<uint32_t, int>> kPlainB = {{0, 16}};
    const std::vector<std::pair<uint32_t, int>> kSentA = {{0, 127}};
    const std::vector<std::pair<uint32_t, int>> kSentB = {{0, 48}};
    XSFLIB_CHECK(Controls(plain, EventKind::kReverb) == kPlainA && Controls(plain, EventKind::kChorus) == kPlainB);
    XSFLIB_CHECK(Controls(sent, EventKind::kReverb) == kSentA && Controls(sent, EventKind::kChorus) == kSentB);
    XSFLIB_CHECK(NoteOns(sent) == NoteOns(plain) &&
                 Controls(sent, EventKind::kExpression) == Controls(plain, EventKind::kExpression));
}

void TestTrackAboveFullLevelLiftsTheZones()
{
    // Volume2 255 takes the track's volumes to (255 / 127)^2 = 4.03 times full level. MIDI's CC7 and CC11 stop at full
    // level. The preset lifts its zones by the rest: 200 log10 4.03 = 121.1 cB, rounded to 121. In the game, a channel
    // plays at the velocity squared times the region's volume times the track's volumes. The voice limits that to full
    // level. The regions split at velocity 63. Key 60 at velocity 63 plays the region of volume 20. The game plays the
    // note at 20 / 127 * 4.03 = 0.635 times the velocity's level, 3.95 dB down. The region's zone is
    // 200 log10(127 / 20) - 121 = 39.55 cB down: -3.96 dB in the SFZ file. The SoundFont's generator holds 2.5 times
    // that, 99 cB (seqlib's ToZoneAttenuation). Key 60 at velocity 127 plays the region of volume 64. The game would
    // play the note at 64 / 127 * 4.03 = 2.03 times full level. The voice holds the note at full level. That region's
    // zone has no attenuation and no volume opcode.
    const SoundArchive archive = SequenceArchive(
        {{0xd5, 0xff, 0x3c, 0x3f, 0x18, 0x3c, 0x7f, 0x18, 0xff}},
        {.velocity_regions = {TestRegion{.volume = 20}, TestRegion{.volume = 64}}, .upper_velocities = {63, 127}});

    const Conversion c = ConvertSequence(archive, 0);

    const Performance& p = c.performance;
    const std::vector<std::tuple<uint32_t, int, int>> kNotes = {{0, 60, 63}, {24, 60, 127}};
    const std::vector<std::pair<uint32_t, int>> kFullLevel = {{0, 127}};
    XSFLIB_CHECK(NoteOns(p) == kNotes && Controls(p, EventKind::kVolume) == kFullLevel &&
                 Controls(p, EventKind::kExpression) == kFullLevel);
    XSFLIB_CHECK(p.presets.size() == 1 && p.presets[0].lift == 121 && p.level_cut_db == 0.0);
    XSFLIB_CHECK(p.approximations.empty());

    // The attenuation of each zone, by its lowest velocity.
    const sf2::Records v = sf2::Read(c.sf2);
    std::map<int, int> attenuations;
    for (std::size_t z = 0; z + 1 < v.Count("ibag", 4); z++)
    {
        const std::map<uint16_t, uint16_t> gens = v.ZoneGens(z);
        if (gens.contains(sf2::kSampleId))
        {
            const auto attenuation = gens.find(sf2::kInitialAttenuation);
            attenuations[gens.at(sf2::kVelRange) & 0xff] = attenuation == gens.end() ? 0 : attenuation->second;
        }
    }

    const std::map<int, int> kAttenuations = {{0, 99}, {64, 0}};
    XSFLIB_CHECK(seqlib::sf2::Check(c.sf2).empty() && attenuations == kAttenuations);

    XSFLIB_CHECK(c.sfz.files.size() == 1);
    for (const SfzFile& f : c.sfz.files)
    {
        XSFLIB_CHECK(f.text.find(" lovel=1 hivel=63 volume=-3.96 pitch_keycenter=60") != std::string::npos);
        XSFLIB_CHECK(f.text.find(" lovel=64 hivel=127 pitch_keycenter=60") != std::string::npos);
    }
}

// NW4R SND picks the resampling filter before the voice has its pitch, by the wave's rate alone. A 44.1 kHz wave plays
// through the 8 kHz set at every key, bent or not. Every zone takes the set's low-pass: 0.2518 of 44.1 kHz at the
// original key: 347.0 Hz or 6489 cents at key 0, and 100 cents more a key. The SFZ region takes lpf_1p at 0.1784 of
// 44.1 kHz. Nothing is approximated.
void TestTheWavesRateAlonePicksTheLowPass()
{
    const SoundArchive archive = SequenceArchive({{0xc4, 0x40, 0x3c, 0x7f, 0x30, 0xff}},
                                                 {.wave = Rwav(64, std::vector<int16_t>(64, 1000), 1, true, 44100)});

    const Conversion c = ConvertSequence(archive, 0);

    const std::vector<ZoneCutoff> kZones = {{{0, 127}, 6489, 0x0003, 12800}};
    XSFLIB_CHECK(Sf2Problem(c.sf2).empty() && ZoneCutoffs(c.sf2) == kZones);
    const std::string text = c.sfz.files.size() == 1 ? c.sfz.files[0].text : "";
    XSFLIB_CHECK(RegionLowPass(text, {0, 127}) == " fil_type=lpf_1p cutoff=7867.4 fil_keytrack=100 fil_keycenter=60");
    XSFLIB_CHECK(c.performance.approximations.empty());
}

void TestLegatoLeavesOutTheRegionsVolume()
{
    // With volume2 64 and tie on, play key 60 at velocity 100 on a region of volume 64, then key 62 at 100 and key 64
    // at 127. NW4R SND continues the channel with the velocity's gain alone and leaves the region's volume out. The
    // MIDI note starts at 127, the highest velocity the legatos reach. CC11 takes it down to 100. The note continues
    // through both legatos. There, CC11 also makes up for the zone's region volume, by 127 / 64 in power.
    const Performance p =
        PerformCommands({0xd5, 0x40, 0xc8, 0x01, 0x3c, 0x64, 0x30, 0x3e, 0x64, 0x30, 0x40, 0x7f, 0x30, 0xff},
                        {.region = {.volume = 64}});

    const std::vector<std::pair<uint32_t, int>> kExpression = {{0, 50}, {48, 71}, {96, 90}};
    const std::vector<std::tuple<uint32_t, int, int>> kNotes = {{0, 60, 127}};
    XSFLIB_CHECK(Controls(p, EventKind::kExpression) == kExpression && NoteOns(p) == kNotes);
    XSFLIB_CHECK(p.level_cut_db == 0.0);
}

void TestRisingLegatoCarriesItsNoteOn()
{
    // With tie on and note wait off, play key 60 at velocity 40, then key 62 at 100 at tick 10 and key 60 at 70 at tick
    // 20, and finish at tick 30. NW4R SND plays one channel throughout, and its sample runs on. So does the MIDI note.
    // The note starts at velocity 100, the highest the legatos reach. Until then, CC11 takes it down to 40.
    const Performance p = PerformCommands({0xc8, 0x01, 0xc7, 0x00, 0x3c, 0x28, 0x00, 0x80, 0x0a, 0x3e,
                                           0x64, 0x00, 0x80, 0x0a, 0x3c, 0x46, 0x00, 0x80, 0x0a, 0xff});

    const std::vector<std::tuple<uint32_t, int, int>> kNotes = {{0, 60, 100}};
    const std::vector<std::pair<uint32_t, int>> kExpression = {{0, 51}, {10, 127}, {20, 89}};
    XSFLIB_CHECK(NoteOns(p) == kNotes && Controls(p, EventKind::kExpression) == kExpression);
    XSFLIB_CHECK(p.level_cut_db == 0.0 && !p.approximations.contains("legato (tie or mono) played as a new note"));
}

void TestRisingLegatoBesideATailTakesAnotherPart()
{
    // With note wait off and release 20, play key 60 for 5 ticks, then turn tie on. That detaches 60 while its release
    // sounds. Play key 62 at velocity 40 and raise it to 100 at tick 10. The track goes on to another part. There 62
    // starts at 100 with CC11 taking it down to 40, and 60's release keeps the first part's CC11.
    const Performance p = PerformCommands({0xc7, 0x00, 0xd3, 0x14, 0x3c, 0x64, 0x05, 0x80, 0x05, 0xc8, 0x01,
                                           0x3e, 0x28, 0x00, 0x80, 0x05, 0x3e, 0x64, 0x00, 0x80, 0x05, 0xff});

    const std::vector<std::tuple<uint32_t, int, int>> kNotes = {{0, 60, 100}, {5, 62, 100}};
    const std::vector<std::pair<uint32_t, int>> kFirstPart = {{0, 127}};
    const std::vector<std::pair<uint32_t, int>> kSecondPart = {{5, 51}, {10, 127}};
    const std::vector<int> kParts = {0, 1};
    XSFLIB_CHECK(NoteOns(p) == kNotes && NoteParts(p) == kParts);
    XSFLIB_CHECK(PartControls(p, EventKind::kExpression, 0) == kFirstPart &&
                 PartControls(p, EventKind::kExpression, 1) == kSecondPart);
    XSFLIB_CHECK(!p.approximations.contains("legato (tie or mono) played as a new note"));
}

void TestDetachedTailKeepsItsSettings()
{
    // With note wait off and release 64, play key 60 for 96 ticks and turn tie on at tick 1. That detaches the note
    // while its release sounds. Set the volume to 64 at tick 2. In the game the release keeps its volume. The track
    // therefore goes on to another part, and that part gets the new volume. The first part keeps 127.
    const Performance p = PerformCommands(
        {0xc7, 0x00, 0xd3, 0x40, 0x3c, 0x64, 0x60, 0x80, 0x01, 0xc8, 0x01, 0x80, 0x01, 0xc1, 0x40, 0x80, 0x30, 0xff});

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
    const Performance p = PerformCommands(
        {0xc7, 0x00, 0xd3, 0x14, 0x3c, 0x64, 0x05, 0x80, 0x05, 0xe3, 0x01, 0x00, 0x40, 0x64, 0x0a, 0x80, 0x0a, 0xff});

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
    const Performance p = PerformCommands({0xc7, 0x00, 0xd3, 0x14, 0x3c, 0x64, 0x05, 0x80, 0x05, 0xc4,
                                           0x40, 0xe3, 0x01, 0x00, 0x40, 0x64, 0x0a, 0x80, 0x0a, 0xff});

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

void TestLegatoAfterAnEnvelopeResetCarriesOn()
{
    // With tie on and attack 100, play key 60, reset the envelope settings and tie key 62 on. The reset leaves the
    // channel's attack as it was. The MIDI note therefore continues.
    const Performance p = PerformCommands({0xc8, 0x01, 0xd0, 0x64, 0x3c, 0x64, 0x18, 0xfb, 0x3e, 0x64, 0x18, 0xff});

    const auto [note_ons, restarted] = NoteOnsAndRestarts(p);
    XSFLIB_CHECK(note_ons == 1 && !restarted);
}

void TestLegatoOnADecayedNoteStaysSilent()
{
    // With tie on, play key 60 on a region that ignores note-off and decays to silence at once. Tie key 62 on 48 ticks
    // later. The channel has decayed by then, and a legato doesn't start the envelope again. No MIDI note starts.
    const Performance p = PerformCommands({0xc8, 0x01, 0x3c, 0x64, 0x30, 0x3e, 0x64, 0x30, 0xff},
                                          {.region = {.sustain = 0, .note_off_type = 1}});

    const auto [note_ons, restarted] = NoteOnsAndRestarts(p);
    XSFLIB_CHECK(note_ons == 1 && !restarted);
}

void TestLegatoBelowHearingKeepsItsMidiNote()
{
    // With tie on, sustain 0, decay 100 and hold 0, play key 60 with note wait off. After 100 ticks the channel has
    // sunk below hearing (kInaudibleSustain) but hasn't faded to kFadedAttenuation. Its MIDI note still sounds. Then
    // tie key 62 on, once after setting release 64 and once at velocity 127. The game's legato doesn't start the
    // envelope again. Neither legato starts a new MIDI note, and velocity 127 doesn't start the note higher on a later
    // pass.
    const EnvelopeValues kEnvelope{127, 100, 0, 0, 127};
    auto at_ms = [&](int attenuation)
    {
        return static_cast<double>(FramesToLevel(kEnvelope, -static_cast<float>(attenuation)) + 1) * kFrameMs;
    };

    const Performance released =
        PerformCommands({0xc8, 0x01, 0xd2, 0x00, 0xd1, 0x64, 0xb1, 0x00, 0xc7, 0x00, 0x3c, 0x64,
                         0x01, 0x80, 0x64, 0xd3, 0x40, 0x3e, 0x64, 0x01, 0x80, 0x30, 0xff});
    const Performance louder = PerformCommands({0xc8, 0x01, 0xd2, 0x00, 0xd1, 0x64, 0xb1, 0x00, 0xc7, 0x00, 0x3c,
                                                0x64, 0x01, 0x80, 0x64, 0x3e, 0x7f, 0x01, 0x80, 0x30, 0xff});

    const std::vector<std::tuple<uint32_t, int, int>> kStruck = {{0, 60, 100}};
    XSFLIB_CHECK(at_ms(kInaudibleSustain) < 100 * kTickMs && 100 * kTickMs < at_ms(kFadedAttenuation));
    XSFLIB_CHECK(NoteOns(released).size() == 1 &&
                 !released.approximations.contains("legato (tie or mono) played as a new note"));
    XSFLIB_CHECK(NoteOns(louder) == kStruck);
}

void TestMidiNoteOfAnEndlessNoteEndsBelowHearing()
{
    // With sustain 0, decay 100 and hold 0, play key 60 with note wait off and no length. Then wait 192 ticks (2 s) and
    // end. With no length, only the track's end releases the note. The MIDI note and the performance end where the
    // channel has faded to kFadedAttenuation. With sustain 5, the level stays above that, and the MIDI note and the
    // performance end below hearing (kInaudibleSustain). The same note with a length of 96 ticks keeps its MIDI note
    // until that length runs out.
    const Performance faded =
        PerformCommands({0xd2, 0x00, 0xd1, 0x64, 0xb1, 0x00, 0xc7, 0x00, 0x3c, 0x64, 0x00, 0x80, 0x81, 0x40, 0xff});
    const Performance shallow =
        PerformCommands({0xd2, 0x05, 0xd1, 0x64, 0xb1, 0x00, 0xc7, 0x00, 0x3c, 0x64, 0x00, 0x80, 0x81, 0x40, 0xff});
    const Performance timed = PerformCommands({0xd2, 0x00, 0xd1, 0x64, 0xb1, 0x00, 0x3c, 0x64, 0x60, 0xff});

    auto at_ms = [](int attenuation, int sustain)
    {
        const EnvelopeValues v{127, 100, sustain, 0, 127};
        return static_cast<double>(FramesToLevel(v, -static_cast<float>(attenuation)) + 1) * kFrameMs;
    };
    auto off_tick = [](const Performance& p)
    {
        const auto off = std::ranges::find(p.tracks[0], EventKind::kNoteOff, &TrackEvent::kind);
        return off == p.tracks[0].end() ? 0u : off->tick;
    };
    auto ends_at = [&](const Performance& p, double ms)
    {
        const double off_ms = off_tick(p) * kTickMs;
        return NoteCount(p) == 1 && off_ms >= ms && off_ms < ms + kTickMs + kFrameMs && p.seconds * 1000.0 >= ms &&
               p.seconds * 1000.0 < ms + kTickMs + kFrameMs;
    };
    const double faded_ms = at_ms(kFadedAttenuation, 0), quiet_ms = at_ms(kInaudibleSustain, 5);
    XSFLIB_CHECK(quiet_ms < faded_ms && faded_ms < 1900.0);
    XSFLIB_CHECK(ends_at(faded, faded_ms));
    XSFLIB_CHECK(ends_at(shallow, quiet_ms));
    XSFLIB_CHECK(off_tick(timed) == 96);
}

void TestHeldNoteBelowHearingEndsTheSequence()
{
    // With sustain 0, decay 100 and hold 0, play key 60 for 10,000 ticks and end. The track waits out a held note. The
    // note sinks below hearing long before its length would release it, and no track reads on. The sequence ends where
    // the note has faded to kFadedAttenuation. A tie's MIDI note ends there too. The sequence doesn't hold the sound
    // for 10 seconds. With sustain 5, the level stays above that, and the sequence ends below hearing
    // (kInaudibleSustain).
    const Performance faded = PerformCommands({0xd2, 0x00, 0xd1, 0x64, 0xb1, 0x00, 0x3c, 0x64, 0xce, 0x10, 0xff});
    const Performance shallow = PerformCommands({0xd2, 0x05, 0xd1, 0x64, 0xb1, 0x00, 0x3c, 0x64, 0xce, 0x10, 0xff});

    auto at_ms = [](int attenuation, int sustain)
    {
        const EnvelopeValues v{127, 100, sustain, 0, 127};
        return static_cast<double>(FramesToLevel(v, -static_cast<float>(attenuation)) + 1) * kFrameMs;
    };
    auto ends_at = [](const Performance& p, double ms)
    {
        const auto off = std::ranges::find(p.tracks[0], EventKind::kNoteOff, &TrackEvent::kind);
        const double off_ms = off == p.tracks[0].end() ? 0.0 : off->tick * kTickMs;
        return !p.holds && !p.truncated && !p.loop && NoteCount(p) == 1 && off_ms >= ms &&
               off_ms < ms + kTickMs + kFrameMs && p.seconds * 1000.0 >= ms &&
               p.seconds * 1000.0 < ms + kTickMs + kFrameMs;
    };
    XSFLIB_CHECK(ends_at(faded, at_ms(kFadedAttenuation, 0)));
    XSFLIB_CHECK(ends_at(shallow, at_ms(kInaudibleSustain, 5)));
}

void TestSilentLegatoIsNotReported()
{
    // With tie on, play key 60 at velocity 0, then tie key 62 on at velocity 0. Neither sounds, and the legato isn't
    // reported as a new note.
    const Performance p = PerformCommands({0xc8, 0x01, 0x3c, 0x00, 0x18, 0x3e, 0x00, 0x18, 0xff});

    const auto [note_ons, restarted] = NoteOnsAndRestarts(p);
    XSFLIB_CHECK(note_ons == 0 && !restarted);
}

void TestSilentNoteLeavesTheTailItsPitch()
{
    // With note wait off and release 64, play key 60 for 48 ticks and turn tie on a tick later. That detaches the note
    // while its release sounds, and the track goes on to another part. Then play key 60 at velocity 0 with a sweep down
    // from 18 semitones up. The game plays that note silently, and the tail keeps its pitch. No part's pitch moves.
    const Performance p = PerformCommands({0xc7, 0x00, 0xd3, 0x40, 0x3c, 0x64, 0x30, 0x80, 0x01, 0xc8,
                                           0x01, 0xe3, 0x04, 0x80, 0x3c, 0x00, 0x30, 0x80, 0x30, 0xff});

    const auto moved = [](const TrackEvent& e)
    {
        return e.kind == EventKind::kPitch && e.semitones != 0.0f;
    };
    XSFLIB_CHECK(NoteCount(p) == 1 && std::ranges::none_of(p.tracks[0], moved));
}

void TestTieGivesTheTailTheTicksVolume()
{
    // With note wait off and release 20, hold key 60, and 5 ticks later set the track's volume to 64 and turn tie on.
    // Tie releases the note and detaches it, but the game first gives its channel the track's settings, the volume
    // among them. So the note's part takes CC7 64 at tick 5, before the track moves to another part.
    const Performance p = PerformCommands(
        {0xc7, 0x00, 0xd3, 0x14, 0x3c, 0x64, 0x00, 0x80, 0x05, 0xc1, 0x40, 0xc8, 0x01, 0x80, 0x30, 0xff});

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
    // Velocity range 255 takes velocity 100 to 200. The game plays the note at (200 / 127)^2 times the region's volume.
    // The voice limits that to full level. The MIDI note plays at 127. The preset lifts its zones by the rest:
    // 200 log10((200 / 127)^2) = 78.9 cB, rounded to 79. CC11 stays at 127. Nothing cuts the sequence's level.
    const Performance p = PerformCommands({0xb3, 0xff, 0x3c, 0x64, 0x30, 0xff});

    const std::vector<std::tuple<uint32_t, int, int>> kNotes = {{0, 60, 127}};
    const std::vector<std::pair<uint32_t, int>> kExpression = {{0, 127}};
    XSFLIB_CHECK(NoteOns(p) == kNotes && Controls(p, EventKind::kExpression) == kExpression);
    XSFLIB_CHECK(p.presets.size() == 1 && p.presets[0].lift == 79 && p.level_cut_db == 0.0);
    XSFLIB_CHECK(p.approximations.empty());
}

void TestMuteStopLeavesDetachedTails()
{
    // With note wait off and release 64, play key 60 for 96 ticks and turn tie on at tick 1. That detaches the note
    // while its release sounds. Then play key 62. Mute mode 3 at tick 2 stops 62 at once, but not 60's release: the
    // track detached 60. CC120 goes to 62's part alone.
    const Performance p = PerformCommands({0xc7, 0x00, 0xd3, 0x40, 0x3c, 0x64, 0x60, 0x80, 0x01, 0xc8, 0x01,
                                           0x3e, 0x64, 0x00, 0x80, 0x01, 0xdd, 0x03, 0x80, 0x30, 0xff});

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
        PerformCommands({0xc8, 0x01, 0xc7, 0x00, 0x3c, 0x28, 0x00, 0x80, 0x0a, 0x3c, 0x64, 0x00, 0x80, 0x0a, 0xff},
                        {.velocity_regions = {TestRegion{}, TestRegion{.pan = 32}}, .upper_velocities = {63, 127}});

    const double kCutDb = 40.0 * std::log10(100.0 / 63.0);
    const std::vector<std::tuple<uint32_t, int, int>> kNotes = {{0, 60, 63}};
    const std::vector<std::pair<uint32_t, int>> kExpression = {{0, 51}, {10, 127}};
    XSFLIB_CHECK(NoteOns(p) == kNotes && Controls(p, EventKind::kExpression) == kExpression);
    XSFLIB_CHECK(std::fabs(p.level_cut_db - kCutDb) < 1e-9);
}

void TestLevelCutFitsTheRunThatsKept()
{
    // Tie key 60 at velocity 127 on a region of volume 32. At tick 24, change the filter and play key 62 at velocity
    // 100. The first pass plays key 62 as a new preset and note, with CC11 253 to undo the region's attenuation (127 /
    // 32 in power). With the filter on a controller, the second pass continues the MIDI note at 100 / 127 of its
    // velocity. That takes CC11 to 199. The level cut has to follow that peak.
    const Performance p = PerformCommands(
        {0xc8, 0x01, 0x3c, 0x7f, 0x18, 0xd8, 0x20, 0x3e, 0x64, 0x18, 0x80, 0x30, 0xff}, {.region = {.volume = 32}});

    const double kCutDb = 40.0 * std::log10(100.0 * std::sqrt(127.0 / 32.0) / 127.0);
    const std::vector<std::pair<uint32_t, int>> kExpression = {{0, 81}, {24, 127}};
    XSFLIB_CHECK(Controls(p, EventKind::kExpression) == kExpression && std::fabs(p.level_cut_db - kCutDb) < 1e-9);
}

void TestTiedNoteTakesPartInTheMainLoop()
{
    // Track 0 opens track 1, ties key 72 for 30 ticks and then for good. Track 1 loops a 26-tick key 60. The tie at
    // tick 30 changes nothing in MIDI. It still falls in track 1's pass from tick 26. So the main loop is the pass
    // after that.
    const Performance p = PerformCommands({0x88, 0x01, 0x00, 0x00, 0x0d, 0xc8, 0x01, 0x48, 0x78, 0x1e,
                                           0x48, 0x78, 0x00, 0x3c, 0x64, 0x1a, 0x89, 0x00, 0x00, 0x0d},
                                          {.track_flags = 3});

    XSFLIB_CHECK(p.loop && p.loop->start == 52 && p.loop->end == 78);
}

void TestSilentLegatoCarriesItsNoteOn()
{
    // With monophonic on (a 2008 command) and note wait off, play key 60 for 30 ticks. At tick 10, change the release.
    // That makes another preset. At tick 20, play key 60 at velocity 0, then finish at tick 50. The MIDI note goes on,
    // silenced by CC11. A note-off would leave a release tail. The preset that isn't used isn't made.
    const Performance p = PerformCommands({0xb2, 0x01, 0xc7, 0x00, 0x3c, 0x64, 0x1e, 0x80, 0x0a, 0xd3, 0x40, 0x80, 0x0a,
                                           0x3c, 0x00, 0x3c, 0x80, 0x1e, 0xff});

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
    // Track 0 opens track 1, sets tempo 1023, turns on tie and portamento (time 100) and holds key 60. At tick 1, track
    // 0 changes the low-pass during the note and plays key 62 legato. The first pass plays that as a new MIDI note. The
    // second, with the filter on a controller, continues the note with a bend of 2 semitones. Track 1 loops every 6
    // ticks. Only the first pass bends back to 0 in MIDI. On both passes, though, the sweep's end counts as a change on
    // track 0. Both passes then select the same main loop.
    const Performance p = PerformCommands(
        {0x88, 0x01, 0x00, 0x00, 0x1d, 0xe1, 0x03, 0xff, 0xc8, 0x01, 0xce, 0x01, 0xcf, 0x64, 0xc7, 0x00, 0x3c, 0x64,
         0x00, 0x80, 0x01, 0xd8, 0x10, 0xc7, 0x01, 0x3e, 0x64, 0x00, 0xff, 0x80, 0x06, 0x89, 0x00, 0x00, 0x1d},
        {.track_flags = 3});

    XSFLIB_CHECK(p.loop && p.loop->start == 2562 && p.loop->end == 2568);
}

void TestMonoLegatoTakesItsLengthWithTieOn()
{
    // With monophonic (2008) and tie on, play key 60 for 48 ticks, then key 62 for 24 ticks, and wait. A monophonic
    // track gives its channel the new length even in a tie. The note ends at tick 72.
    const Performance p =
        PerformCommands({0xb2, 0x01, 0xc8, 0x01, 0x3c, 0x64, 0x30, 0x3e, 0x64, 0x18, 0x80, 0x60, 0xff});

    const std::vector<std::pair<uint32_t, EventKind>> kNotes = {{0, EventKind::kNoteOn}, {72, EventKind::kNoteOff}};
    XSFLIB_CHECK(NoteEvents(p) == kNotes);
}

void TestNoteReleasedBeforeItsFirstFrameIsSilent()
{
    // With note wait off, play key 60 and turn monophonic on (2008). That releases the note. Released at once, before
    // the envelope's first update, the note releases from -90.4 dB and makes no sound. Released a tick later, it makes
    // a sound.
    const SoundArchive archive =
        SequenceArchive({{0xc7, 0x00, 0x3c, 0x64, 0x30, 0xb2, 0x01, 0x80, 0x30, 0xff},
                         {0xc7, 0x00, 0x3c, 0x64, 0x30, 0x80, 0x01, 0xb2, 0x01, 0x80, 0x30, 0xff}});

    const Performance at_once = PerformSound(archive, 0);
    const Performance later = PerformSound(archive, 1);

    const std::vector<std::pair<uint32_t, EventKind>> kNotes = {{0, EventKind::kNoteOn}, {1, EventKind::kNoteOff}};
    XSFLIB_CHECK(NoteCount(at_once) == 0 && NoteEvents(later) == kNotes);
}

void TestWhatMidiCantSeparateIsReported()
{
    // In the first sequence, all 16 tracks release a key 60 with a slow release by turning tie on a tick later. Then
    // they change the volume during the release. Every channel goes to a track, and none is left for another part. MIDI
    // then can't keep the new volume away from the releasing notes. The second sequence puts the track's pan left of a
    // region panned right.
    std::vector<uint8_t> all_tracks;
    for (uint8_t t = 1; t < 16; t++)
    {
        all_tracks.insert(all_tracks.end(), {0x88, t, 0x00, 0x00, 15 * 5});
    }
    all_tracks.insert(all_tracks.end(), {0xc7, 0x00, 0xd3, 0x40, 0x3c, 0x64, 0x60, 0x80, 0x01, 0xc8, 0x01, 0x80, 0x01,
                                         0xc1, 0x40, 0x80, 0x30, 0xff});
    const SoundArchive archive = SequenceArchive({all_tracks, {0xc0, 0x20, 0x3c, 0x64, 0x30, 0xff}},
                                                 {.track_flags = 0xffff, .region = {.pan = 100}});

    const Performance detached = PerformSound(archive, 0);
    const Performance panned = PerformSound(archive, 1);

    const std::string kDetached = "detached notes (still receive track controller changes in MIDI)";
    const std::string kPans = "combined region and track pan (using the synth pan law)";
    XSFLIB_CHECK(detached.approximations.contains(kDetached) && !detached.approximations.contains(kPans));
    XSFLIB_CHECK(panned.approximations.contains(kPans) && !panned.approximations.contains(kDetached));
}

void TestOneShotWaveEndsAsTheDspPlaysIt()
{
    // Play a one-shot wave at four times AX's rate, 384 samples a frame, and wait for it. The wave's voice starts the
    // frame after the note's frame. NW4R SND reads a wave's end one frame after the frame that passes it, at the start
    // of the frame after that. A wave of 3072 samples passes its end in frame 8. The channel has then gone at 30 ms, in
    // time for tick 3 (31.25 ms). A sample more takes the end to frame 9, after tick 3. The track then waits until tick
    // 4.
    const std::vector<uint8_t> kCommands = {0x3c, 0x7f, 0x00, 0xff};
    std::vector<uint32_t> note_offs, ends;
    for (const uint32_t length : {3072u, 3073u})
    {
        const std::vector<int16_t> samples(length, 1000);
        const Performance p = PerformCommands(kCommands, {.wave = Rwav(length, samples, 1, false, 4 * 32000)});
        for (const auto& [tick, kind] : NoteEvents(p))
        {
            if (kind == EventKind::kNoteOff)
            {
                note_offs.push_back(tick);
            }
        }
        ends.push_back(p.end_tick);
    }

    XSFLIB_CHECK(note_offs == (std::vector<uint32_t>{3, 4}) && ends == (std::vector<uint32_t>{3, 4}));
}

void TestTicksFallInTheGamesSoundFrames()
{
    // At tempo 40, a tick lasts 31.25 ms. Tick 12 then falls on the start of frame 125, at 375 ms. NW4R SND's single
    // precision falls a little short and puts the tick at the end of frame 124. MIDI events of tick 12 go at the start
    // of that frame: 372 ms, or tick 11.904.
    const Performance p = PerformCommands({0xe1, 0x00, 0x28, 0x80, 0x0c, 0xff});

    XSFLIB_CHECK(p.frame_ticks.size() == 13 && std::fabs(p.frame_ticks[12] - 372.0 / 31.25) < 1e-4);
}

void TestOddArgumentsAreReadAsTheGameReadsThem()
{
    // A volume ramp over 0x8000 ticks: a negative count in NW4R SND's 16 bits, applied at once. A wait of 128 ticks in
    // seven bytes (reading only five would leave bytes to be read as a program change). And timebase 0. That stops the
    // sequence's timing.
    const SoundArchive archive =
        SequenceArchive({{0xa3, 0xc1, 0x40, 0x80, 0x00, 0x3c, 0x64, 0x30, 0xff},
                         {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x81, 0x00, 0x3c, 0x64, 0x30, 0xff},
                         {0xb0, 0x00, 0x80, 0x30, 0xff}});

    const Performance ramp = PerformSound(archive, 0);
    const Performance wait = PerformSound(archive, 1);
    const Performance timebase = PerformSound(archive, 2);

    const std::vector<std::pair<uint32_t, int>> kVolume = {{0, 64}};
    const std::vector<std::tuple<uint32_t, int, int>> kWaited = {{128, 60, 100}};
    const std::vector<std::string> kStopped = {"timebase 0: the sequence stops advancing"};
    XSFLIB_CHECK(Controls(ramp, EventKind::kVolume) == kVolume && NoteOns(wait) == kWaited);
    XSFLIB_CHECK(timebase.warnings == kStopped && timebase.end_tick == 0 && timebase.timebase == 48);
}

void TestSequenceThatStopsAdvancingPlaysOut()
{
    // The first three sequences turn note wait off, play key 60, wait and set tempo 0. A note of 96 ticks then holds
    // for good, and plays for 10 s at MIDI's 120 BPM. A note of one tick at release 90 has already been released, and
    // ends as it would have; the third sequence also finishes on the tick that stops. A fourth sequence plays the same
    // notes without tempo 0, and both end with it.
    const SoundArchive archive =
        SequenceArchive({{0xc7, 0x00, 0x3c, 0x64, 0x60, 0x80, 0x01, 0xe1, 0x00, 0x00, 0x80, 0x30, 0xff},
                         {0xc7, 0x00, 0xd3, 0x5a, 0x3c, 0x64, 0x01, 0x80, 0x02, 0xe1, 0x00, 0x00, 0x80, 0x30, 0xff},
                         {0xc7, 0x00, 0xd3, 0x5a, 0x3c, 0x64, 0x01, 0x80, 0x02, 0xe1, 0x00, 0x00, 0xff},
                         {0xc7, 0x00, 0xd3, 0x5a, 0x3c, 0x64, 0x01, 0x80, 0x02, 0xff}});

    const Performance held = PerformSound(archive, 0);
    const Performance released = PerformSound(archive, 1);
    const Performance ending = PerformSound(archive, 2);
    const Performance unstopped = PerformSound(archive, 3);

    const std::vector<std::string> kStopped = {"tempo 0: the sequence stops advancing"};
    XSFLIB_CHECK(held.warnings == kStopped && held.holds && held.end_tick == 961 && held.tracks[0].back().tick == 961);
    XSFLIB_CHECK(released.warnings == kStopped && !released.holds && unstopped.end_tick > 200 &&
                 released.end_tick == unstopped.end_tick && ending.end_tick == unstopped.end_tick);
}

void TestTieLoopThatHoldsEndsAsAHeldSound()
{
    // The first sequence turns tie on, plays key 60 for 2400 ticks (25 s) and jumps back to the note command. With tie
    // on, that command continues the note. The sound holds for good. The file ends 10 s after the note starts, without
    // loop markers. The second sequence does the same without tie. Each pass then releases the note and strikes it
    // again: a loop of 25 s.
    const SoundArchive archive = SequenceArchive({{0xc8, 0x01, 0x3c, 0x64, 0x92, 0x60, 0x89, 0x00, 0x00, 0x02},
                                                  {0x3c, 0x64, 0x92, 0x60, 0x89, 0x00, 0x00, 0x00}});

    const Performance tied = PerformSound(archive, 0);
    const Performance struck = PerformSound(archive, 1);

    XSFLIB_CHECK(tied.holds && !tied.loop && std::fabs(tied.seconds - 10.0) < 0.02);
    XSFLIB_CHECK(!struck.holds && struck.loop && struck.loop->start == 0 && struck.loop->end == 2400);
}

void TestTieLoopOverAOneShotWaveStaysALoop()
{
    // The tied note of the test above, on a one-shot wave of 30 s. The wave is still playing when the pass of 25 s
    // ends. It ends by itself, though. The pass doesn't hold it for good.
    const std::vector<int16_t> kSamples(30 * 32000, 1000);

    const Performance p = PerformCommands({0xc8, 0x01, 0x3c, 0x64, 0x92, 0x60, 0x89, 0x00, 0x00, 0x02},
                                          {.wave = Rwav(30 * 32000, kSamples)});

    XSFLIB_CHECK(!p.holds && p.loop && p.loop->end == 2400);
}

// The tests on the Wii.
const Test kTests[] = {
    TestAdpcmLoopIsInSamples,
    TestVersionOneBankDescribesItsWaves,
    TestBankSelectsRegionsAsTheGameDoes,
    TestOldBankHasNoVolumeOrTune,
    TestFilesAreFoundInGroups,
    TestDamagedNameTableLeavesSoundsNumbered,
    TestTruncatedArchiveKeepsWhatItHolds,
    TestCopyWithWaveDataIsPreferred,
    TestTruncatedArchiveWhoseTablesCantBeReadIsReported,
    TestOtherPlatformsArchivesAreReported,
    TestNamesHaveNoControlCharacters,
    TestBrokenBankReferencesAreReported,
    TestWaveDataPastTheEndIsReported,
    TestSequenceStartingPastItsEndPlaysNothing,
    TestOnlyAJumpBackToPlayedCommandsIsALoop,
    TestMainLoopWaitsForAFade,
    TestLevelTooLoudForMidiIsCut,
    TestFullLevelStopsTheTrackVolumes,
    TestTrackThatTheGameSilencesKeepsItsNotes,
    TestSoundsSendsAddToTheTracks,
    TestTrackAboveFullLevelLiftsTheZones,
    TestTheWavesRateAlonePicksTheLowPass,
    TestLegatoLeavesOutTheRegionsVolume,
    TestRisingLegatoCarriesItsNoteOn,
    TestRisingLegatoBesideATailTakesAnotherPart,
    TestDetachedTailKeepsItsSettings,
    TestSweepLeavesTheOlderNoteItsPitch,
    TestPartLeftTakesTheBendInItsTick,
    TestLegatoAfterAnEnvelopeResetCarriesOn,
    TestLegatoOnADecayedNoteStaysSilent,
    TestLegatoBelowHearingKeepsItsMidiNote,
    TestMidiNoteOfAnEndlessNoteEndsBelowHearing,
    TestHeldNoteBelowHearingEndsTheSequence,
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
    TestOneShotWaveEndsAsTheDspPlaysIt,
    TestTicksFallInTheGamesSoundFrames,
    TestOddArgumentsAreReadAsTheGameReadsThem,
    TestSequenceThatStopsAdvancingPlaysOut,
    TestTieLoopThatHoldsEndsAsAHeldSound,
    TestTieLoopOverAOneShotWaveStaysALoop,
};

} // namespace

int main()
{
    RunOn(Wii(), kTests);

    return xsflib::testing::Summary("sequence_export_revolution");
}
