// SPDX-License-Identifier: MIT

// Unit tests for Konami's driver: its decoder, its sequencer, the loops, the later builds and the Live Stage samples.
// konami_detection_test.cpp tests the detection. They build small synthetic cartridges in memory. No game ROM is
// needed.

#include "konami_test_util.h"

namespace ntdlib::sequence_export::konami
{
namespace
{

void TestDecoding()
{
    std::vector<uint8_t> d(0x40, 0);
    const std::vector<uint8_t> kStream = {0x05, 0xE3, 0x10, 0xF0, 0x34, 0x12, 0xF3, 0xAA, 0xBB, 0xB8, 0x20,
                                          0x07, 0xFE, 0xA3, 0x09, 0xC9, 0x0C, 0xD2, 0x30, 0x83, 0x06, 0xFF};
    std::copy(kStream.begin(), kStream.end(), d.begin());
    Rom rom;
    rom.Assign(d);
    uint32_t delay = 0;

    SEQLIB_CHECK_EQ(ReadDelay(rom, kRomBase + 0, Revision::kUltimateMasters, delay), 1);
    SEQLIB_CHECK_EQ(delay, 5);
    SEQLIB_CHECK_EQ(ReadDelay(rom, kRomBase + 1, Revision::kUltimateMasters, delay), 2);
    SEQLIB_CHECK_EQ(delay, 0x310);
    SEQLIB_CHECK_EQ(ReadDelay(rom, kRomBase + 3, Revision::kUltimateMasters, delay), 3);
    SEQLIB_CHECK_EQ(delay, 0x1234);

    // F3 carries two extra bytes on sample tracks only.
    SEQLIB_CHECK_EQ(DecodeCommand(rom, kRomBase + 6, 0, Revision::kUltimateMasters).length, 1);
    SEQLIB_CHECK(DecodeCommand(rom, kRomBase + 6, 4, Revision::kUltimateMasters).op == Op::kLoopPoint);
    SEQLIB_CHECK_EQ(DecodeCommand(rom, kRomBase + 6, 4, Revision::kUltimateMasters).length, 3);

    Command c =
        DecodeCommand(rom, kRomBase + 9, 4, Revision::kUltimateMasters); // B8 20 07 FE: vol byte, sample, semitone

    SEQLIB_CHECK(c.op == Op::kNote);
    SEQLIB_CHECK_EQ(c.vol, 0x20);
    SEQLIB_CHECK_EQ(c.sample, 7);
    SEQLIB_CHECK_EQ(c.semitone, -2);
    SEQLIB_CHECK_EQ(c.length, 4);

    c = DecodeCommand(rom, kRomBase + 13, 4, Revision::kUltimateMasters); // A3 09: vol 3, sample 9
    SEQLIB_CHECK(c.op == Op::kNote && c.vol == 3 && c.sample == 9 && c.semitone == 0 && c.length == 2);

    c = DecodeCommand(rom, kRomBase + 15, 0, Revision::kUltimateMasters); // C9 0C: volume byte
    SEQLIB_CHECK(c.op == Op::kVolume && c.vol == 12 && c.length == 2);

    c = DecodeCommand(rom, kRomBase + 17, 0, Revision::kUltimateMasters); // D2 30: PSG note, vol 2
    SEQLIB_CHECK(c.op == Op::kPsgNote && c.vol == 2 && c.note == 0x30 && c.length == 2);

    c = DecodeCommand(rom, kRomBase + 19, 0, Revision::kUltimateMasters); // 83: duty 75%
    SEQLIB_CHECK(c.op == Op::kDuty && c.value == 0xC0);

    c = DecodeCommand(rom, kRomBase + 20, 2, Revision::kUltimateMasters); // 06: wave 2
    SEQLIB_CHECK(c.op == Op::kDuty && c.value == 2);

    c = DecodeCommand(rom, kRomBase + 21, 4, Revision::kUltimateMasters); // FF 00: stop
    SEQLIB_CHECK(c.op == Op::kJump && c.value == 0 && c.length == 2);

    // F2 bends by its byte less 0x40. A byte above 7F takes the next one too: F2 xh ll bends by 0xhll - 0x400.
    Rom bends;
    bends.Assign({0xF2, 0x50, 0xF2, 0x84, 0x61, 0xF2, 0x90, 0x00});

    c = DecodeCommand(bends, kRomBase + 0, 4, Revision::kUltimateMasters);
    SEQLIB_CHECK(c.op == Op::kPitchBend && c.value == 0x10 && c.length == 2);

    c = DecodeCommand(bends, kRomBase + 2, 4, Revision::kUltimateMasters);
    SEQLIB_CHECK(c.op == Op::kPitchBend && c.value == 0x61 && c.length == 3);

    c = DecodeCommand(bends, kRomBase + 5, 0, Revision::kUltimateMasters);
    SEQLIB_CHECK(c.op == Op::kPitchBend && c.value == -0x400 && c.length == 3);
}

void TestSequencer()
{
    const Rom rom = SyntheticRom();
    UltimateMastersSequencer seq(rom, SongsAt(kSongTable), 0);
    SEQLIB_CHECK(seq.Valid());

    std::vector<std::array<TrackOutput, kTracks>> frames;
    for (int f = 0; f < 20; f++)
    {
        frames.push_back(seq.Step());

        if (f == 15)
        {
            SEQLIB_CHECK(seq.LoopedLastFrame());
            SEQLIB_CHECK_EQ(seq.LoopsDone(), 1);
            SEQLIB_CHECK_EQ(seq.LoopStartFrame(), 0); // track 4 loops the song, and has no loop point
        }
        else
        {
            SEQLIB_CHECK(!seq.LoopedLastFrame());
        }
    }

    // Track 4: note on at 0, rest at 10, note again at 15 after the song loops.
    SEQLIB_CHECK(frames[0][4].flags & kOutNoteOn);
    SEQLIB_CHECK_EQ(frames[0][4].key, 5);
    SEQLIB_CHECK_EQ(frames[0][4].vol, 16);
    for (int f = 1; f < 10; f++)
    {
        SEQLIB_CHECK(!frames[size_t(f)][4].Any());
    }
    SEQLIB_CHECK(frames[10][4].flags & kOutStop);
    SEQLIB_CHECK(frames[15][4].flags & kOutNoteOn);

    // Track 5: loop point at 5 (skipping its two extra bytes), note at 7 an octave up; the loop restarts it from the
    // loop point: note again at 17.
    SEQLIB_CHECK(frames[7][5].flags & kOutNoteOn);
    SEQLIB_CHECK_EQ(frames[7][5].note_pitch, 12 * 32);
    SEQLIB_CHECK_EQ(frames[7][5].vol, 7);
    SEQLIB_CHECK(frames[10][5].flags & kOutStop);
    SEQLIB_CHECK(frames[17][5].flags & kOutNoteOn);
    for (int f = 11; f < 17; f++)
    {
        SEQLIB_CHECK(!(frames[size_t(f)][5].flags & kOutNoteOn));
    }

    // Track 0: 25% duty note 24 at frame 0, then the rest silences it at 4.
    SEQLIB_CHECK(frames[0][0].trig && (frames[0][0].flags & kOutPsgNote));
    SEQLIB_CHECK_EQ(frames[0][0].b2 >> 6, 1);
    SEQLIB_CHECK_EQ(frames[0][0].pitch, 24 * 32);
    SEQLIB_CHECK_EQ(frames[0][0].vol, 3);
    SEQLIB_CHECK(frames[4][0].trig && frames[4][0].vol == 0);

    // Tracks ending with FD are gone for good, and don't come back on the loop.
    SEQLIB_CHECK(!frames[1][7].active);
    SEQLIB_CHECK(!frames[16][7].active);
}

// Runs an Ultimate Masters song until it first loops, and returns the frame it loops in.
int FirstLoop(UltimateMastersSequencer& seq)
{
    for (int f = 0; f < 20; f++)
    {
        seq.Step();
        if (seq.LoopedLastFrame())
        {
            return f;
        }
    }

    return -1;
}

void TestLoopStart()
{
    // Track 4 ends where it looped the song, and track 5 loops the song where it ended, at frame 11. It passed its loop
    // point at frame 5.
    Image m = SyntheticImage();
    m.PutBytes(kSongBase + 0x07, {0xFE});
    m.PutBytes(kSongBase + 0x8B, {0xFF, 0x01, 0x00});
    const Rom rom = m.ToRom();
    UltimateMastersSequencer seq(rom, SongsAt(kSongTable), 0);

    const int looped_at = FirstLoop(seq);

    // The loop goes back to the frame where track 5 passed its loop point.
    SEQLIB_CHECK_EQ(looped_at, 11);
    SEQLIB_CHECK_EQ(seq.LoopStartFrame(), 5);
}

void TestLoopStartEarliest()
{
    // As in TestLoopStart, but track 0 also passes a loop point, at frame 4: duty 25%, PSG note (vol 3, note 24), wait
    // 4, loop point, rest, end. The loop goes back to frame 4, the earlier of the two.
    Image m = SyntheticImage();
    m.PutBytes(kSongBase + 0x07, {0xFE});
    m.PutBytes(kSongBase + 0x8B, {0xFF, 0x01, 0x00});
    m.PutBytes(kSongBase + 0x40, {0x00, 0x81, 0x00, 0xD3, 0x18, 0x04, 0xF3, 0x00, 0xE0, 0x00, 0xFE});
    const Rom rom = m.ToRom();
    UltimateMastersSequencer seq(rom, SongsAt(kSongTable), 0);

    const int looped_at = FirstLoop(seq);

    SEQLIB_CHECK_EQ(looped_at, 11);
    SEQLIB_CHECK_EQ(seq.LoopStartFrame(), 4);
}

void TestLoopStartLast()
{
    // As in TestLoopStart, but track 5 passes a second loop point, at frame 8: wait 5, loop point, wait 3, loop point,
    // wait 2, note, wait 3, rest, wait 1, loop the song. Each loop takes the track back to the second loop point, so
    // the song loops at frame 14 and every 6 frames after it.
    Image m = SyntheticImage();
    m.PutBytes(kSongBase + 0x07, {0xFE});
    m.PutBytes(kSongBase + 0x80, {0x05, 0xF3, 0xAA, 0xBB, 0x03, 0xF3, 0xAA, 0xBB, 0x02, 0xB7, 0x05, 0x0C, 0x03, 0xE0,
                                  0x01, 0xFF, 0x01, 0x00});
    const Rom rom = m.ToRom();
    UltimateMastersSequencer seq(rom, SongsAt(kSongTable), 0);

    std::vector<int> looped_at;
    for (int f = 0; f < 30; f++)
    {
        seq.Step();
        if (seq.LoopedLastFrame())
        {
            looped_at.push_back(f);

            // The loop goes back to the frame where track 5 passed the second loop point, and stays there.
            SEQLIB_CHECK_EQ(seq.LoopStartFrame(), 8);
        }
    }

    SEQLIB_CHECK(looped_at == (std::vector<int>{14, 20, 26}));
}

// Track 5 plays a note of sample F0. Sample numbers above EF select a mode that the model leaves out. The track stops
// with a warning that names the sample.
void TestSampleAboveEf()
{
    Image m = SyntheticImage();
    m.PutBytes(kSongBase + 0x80, {0x00, 0xA0, 0xF0, 0x03, 0xFE});
    const Rom rom = m.ToRom();
    UltimateMastersSequencer seq(rom, SongsAt(kSongTable), 0);

    for (int f = 0; f < 4; f++)
    {
        seq.Step();
    }

    const std::vector<std::string>& warnings = seq.Warnings();
    SEQLIB_CHECK(std::any_of(warnings.begin(), warnings.end(),
                             [](const std::string& w) { return w.find("note of sample F0") != std::string::npos; }));
}

void TestLoopStartFarEarlier()
{
    // As in TestLoopStart, but track 0 passes a loop point at its first command, in frame 0. Such a loop point means
    // the same as none: loop point, duty 25%, PSG note (vol 3, note 24), wait 12, rest, end. It comes more than
    // kLoopPointWindow frames before track 5's loop point, at frame 5. So the loop starts at frame 5.
    Image m = SyntheticImage();
    m.PutBytes(kSongBase + 0x07, {0xFE});
    m.PutBytes(kSongBase + 0x8B, {0xFF, 0x01, 0x00});
    m.PutBytes(kSongBase + 0x40, {0x00, 0xF3, 0x00, 0x81, 0x00, 0xD3, 0x18, 0x0C, 0xE0, 0x00, 0xFE});
    const Rom rom = m.ToRom();
    UltimateMastersSequencer seq(rom, SongsAt(kSongTable), 0);

    const int looped_at = FirstLoop(seq);

    SEQLIB_CHECK_EQ(looped_at, 11);
    SEQLIB_CHECK_EQ(seq.LoopStartFrame(), 5);
}

void TestLoopStartEndedTrack()
{
    // As in TestLoopStartEarliest, but track 0 ends with FD. No loop restarts a track after FD. Track 0's loop point,
    // at frame 4, no longer counts, and the loop starts at track 5's loop point, at frame 5.
    Image m = SyntheticImage();
    m.PutBytes(kSongBase + 0x07, {0xFE});
    m.PutBytes(kSongBase + 0x8B, {0xFF, 0x01, 0x00});
    m.PutBytes(kSongBase + 0x40, {0x00, 0x81, 0x00, 0xD3, 0x18, 0x04, 0xF3, 0x00, 0xE0, 0x00, 0xFD});
    const Rom rom = m.ToRom();
    UltimateMastersSequencer seq(rom, SongsAt(kSongTable), 0);

    const int looped_at = FirstLoop(seq);

    SEQLIB_CHECK_EQ(looped_at, 11);
    SEQLIB_CHECK_EQ(seq.LoopStartFrame(), 5);
}

void TestBendKept()
{
    // Track 4: note, wait 1, bend by +16/32, wait 1, note, wait 1, vibrato of depth 16, wait 2, end. The other tracks
    // end at once.
    Image m(0x200);
    constexpr uint32_t kTable = kRomBase + 0x100;
    constexpr uint32_t kBase = kTable + kSongEntrySize;
    m.Song(kTable, 0, kBase, 0x20, {{4, 0}});
    m.PutBytes(kBase, {0x00, 0xA8, 0x10, 0x05, 0x01, 0xF2, 0x50, 0x01, 0xA8, 0x10, 0x05, 0x01, 0xF1, 0x20, 0x02, 0xFD});
    m.PutBytes(kBase + 0x20, {0x00, 0xFD});
    const Rom rom = m.ToRom();

    for (const bool kept : {true, false})
    {
        UltimateMastersSequencer seq(rom, SongsAt(kTable, kept), 0);

        std::vector<TrackOutput> track4;
        for (int f = 0; f < 4; f++)
        {
            track4.push_back(seq.Step()[4]);
        }

        // The bend raises the note that's playing. A driver that keeps it raises the next note and the vibrato by it
        // too. The vibrato's first step adds (sin[0x18] * 16) >> 12 = 8.
        const int later = kept ? 16 : 0;
        SEQLIB_CHECK_EQ(track4[1].pitch, 16);
        SEQLIB_CHECK(track4[2].flags & kOutNoteOn);
        SEQLIB_CHECK_EQ(track4[2].pitch, later);
        SEQLIB_CHECK_EQ(track4[3].pitch, later + 8);
    }
}

void TestLaterBuildDetection()
{
    // A song that rests, in a cartridge of the Ultimate Masters revision without the later builds' duty command.
    Image m(0x1000);
    constexpr uint32_t kTable = kRomBase + 0x100;
    m.Song(kTable, 0, kRomBase + 0x200, 0, {});
    m.PutBytes(kRomBase + 0x200, {0x00, 0xE0, 0x00, 0xFD});

    DriverOverrides ov;
    ov.song_table = kTable;
    ov.song_count = 1;
    ov.mix_rate = 16000;
    DriverInfo info;
    std::string err;

    const bool found = DetectDriver(m.ToRom(), ov, info, err);

    SEQLIB_CHECK(found);
    SEQLIB_CHECK(info.revision == Revision::kUltimateMasters);
    SEQLIB_CHECK(info.warnings.empty());

    // The duty command that keeps the opcode's low nibble gives a later build, Prince of Tennis 2004's.
    // movs r0,#0xf / ands r5,r0 / cmp r5,#3 / ble / subs r5,#4 / b / lsls r5,r5,#6.
    m.PutHalfwords(kRomBase + 0x800, {0x200F, 0x4005, 0x2D03, 0xDD01, 0x3D04, 0xE000, 0x01AD});
    DriverInfo tennis;

    const bool found_tennis = DetectDriver(m.ToRom(), ov, tennis, err);

    SEQLIB_CHECK(found_tennis);
    SEQLIB_CHECK(tennis.revision == Revision::kPrinceOfTennis);
    SEQLIB_CHECK(!tennis.bend_kept);
    SEQLIB_CHECK(std::find(tennis.log.begin(), tennis.log.end(),
                           "command reader not recognised, Prince of Tennis 2004 revision of the driver (from the duty "
                           "command at 0x08000800)") != tennis.log.end());
    SEQLIB_CHECK(tennis.warnings.empty());

    // Code that reads a sample byte above EF with the next one gives Live Start's build.
    // cmp r2,#0xef / ble / adds r7,#1 / adds r3,#1 / movs r0,#0xf / ands r0,r2 / lsls r2,r0,#8.
    m.PutHalfwords(kRomBase + 0x900, {0x2AEF, 0xDD06, 0x3701, 0x3301, 0x200F, 0x4010, 0x0202});
    DriverInfo live;

    const bool found_live = DetectDriver(m.ToRom(), ov, live, err);

    SEQLIB_CHECK(found_live);
    SEQLIB_CHECK(live.revision == Revision::kLiveStart);
    SEQLIB_CHECK(!live.bend_kept);
    SEQLIB_CHECK(live.warnings.empty());
}

void TestLaterBuildDecoding()
{
    // Delays read as in WCT 2004: a byte up to DF, or two bytes that give 13 bits from E0 up.
    Rom delays;
    delays.Assign({0xDF, 0xE5, 0x10, 0xFF, 0x34});
    uint32_t delay = 0;

    SEQLIB_CHECK_EQ(ReadDelay(delays, kRomBase + 0, Revision::kPrinceOfTennis, delay), 1);
    SEQLIB_CHECK_EQ(delay, 0xDF);
    SEQLIB_CHECK_EQ(ReadDelay(delays, kRomBase + 1, Revision::kPrinceOfTennis, delay), 2);
    SEQLIB_CHECK_EQ(delay, 0x510);
    SEQLIB_CHECK_EQ(ReadDelay(delays, kRomBase + 3, Revision::kPrinceOfTennis, delay), 2);
    SEQLIB_CHECK_EQ(delay, 0x1F34);

    // 13 / 86 / 80 / 95 0A 00 03 / A3 F5 / F4 20 / F5 10 / F6 01 / FA 01
    Rom rom;
    rom.Assign({0x13, 0x86, 0x80, 0x95, 0x0A, 0x00, 0x03, 0xA3, 0xF5, 0xF4, 0x20, 0xF5, 0x10, 0xF6, 0x01, 0xFA, 0x01});
    auto at = [&](uint32_t offset, int track)
    {
        return DecodeCommand(rom, kRomBase + offset, track, Revision::kPrinceOfTennis);
    };

    // A duty command keeps the opcode's low nibble: 0-3 give a duty in bits 6-7, and 4-F the nibble minus 4.
    SEQLIB_CHECK(at(0, 0).op == Op::kDuty && at(0, 0).value == 0xC0);
    SEQLIB_CHECK(at(1, 2).op == Op::kDuty && at(1, 2).value == 2);
    SEQLIB_CHECK(at(2, 0).op == Op::kDuty && at(2, 0).value == 0);

    // 9x lo hi nn calls nn commands at hilo.
    const Command call = at(3, 4);
    SEQLIB_CHECK(call.op == Op::kCall && call.value == 0x0A && call.count == 3 && call.length == 4);

    // In Prince of Tennis 2004, a sample byte above EF is a sample. In Live Start, the next byte gives the sample's low
    // byte, and the first's low nibble its high bits.
    const Command note = at(7, 4);
    SEQLIB_CHECK(note.op == Op::kNote && note.vol == 3 && note.sample == 0xF5 && note.length == 2);
    const Command wide = DecodeCommand(rom, kRomBase + 7, 4, Revision::kLiveStart);
    SEQLIB_CHECK(wide.op == Op::kNote && wide.sample == 0x5F4 && wide.length == 3);
    SEQLIB_CHECK(DecodeCommand(rom, kRomBase + 7, 4, Revision::kUltimateMasters).op == Op::kUnknown);

    // F4 and F5 set the attack and decay rates of PSG notes, F6 an instrument, and FA holds the track's output back.
    SEQLIB_CHECK(at(9, 0).op == Op::kAttack && at(9, 0).value == 0x20 && at(9, 0).length == 2);
    SEQLIB_CHECK(at(11, 0).op == Op::kDecay && at(11, 0).value == 0x10 && at(11, 0).length == 2);
    SEQLIB_CHECK(at(13, 4).op == Op::kInstrument && at(13, 4).value == 1 && at(13, 4).length == 2);
    SEQLIB_CHECK(at(15, 4).op == Op::kHoldOutput && at(15, 4).value == 1 && at(15, 4).length == 2);
    SEQLIB_CHECK(DecodeCommand(rom, kRomBase + 15, 4, Revision::kUltimateMasters).op == Op::kUnknown);
}

void TestLaterBuildSequencer()
{
    // Track 0: duty 25%, PSG note (vol 3, note 24), wait 1, vibrato of depth 16, wait 2, end. Track 1: attack rate
    // 0x40, PSG note (vol byte 8, note 0x30), wait 4, end. Track 4: note (sample 5), wait 1, bend by +16/32, wait 1,
    // note (sample F5), wait 1, end. Track 5: a call of 1 command at +0x0B, whose opcode also sets 75% duty, wait 5,
    // note (vol byte 9, sample 7), wait 3, end, and then the fragment the call plays: wait 2, note (vol 5, sample 11),
    // wait 6. Track 6: hold the output, note (vol 1, sample 7), wait 2, let the output go, the note again, wait 2, end.
    // The other tracks end at once.
    Image m(0x200);
    constexpr uint32_t kTable = kRomBase + 0x100;
    constexpr uint32_t kBase = kTable + kSongEntrySize;
    m.Song(kTable, 0, kBase, 0x60, {{0, 0x00}, {1, 0x10}, {4, 0x20}, {5, 0x40}, {6, 0x50}});
    m.PutBytes(kBase + 0x00, {0x00, 0x81, 0x00, 0xD3, 0x18, 0x01, 0xF1, 0x20, 0x02, 0xFD});
    m.PutBytes(kBase + 0x10, {0x00, 0xF4, 0x40, 0x00, 0xD8, 0x08, 0x30, 0x04, 0xFD});
    m.PutBytes(kBase + 0x20, {0x00, 0xA8, 0x10, 0x05, 0x01, 0xF2, 0x50, 0x01, 0xA8, 0x10, 0xF5, 0x01, 0xFD});
    m.PutBytes(kBase + 0x40,
               {0x00, 0x93, 0x0B, 0x00, 0x01, 0x05, 0xA8, 0x09, 0x07, 0x03, 0xFD, 0x02, 0xA5, 0x0B, 0x06});
    m.PutBytes(kBase + 0x50, {0x00, 0xFA, 0x01, 0x00, 0xA1, 0x07, 0x02, 0xFA, 0x00, 0x00, 0xA1, 0x07, 0x02, 0xFD});
    m.PutBytes(kBase + 0x60, {0x00, 0xFD});
    const Rom rom = m.ToRom();
    DriverInfo info = SongsAt(kTable);
    info.revision = Revision::kPrinceOfTennis;
    UltimateMastersSequencer seq(rom, info, 0);

    std::vector<std::array<TrackOutput, kTracks>> frames;
    for (int f = 0; f < 12; f++)
    {
        frames.push_back(seq.Step());
    }

    // Vibrato hands on the duty byte as the key, where the Ultimate Masters revision and Live Start hand on the note's
    // sample.
    SEQLIB_CHECK(frames[1][0].flags & kOutPitch);
    SEQLIB_CHECK(frames[1][0].key == 0x40 && frames[1][0].b2 == 0x40);

    // The attack takes the note's volume up from 2 to 8 over four frames, restarting the channel at each level.
    for (int f = 0; f < 4; f++)
    {
        SEQLIB_CHECK_EQ(frames[f][1].vol, 2 * (f + 1));
        SEQLIB_CHECK_EQ(frames[f][1].trig, 1);
    }

    // The bend raises the note that's playing, and hands on its duty byte, the sample's low byte. The build never
    // keeps a bend.
    SEQLIB_CHECK(frames[1][4].pitch == 16 && frames[1][4].key == 5);
    SEQLIB_CHECK(frames[2][4].flags == kOutNoteOn && frames[2][4].key == 0xF5 && frames[2][4].pitch == 0);

    // The call sets the duty byte, waits 2, plays the fragment's note, and then waits the 5 after the call instead of
    // the 6 in the fragment.
    SEQLIB_CHECK_EQ(frames[0][5].b2, 0xC0);
    SEQLIB_CHECK(frames[2][5].flags == kOutNoteOn && frames[2][5].key == 11 && frames[2][5].vol == 5);
    SEQLIB_CHECK(frames[7][5].flags == kOutNoteOn && frames[7][5].key == 7 && frames[7][5].vol == 9);
    SEQLIB_CHECK_EQ(frames[10][5].flags, kOutStop);

    // FA holds nothing back after a song request, which sets the game's setting to 0xFF.
    SEQLIB_CHECK(frames[0][6].flags == kOutNoteOn && frames[0][6].key == 7);
    SEQLIB_CHECK(frames[2][6].flags == kOutNoteOn && frames[2][6].key == 7);
}

// In the later builds, FA 01 holds a track back if the game's setting is 0. A held track's output record has no flags
// and no retrigger. Its notes therefore don't start. FA 00 lets the track go.
void TestLaterBuildHold()
{
    // Track 6: hold the output, note (vol 1, sample 7), wait 2, let the output go, the note again, wait 2, end. The
    // other tracks end at once.
    Image m(0x200);
    constexpr uint32_t kTable = kRomBase + 0x100;
    constexpr uint32_t kBase = kTable + kSongEntrySize;
    m.Song(kTable, 0, kBase, 0x20, {{6, 0x00}});
    m.PutBytes(kBase + 0x00, {0x00, 0xFA, 0x01, 0x00, 0xA1, 0x07, 0x02, 0xFA, 0x00, 0x00, 0xA1, 0x07, 0x02, 0xFD});
    m.PutBytes(kBase + 0x20, {0x00, 0xFD});
    const Rom rom = m.ToRom();
    DriverInfo info = SongsAt(kTable);
    info.revision = Revision::kLiveStart;
    UltimateMastersSequencer seq(rom, info, 0, Request{kLiveStageSetting, {}});

    std::vector<std::array<TrackOutput, kTracks>> frames;
    for (int f = 0; f < 4; f++)
    {
        frames.push_back(seq.Step());
    }

    SEQLIB_CHECK(frames[0][6].flags == 0 && frames[0][6].trig == 0);
    SEQLIB_CHECK(frames[1][6].flags == 0 && frames[1][6].trig == 0);
    SEQLIB_CHECK(frames[2][6].flags == kOutNoteOn && frames[2][6].key == 7);
}

// In WCT 2004, FA 01 scales a track's later volumes by the game's setting: volume × setting / 16. A note or a volume
// command that the scale takes to 0 does nothing. A PSG note plays at the scaled volume. FA 00 stops the scale. A song
// request sets the setting to 0xFF. That scales nothing.
void TestWct2004VolumeScale()
{
    // Track 0: scale on, PSG note (vol 8, note 0x30), wait 2, end. Track 4: note (vol 9, sample 7), wait 2, scale on,
    // note (vol 9, sample 8), wait 2, volume 3, wait 2, scale off, note (vol 5, sample 8), wait 2, end. The other
    // tracks end at once.
    Image m(0x200);
    constexpr uint32_t kTable = kRomBase + 0x100;
    constexpr uint32_t kBase = kTable + kSongEntrySize;
    m.Song(kTable, 0, kBase, 0x40, {{0, 0x00}, {4, 0x10}});
    m.PutBytes(kBase + 0x00, {0x00, 0xFA, 0x01, 0x00, 0xD8, 0x30, 0x02, 0xFD});
    m.PutBytes(kBase + 0x10, {0x00, 0xA9, 0x07, 0x02, 0xFA, 0x01, 0x00, 0xA9, 0x08, 0x02, 0xC3, 0x02, 0xFA, 0x00, 0x00,
                              0xA5, 0x08, 0x02, 0xFD});
    m.PutBytes(kBase + 0x40, {0x00, 0xFD});
    const Rom rom = m.ToRom();
    DriverInfo info = SongsAt(kTable);
    info.revision = Revision::kWct2004;

    // Returns the first 8 frames of the song with the game's setting at `setting`.
    const auto play = [&](uint8_t setting)
    {
        Wct2004Sequencer seq(rom, info, 0, Request{setting, {}});
        std::vector<std::array<TrackOutput, kTracks>> frames;
        for (int f = 0; f < 8; f++)
        {
            frames.push_back(seq.Step());
        }

        return frames;
    };
    const auto request = play(kSettingAfterRequest);
    const auto silenced = play(0);
    const auto halved = play(8);

    // After a request, every volume plays as it is.
    SEQLIB_CHECK_EQ(request[0][0].vol, 8);
    SEQLIB_CHECK(request[2][4].flags == kOutNoteOn && request[2][4].key == 8 && request[2][4].vol == 9);
    SEQLIB_CHECK(request[4][4].vol == 3 && request[4][4].trig == 1);

    // At a setting of 0, the PSG note plays at volume 0, and the note and the volume command after FA 01 do nothing.
    SEQLIB_CHECK(silenced[0][0].flags == kOutPsgNote && silenced[0][0].vol == 0);
    SEQLIB_CHECK(silenced[0][4].flags == kOutNoteOn && silenced[0][4].key == 7);
    SEQLIB_CHECK(silenced[2][4].flags == 0 && silenced[2][4].trig == 0);
    SEQLIB_CHECK(silenced[4][4].flags == 0 && silenced[4][4].trig == 0);
    SEQLIB_CHECK(silenced[6][4].flags == kOutNoteOn && silenced[6][4].key == 8 && silenced[6][4].vol == 5);

    // At a setting of 8, the volumes after FA 01 are halved and rounded down.
    SEQLIB_CHECK_EQ(halved[0][0].vol, 4);
    SEQLIB_CHECK(halved[2][4].flags == kOutNoteOn && halved[2][4].vol == 4);
    SEQLIB_CHECK(halved[4][4].vol == 1 && halved[4][4].trig == 1);
    SEQLIB_CHECK_EQ(halved[6][4].vol, 5);
}

// F6 xx up to 63 picks entry xx of the table of samples that the game's request gives the driver. Each later note of
// the track plays the entry's sample. Bit 14 of the entry plays it at pitch 0, and in Live Start bit 12 plays it at
// volume 7F. F6 00, or an entry past the table's end, plays the note's sample. Without a table, F6 does nothing.
void TestLiveStageSamples()
{
    // WCT 2004, track 4: F6 01, note (vol 9, sample 5, semitone +2), wait 2, F6 02, the note again, wait 2, F6 00, the
    // note again, wait 2, end.
    Image m(0x200);
    constexpr uint32_t kTable = kRomBase + 0x100;
    constexpr uint32_t kBase = kTable + kSongEntrySize;
    m.Song(kTable, 0, kBase, 0x40, {{4, 0x00}});
    m.PutBytes(kBase + 0x00, {0x00, 0xF6, 0x01, 0x00, 0xB9, 0x05, 0x02, 0x02, 0xF6, 0x02, 0x00, 0xB9,
                              0x05, 0x02, 0x02, 0xF6, 0x00, 0x00, 0xB9, 0x05, 0x02, 0x02, 0xFD});
    m.PutBytes(kBase + 0x40, {0x00, 0xFD});
    const Rom rom = m.ToRom();
    DriverInfo info = SongsAt(kTable);
    info.revision = Revision::kWct2004;

    // Returns the first 6 frames of track 4 after a request that gives `samples`.
    const auto play = [&](const std::vector<uint16_t>& samples)
    {
        Wct2004Sequencer seq(rom, info, 0, Request{kLiveStageSetting, samples});
        std::vector<TrackOutput> frames;
        for (int f = 0; f < 6; f++)
        {
            frames.push_back(seq.Step()[4]);
        }

        return frames;
    };
    const auto wct = play({0, 0xC503});
    SEQLIB_CHECK(wct[0].flags == kOutNoteOn && wct[0].key == 0x8503 && wct[0].b2 == 0x03);
    SEQLIB_CHECK(wct[0].pitch == 0 && wct[0].vol == 9);
    SEQLIB_CHECK(wct[2].flags == kOutNoteOn && wct[2].key == 5 && wct[2].pitch == 64);
    SEQLIB_CHECK(wct[4].flags == kOutNoteOn && wct[4].key == 5 && wct[4].pitch == 64);
    const auto plain = play({});
    SEQLIB_CHECK(plain[0].flags == kOutNoteOn && plain[0].key == 5 && plain[0].pitch == 64);

    // Live Start, track 4: F6 01, note (vol 3, sample 7), wait 2, end.
    Image later(0x200);
    later.Song(kTable, 0, kBase, 0x40, {{4, 0x00}});
    later.PutBytes(kBase + 0x00, {0x00, 0xF6, 0x01, 0x00, 0xA3, 0x07, 0x02, 0xFD});
    later.PutBytes(kBase + 0x40, {0x00, 0xFD});
    const Rom later_rom = later.ToRom();
    info.revision = Revision::kLiveStart;
    UltimateMastersSequencer seq(later_rom, info, 0, Request{kLiveStageSetting, {0, 0xD4A7}});
    const TrackOutput o = seq.Step()[4];
    SEQLIB_CHECK(o.flags == kOutNoteOn && o.key == 0x84A7 && o.b2 == 0xA7 && o.pitch == 0 && o.vol == 0x7F);
}

// A sample number with bit 15 set is a sample of the second sample table. The live stages' tables of samples reach it.
// Its samples can be packed.
void TestLiveSampleTable()
{
    // The main table's sample 1 and the second table's sample 1, both one-shot PCM of 4 samples, the second one at
    // twice the step.
    Image m(0x200);
    constexpr uint32_t kMain = kRomBase + 0x100;
    constexpr uint32_t kSecond = kRomBase + 0x120;
    constexpr uint32_t kHeaders = kRomBase + 0x140;
    m.Put32(kMain + 4, kHeaders);
    m.Put32(kSecond + 4, kHeaders + 0x20);
    for (uint32_t h : {kHeaders, kHeaders + 0x20})
    {
        m.Put32(h, h == kHeaders ? 0x1000 : 0x2000);
        m.Put32(h + 4, 4);
        m.Put32(h + 8, 0xFFFFFFFF);
        m.PutBytes(h + 12, {1, 2, 3, 4});
    }
    const Rom rom = m.ToRom();
    DriverInfo info;
    info.revision = Revision::kWct2004;
    info.sample_table = kMain;
    SEQLIB_CHECK(info.Sample(rom, 1).valid && info.Sample(rom, 1).step == 0x1000);
    SEQLIB_CHECK(!info.Sample(rom, 0x8001).valid);
    info.live_sample_table = kSecond;
    const SampleInfo s = info.Sample(rom, 0x8001);
    SEQLIB_CHECK(s.valid && s.step == 0x2000 && s.data == kHeaders + 0x2C && s.length == 4);
}

// A song that the game's live stage plays with FA's setting at 0 has a second version in that state, in files named
// with "_live". The first version plays the song as a request leaves it.
void TestLiveStageVersion()
{
    // Song 0 of the WCT 2004 revision. Voice 0: note (vol 9, sample 5), wait 4, scale on, the note again, wait 4, rest,
    // wait 4, end. A WCT 2004 command reader is at kCode.
    Image m = SyntheticImage();
    constexpr uint32_t kCode = kRomBase + 0x800;
    m.PutHalfwords(kCode, CommandReader(0x2DFE, 0x2DFA));
    m.Song(kSongTable, 0, kSongBase, 0x20, {{4, 0x00}});
    m.PutBytes(kSongBase + 0x00, {0x00, 0xA9, 0x05, 0x04, 0xFA, 0x01, 0x00, 0xA9, 0x05, 0x04, 0xE0, 0x04, 0xFD});
    m.PutBytes(kSongBase + 0x20, {0x00, 0xFD});
    const Rom rom = m.ToRom();
    DriverInfo info;
    SEQLIB_CHECK(DetectGiven(rom, kSongTable, 1, info) && info.revision == Revision::kWct2004);
    info.live_songs = {0};

    const fs::path dir = TempPath("live_stage");
    std::error_code ec;
    fs::create_directories(dir, ec);
    ConvertSettings opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);

    // Returns the notes that the song's version `version` starts on voice 0, and the path of its MIDI file.
    const auto convert = [&](int version)
    {
        opt.version = version;
        const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);
        int notes = 0;
        for (const MidiEvent& e : ReadEvents(sum.midi_path))
        {
            notes += e.track == "Voice 0" && (e.status & 0xF0) == 0x90 && e.data2 > 0;
        }

        return std::make_pair(notes, PathFromUtf8(sum.midi_path).filename());
    };
    const auto [request_notes, request_file] = convert(0);
    const auto [live_notes, live_file] = convert(1);

    const std::vector<SongVersion> versions = SongVersions(info, 0);
    SEQLIB_REQUIRE_EQ(versions.size(), 2);
    SEQLIB_CHECK(versions[0].suffix.empty() && versions[1].suffix == "_live");
    SEQLIB_CHECK_EQ(SongVersions(info, 1).size(), 1);
    SEQLIB_CHECK_EQ(request_notes, 2);
    SEQLIB_CHECK_EQ(live_notes, 1);
    SEQLIB_CHECK(request_file == "song_00.mid");
    SEQLIB_CHECK(live_file == "song_00_live.mid");

    fs::remove_all(dir, ec);
}

void TestPackedSamples()
{
    // Sample 7 is packed: bit 31 of its length is set, and a table of 32 pairs of samples comes between its header and
    // its data. Pair 0 plays 0x10 then 0x20, and pair 1 plays 0xF0 then 0xE0.
    Image m(0x1000);
    constexpr uint32_t kHeader = kSampleHeader + 0x100;
    m.Put32(kSampleTable + 4 * 7, kHeader);
    m.Put32(kHeader, 0x0C00);
    m.Put32(kHeader + 4, 0x80000000 | 25);
    m.Put32(kHeader + 8, 0xFFFFFFFF);
    m.PutHalfwords(kHeader + 12, {0x1020, 0xF0E0});

    // Two samples, pair 0, three zeros, five samples of 0x7F, two zeros and three samples of 0xC0 (with their counts
    // in the next byte), pair 1, two samples, one sample of 0x11, and a sample. A zero run whose count is 0 plays one
    // zero and then skips a sample.
    m.PutBytes(kHeader + 0x4C, {0x05, 0xFB, 0xA0, 0x93, 0x85, 0x7F, 0x90, 0x02, 0x80, 0x03, 0xC0,
                                0xA1, 0x7F, 0xC0, 0x81, 0x11, 0x01, 0x90, 0x00, 0x22, 0x33});
    const Rom rom = m.ToRom();
    DriverInfo info;
    info.sample_table = kSampleTable;
    info.revision = Revision::kLiveStart;

    const SampleInfo s = info.Sample(rom, 7);

    SEQLIB_CHECK(s.valid && s.packed);
    SEQLIB_CHECK_EQ(s.step, 0x0C00u);
    SEQLIB_CHECK_EQ(s.length, 25);
    SEQLIB_CHECK_EQ(s.data, kHeader + 0x4C);
    SEQLIB_CHECK(!s.Looped());
    const std::vector<int8_t> pcm = {5, -5,  16,  32,  0,   0,   0,   127, 127, 127, 127, 127, 0,
                                     0, -64, -64, -64, -16, -32, 127, -64, 17,  1,   0,   51};
    SEQLIB_CHECK(SamplePcm(rom, s) == pcm);

    // Prince of Tennis 2004 reads packed samples too, and the older build doesn't. Neither later build reads a packed
    // sample with a loop.
    info.revision = Revision::kPrinceOfTennis;
    SEQLIB_CHECK(info.Sample(rom, 7).packed);
    info.revision = Revision::kUltimateMasters;
    SEQLIB_CHECK(!info.Sample(rom, 7).valid);
    m.Put32(kHeader + 8, 0);
    info.revision = Revision::kLiveStart;
    SEQLIB_CHECK(!info.Sample(m.ToRom(), 7).valid);
}

void TestOverrides()
{
    // A table address given by hand that isn't in the ROM, above it or below it, is reported as such.
    const Rom rom = SyntheticRom();
    DriverInfo info;
    std::string err;
    DriverOverrides ov;
    ov.song_table = kRomBase + 0x100000;

    SEQLIB_CHECK(!DetectDriver(rom, ov, info, err));
    SEQLIB_CHECK(err == "the song table address 0x08100000 is outside the ROM (0x08000000-0x08000FFF)");

    ov.song_table = kSongTable;
    ov.sample_table = 0x02000000;

    SEQLIB_CHECK(!DetectDriver(rom, ov, info, err));
    SEQLIB_CHECK(err == "the sample table address 0x02000000 is outside the ROM (0x08000000-0x08000FFF)");

    ov.sample_table = kSampleTable;

    SEQLIB_CHECK(DetectDriver(rom, ov, info, err));

    // The song table ends quietly at an entry that doesn't point into the ROM, which is no song at all.
    Image m = SyntheticImage();
    m.Put32(kSongTable + 2 * kSongEntrySize, 0x12345678);
    ov.mix_rate = 16000;

    SEQLIB_CHECK(DetectDriver(m.ToRom(), ov, info, err));
    SEQLIB_CHECK_EQ(info.song_count, 2);
    SEQLIB_CHECK(info.warnings.empty());
}

// Sample 6 has 20 points and loops from point 14. Its loop of 6 points is repeated 6 times: 36 points, the first length
// of kMinLoop points or more.
void TestShortLoop()
{
    Image m = SyntheticImage();
    constexpr uint32_t kShortLoop = kRomBase + 0x600;
    m.Put32(kSampleTable + 4 * 6, kShortLoop);
    m.Put32(kShortLoop, 0x1000);
    m.Put32(kShortLoop + 4, 20);
    m.Put32(kShortLoop + 8, 14);
    for (uint32_t i = 0; i < 20; i++)
    {
        m.Put8(kShortLoop + 12 + i, uint8_t(i * 9));
    }
    const Rom rom = m.ToRom();
    DriverInfo info;
    SEQLIB_REQUIRE(DetectGiven(rom, kSongTable, 2, info));
    SoundfontBuilder sf(rom, info);
    const int index = sf.DsSample(6);
    SEQLIB_REQUIRE(index >= 0);
    SEQLIB_CHECK(ntdlib::testing::agb_files::LengthenedLoop(sf.File().samples[size_t(index)], 14, 6));
}

int RunTests()
{
    TestDecoding();
    TestSequencer();
    TestLoopStart();
    TestLoopStartEarliest();
    TestLoopStartLast();
    TestLoopStartFarEarlier();
    TestSampleAboveEf();
    TestLoopStartEndedTrack();
    TestBendKept();
    TestLaterBuildDetection();
    TestLaterBuildDecoding();
    TestLaterBuildSequencer();
    TestLaterBuildHold();
    TestWct2004VolumeScale();
    TestLiveStageVersion();
    TestLiveStageSamples();
    TestLiveSampleTable();
    TestPackedSamples();
    TestOverrides();
    TestShortLoop();

    return seqlib::testing::Summary("konami");
}

} // namespace
} // namespace ntdlib::sequence_export::konami

int main()
{
    return ntdlib::sequence_export::konami::RunTests();
}
