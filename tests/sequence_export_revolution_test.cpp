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
    std::vector<uint8_t> rwav = Header("RWAV", 0x0102, 2);
    const std::size_t info = Block(rwav, "INFO", 0);
    Grow(rwav, WaveInfoSize(1) + 0x30);
    WaveInfo(rwav, info + 8, 1, 1, true, 32000, 0, 0);
    rwav[info + 8] = 2;
    xsflib::StoreBe32(&rwav[info + 8 + 8], 18);
    xsflib::StoreBe32(&rwav[info + 8 + 0xC], 47);
    // The ADPCM info, after the wave info.
    xsflib::StoreBe32(&rwav[info + 8 + 0x1C + 4 + 4], static_cast<uint32_t>(WaveInfoSize(1)));
    EndBlock(rwav, info, 0);
    const std::size_t data = Block(rwav, "DATA", 1);
    Grow(rwav, 24);
    EndBlock(rwav, data, 1);

    const Pcm pcm = ToPcm(DecodeWave(Wave::ParseFile(rwav)));

    XSFLIB_CHECK(pcm.loop && pcm.loop_start == 14 && pcm.loop_end == 42 && pcm.channels[0].size() == 42);
}

void TestVersionOneBankDescribesItsWaves()
{
    // An RBNK 1.1 bank's WAVE block describes its waves: two four-sample waves. Their samples are the raw wave data.
    std::vector<ArchiveFile> files(2);
    files[0].data = MakeSequence({0xff}, {});
    files[1].data = Rbnk({TestInstrument{{{.wave_index = 1}}}}, 0x0101, 2, 4);
    AddSamples(files[1].wave_data, {1, 2, 3, 4, -5, -6, -7, -8});
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
        {MakeSequence({0x3c, 0x64, 0x18, 0xff}, {})},
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
    files[0].data = MakeSequence({0xff}, {});

    const SoundArchive archive = SoundArchive::Load(Rsar(files, {.sequences = {{}, {}}, .names = {name}}));

    XSFLIB_CHECK(archive.Sounds().size() == 2 && archive.Sounds()[0].name == printable);
    XSFLIB_CHECK(archive.Sounds()[1].name == "SEQ_1");
}

void TestBrokenBankReferencesAreReported()
{
    // Sound 0 names bank 5. No such bank exists. Sound 1 names bank 0. No group holds bank 0's file. Sound 2 names bank
    // 1. Bank 1's region plays wave 3 of an RWAR with one wave.
    std::vector<ArchiveFile> files(3);
    files[0].data = MakeSequence({0xff}, {});
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
    TestMainLoopWaitsForAFade,
    TestLevelTooLoudForMidiIsCut,
    TestFullLevelStopsTheTrackVolumes,
    TestTrackThatTheGameSilencesKeepsItsNotes,
    TestSoundsSendsAddToTheTracks,
    TestTrackAboveFullLevelLiftsTheZones,
    TestTheWavesRateAlonePicksTheLowPass,
    TestHeldNoteBelowHearingEndsTheSequence,
    TestNoteAboveVelocity127TakesItsLevel,
    TestLevelCutFitsTheRunThatsKept,
    TestOneShotWaveEndsAsTheDspPlaysIt,
    TestTicksFallInTheGamesSoundFrames,
};

} // namespace

int main()
{
    RunOn(Wii(), kTests);

    return xsflib::testing::Summary("sequence_export_revolution");
}
