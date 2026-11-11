// SPDX-License-Identifier: MIT

// Unit tests for the detection of Konami's driver: the scan, the song table, the mixer's rate, the bends, the
// revisions, the code and the sample tables. They build small synthetic cartridges in memory. No game ROM is needed.

#include "konami_test_util.h"

namespace ntdlib::sequence_export::konami
{
namespace
{

// The play-song routine's code. When it starts at a multiple of 4, its ldr at +6 loads the song table's address from
// +0x18.
const std::vector<uint16_t> kPlaySongCode = {0x00E0, 0x1900, 0x0080, 0x4904, 0x1842, 0x8811, 0x8850, 0x0400, 0x4301};
// The note-start routine's code. When it starts at a multiple of 4, its ldr at +14 loads the sample table's address
// from +0x14.
const std::vector<uint16_t> kNoteStartCode = {0x4B00, 0x403B, 0x20A0, 0x0200, 0x4038, 0x2800, 0xD100, 0x4901};

void TestDetectionScan()
{
    // Without the driver's code to go by, the tables are found in the data.
    DriverInfo info;
    std::string err;

    SEQLIB_CHECK(DetectDriver(ScanRom(), DriverOverrides(), info, err));

    SEQLIB_CHECK_EQ(info.song_table, kScanSongTable);
    SEQLIB_CHECK_EQ(info.song_count, 2);
    SEQLIB_CHECK_EQ(info.sample_table, kScanSampleTable);
    SEQLIB_CHECK(info.warnings == std::vector<std::string>{"mixer timer not found, assuming 21024 Hz"});

    int scanned = 0;
    for (const std::string& line : info.log)
    {
        scanned += line.find("(from data scan)") != std::string::npos;
    }
    SEQLIB_CHECK_EQ(scanned, 2);

    // Data that parses but isn't laid out like the driver's isn't taken for a song table: here, one byte between the
    // songs.
    SEQLIB_CHECK(!DetectDriver(ScanRom(1), DriverOverrides(), info, err));
    SEQLIB_CHECK(err.empty());

    // A table whose first song doesn't parse isn't taken, and neither is the rest of it after that song, which looks
    // like a table of its own. Song 0 uses F4 and runs up to song 1, and songs 1 and 2 are like ScanRom's.
    Image partial(0x400);
    constexpr uint32_t kTable = kRomBase + 0x100;
    partial.Song(kTable, 0, kRomBase + 0x16C, 0, {});
    partial.PutBytes(kRomBase + 0x16C, {0x00, 0xF4, 0x00, 0xFD});
    partial.Song(kTable, 1, kRomBase + 0x190, 8, {{4, 0}});
    partial.PutBytes(kRomBase + 0x190, {0x00, 0xA8, 0x10, 0x00, 0x0A, 0xE0, 0x05, 0xFE, 0x00, 0xFD});
    partial.Song(kTable, 2, kRomBase + 0x19A, 8, {{5, 0}});
    partial.PutBytes(kRomBase + 0x19A, {0x00, 0xA3, 0x01, 0x08, 0xE0, 0x00, 0xFF, 0x00, 0x00, 0xFD});

    SEQLIB_CHECK(!DetectDriver(partial.ToRom(), DriverOverrides(), info, err));
    SEQLIB_CHECK(err.empty());

    // But a table whose third song doesn't parse is taken, with its first two songs and a warning. Songs 0 and 1 play a
    // PSG note, and song 2 uses F4.
    Image later(0x400);
    later.Song(kTable, 0, kRomBase + 0x16C, 9, {{0, 0}});
    later.PutBytes(kRomBase + 0x16C, {0x00, 0x81, 0x00, 0xD3, 0x18, 0x04, 0xE0, 0x00, 0xFE, 0x00, 0xFD});
    later.Song(kTable, 1, kRomBase + 0x177, 9, {{0, 0}});
    later.PutBytes(kRomBase + 0x177, {0x00, 0x81, 0x00, 0xD3, 0x18, 0x04, 0xE0, 0x00, 0xFE, 0x00, 0xFD});
    later.Song(kTable, 2, kRomBase + 0x182, 0, {});
    later.PutBytes(kRomBase + 0x182, {0x00, 0xF4, 0x00, 0xFD});

    DriverOverrides rate_only; // so that the mixer rate, which it has no code for, isn't a warning
    rate_only.mix_rate = 16000;

    const bool found = DetectDriver(later.ToRom(), rate_only, info, err);

    const std::vector<std::string> kWarnings = {
        "song 2's tracks don't read cleanly, so the song table is taken to end before it (--song-count overrides "
        "this)"};
    SEQLIB_CHECK(found);
    SEQLIB_CHECK_EQ(info.song_table, kTable);
    SEQLIB_CHECK_EQ(info.song_count, 2);
    SEQLIB_CHECK(info.warnings == kWarnings);

    // The driver's code doesn't stop the scans when the tables it loads aren't in the ROM, and the log says why they
    // ran.
    Image outside = ScanImage();
    outside.PutHalfwords(kRomBase + 0x300, kPlaySongCode);
    outside.Put32(kRomBase + 0x318, 0x02000000);
    outside.PutHalfwords(kRomBase + 0x340, kNoteStartCode);
    outside.Put32(kRomBase + 0x354, 0x02000000);

    SEQLIB_CHECK(DetectDriver(outside.ToRom(), DriverOverrides(), info, err));

    auto logged = [&](const char* line)
    {
        return std::count(info.log.begin(), info.log.end(), line) == 1;
    };

    SEQLIB_CHECK_EQ(info.song_table, kScanSongTable);
    SEQLIB_CHECK_EQ(info.sample_table, kScanSampleTable);
    SEQLIB_CHECK(
        logged("the song table that the play-song code loads isn't in the ROM, scanning data for a song table"));
    SEQLIB_CHECK(
        logged("the sample table that the note-start code loads isn't in the ROM, scanning data for the sample table"));

    // A sample table whose entries aren't in order is only a guess, which is a warning.
    Image swapped = ScanImage();
    swapped.Put32(kScanSampleTable, kScanSampleTable + 52);
    swapped.Put32(kScanSampleTable + 4, kScanSampleTable + 8);

    SEQLIB_CHECK(DetectDriver(swapped.ToRom(), DriverOverrides(), info, err));
    SEQLIB_CHECK_EQ(info.sample_table, kScanSampleTable);
    SEQLIB_CHECK(std::count(info.warnings.begin(), info.warnings.end(),
                            "sample table 0x08000168 guessed from the data; check the result") == 1);

    // Noise isn't taken for the driver's data.
    Image noise(0x40000);
    uint32_t x = 1;
    for (uint32_t a = kRomBase; a < kRomBase + 0x40000; a++)
    {
        noise.Put8(a, uint8_t((x = x * 1103515245u + 12345u) >> 16));
    }

    SEQLIB_CHECK(!DetectDriver(noise.ToRom(), DriverOverrides(), info, err));
}

void TestSongTableFromOtherCode()
{
    // Code that works out song * 36 + table like the play-song routine, with other registers: lsls r0,r2,#3 /
    // adds r0,r0,r2 / lsls r0,r0,#2 / ldr r1,=table. Its ldr loads the address of ScanRom's song table from +0x10.
    constexpr uint32_t kCode = kRomBase + 0x300;
    const std::vector<uint16_t> kCodeHalfwords = {0x00D0, 0x1880, 0x0080, 0x4902};
    Image m = ScanImage();
    m.PutHalfwords(kCode, kCodeHalfwords);
    m.Put32(kCode + 0x10, kScanSongTable);
    DriverInfo info;
    std::string err;

    const bool found = DetectDriver(m.ToRom(), DriverOverrides(), info, err);

    // The table is laid out like the driver's song data, so it's taken.
    SEQLIB_CHECK(found);
    SEQLIB_CHECK_EQ(info.song_table, kScanSongTable);
    SEQLIB_CHECK(std::count(info.log.begin(), info.log.end(), "song table 0x08000100 (from play-song code)") == 1);

    // With a byte between its two songs, it isn't, although both songs parse and play notes. The data scan doesn't take
    // it either.
    Image gap = ScanImage(1);
    gap.PutHalfwords(kCode, kCodeHalfwords);
    gap.Put32(kCode + 0x10, kScanSongTable);

    const bool found_gap = DetectDriver(gap.ToRom(), DriverOverrides(), info, err);

    SEQLIB_CHECK(!found_gap);
    SEQLIB_CHECK(err.empty());
}

void TestMixerRate()
{
    // The timer setup code, whose ldr at +0 loads the timer table's address from +0x14, and a timer table that sets
    // timer 0 to 0xFC00 without a prescaler, for 16777216 / 1024 Hz.
    Image m = ScanImage();
    constexpr uint32_t kTimerCode = kRomBase + 0x380;
    constexpr uint32_t kTimerTable = kRomBase + 0x3A0;
    m.PutHalfwords(kTimerCode, {0x4904, 0x0098, 0x1840, 0x6800, 0x2180, 0x0409, 0x4308, 0x6010});
    m.Put32(kTimerCode + 0x14, kTimerTable);
    m.Put32(kTimerTable, 0xFC00);
    DriverInfo info;
    std::string err;

    const bool found = DetectDriver(m.ToRom(), DriverOverrides(), info, err);

    // The rate comes from the table, without a warning.
    SEQLIB_CHECK(found);
    SEQLIB_CHECK_EQ(info.timer_table, kTimerTable);
    SEQLIB_CHECK(info.mix_rate == 16384.0);
    SEQLIB_CHECK(info.warnings.empty());

    // The rate is also detected when the code loads the setting into r3.
    m.PutHalfwords(kTimerCode + 6, {0x6803, 0x2080, 0x0400, 0x4318});

    const bool found_r3 = DetectDriver(m.ToRom(), DriverOverrides(), info, err);

    SEQLIB_CHECK(found_r3);
    SEQLIB_CHECK_EQ(info.timer_table, kTimerTable);
    SEQLIB_CHECK(info.mix_rate == 16384.0);
    SEQLIB_CHECK(info.warnings.empty());

    // A setting whose rate is out of range (256 Hz) isn't used, and the driver's usual rate, 16777216 / 798 Hz, is
    // assumed, with a warning.
    m.Put32(kTimerTable, 0);

    const bool found_slow = DetectDriver(m.ToRom(), DriverOverrides(), info, err);

    SEQLIB_CHECK(found_slow);
    SEQLIB_CHECK_EQ(info.timer_table, kTimerTable);
    SEQLIB_CHECK(info.mix_rate == 16777216.0 / 798);
    SEQLIB_CHECK(info.warnings ==
                 std::vector<std::string>{"mixer rate in the timer table is outside 4000-65536 Hz, assuming 21024 Hz"});
}

void TestBendDetection()
{
    // The end of the bend command's code. A driver that keeps the bend for later notes stores it next, with
    // strh r4,[r6,#0x1e].
    const std::vector<uint16_t> kBendCode = {0x8BB0, 0x4647, 0x80F8, 0x8830, 0x1900,
                                             0x8038, 0x7978, 0x2120, 0x4308, 0x7178};
    constexpr uint32_t kCode = kRomBase + 0x300;
    Image m = ScanImage();
    m.PutHalfwords(kCode, kBendCode);
    m.Put16(kCode + 20, 0x83F4);
    DriverInfo info;
    std::string err;

    const bool found_kept = DetectDriver(m.ToRom(), DriverOverrides(), info, err);

    SEQLIB_CHECK(found_kept && info.bend_kept);
    SEQLIB_CHECK(info.log.back() == "pitch bends carry over to later notes (from bend code)");

    // A driver that branches away instead only bends the note that's playing.
    m.Put16(kCode + 20, 0xE137);

    const bool found_once = DetectDriver(m.ToRom(), DriverOverrides(), info, err);

    SEQLIB_CHECK(found_once && !info.bend_kept);
    SEQLIB_CHECK(info.log.back() == "pitch bends apply to the playing note only (from bend code)");

    // Without the code, the bend is taken to carry over, and the log says that's an assumption.
    const bool found_without = DetectDriver(ScanRom(), DriverOverrides(), info, err);

    SEQLIB_CHECK(found_without && info.bend_kept);
    SEQLIB_CHECK(info.log.back() == "pitch bend code not recognised, assuming a bend carries over to later notes");
}

void TestRevisionDetection()
{
    // SyntheticImage() with a command reader at kCode, and its tables given by hand.
    constexpr uint32_t kCode = kRomBase + 0x800;
    DriverOverrides ov;
    ov.song_table = kSongTable;
    ov.song_count = 2;
    ov.sample_table = kSampleTable;
    ov.mix_rate = 16000;

    // The result of a detection: its return value, the driver info it filled in, and its error.
    struct Result
    {
        bool found;
        DriverInfo info;
        std::string error;
    };

    // Returns the result of detection with the reader that compares `third` and `fifth`, and then runs the halfwords in
    // `more`, or with none if `third` and `fifth` are 0.
    auto detect = [&](uint16_t third, uint16_t fifth, const std::vector<uint16_t>& more = {})
    {
        Image m = SyntheticImage();
        if (third)
        {
            std::vector<uint16_t> reader = CommandReader(third, fifth);
            reader.insert(reader.end(), more.begin(), more.end());
            m.PutHalfwords(kCode, reader);
        }

        Result r;
        r.found = DetectDriver(m.ToRom(), ov, r.info, r.error);

        return r;
    };

    const Result wct2004 = detect(0x2DFE, 0x2DFA);
    const Result rave_master = detect(0x2DFE, 0x2DF5);
    const Result rave_master_f1 =
        detect(0x2DFE, 0x2DF5, {0x2DF4, 0xD100, 0x2DF3, 0xD100, 0x2DF2, 0xD100, 0x2DF1, 0xD100});
    const Result tsubasa = detect(0x2DFE, 0x2DF5, {0x2DF4, 0xD100, 0x2DF3, 0xD100, 0x2DF2, 0xD100, 0x2DF0, 0xD100});
    const Result hunter = detect(0x2DFE, 0x2DF6);
    const Result kitaro = detect(0x2DFE, 0x2DF9);
    const Result oujisama = detect(0x2DFE, 0x2DFB);
    const Result ultimate_masters = detect(0x2DFD, 0x2DFB);
    const Result eternal_duelist = detect(0x2DFE, 0x2DF3);
    const Result dungeon_dice = detect(0x2DFE, 0x2DFC);
    const Result older = detect(0x2DFE, 0x2DF8);
    const Result none = detect(0, 0);

    // A reader that compares FF and then FE is from an older revision than the Ultimate Masters revision's, which
    // compares FD. Of those, the WCT 2004 revision goes on with EF and FA, the Rave Master revision with EF and F5, the
    // Eternal Duelist revision with EF and F3, and the Dungeon Dice Monsters revision with EF and FC. The synthetic
    // image has none of the tables that the Dungeon Dice Monsters revision's notes read.
    SEQLIB_CHECK(wct2004.found && wct2004.info.revision == Revision::kWct2004);
    SEQLIB_CHECK(wct2004.info.log[0] == "WCT 2004 revision of the driver (from the command reader at 0x08000800)");
    SEQLIB_CHECK(rave_master.found && rave_master.info.revision == Revision::kRaveMaster);
    SEQLIB_CHECK(rave_master.info.log[0] ==
                 "Rave Master revision of the driver (from the command reader at 0x08000800)");
    SEQLIB_CHECK(eternal_duelist.found && eternal_duelist.info.revision == Revision::kEternalDuelist);

    // The Captain Tsubasa revision's reader goes on from F5 as Rave Master's does, with F0 where Rave Master's compares
    // F1.
    SEQLIB_CHECK(rave_master_f1.found && rave_master_f1.info.revision == Revision::kRaveMaster);
    SEQLIB_CHECK(tsubasa.found && tsubasa.info.revision == Revision::kCaptainTsubasa);
    SEQLIB_CHECK(tsubasa.info.log[0] ==
                 "Captain Tsubasa revision of the driver (from the command reader at 0x08000800)");

    // The Hunter X Hunter revision's reader compares F6 after EF, and then Rave Master's opcodes from F5.
    SEQLIB_CHECK(hunter.found && hunter.info.revision == Revision::kHunterXHunter);
    SEQLIB_CHECK(hunter.info.log[0] ==
                 "Hunter X Hunter revision of the driver (from the command reader at 0x08000800)");

    // The Kitaro revision's reader compares F9 after EF, and then WCT 2004's opcodes from F8.
    SEQLIB_CHECK(kitaro.found && kitaro.info.revision == Revision::kKitaro);
    SEQLIB_CHECK(kitaro.info.log[0] == "Kitaro revision of the driver (from the command reader at 0x08000800)");

    // The Minna no Ouji-sama revision's reader compares FB after EF, and then WCT 2004's opcodes from FA.
    SEQLIB_CHECK(oujisama.found && oujisama.info.revision == Revision::kMinnaNoOujisama);
    SEQLIB_CHECK(oujisama.info.log[0] ==
                 "Minna no Ouji-sama revision of the driver (from the command reader at 0x08000800)");
    SEQLIB_CHECK(!dungeon_dice.found && dungeon_dice.info.revision == Revision::kDungeonDiceMonsters);
    SEQLIB_CHECK(dungeon_dice.error ==
                 "found the command reader of the Dungeon Dice Monsters revision, but not the sample map or the "
                 "sample period table that its notes read");
    SEQLIB_CHECK(ultimate_masters.found && ultimate_masters.info.revision == Revision::kUltimateMasters);
    SEQLIB_CHECK(ultimate_masters.info.log[0] ==
                 "Ultimate Masters revision of the driver (from the command reader at 0x08000800)");
    SEQLIB_CHECK(!older.found);
    SEQLIB_CHECK(older.error ==
                 "this game has an older revision of Konami's driver with commands that ntdlib can't read "
                 "(its command reader is at 0x08000800)");
    SEQLIB_CHECK(none.found && none.info.revision == Revision::kUltimateMasters);
    SEQLIB_CHECK(none.info.log[0] ==
                 "command reader not recognised, assuming the Ultimate Masters revision of the driver");

    // Without its tables given, a cartridge that holds only the command reader has no song table to find. The reader
    // still shows that the game has the driver, so detection fails with an error that names it, where a cartridge
    // without the reader shows no sign of the driver.
    Image reader_only(0x1000);
    reader_only.PutHalfwords(kCode, CommandReader(0x2DFE, 0x2DFA));
    DriverInfo info;
    std::string reader_error, blank_error;

    const bool found_reader_only = DetectDriver(reader_only.ToRom(), DriverOverrides(), info, reader_error);
    const bool found_blank = DetectDriver(Image(0x1000).ToRom(), DriverOverrides(), info, blank_error);

    SEQLIB_CHECK(!found_reader_only);
    SEQLIB_CHECK(reader_error ==
                 "found the command reader of the WCT 2004 revision of Konami's driver, at 0x08000800, but not "
                 "its song table (--song-table and --song-count override detection)");
    SEQLIB_CHECK(!found_blank && blank_error.empty());
}

void TestDetectionFromCode()
{
    // A cartridge with the play-song and note-start code, and three songs. Every track of song 0 plays samples 0, 1 and
    // 2. Sample 1's negative length selects a mode that the model doesn't support. The tracks of song 1 use F4. The
    // model doesn't support that command either. Song 2 rests.
    Image m(0x1000);
    constexpr uint32_t kTable = kRomBase + 0x100;
    constexpr uint32_t kSamples = kRomBase + 0x400;
    constexpr uint32_t kPlaySong = kRomBase + 0x800;
    constexpr uint32_t kNoteStart = kRomBase + 0x840;

    m.PutHalfwords(kPlaySong, kPlaySongCode);
    m.Put32(kPlaySong + 0x18, kTable);
    m.PutHalfwords(kNoteStart, kNoteStartCode);
    m.Put32(kNoteStart + 0x14, kSamples);

    m.Song(kTable, 0, kRomBase + 0x16C, 0, {});
    m.PutBytes(kRomBase + 0x16C, {0x00, 0xA7, 0x00, 0x01, 0xA7, 0x01, 0x01, 0xA7, 0x02, 0x01, 0xFE});
    m.Song(kTable, 1, kRomBase + 0x180, 0, {});
    m.PutBytes(kRomBase + 0x180, {0x00, 0xF4, 0x00, 0xFD});
    m.Song(kTable, 2, kRomBase + 0x190, 0, {});
    m.PutBytes(kRomBase + 0x190, {0x00, 0xE0, 0x00, 0xFD});

    for (uint32_t s = 0; s < 3; s++)
    {
        const uint32_t header = kRomBase + 0x500 + 0x40 * s;
        m.Put32(kSamples + 4 * s, header);
        m.Put32(header, 0x1000);
        m.Put32(header + 4, s == 1 ? uint32_t(-32) : 32u);
        m.Put32(header + 8, 0xFFFFFFFF);
    }

    DriverOverrides rate_only; // so that the mixer rate, which it has no code for, isn't a warning
    rate_only.mix_rate = 16000;
    rate_only.program = "mytool"; // the name that the messages give
    DriverInfo info;
    std::string err;

    const bool found = DetectDriver(m.ToRom(), rate_only, info, err);

    // Both tables come from the code. The count of songs ends before song 1, with a warning that says why. The sample
    // table holds sample 1 too, whose notes are skipped.
    const std::vector<std::string> kWarnings = {
        "song 1's tracks don't read cleanly, so the song table is taken to end before it (--song-count overrides this)",
        "mytool can't read these samples and skips their notes: 1"};
    SEQLIB_CHECK(found);
    SEQLIB_CHECK_EQ(info.song_table, kTable);
    SEQLIB_CHECK_EQ(info.song_count, 1);
    SEQLIB_CHECK_EQ(info.sample_table, kSamples);
    SEQLIB_CHECK(info.warnings == kWarnings);

    // When samples 0 and 2 have negative lengths too, the table the code loads doesn't fit. The error says so, and a
    // table at 0xB00 that the data scan would find, with its headers right after it, isn't taken instead.
    m.Put32(kRomBase + 0x504, uint32_t(-32));
    m.Put32(kRomBase + 0x584, uint32_t(-32));
    for (uint32_t s = 0; s < 3; s++)
    {
        const uint32_t header = kRomBase + 0xB0C + 44 * s;
        m.Put32(kRomBase + 0xB00 + 4 * s, header);
        m.Put32(header, 0x1000);
        m.Put32(header + 4, 32);
        m.Put32(header + 8, 0xFFFFFFFF);
    }

    SEQLIB_CHECK(!DetectDriver(m.ToRom(), rate_only, info, err));
    SEQLIB_CHECK(err ==
                 "found the note-start code, but mytool can't read most of the songs' samples in the sample table "
                 "it loads, at 0x08000400 (--sample-table overrides detection)");

    // When song 0 only rests, the table the code loads is still taken, although none of the songs counted plays a note.
    // With no samples played, no sample table is needed.
    m.PutBytes(kRomBase + 0x16C, {0x00, 0xE0, 0x00, 0xFD});

    const bool found_resting = DetectDriver(m.ToRom(), rate_only, info, err);

    SEQLIB_CHECK(found_resting);
    SEQLIB_CHECK_EQ(info.song_table, kTable);
    SEQLIB_CHECK_EQ(info.song_count, 1);
    SEQLIB_CHECK(info.warnings == std::vector<std::string>{kWarnings[0]});

    // When song 0 uses F4 too, there's no song to go by. The error says the code was found, and a table at 0xA00 that
    // the data scan would find, laid out like ScanRom's, isn't taken instead.
    m.PutBytes(kRomBase + 0x16C, {0x00, 0xF4, 0x00, 0xFD});
    m.Song(kRomBase + 0xA00, 0, kRomBase + 0xA48, 17, {{4, 0}, {0, 8}});
    m.PutBytes(kRomBase + 0xA48, {0x00, 0xA8, 0x10, 0x00, 0x0A, 0xE0, 0x05, 0xFE, 0x00, 0x81, 0x00, 0xD3, 0x18, 0x04,
                                  0xE0, 0x00, 0xFE, 0x00, 0xFD});
    m.Song(kRomBase + 0xA00, 1, kRomBase + 0xA5B, 8, {{5, 0}});
    m.PutBytes(kRomBase + 0xA5B, {0x00, 0xA3, 0x01, 0x08, 0xE0, 0x00, 0xFF, 0x00, 0x00, 0xFD});

    SEQLIB_CHECK(!DetectDriver(m.ToRom(), rate_only, info, err));
    SEQLIB_CHECK(err ==
                 "found the play-song code, but the song table it loads, at 0x08000100, holds no songs mytool can "
                 "read (--song-table and --song-count override detection)");
}

void TestSampleTableChoice()
{
    // Two copies of the note-start code load two sample tables. In the first, sample 1 has a negative length; in the
    // second, all three samples the song plays read.
    Image m(0x1000);
    constexpr uint32_t kTable = kRomBase + 0x100;
    m.Song(kTable, 0, kRomBase + 0x200, 0, {});
    m.PutBytes(kRomBase + 0x200, {0x00, 0xA7, 0x00, 0x01, 0xA7, 0x01, 0x01, 0xA7, 0x02, 0x01, 0xFE});

    for (uint32_t copy = 0; copy < 2; copy++)
    {
        const uint32_t code = kRomBase + 0x800 + 0x40 * copy;
        const uint32_t samples = kRomBase + 0x400 + 0x200 * copy;
        m.PutHalfwords(code, kNoteStartCode);
        m.Put32(code + 0x14, samples);

        for (uint32_t s = 0; s < 3; s++)
        {
            const uint32_t header = samples + 0x100 + 0x40 * s;
            m.Put32(samples + 4 * s, header);
            m.Put32(header, 0x1000);
            m.Put32(header + 4, copy == 0 && s == 1 ? uint32_t(-32) : 32u);
            m.Put32(header + 8, 0xFFFFFFFF);
        }
    }

    DriverOverrides ov;
    ov.song_table = kTable;
    ov.song_count = 1;
    ov.mix_rate = 16000;
    DriverInfo info;
    std::string err;

    const bool found = DetectDriver(m.ToRom(), ov, info, err);

    // The table in which every sample reads is taken, rather than the first one found.
    SEQLIB_CHECK(found);
    SEQLIB_CHECK_EQ(info.sample_table, kRomBase + 0x600);
    SEQLIB_CHECK(info.warnings.empty());

    // Without the second copy of the code, and with a song that only plays samples 0 and 1, half of the samples in the
    // first table read, which is enough.
    m.PutHalfwords(kRomBase + 0x840, std::vector<uint16_t>(kNoteStartCode.size(), 0));
    m.PutBytes(kRomBase + 0x200, {0x00, 0xA7, 0x00, 0x01, 0xA7, 0x01, 0x01, 0xFE});

    const bool found_half = DetectDriver(m.ToRom(), ov, info, err);

    SEQLIB_CHECK(found_half);
    SEQLIB_CHECK_EQ(info.sample_table, kRomBase + 0x400);
    SEQLIB_CHECK(info.warnings == std::vector<std::string>{"ntdlib can't read these samples and skips their notes: 1"});
}

void TestWaveLoaderForms()
{
    // A song that rests, and the wave RAM loader in the form that checks for volume 0 first. Its first table is at
    // 0x600 and its second at 0x700.
    Image m(0x1000);
    constexpr uint32_t kTable = kRomBase + 0x100;
    constexpr uint32_t kLoader = kRomBase + 0x800;
    m.Song(kTable, 0, kRomBase + 0x200, 0, {});
    m.PutBytes(kRomBase + 0x200, {0x00, 0xE0, 0x00, 0xFD});
    CodeWithLiterals(m, kLoader, {0xB510, 0x1C04, 0x4B00, 0x2A00, 0xD01E, 0x2080, 0x4008, 0x2800, 0xD002, 0x4B00},
                     {{2, kRomBase + 0x600}, {9, kRomBase + 0x700}});

    DriverOverrides ov;
    ov.song_table = kTable;
    ov.song_count = 1;
    ov.mix_rate = 16000;
    DriverInfo info;
    std::string err;

    const bool found = DetectDriver(m.ToRom(), ov, info, err);

    SEQLIB_CHECK(found);
    SEQLIB_CHECK_EQ(info.wave_table, kRomBase + 0x600);
    SEQLIB_CHECK_EQ(info.sfx_wave_table, kRomBase + 0x700);

    // The form without the check for volume 0, as in Hikaru no Go 2, gives both tables too.
    m.PutHalfwords(kLoader, std::vector<uint16_t>(16, 0));
    CodeWithLiterals(m, kLoader, {0xB510, 0x1C04, 0x4B00, 0x2080, 0x4008, 0x2800, 0xD002, 0x4B00},
                     {{2, kRomBase + 0x680}, {7, kRomBase + 0x780}});

    const bool found_without_check = DetectDriver(m.ToRom(), ov, info, err);

    SEQLIB_CHECK(found_without_check);
    SEQLIB_CHECK_EQ(info.wave_table, kRomBase + 0x680);
    SEQLIB_CHECK_EQ(info.sfx_wave_table, kRomBase + 0x780);
}

int RunTests()
{
    TestDetectionScan();
    TestSongTableFromOtherCode();
    TestMixerRate();
    TestBendDetection();
    TestRevisionDetection();
    TestDetectionFromCode();
    TestSampleTableChoice();
    TestWaveLoaderForms();

    return seqlib::testing::Summary("konami_detection");
}

} // namespace
} // namespace ntdlib::sequence_export::konami

int main()
{
    return ntdlib::sequence_export::konami::RunTests();
}
