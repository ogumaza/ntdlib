// SPDX-License-Identifier: MIT

// Unit tests for the Dungeon Dice Monsters revision of Konami's driver: its decoding, its sequencer, its detection and
// its conversion. They build small synthetic cartridges in memory. No game ROM is needed.

#include "konami_test_util.h"

namespace ntdlib::sequence_export::konami
{
namespace
{

void TestDungeonDiceDecoding()
{
    // Delays: a byte up to EF, or two bytes that give 12 bits from F0 up, as in the Eternal Duelist revision.
    Rom delays;
    delays.Assign({0xEF, 0xF1, 0x23});
    uint32_t delay = 0;

    SEQLIB_CHECK_EQ(ReadDelay(delays, kRomBase + 0, Revision::kDungeonDiceMonsters, delay), 1);
    SEQLIB_CHECK_EQ(delay, 0xEF);
    SEQLIB_CHECK_EQ(ReadDelay(delays, kRomBase + 1, Revision::kDungeonDiceMonsters, delay), 2);
    SEQLIB_CHECK_EQ(delay, 0x123);

    // E5 30 / B9 0C / A2 3C / 9A 18 / C7 / D6 / 84 / 71 / 7D / 45 / F0 53 / F1 30 / F2 10 / F3 30 / F4 34 12 05 /
    // F5 34 12 05 / F7 / F9 / FA / FB / FC / FD / FE / FF
    Rom rom;
    rom.Assign({0xE5, 0x30, 0xB9, 0x0C, 0xA2, 0x3C, 0x9A, 0x18, 0xC7, 0xD6, 0x84, 0x71, 0x7D,
                0x45, 0xF0, 0x53, 0xF1, 0x30, 0xF2, 0x10, 0xF3, 0x30, 0xF4, 0x34, 0x12, 0x05,
                0xF5, 0x34, 0x12, 0x05, 0xF7, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF});
    auto at = [&](uint32_t offset)
    {
        return DecodeCommand(rom, kRomBase + offset, 4, Revision::kDungeonDiceMonsters);
    };

    // The notes: Ex nn on the PSG, Bx nn from the sample map, Ax nn the same on the next track, and 9x nn the track's
    // sample at note nn, each at volume x. Cx restarts the note at volume x, and Dx only sets the volume.
    SEQLIB_CHECK(at(0).op == Op::kPsgNote && at(0).vol == 5 && at(0).note == 0x30 && at(0).length == 2);
    SEQLIB_CHECK(at(2).op == Op::kNote && at(2).vol == 9 && at(2).sample == 0x0C && at(2).length == 2);
    SEQLIB_CHECK(at(4).op == Op::kNextNote && at(4).vol == 2 && at(4).sample == 0x3C);
    SEQLIB_CHECK(at(6).op == Op::kSampleAtNote && at(6).vol == 10 && at(6).note == 0x18);
    SEQLIB_CHECK(at(8).op == Op::kVolume && at(8).vol == 7 && at(8).length == 1);
    SEQLIB_CHECK(at(9).op == Op::kSetVolume && at(9).vol == 6);

    // 8x is vibrato at depth x, 70-7B load a wave, 7C-7F set a duty, and 00-6F set the duty byte itself.
    SEQLIB_CHECK(at(10).op == Op::kVibrato && at(10).value == 4);
    SEQLIB_CHECK(at(11).op == Op::kWave && at(11).value == 1);
    SEQLIB_CHECK(at(12).op == Op::kDuty && at(12).value == 0x40);
    SEQLIB_CHECK(at(13).op == Op::kDuty && at(13).value == 0x45);

    // F0 sets NR50, F1 and F2 bend a sample note, of the next track and this one, and F3 bends a PSG note, all in
    // sixteenths from the note.
    SEQLIB_CHECK(at(14).op == Op::kPsgVolume && at(14).value == 0x53 && at(14).length == 2);
    SEQLIB_CHECK(at(16).op == Op::kSampleBend && at(16).opcode == 0xF1 && at(16).value == 16);
    SEQLIB_CHECK(at(18).op == Op::kSampleBend && at(18).opcode == 0xF2 && at(18).value == -16);
    SEQLIB_CHECK(at(20).op == Op::kPitchBend && at(20).value == 16 && at(20).length == 2);

    // F4 and F5 call commands of the track's data. F5's fifth byte is read after the call.
    SEQLIB_CHECK(at(22).op == Op::kCall && at(22).value == 0x1234 && at(22).count == 5 && at(22).length == 4);
    SEQLIB_CHECK(at(26).op == Op::kCall && at(26).opcode == 0xF5 && at(26).length == 4);

    // F7 sets a flag that nothing reads, F9 fades the next track out, FA is a loop point with two bytes after it, FB
    // releases the note, FC rests, FD ends the track, FE loops the song and FF stops it.
    SEQLIB_CHECK(at(30).op == Op::kNop);
    SEQLIB_CHECK(at(31).op == Op::kFade && at(31).opcode == 0xF9);
    SEQLIB_CHECK(at(32).op == Op::kLoopPoint && at(32).length == 3);
    SEQLIB_CHECK(at(33).op == Op::kRelease);
    SEQLIB_CHECK(at(34).op == Op::kRest);
    SEQLIB_CHECK(at(35).op == Op::kEndTrack);
    SEQLIB_CHECK(at(36).op == Op::kJump && at(36).value == 1);
    SEQLIB_CHECK(at(37).op == Op::kJump && at(37).value == 0);
}

void TestDungeonDiceSequencer()
{
    // Song 0. Square 1: NR50 53, note (vol 8, note 0x20), wait 4, vibrato at depth 2 and note 0x21, wait 12, fade out,
    // wait 6, end. Noise: note (vol 4, note 0x48), wait 1, end. Voice 0: note (vol 12, map entry 1) and a note on voice
    // 1 (vol 6, map entry 2), wait 4, fade voice 1 out, wait 8, a bend of +16 from sample 5's rate, wait 4, note 0x1C
    // of the track's sample (vol 9) and a bend of +2 in the same frame, wait 4, end. Square 2: wait 2, loop point with
    // a wait of 0 and duty 75% for the loop, wait 2, note (vol 7, note 0x10), wait 18, loop the song.
    Image m(0x1000);
    constexpr uint32_t kTable = kRomBase + 0x100;
    constexpr uint32_t kBase = kRomBase + 0x200;
    constexpr uint32_t kMap = kRomBase + 0x600;
    constexpr uint32_t kPeriods = kRomBase + 0x800;
    m.Song(kTable, 0, kBase, 0x60, {{0, 0x00}, {1, 0x40}, {3, 0x20}, {4, 0x28}}, Revision::kDungeonDiceMonsters);
    m.PutBytes(kBase + 0x00,
               {0x00, 0xF0, 0x53, 0x00, 0xE8, 0x20, 0x04, 0x82, 0x00, 0xE8, 0x21, 0x0C, 0xF8, 0x06, 0xFD});
    m.PutBytes(kBase + 0x20, {0x00, 0xE4, 0x48, 0x01, 0xFD});
    m.PutBytes(kBase + 0x28, {0x00, 0xBC, 0x01, 0x00, 0xA6, 0x02, 0x04, 0xF9, 0x08, 0xF2, 0x30, 0x04, 0x99, 0x1C, 0x00,
                              0xF2, 0x22, 0x04, 0xFD});
    m.PutBytes(kBase + 0x40, {0x02, 0xFA, 0x00, 0x7F, 0x02, 0xE7, 0x10, 0x12, 0xFE});
    m.PutBytes(kBase + 0x60, {0x00, 0xFD});
    m.PutBytes(kMap, {0x00, 0x05, 0x07});
    for (uint32_t p = 0; p < 0x200; p++)
    {
        m.Put16(kPeriods + 2 * p, uint16_t(0x4000 - p));
    }

    // Song 1. Square 1: a call of 1 command from the fragment after the end, with duty byte 10 first, which is also the
    // wait after the call, note (vol 9, note 0x20), then 260 duty bytes of 07, each followed by a wait of 1, and a
    // stop. The fragment: wait 3, note (vol 5, note 0x24), wait 6.
    constexpr uint32_t kBase1 = kRomBase + 0x280;
    std::vector<uint8_t> calls = {0x00, 0xF5, 0x00, 0x00, 0x01, 0x10, 0xE9, 0x20, 0x01};
    for (int i = 0; i < 260; i++)
    {
        calls.insert(calls.end(), {0x07, 0x01});
    }
    calls.push_back(0xFF);
    calls[2] = uint8_t(calls.size());
    calls[3] = uint8_t(calls.size() >> 8);
    calls.insert(calls.end(), {0x03, 0xE5, 0x24, 0x06});
    m.Song(kTable, 1, kBase1, 0x260, {{0, 0x00}}, Revision::kDungeonDiceMonsters);
    m.PutBytes(kBase1, calls);
    m.PutBytes(kBase1 + 0x260, {0x00, 0xFD});

    const Rom rom = m.ToRom();
    DriverInfo info = SongsAt(kTable);
    info.revision = Revision::kDungeonDiceMonsters;
    info.sample_map = kMap;
    info.sample_period_table = kPeriods;
    info.vibrato_entry = 1000;
    DungeonDiceSequencer seq(rom, info, 0);
    DungeonDiceSequencer calling(rom, info, 1);

    std::vector<std::array<TrackOutput, kTracks>> frames;
    std::vector<int> psg_volume;
    int looped_at = -1;
    for (int f = 0; f < 26; f++)
    {
        frames.push_back(seq.Step());
        psg_volume.push_back(seq.PsgVolume());
        looped_at = looped_at < 0 && seq.LoopedLastFrame() ? f : looped_at;
    }

    std::vector<std::pair<int, int>> starts; // (frame, duty byte) of each of square 1's notes
    int stopped_at = -1;
    for (int f = 0; f < 300 && stopped_at < 0; f++)
    {
        const TrackOutput& o = calling.Step()[0];
        if (o.trig && o.flags == 1 && o.vol)
        {
            starts.emplace_back(f, o.b2);
        }
        stopped_at = calling.Stopped() ? f : -1;
    }

    // Pitches go up 16 entries a note until note 71, and one entry a note from note 72 on. F0 sets NR50.
    SEQLIB_CHECK(seq.Valid() && frames[0][7].active);
    SEQLIB_CHECK(frames[0][0].pitch == 0x200 && frames[0][0].flags == 1 && frames[0][0].vol == 8);
    SEQLIB_CHECK_EQ(frames[0][3].pitch, 0x480);
    SEQLIB_CHECK_EQ(psg_volume[0], 0x53);

    // Every 4th frame, vibrato outputs a pitch from the table's vibrato section, alternating with 2 entries below it.
    SEQLIB_CHECK(frames[6][0].flags == 0 && frames[7][0].flags == 1 && frames[7][0].trig == 0);
    SEQLIB_CHECK_EQ(frames[7][0].pitch, 1000 + 0x84 - 2);
    SEQLIB_CHECK_EQ(frames[11][0].pitch, 1000 + 0x84);

    // The fade takes the volume down a quarter a frame, and ends by handing the channel a duty byte and volume of 0.
    SEQLIB_CHECK(frames[16][0].vol == 6 && frames[17][0].vol == 4 && frames[18][0].vol == 2);
    SEQLIB_CHECK(frames[19][0].vol == 0 && frames[19][0].trig == 1 && frames[19][0].b2 == 0);

    // Voice 0's note plays sample 5, and its note on voice 1 goes into voice 1's record. F9 fades voice 1 out from the
    // next frame on, since voice 1's track has had its turn.
    SEQLIB_CHECK(frames[0][4].b2 == 5 && frames[0][4].pitch == 0 && frames[0][4].flags == 1);
    SEQLIB_CHECK(frames[0][5].b2 == 7 && frames[0][5].vol == 6 && frames[0][5].trig == 1);
    SEQLIB_CHECK(frames[4][5].vol == 6 && frames[5][5].vol == 4 && frames[7][5].vol == 1);
    SEQLIB_CHECK(frames[8][5].vol == 0 && frames[8][5].trig == 1);

    // A bend without a new note outputs the period for pitch 24 * 16 + 1 + 16 with flag 40. A bend in the same frame as
    // a new note starts that note at the bent pitch.
    SEQLIB_CHECK(frames[12][4].flags == 0x40 && frames[12][4].pitch == 0x4000 - 0x191);
    SEQLIB_CHECK(frames[16][4].flags == 1 && frames[16][4].pitch == 0x1C1 + 2 && frames[16][4].vol == 9);

    // Square 2 skips its loop point's wait and duty until the song loops, and then reads them.
    SEQLIB_CHECK_EQ(looped_at, 22);
    SEQLIB_CHECK(frames[4][1].b2 == 0x80 && frames[4][1].trig == 1);
    SEQLIB_CHECK(frames[24][1].b2 == 0xC0 && frames[24][1].vol == 7);

    // F5's duty byte starts the call, whose note has it, and the note after the call comes 16 frames after the call
    // returns. The driver keeps the call's return position, so the 256th command after the call takes the track back
    // there, and the track never reaches its stop.
    SEQLIB_CHECK(starts == (std::vector<std::pair<int, int>>{{3, 0x10}, {19, 0x10}, {290, 0x07}}));
    SEQLIB_CHECK_EQ(stopped_at, -1);
}

// The tables of DungeonDiceImage().
constexpr uint32_t kDdmTable = kRomBase + 0x100;
constexpr uint32_t kDdmBase = kRomBase + 0x200;
constexpr uint32_t kDdmSamples = kRomBase + 0x2000;
constexpr uint32_t kDdmMap = kRomBase + 0x2200;
constexpr uint32_t kDdmPeriods = kRomBase + 0x2400;
constexpr uint32_t kDdmFreq = kRomBase + 0x2C00;
constexpr uint32_t kDdmWaves = kRomBase + 0x3800;
constexpr uint32_t kDdmWaveVolumes = kRomBase + 0x3900;

// Returns a cartridge with the code of the Dungeon Dice Monsters revision that detection looks for, its tables, and a
// song. Square 1: NR50 53, vibrato at depth 2, note (vol 8, note 0x20), wait 16, vibrato off, rest, end. Wave: wave 0,
// note (vol 9, note 0x20), wait 8, wave 1, wait 8, rest, end. Noise: note (vol 8, note 0x4A), wait 4, rest, end. Voice
// 0: note (vol 8, map entry 5, sample 3), wait 8, duty byte 3 and note 0x1C of the track's sample (vol 10), wait 8, a
// bend of +16, wait 8, rest, end.
Image DungeonDiceImage()
{
    Image m(0x4000);
    m.PutHalfwords(kRomBase + 0x800, CommandReader(0x2DFE, 0x2DFC));
    CodeWithLiterals(m, kRomBase + 0x1000, {0x00A0, 0x1900, 0x0080, 0x4900, 0x1841, 0x884C, 0x0420, 0x880D, 0x4328},
                     {{3, kDdmTable}});
    CodeWithLiterals(m, kRomBase + 0x1040, {0x00D1, 0x4800, 0x1808, 0x6805, 0x6841}, {{1, kDdmSamples}});
    CodeWithLiterals(m, kRomBase + 0x1080, {0x4900, 0x2400, 0x5F30, 0x0040, 0x1840, 0x8800}, {{0, kDdmFreq}});
    CodeWithLiterals(m, kRomBase + 0x10C0, {0x4800, 0x786D, 0x1828, 0x7800, 0x70A0}, {{0, kDdmMap}});
    CodeWithLiterals(m, kRomBase + 0x1100, {0x4800, 0x0051, 0x1809, 0x8808, 0x8038, 0x8808, 0x8018},
                     {{0, kDdmPeriods}});
    CodeWithLiterals(m, kRomBase + 0x1140, {0x0118, 0x4900, 0x1840, 0x6010}, {{1, kDdmWaves}});
    CodeWithLiterals(m, kRomBase + 0x1180, {0x4900, 0x7EF2, 0x0050, 0x1840, 0x7EB3, 0x8800, 0x4303},
                     {{0, kDdmWaveVolumes}});
    CodeWithLiterals(m, kRomBase + 0x11C0, {0x109B, 0x4800, 0x4900, 0x1A40}, {{1, kDdmFreq + 2 * 1164}, {2, kDdmFreq}});

    m.Song(kDdmTable, 0, kDdmBase, 0x60, {{0, 0x00}, {2, 0x10}, {3, 0x20}, {4, 0x30}}, Revision::kDungeonDiceMonsters);
    m.PutBytes(kDdmBase + 0x00, {0x00, 0xF0, 0x53, 0x00, 0x82, 0x00, 0xE8, 0x20, 0x10, 0x80, 0x00, 0xFC, 0x00, 0xFD});
    m.PutBytes(kDdmBase + 0x10, {0x00, 0x70, 0x00, 0xE9, 0x20, 0x08, 0x71, 0x08, 0xFC, 0x00, 0xFD});
    m.PutBytes(kDdmBase + 0x20, {0x00, 0xE8, 0x4A, 0x04, 0xFC, 0x00, 0xFD});
    m.PutBytes(kDdmBase + 0x30,
               {0x00, 0xB8, 0x05, 0x08, 0x03, 0x00, 0x9A, 0x1C, 0x08, 0xF2, 0x30, 0x08, 0xFC, 0x00, 0xFD});
    m.PutBytes(kDdmBase + 0x60, {0x00, 0xFD});

    // Sample 3: 64 points of a sawtooth at a period of 1677, with map entry 5 naming it. Pitch 24 * 16 + 1 has the
    // sample's period, and the pitches of notes 28 and 29 the periods 4 and 5 semitones up.
    m.Put32(kDdmSamples + 8 * 3, kDdmSamples + 0x100);
    m.Put32(kDdmSamples + 8 * 3 + 4, (4u << 20) | 1677);
    for (uint32_t i = 0; i < 64; i++)
    {
        m.Put8(kDdmSamples + 0x100 + i, uint8_t(i * 4));
    }
    m.Put8(kDdmMap + 5, 3);
    m.Put16(kDdmPeriods + 2 * (24 * 16 + 1), 1677);
    m.Put16(kDdmPeriods + 2 * (28 * 16 + 1), 1331);
    m.Put16(kDdmPeriods + 2 * (29 * 16 + 1), 1256);

    // The frequency table: a register value for each 1/16 semitone, restarting the channel, then 12 noise settings, and
    // the vibrato part, 4 entries for each note.
    for (uint32_t p = 0; p < 1152; p++)
    {
        m.Put16(kDdmFreq + 2 * p, uint16_t(0x8000 | std::min<uint32_t>(44 + p, 2047)));
    }
    for (uint32_t i = 0; i < 12; i++)
    {
        m.Put16(kDdmFreq + 2 * (1152 + i), uint16_t(0x8010 + 0x10 * i));
    }
    for (uint32_t i = 0; i < 288; i++)
    {
        m.Put16(kDdmFreq + 2 * (1164 + i), uint16_t(0x8000 | std::min<uint32_t>(44 + 4 * i, 2047)));
    }

    // Waves 0 and 1, a square and a sawtooth, and the volume codes: 25%, 50%, 75% and 100% from volumes 1, 3, 6 and 9.
    for (uint32_t i = 0; i < 16; i++)
    {
        m.Put8(kDdmWaves + i, i < 8 ? 0xFF : 0x00);
        m.Put8(kDdmWaves + 16 + i, uint8_t(i * 0x11));
    }
    constexpr uint16_t kCodes[16] = {0x0000, 0x6000, 0x6000, 0x4000, 0x4000, 0x4000, 0x8000, 0x8000,
                                     0x8000, 0x2000, 0x2000, 0x2000, 0x2000, 0x2000, 0x2000, 0x2000};
    for (uint32_t v = 0; v < 16; v++)
    {
        m.Put16(kDdmWaveVolumes + 2 * v, kCodes[v]);
    }

    return m;
}

void TestDungeonDiceLoopDelay()
{
    // Square 2: wait 2, a loop point with a wait for the loop and duty 75%, wait 2, note (vol 7, note 0x10), wait 18,
    // loop the song. The first pass skips the wait. With a wait of 3, the later passes play the track 3 frames later,
    // and the conversion marks the second pass as the loop. A wait of 0 moves nothing.
    for (const uint8_t wait : {uint8_t(0), uint8_t(3)})
    {
        Image m(0x1000);
        constexpr uint32_t kTable = kRomBase + 0x100;
        constexpr uint32_t kBase = kRomBase + 0x200;
        constexpr uint32_t kMap = kRomBase + 0x600;
        constexpr uint32_t kPeriods = kRomBase + 0x800;
        m.Song(kTable, 0, kBase, 0x60, {{1, 0x40}}, Revision::kDungeonDiceMonsters);
        m.PutBytes(kBase + 0x40, {0x02, 0xFA, wait, 0x7F, 0x02, 0xE7, 0x10, 0x12, 0xFE});
        m.PutBytes(kBase + 0x60, {0x00, 0xFD});
        m.PutBytes(kMap, {0x00, 0x05, 0x07});
        for (uint32_t p = 0; p < 0x200; p++)
        {
            m.Put16(kPeriods + 2 * p, uint16_t(0x4000 - p));
        }
        const Rom rom = m.ToRom();
        DriverInfo info = SongsAt(kTable);
        info.revision = Revision::kDungeonDiceMonsters;
        info.sample_map = kMap;
        info.sample_period_table = kPeriods;
        DungeonDiceSequencer seq(rom, info, 0);

        bool looped = false;
        for (int f = 0; f < 40 && !looped; f++)
        {
            seq.Step();
            looped = seq.LoopedLastFrame();
        }

        SEQLIB_CHECK(looped);
        SEQLIB_CHECK_EQ(seq.LoopPointDelayed(), wait > 0);
    }
}

void TestDungeonDiceDetection()
{
    const Rom rom = DungeonDiceImage().ToRom();
    DriverInfo info;
    std::string error;

    const bool found = DetectDriver(rom, DriverOverrides(), info, error);

    // Every table comes from the code that loads it. The noise settings are the frequency table's entries from note
    // 72's on, and there's no mixer.
    SEQLIB_CHECK(found && info.revision == Revision::kDungeonDiceMonsters);
    SEQLIB_CHECK(info.song_table == kDdmTable && info.song_count == 1);
    SEQLIB_CHECK_EQ(info.sample_table, kDdmSamples);
    SEQLIB_CHECK(info.sample_map == kDdmMap && info.sample_period_table == kDdmPeriods);
    SEQLIB_CHECK(info.psg_freq_table == kDdmFreq && info.noise_table == kDdmFreq + 2 * 1152);
    SEQLIB_CHECK(info.wave_table == kDdmWaves && info.wave_volume_table == kDdmWaveVolumes);
    SEQLIB_CHECK_EQ(info.vibrato_entry, 1164);
    SEQLIB_CHECK(info.mix_rate == 0 && info.warnings.empty());

    // A sample table entry gives the data, the length in blocks of 16 samples, and the sample's rate.
    const SampleInfo s = info.Sample(rom, 3);
    SEQLIB_CHECK(s.valid && s.data == kDdmSamples + 0x100 && s.length == 64 && !s.Looped());
    SEQLIB_CHECK(std::abs(s.Rate(0) - 16777216.0 / 1677) < 1e-6);
}

// Without the code that loads the PSG frequency table, detection still finds the driver, and warns that the square,
// wave and noise notes can't play as they should.
void TestDungeonDiceNoFrequencyTable()
{
    Image m = DungeonDiceImage();
    m.PutHalfwords(kRomBase + 0x1080, {0, 0, 0, 0, 0, 0});
    const Rom rom = m.ToRom();
    DriverInfo info;
    std::string error;

    const bool found = DetectDriver(rom, DriverOverrides(), info, error);

    SEQLIB_CHECK(found && info.psg_freq_table == 0);
    SEQLIB_CHECK(info.warnings.size() == 1 &&
                 info.warnings[0] ==
                     "PSG frequency table not found, so square notes are left out, wave notes "
                     "play at the wrong pitch and noise notes use default settings");
}

// Detection recognises code that works the song table's address out as the play-song routine does, in other registers.
// This table isn't laid out the way the data scan's checks expect, and detection says so.
void TestDungeonDiceSongTableFallback()
{
    Image m = DungeonDiceImage();
    CodeWithLiterals(m, kRomBase + 0x1000, {0x00AA, 0x1952, 0x0092, 0x4900, 0x1889, 0x884C, 0x0420, 0x880D, 0x4328},
                     {{3, kDdmTable}});
    const Rom rom = m.ToRom();
    DriverInfo info;
    std::string error;

    DetectDriver(rom, DriverOverrides(), info, error);

    const auto recognised = [](const std::string& line)
    {
        return line.find("found code like the play-song routine") == 0;
    };
    SEQLIB_CHECK(std::any_of(info.log.begin(), info.log.end(), recognised));
}

void TestDungeonDiceConversion()
{
    const Rom rom = DungeonDiceImage().ToRom();
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, DriverOverrides(), info, error));

    const fs::path dir = TempPath("dungeon_dice");
    std::error_code ec;
    fs::create_directories(dir, ec);
    ConvertSettings opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    // Each track's note-ons (frame, key, program), bends (frame, value) and pans after the first, which centres it.
    std::map<std::string, std::vector<std::array<int, 3>>> notes;
    std::map<std::string, std::vector<std::pair<uint32_t, int>>> bends;
    std::map<std::string, int> program;
    std::map<std::string, std::vector<int>> pans;
    std::set<std::string> centred;
    for (const MidiEvent& e : ReadEvents(sum.midi_path))
    {
        const int kind = e.status & 0xF0;
        if (kind == 0xC0)
        {
            program[e.track] = e.data1;
        }
        if (kind == 0x90 && e.data2 > 0)
        {
            notes[e.track].push_back({int(e.frame), e.data1, program[e.track]});
        }
        if (kind == 0xE0 && e.frame > 0)
        {
            bends[e.track].emplace_back(e.frame, e.data1 | (e.data2 << 7));
        }
        if (kind == 0xB0 && e.data1 == cc::kPan && !centred.insert(e.track).second)
        {
            pans[e.track].push_back(e.data2);
        }
    }

    // Square 1's vibrato steps restart the channel, but bend its one note, and NR50 plays it louder on the left.
    SEQLIB_CHECK(sum.ok);
    SEQLIB_REQUIRE_EQ(notes["Square 1"].size(), 1);
    SEQLIB_REQUIRE(bends["Square 1"].size() >= 2);
    SEQLIB_CHECK(pans["Square 1"].size() == 1 && pans["Square 1"][0] < 64);

    // Loading a wave while a note plays restarts the channel with the new wave, a new note on another program.
    SEQLIB_REQUIRE_EQ(notes["Wave"].size(), 2);
    SEQLIB_CHECK_EQ(notes["Wave"][1][0], 8);
    SEQLIB_CHECK(notes["Wave"][1][2] != notes["Wave"][0][2]);

    // Noise note 0x4A is the third noise setting, on key 38.
    SEQLIB_CHECK(notes["Noise"].size() == 1 && notes["Noise"][0][1] == 38);

    // Voice 0's note at pitch 28 * 16 + 1 plays 4 semitones above its note at the sample's rate, and F2 bends it up a
    // semitone, the most the range of 2 semitones needs.
    SEQLIB_REQUIRE_EQ(notes["Voice 0"].size(), 2);
    SEQLIB_CHECK_EQ(notes["Voice 0"][1][1] - notes["Voice 0"][0][1], 4);
    SEQLIB_CHECK(bends["Voice 0"] == (std::vector<std::pair<uint32_t, int>>{{16, 12288}}));

    fs::remove_all(dir, ec);
}

int RunTests()
{
    TestDungeonDiceDecoding();
    TestDungeonDiceSequencer();
    TestDungeonDiceLoopDelay();
    TestDungeonDiceDetection();
    TestDungeonDiceNoFrequencyTable();
    TestDungeonDiceSongTableFallback();
    TestDungeonDiceConversion();

    return seqlib::testing::Summary("konami_dungeon_dice");
}

} // namespace
} // namespace ntdlib::sequence_export::konami

int main()
{
    return ntdlib::sequence_export::konami::RunTests();
}
