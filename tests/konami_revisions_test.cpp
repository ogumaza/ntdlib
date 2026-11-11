// SPDX-License-Identifier: MIT

// Unit tests for the revisions of Konami's driver after Ultimate Masters: WCT 2004, Rave Master, Eternal Duelist,
// Captain Tsubasa, Hunter X Hunter, Kitaro and Minna no Ouji-sama. konami_dungeon_dice_test.cpp tests Dungeon Dice
// Monsters. They build small synthetic cartridges in memory. No game ROM is needed.

#include "konami_test_util.h"

namespace ntdlib::sequence_export::konami
{
namespace
{

void TestWct2004Decoding()
{
    // Delays: a byte up to DF, or two bytes that give 13 bits from E0 up.
    Rom delays;
    delays.Assign({0xDF, 0xE1, 0x23, 0xFF, 0xFF});
    uint32_t delay = 0;

    SEQLIB_CHECK_EQ(ReadDelay(delays, kRomBase + 0, Revision::kWct2004, delay), 1);
    SEQLIB_CHECK_EQ(delay, 0xDF);
    SEQLIB_CHECK_EQ(ReadDelay(delays, kRomBase + 1, Revision::kWct2004, delay), 2);
    SEQLIB_CHECK_EQ(delay, 0x123);
    SEQLIB_CHECK_EQ(ReadDelay(delays, kRomBase + 3, Revision::kWct2004, delay), 2);
    SEQLIB_CHECK_EQ(delay, 0x1FFF);

    // FD, FE, FF / F0 8F / F2 90 / F3 / F4 20 / FB / D5 30 / C9 / A7 09 / B3 0A FE / 93 34 12 02 / 02 / 1E
    Rom rom;
    rom.Assign({0xFD, 0xFE, 0xFF, 0xF0, 0x8F, 0xF2, 0x90, 0xF3, 0xF4, 0x20, 0xFB, 0xD5, 0x30,
                0xC9, 0xA7, 0x09, 0xB3, 0x0A, 0xFE, 0x93, 0x34, 0x12, 0x02, 0x02, 0x1E});
    auto at = [&](uint32_t offset, int track)
    {
        return DecodeCommand(rom, kRomBase + offset, track, Revision::kWct2004);
    };

    // The end commands take no argument: FD ends the track, FE loops the song and FF stops it.
    SEQLIB_CHECK(at(0, 4).op == Op::kEndTrack && at(0, 4).length == 1);
    SEQLIB_CHECK(at(1, 4).op == Op::kJump && at(1, 4).value == 1 && at(1, 4).length == 1);
    SEQLIB_CHECK(at(2, 4).op == Op::kJump && at(2, 4).value == 0 && at(2, 4).length == 1);

    // F0 holds two pan levels, a bend takes one byte whatever it is, F3 carries two more bytes on sample tracks, F4
    // takes one, and FB none.
    SEQLIB_CHECK(at(3, 4).op == Op::kPanLevels && at(3, 4).value == 0x8F && at(3, 4).length == 2);
    SEQLIB_CHECK(at(5, 4).op == Op::kPitchBend && at(5, 4).value == 0x50 && at(5, 4).length == 2);
    SEQLIB_CHECK(at(7, 0).length == 1 && at(7, 4).op == Op::kLoopPoint && at(7, 4).length == 3);
    SEQLIB_CHECK(at(8, 0).op == Op::kAttack && at(8, 0).value == 0x20 && at(8, 0).length == 2);
    SEQLIB_CHECK(at(10, 0).op == Op::kNop && at(10, 0).length == 1);

    // Notes and volumes take their volume from the opcode's low nibble.
    Command c = at(11, 0);
    SEQLIB_CHECK(c.op == Op::kPsgNote && c.vol == 5 && c.note == 0x30 && c.length == 2);
    c = at(13, 0);
    SEQLIB_CHECK(c.op == Op::kVolume && c.vol == 9 && c.length == 1);
    c = at(14, 4);
    SEQLIB_CHECK(c.op == Op::kNote && c.vol == 7 && c.sample == 9 && c.semitone == 0 && c.length == 2);
    c = at(16, 4);
    SEQLIB_CHECK(c.op == Op::kNote && c.vol == 3 && c.sample == 10 && c.semitone == -2 && c.length == 3);

    // 9x lo hi nn calls nn commands at hilo, and 00-8F set the duty from a low nibble up to 3, or the wave above it.
    c = at(19, 4);
    SEQLIB_CHECK(c.op == Op::kCall && c.value == 0x1234 && c.count == 2 && c.length == 4);
    c = at(23, 0);
    SEQLIB_CHECK(c.op == Op::kDuty && c.value == 0x80 && c.length == 1);
    c = at(24, 2);
    SEQLIB_CHECK(c.op == Op::kWave && c.value == 10 && c.length == 1);

    // The Rave Master revision's F3 takes no extra bytes, F6-FA and 00-7F do nothing, and 80-8F store the duty as it
    // is.
    Rom rave;
    rave.Assign({0xF3, 0xF7, 0x07, 0x82, 0x85, 0xF4, 0x20});
    auto rave_at = [&](uint32_t offset, int track)
    {
        return DecodeCommand(rave, kRomBase + offset, track, Revision::kRaveMaster);
    };

    SEQLIB_CHECK(rave_at(0, 4).op == Op::kLoopPoint && rave_at(0, 4).length == 1);
    SEQLIB_CHECK(rave_at(1, 4).op == Op::kNop && rave_at(1, 4).length == 1);
    SEQLIB_CHECK(rave_at(2, 0).op == Op::kNop && rave_at(2, 0).length == 1);
    SEQLIB_CHECK(rave_at(3, 0).op == Op::kDuty && rave_at(3, 0).value == 2);
    SEQLIB_CHECK(rave_at(4, 2).op == Op::kWave && rave_at(4, 2).value == 1);
    SEQLIB_CHECK(rave_at(5, 0).op == Op::kAttack && rave_at(5, 0).value == 0x20 && rave_at(5, 0).length == 2);
}

void TestRaveMasterSequencer()
{
    // A song of 12 tracks, with 28-byte song table entries. Track 4: pan left 8 right 15, note (vol 7, sample 7), wait
    // 32, loop the song. The other tracks end at once.
    Image m(0x200);
    constexpr uint32_t kTable = kRomBase + 0x100;
    constexpr uint32_t kBase = kTable + 28;
    m.Song(kTable, 0, kBase, 0x10, {{4, 0}}, Revision::kRaveMaster);
    m.PutBytes(kBase, {0x00, 0xF0, 0x8F, 0x00, 0xA7, 0x07, 0xE0, 0x20, 0xFE});
    m.PutBytes(kBase + 0x10, {0x00, 0xFD});
    const Rom rom = m.ToRom();
    DriverInfo info = SongsAt(kTable);
    info.revision = Revision::kRaveMaster;
    Wct2004Sequencer seq(rom, info, 0);

    std::vector<std::array<TrackOutput, kTracks>> frames;
    for (int f = 0; f < 34; f++)
    {
        frames.push_back(seq.Step());
    }

    // Only the song's 12 tracks run. A loop centres the pan again, where WCT 2004 keeps it.
    SEQLIB_CHECK(seq.Valid());
    SEQLIB_CHECK(frames[0][11].active && !frames[0][12].active);
    SEQLIB_CHECK(frames[1][4].pan == 0x8F);
    SEQLIB_CHECK(seq.LoopsDone() == 1);
    SEQLIB_CHECK(frames[32][4].pan_start == 0xFF && frames[32][4].pan == 0x8F);
}

void TestWct2004Sequencer()
{
    // Song 0. Track 0: attack rate 0x40, PSG note (vol 8, note 0x30), wait 4, end. Track 4: a call of 1 command at
    // +0x0A, whose opcode also sets 75% duty, wait 5, note (vol 9, sample 7), wait 3, end, and then the fragment the
    // call plays: wait 2, note (vol 5, sample 11), wait 6. Track 5: pan left 8 right 15, note (vol 7, sample 7), wait
    // 32, loop the song. The other tracks end at once. Song 1: track 4 plays a note, waits 3 and stops the song.
    Image m(0x200);
    constexpr uint32_t kTable = kRomBase + 0x100;
    constexpr uint32_t kBase = kTable + 2 * kSongEntrySize;
    m.Song(kTable, 0, kBase, 0x40, {{0, 0x00}, {4, 0x10}, {5, 0x30}});
    m.PutBytes(kBase + 0x00, {0x00, 0xF4, 0x40, 0x00, 0xD8, 0x30, 0x04, 0xFD});
    m.PutBytes(kBase + 0x10, {0x00, 0x93, 0x0A, 0x00, 0x01, 0x05, 0xA9, 0x07, 0x03, 0xFD, 0x02, 0xA5, 0x0B, 0x06});
    m.PutBytes(kBase + 0x30, {0x00, 0xF0, 0x8F, 0x00, 0xA7, 0x07, 0xE0, 0x20, 0xFE});
    m.PutBytes(kBase + 0x40, {0x00, 0xFD});
    m.Song(kTable, 1, kBase + 0x50, 0x10, {{4, 0x00}});
    m.PutBytes(kBase + 0x50, {0x00, 0xA7, 0x07, 0x03, 0xFF});
    m.PutBytes(kBase + 0x60, {0x00, 0xFD});
    const Rom rom = m.ToRom();
    DriverInfo info = SongsAt(kTable);
    info.revision = Revision::kWct2004;
    Wct2004Sequencer seq(rom, info, 0);
    Wct2004Sequencer stopping(rom, info, 1);

    std::vector<std::array<TrackOutput, kTracks>> frames;
    int looped_at = -1;
    for (int f = 0; f < 40; f++)
    {
        frames.push_back(seq.Step());
        looped_at = looped_at < 0 && seq.LoopedLastFrame() ? f : looped_at;
    }

    // The attack takes the note's volume up from 2 to 8 over four frames, restarting the channel at each level.
    SEQLIB_CHECK(frames[0][0].flags == kOutPsgNote && frames[0][0].pitch == 0x600);
    for (int f = 0; f < 4; f++)
    {
        SEQLIB_CHECK_EQ(frames[f][0].vol, 2 * (f + 1));
        SEQLIB_CHECK_EQ(frames[f][0].trig, 1);
    }
    SEQLIB_CHECK_EQ(frames[4][0].flags, kOutStop);

    // The call sets the duty byte, waits 2, plays the fragment's note, and then waits the 5 after the call instead of
    // the 6 in the fragment.
    SEQLIB_CHECK_EQ(frames[0][4].b2, 0xC0);
    SEQLIB_CHECK(frames[2][4].flags == kOutNoteOn && frames[2][4].key == 11 && frames[2][4].vol == 5);
    SEQLIB_CHECK(frames[7][4].flags == kOutNoteOn && frames[7][4].key == 7 && frames[7][4].vol == 9);
    SEQLIB_CHECK_EQ(frames[10][4].flags, kOutStop);

    // The record holds the pan byte, and the byte as the frame started. A loop keeps the pan, and the attack starts
    // again.
    SEQLIB_CHECK(frames[0][5].pan == 0x8F && frames[0][5].pan_start == 0xFF);
    SEQLIB_CHECK(frames[1][5].pan == 0x8F && frames[1][5].pan_start == 0x8F);
    SEQLIB_CHECK_EQ(looped_at, 32);
    SEQLIB_CHECK(frames[32][5].pan == 0x8F && frames[32][5].pan_start == 0x8F);
    SEQLIB_CHECK_EQ(frames[32][0].vol, 2);

    for (int f = 0; f < 4; f++)
    {
        stopping.Step();
    }

    // FF stops the song.
    SEQLIB_CHECK(stopping.Stopped());
}

// --dump lists the commands that a call plays after the call's line, and counts their delays in the frames after the
// call. The call's duty reads like the duty of 80-83. Rave Master stores it as it is. 93 therefore gives 12.5% there.
void TestCallDump()
{
    // Square 1: a call of 1 command at +0x0A that also sets the duty, wait 5, PSG note (vol 9, note 0x30), wait 3, end,
    // and then the fragment: wait 2, PSG note (vol 5, note 0x30), wait 6. The other tracks end at once.
    Image m(0x200);
    constexpr uint32_t kTable = kRomBase + 0x100;
    constexpr uint32_t kBase = kTable + 2 * kSongEntrySize;
    m.Song(kTable, 0, kBase, 0x20, {{0, 0x00}});
    m.PutBytes(kBase, {0x00, 0x93, 0x0A, 0x00, 0x01, 0x05, 0xD9, 0x30, 0x03, 0xFD, 0x02, 0xD5, 0x30, 0x06});
    m.PutBytes(kBase + 0x20, {0x00, 0xFD});
    const Rom rom = m.ToRom();
    DriverInfo info = SongsAt(kTable);
    std::string error;

    std::string listings[2];
    for (const Revision revision : {Revision::kWct2004, Revision::kRaveMaster})
    {
        info.revision = revision;
        const std::string path = Utf8(TempPath("call_dump.txt"));
        SEQLIB_CHECK(DumpSong(rom, info, 0, path, error));
        const std::vector<uint8_t> text = ReadAll(path);
        listings[revision == Revision::kRaveMaster].assign(text.begin(), text.end());
    }

    const auto line_at = [](const std::string& s, uint32_t addr)
    {
        char key[16];
        std::snprintf(key, sizeof key, "0x%08X", addr);
        const size_t at = s.find(key);
        return at == std::string::npos ? std::string() : s.substr(at, s.find('\n', at) - at);
    };
    for (const std::string& s : listings)
    {
        SEQLIB_CHECK(line_at(s, kBase + 0x01).find("  0  93 0A 00 01 ") != std::string::npos);
        SEQLIB_CHECK(line_at(s, kBase + 0x01).ends_with(" wait 2"));
        SEQLIB_CHECK(line_at(s, kBase + 0x0B).find("  2  D5 30 ") != std::string::npos);
        SEQLIB_CHECK(line_at(s, kBase + 0x0B).ends_with(" wait 5"));
        SEQLIB_CHECK(line_at(s, kBase + 0x06).find("  7  D9 30 ") != std::string::npos);
        SEQLIB_CHECK(line_at(s, kBase + 0x09).find(" 10  FD ") != std::string::npos);
    }
    SEQLIB_CHECK(listings[0].find("call       1 commands at +0x000A, duty 75%") != std::string::npos);
    SEQLIB_CHECK(listings[1].find("call       1 commands at +0x000A, duty 12.5%") != std::string::npos);
}

void TestWct2004Panning()
{
    // Song 0: square 1 pans left 8 right 15, plays PSG note 0x30 at volume 12, waits 8 and ends. Square 2 plays note
    // 0x20 at volume 5. A WCT 2004 command reader is at kCode, and the tables are given by hand.
    Image m = SyntheticImage();
    constexpr uint32_t kCode = kRomBase + 0x800;
    m.PutHalfwords(kCode, CommandReader(0x2DFE, 0x2DFA));
    m.Song(kSongTable, 0, kSongBase, 0x60, {{0, 0x00}, {1, 0x20}});
    m.PutBytes(kSongBase + 0x00, {0x00, 0xF0, 0x8F, 0x00, 0xDC, 0x30, 0x08, 0xFD});
    m.PutBytes(kSongBase + 0x20, {0x00, 0xD5, 0x20, 0x10, 0xFD});
    m.PutBytes(kSongBase + 0x60, {0x00, 0xFD});
    const Rom rom = m.ToRom();

    DriverInfo info;
    SEQLIB_CHECK(DetectGiven(rom, kSongTable, 1, info) && info.revision == Revision::kWct2004);

    const fs::path dir = TempPath("wct2004_pan");
    std::error_code ec;
    fs::create_directories(dir, ec);
    ConvertSettings opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    // Each square's note-ons (key), and the pan (CC10) each starts with, the default of 64 if its track has none yet.
    std::map<std::string, std::vector<std::pair<int, int>>> notes;
    std::map<std::string, int> pan;
    for (const MidiEvent& e : ReadEvents(sum.midi_path))
    {
        if ((e.status & 0xF0) == 0xB0 && e.data1 == cc::kPan)
        {
            pan[e.track] = e.data2;
        }
        if ((e.status & 0xF0) == 0x90 && e.data2 > 0)
        {
            notes[e.track].emplace_back(e.data1, pan.count(e.track) ? pan[e.track] : 64);
        }
    }

    // Square 1 plays its right side, and square 2 its left side, a copy of square 1's note in place of its own.
    SEQLIB_CHECK(sum.ok);
    SEQLIB_REQUIRE_EQ(notes["Square 1"].size(), 1);
    SEQLIB_REQUIRE_EQ(notes["Square 2"].size(), 1);
    SEQLIB_CHECK_EQ(notes["Square 1"][0].second, 127);
    SEQLIB_CHECK_EQ(notes["Square 2"][0].second, 0);
    SEQLIB_CHECK_EQ(notes["Square 2"][0].first, notes["Square 1"][0].first);

    fs::remove_all(dir, ec);
}

void TestEternalDuelistDecoding()
{
    // Delays: a byte up to EF, or two bytes that give 12 bits from F0 up.
    Rom delays;
    delays.Assign({0xEF, 0xF1, 0x23, 0xFF, 0xFF});
    uint32_t delay = 0;

    SEQLIB_CHECK_EQ(ReadDelay(delays, kRomBase + 0, Revision::kEternalDuelist, delay), 1);
    SEQLIB_CHECK_EQ(delay, 0xEF);
    SEQLIB_CHECK_EQ(ReadDelay(delays, kRomBase + 1, Revision::kEternalDuelist, delay), 2);
    SEQLIB_CHECK_EQ(delay, 0x123);
    SEQLIB_CHECK_EQ(ReadDelay(delays, kRomBase + 3, Revision::kEternalDuelist, delay), 2);
    SEQLIB_CHECK_EQ(delay, 0xFFF);

    // F0 5A / F4 20 / FC 06 / F3 / 07 / 82
    Rom rom;
    rom.Assign({0xF0, 0x5A, 0xF4, 0x20, 0xFC, 0x06, 0xF3, 0x07, 0x82});
    auto at = [&](uint32_t offset, int track)
    {
        return DecodeCommand(rom, kRomBase + offset, track, Revision::kEternalDuelist);
    };

    // F0 sets a PSG track's NR51 bits, or a sample track's volume and the next one's. F4-FC are vibrato, as F1 is.
    SEQLIB_CHECK(at(0, 3).op == Op::kPsgPan && at(0, 3).value == 0x5A && at(0, 3).length == 2);
    SEQLIB_CHECK(at(0, 4).op == Op::kPairVolumes && at(0, 4).value == 0x5A && at(0, 4).length == 2);
    SEQLIB_CHECK(at(2, 4).op == Op::kVibrato && at(2, 4).value == 0x20 && at(2, 4).length == 2);
    SEQLIB_CHECK(at(4, 0).op == Op::kVibrato && at(4, 0).value == 0x06 && at(4, 0).length == 2);

    // As in the Rave Master revision, F3 takes no extra bytes, 00-7F do nothing, and 80-8F store the duty as it is.
    SEQLIB_CHECK(at(6, 4).op == Op::kLoopPoint && at(6, 4).length == 1);
    SEQLIB_CHECK(at(7, 0).op == Op::kNop && at(7, 0).length == 1);
    SEQLIB_CHECK(at(8, 0).op == Op::kDuty && at(8, 0).value == 2);
}

void TestEternalDuelistSequencer()
{
    // Song 0. Square 1: NR51 bits 10, loop point, PSG note (vol 8, note 0x30), wait 4, note 0x24 at the same volume,
    // wait 4, note 0x24 at volume 10, wait 4, end. Voice 1: note (vol 12, sample 7), wait 4, volume 10 and 5 for the
    // next voice, wait 4, end. Voice 2: note (vol 8, sample 7), wait 4, vibrato, wait 4, note (vol 9, sample 11), wait
    // 4, end. Voice 3: rest, wait 32, loop the song.
    Image m(0x800);
    constexpr uint32_t kTable = kRomBase + 0x100;
    constexpr uint32_t kBase = kRomBase + 0x200;
    m.Song(kTable, 0, kBase, 0x60, {{0, 0x00}, {5, 0x20}, {6, 0x30}, {7, 0x50}}, Revision::kEternalDuelist);
    m.PutBytes(kBase + 0x00,
               {0x00, 0xF0, 0x10, 0x00, 0xF3, 0x00, 0xD8, 0x30, 0x04, 0xD8, 0x24, 0x04, 0xDA, 0x24, 0x04, 0xFD});
    m.PutBytes(kBase + 0x20, {0x00, 0xAC, 0x07, 0x04, 0xF0, 0x5A, 0x04, 0xFD});
    m.PutBytes(kBase + 0x30, {0x00, 0xA8, 0x07, 0x04, 0xF1, 0x20, 0x04, 0xA9, 0x0B, 0x04, 0xFD});
    m.PutBytes(kBase + 0x50, {0x00, 0xE0, 0x20, 0xFE});
    m.PutBytes(kBase + 0x60, {0x00, 0xFD});

    // Song 1. Voice 0: a call of 1 command from the fragment after the end, wait 2, note (vol 9, sample 7), wait 1,
    // then 260 commands that do nothing, each followed by a wait of 1, and the end. The fragment: wait 3, note (vol 5,
    // sample 11), wait 6.
    constexpr uint32_t kBase1 = kRomBase + 0x280;
    std::vector<uint8_t> calls = {0x00, 0x9F, 0x00, 0x00, 0x01, 0x02, 0xA9, 0x07, 0x01};
    for (int i = 0; i < 260; i++)
    {
        calls.insert(calls.end(), {0x07, 0x01});
    }
    calls.push_back(0xFD);
    calls[2] = uint8_t(calls.size());
    calls[3] = uint8_t(calls.size() >> 8);
    calls.insert(calls.end(), {0x03, 0xA5, 0x0B, 0x06});
    m.Song(kTable, 1, kBase1, 0x220, {{4, 0x00}}, Revision::kEternalDuelist);
    m.PutBytes(kBase1, calls);
    m.PutBytes(kBase1 + 0x220, {0x00, 0xFD});
    const Rom rom = m.ToRom();
    DriverInfo info = SongsAt(kTable);
    info.revision = Revision::kEternalDuelist;
    Wct2004Sequencer seq(rom, info, 0);
    Wct2004Sequencer calling(rom, info, 1);

    std::vector<std::array<TrackOutput, kTracks>> frames;
    int looped_at = -1;
    for (int f = 0; f < 34; f++)
    {
        frames.push_back(seq.Step());
        looped_at = looped_at < 0 && seq.LoopedLastFrame() ? f : looped_at;
    }

    std::vector<int> starts;
    for (int f = 0; f < 270; f++)
    {
        if (calling.Step()[4].flags == kOutNoteOn)
        {
            starts.push_back(f);
        }
    }

    // Square 1's F0 goes into its record at once. A note at the volume that's playing only changes the pitch, and a new
    // volume retriggers the channel.
    SEQLIB_CHECK(seq.Valid() && frames[0][9].active);
    SEQLIB_CHECK(frames[0][0].pan == 0x10 && frames[0][0].trig == 1);
    SEQLIB_CHECK(frames[4][0].flags == kOutPsgNote && frames[4][0].trig == 0 && frames[4][0].pitch == 0x480);
    SEQLIB_CHECK(frames[8][0].trig == 1 && frames[8][0].vol == 10);

    // F0 on voice 1 sets its own volume, and voice 2's through the next record.
    SEQLIB_CHECK(frames[4][5].vol == 10 && frames[4][5].trig == 1);
    SEQLIB_CHECK(frames[4][6].vol == 5 && frames[4][6].trig == 1);

    // Vibrato replaces the flags of voice 2's second note, so the note doesn't start.
    SEQLIB_CHECK(frames[8][6].flags == kOutEternalDuelistPitch && frames[8][6].key == 11);

    // The loop sets square 1's pan back to the default, and the track goes on from its loop point, after its F0.
    SEQLIB_CHECK_EQ(looped_at, 32);
    SEQLIB_CHECK_EQ(frames[32][0].pan, 0x11);

    // The driver keeps the call's return position, so the 256th command after the call takes the track back there.
    SEQLIB_CHECK(starts == std::vector<int>({3, 5, 262}));
}

void TestCaptainTsubasaDecoding()
{
    // Delays read as in Eternal Duelist: a byte up to EF, or two bytes that give 12 bits from F0 up.
    Rom delays;
    delays.Assign({0xEF, 0xF1, 0x23});
    uint32_t delay = 0;

    SEQLIB_CHECK_EQ(ReadDelay(delays, kRomBase + 0, Revision::kCaptainTsubasa, delay), 1);
    SEQLIB_CHECK_EQ(delay, 0xEF);
    SEQLIB_CHECK_EQ(ReadDelay(delays, kRomBase + 1, Revision::kCaptainTsubasa, delay), 2);
    SEQLIB_CHECK_EQ(delay, 0x123);

    // F0 5A / F4 20 / F5 10 / F6 06 / FC 06 / F1 08 / F3 / 07 / 82
    Rom rom;
    rom.Assign({0xF0, 0x5A, 0xF4, 0x20, 0xF5, 0x10, 0xF6, 0x06, 0xFC, 0x06, 0xF1, 0x08, 0xF3, 0x07, 0x82});
    auto at = [&](uint32_t offset, int track)
    {
        return DecodeCommand(rom, kRomBase + offset, track, Revision::kCaptainTsubasa);
    };

    // F0 works as in Eternal Duelist. F4 and F5 set the attack and decay rates, and F1 and F6-FC are vibrato.
    SEQLIB_CHECK(at(0, 3).op == Op::kPsgPan && at(0, 3).value == 0x5A && at(0, 3).length == 2);
    SEQLIB_CHECK(at(0, 4).op == Op::kPairVolumes && at(0, 4).value == 0x5A && at(0, 4).length == 2);
    SEQLIB_CHECK(at(2, 0).op == Op::kAttack && at(2, 0).value == 0x20 && at(2, 0).length == 2);
    SEQLIB_CHECK(at(4, 0).op == Op::kDecay && at(4, 0).value == 0x10 && at(4, 0).length == 2);
    SEQLIB_CHECK(at(6, 4).op == Op::kVibrato && at(6, 4).value == 0x06 && at(6, 4).length == 2);
    SEQLIB_CHECK(at(8, 0).op == Op::kVibrato && at(8, 0).value == 0x06 && at(8, 0).length == 2);
    SEQLIB_CHECK(at(10, 0).op == Op::kVibrato && at(10, 0).value == 0x08);

    // As in the Rave Master revision, F3 takes no extra bytes, 00-7F do nothing, and 80-8F store the duty as it is.
    SEQLIB_CHECK(at(12, 4).op == Op::kLoopPoint && at(12, 4).length == 1);
    SEQLIB_CHECK(at(13, 0).op == Op::kNop && at(13, 0).length == 1);
    SEQLIB_CHECK(at(14, 0).op == Op::kDuty && at(14, 0).value == 2);
}

void TestCaptainTsubasaSequencer()
{
    // Song 0. Square 1: NR51 bits 10, loop point, PSG note (vol 8, note 0x30), wait 4, note 0x24 at the same volume,
    // wait 4, end. Square 2: attack rate 40, PSG note (vol 15, note 0x30), wait 8, end. Voice 1: note (vol 12, sample
    // 7), wait 4, volume 10 and 5 for the next voice, wait 4, end. Voice 2: note (vol 8, sample 7), wait 4, vibrato,
    // wait 4, note (vol 9, sample 11), wait 4, end. Voice 3: rest, wait 32, loop the song.
    Image m(0x800);
    constexpr uint32_t kTable = kRomBase + 0x100;
    constexpr uint32_t kBase = kRomBase + 0x200;
    m.Song(kTable, 0, kBase, 0x60, {{0, 0x00}, {1, 0x10}, {5, 0x20}, {6, 0x30}, {7, 0x50}}, Revision::kCaptainTsubasa);
    m.PutBytes(kBase + 0x00, {0x00, 0xF0, 0x10, 0x00, 0xF3, 0x00, 0xD8, 0x30, 0x04, 0xD8, 0x24, 0x04, 0xFD});
    m.PutBytes(kBase + 0x10, {0x00, 0xF4, 0x40, 0x00, 0xDF, 0x30, 0x08, 0xFD});
    m.PutBytes(kBase + 0x20, {0x00, 0xAC, 0x07, 0x04, 0xF0, 0x5A, 0x04, 0xFD});
    m.PutBytes(kBase + 0x30, {0x00, 0xA8, 0x07, 0x04, 0xF1, 0x20, 0x04, 0xA9, 0x0B, 0x04, 0xFD});
    m.PutBytes(kBase + 0x50, {0x00, 0xE0, 0x20, 0xFE});
    m.PutBytes(kBase + 0x60, {0x00, 0xFD});

    // Song 1. Voice 0: a call of 1 command from the fragment after the end, wait 2, note (vol 9, sample 7), wait 1,
    // then 260 commands that do nothing, each followed by a wait of 1, and the end. The fragment: wait 3, note (vol 5,
    // sample 11), wait 6.
    constexpr uint32_t kBase1 = kRomBase + 0x280;
    std::vector<uint8_t> calls = {0x00, 0x9F, 0x00, 0x00, 0x01, 0x02, 0xA9, 0x07, 0x01};
    for (int i = 0; i < 260; i++)
    {
        calls.insert(calls.end(), {0x07, 0x01});
    }
    calls.push_back(0xFD);
    calls[2] = uint8_t(calls.size());
    calls[3] = uint8_t(calls.size() >> 8);
    calls.insert(calls.end(), {0x03, 0xA5, 0x0B, 0x06});
    m.Song(kTable, 1, kBase1, 0x220, {{4, 0x00}}, Revision::kCaptainTsubasa);
    m.PutBytes(kBase1, calls);
    m.PutBytes(kBase1 + 0x220, {0x00, 0xFD});
    const Rom rom = m.ToRom();
    DriverInfo info = SongsAt(kTable);
    info.revision = Revision::kCaptainTsubasa;
    Wct2004Sequencer seq(rom, info, 0);
    Wct2004Sequencer calling(rom, info, 1);

    std::vector<std::array<TrackOutput, kTracks>> frames;
    int looped_at = -1;
    for (int f = 0; f < 34; f++)
    {
        frames.push_back(seq.Step());
        looped_at = looped_at < 0 && seq.LoopedLastFrame() ? f : looped_at;
    }

    std::vector<int> starts;
    for (int f = 0; f < 270; f++)
    {
        if (calling.Step()[4].flags == kOutNoteOn)
        {
            starts.push_back(f);
        }
    }

    // Square 1's F0 goes into its record at once. A note at the volume that's playing retriggers the channel too.
    SEQLIB_CHECK(seq.Valid() && frames[0][9].active);
    SEQLIB_CHECK(frames[0][0].pan == 0x10 && frames[0][0].trig == 1);
    SEQLIB_CHECK(frames[4][0].flags == kOutPsgNote && frames[4][0].trig == 1 && frames[4][0].pitch == 0x480);

    // Square 2's note rises from its attack: level ((0x10F - attack) * 15) >> 8 as the countdown falls by 0x40 a frame
    // from FF.
    SEQLIB_CHECK(frames[0][1].vol == 4 && frames[1][1].vol == 8 && frames[2][1].vol == 12 && frames[3][1].vol == 15);

    // F0 on voice 1 sets its volume, and voice 2's through the next record.
    SEQLIB_CHECK(frames[4][5].vol == 10 && frames[4][5].trig == 1);
    SEQLIB_CHECK(frames[4][6].vol == 5 && frames[4][6].trig == 1);

    // Vibrato adds the pitch flag to voice 2's second note, which still starts.
    SEQLIB_CHECK(frames[8][6].flags == (kOutNoteOn | kOutPitch) && frames[8][6].key == 11);

    // The loop sets square 1's pan back to the default.
    SEQLIB_CHECK_EQ(looped_at, 32);
    SEQLIB_CHECK_EQ(frames[32][0].pan, 0x11);

    // The driver clears the call's return position after the call's last command.
    SEQLIB_CHECK(starts == std::vector<int>({3, 5}));
}

void TestHunterXHunterDecoding()
{
    // Delays read as in Rave Master: a byte up to DF, or two bytes that give 13 bits from E0 up.
    Rom delays;
    delays.Assign({0xDF, 0xE1, 0x23});
    uint32_t delay = 0;

    SEQLIB_CHECK_EQ(ReadDelay(delays, kRomBase + 0, Revision::kHunterXHunter, delay), 1);
    SEQLIB_CHECK_EQ(delay, 0xDF);
    SEQLIB_CHECK_EQ(ReadDelay(delays, kRomBase + 1, Revision::kHunterXHunter, delay), 2);
    SEQLIB_CHECK_EQ(delay, 0x123);

    // F6 05 / F7 / FA / FC / F5 10 / F3 / 07 / 82
    Rom rom;
    rom.Assign({0xF6, 0x05, 0xF7, 0xFA, 0xFC, 0xF5, 0x10, 0xF3, 0x07, 0x82});
    auto at = [&](uint32_t offset, int track)
    {
        return DecodeCommand(rom, kRomBase + offset, track, Revision::kHunterXHunter);
    };

    // F6 stores the entry of a table of samples that the game gives the driver. F7-FC do nothing and take one byte.
    SEQLIB_CHECK(at(0, 4).op == Op::kInstrument && at(0, 4).value == 5 && at(0, 4).length == 2);
    SEQLIB_CHECK(at(2, 4).op == Op::kNop && at(2, 4).length == 1);
    SEQLIB_CHECK(at(3, 4).op == Op::kNop && at(3, 4).length == 1);
    SEQLIB_CHECK(at(4, 4).op == Op::kNop && at(4, 4).length == 1);

    // The other commands are Rave Master's.
    SEQLIB_CHECK(at(5, 0).op == Op::kDecay && at(5, 0).value == 0x10 && at(5, 0).length == 2);
    SEQLIB_CHECK(at(7, 4).op == Op::kLoopPoint && at(7, 4).length == 1);
    SEQLIB_CHECK(at(8, 0).op == Op::kNop && at(8, 0).length == 1);
    SEQLIB_CHECK(at(9, 0).op == Op::kDuty && at(9, 0).value == 2);
}

void TestKitaroDecoding()
{
    // FA / F9 12 / F8 85 / F7 40 / F6 05 / F3 / FB
    Rom rom;
    rom.Assign({0xFA, 0xF9, 0x12, 0xF8, 0x85, 0xF7, 0x40, 0xF6, 0x05, 0xF3, 0xFB});
    auto at = [&](uint32_t offset, int track)
    {
        return DecodeCommand(rom, kRomBase + offset, track, Revision::kKitaro);
    };

    // FA does nothing and takes one byte, as FB does.
    SEQLIB_CHECK(at(0, 4).op == Op::kNop && at(0, 4).length == 1);
    SEQLIB_CHECK(at(10, 4).op == Op::kNop && at(10, 4).length == 1);

    // The other commands are WCT 2004's: the echo's routing, delay and feedback, F6 for a table of samples, and an F3
    // with two more bytes on sample tracks.
    SEQLIB_CHECK(at(1, 4).op == Op::kEchoRoute && at(1, 4).value == 0x12 && at(1, 4).length == 2);
    SEQLIB_CHECK(at(3, 4).op == Op::kEchoDelay && at(3, 4).value == 0x85 && at(3, 4).length == 2);
    SEQLIB_CHECK(at(5, 4).op == Op::kEchoFeedback && at(5, 4).value == 0x40 && at(5, 4).length == 2);
    SEQLIB_CHECK(at(7, 4).op == Op::kInstrument && at(7, 4).value == 5 && at(7, 4).length == 2);
    SEQLIB_CHECK(at(9, 0).op == Op::kLoopPoint && at(9, 0).length == 1);
    SEQLIB_CHECK(at(9, 4).op == Op::kLoopPoint && at(9, 4).length == 3);
}

void TestKitaroSequencer()
{
    // Track 4: FA, wait 0, note (vol 9, sample 7), wait 2, end. The other tracks end at once. FA takes no argument, so
    // the note follows at once, at its volume, whatever the game's setting.
    Image m(0x200);
    constexpr uint32_t kTable = kRomBase + 0x100;
    constexpr uint32_t kBase = kTable + kSongEntrySize;
    m.Song(kTable, 0, kBase, 0x40, {{4, 0x00}});
    m.PutBytes(kBase + 0x00, {0x00, 0xFA, 0x00, 0xA9, 0x07, 0x02, 0xFD});
    m.PutBytes(kBase + 0x40, {0x00, 0xFD});
    const Rom rom = m.ToRom();
    DriverInfo info = SongsAt(kTable);
    info.revision = Revision::kKitaro;

    Wct2004Sequencer seq(rom, info, 0, Request{0, {}});
    const std::array<TrackOutput, kTracks> first = seq.Step();
    SEQLIB_CHECK(first[4].flags == kOutNoteOn && first[4].key == 7 && first[4].vol == 9);
}

void TestMinnaNoOujisamaDecoding()
{
    // FB 08 / FA 01 / F9 12 / 82 / FC
    Rom rom;
    rom.Assign({0xFB, 0x08, 0xFA, 0x01, 0xF9, 0x12, 0x82, 0xFC});
    auto at = [&](uint32_t offset, int track)
    {
        return DecodeCommand(rom, kRomBase + offset, track, Revision::kMinnaNoOujisama);
    };

    // FB sets the song's volume.
    SEQLIB_CHECK(at(0, 4).op == Op::kSongVolume && at(0, 4).value == 8 && at(0, 4).length == 2);

    // The other commands are WCT 2004's: FA's volume scale, the echo's routing, a duty shifted into bits 6-7, and an FC
    // that does nothing.
    SEQLIB_CHECK(at(2, 4).op == Op::kVolumeScale && at(2, 4).value == 1 && at(2, 4).length == 2);
    SEQLIB_CHECK(at(4, 4).op == Op::kEchoRoute && at(4, 4).value == 0x12 && at(4, 4).length == 2);
    SEQLIB_CHECK(at(6, 0).op == Op::kDuty && at(6, 0).value == 0x80);
    SEQLIB_CHECK(at(7, 4).op == Op::kNop && at(7, 4).length == 1);
}

void TestMinnaNoOujisamaSequencer()
{
    // Track 4: note (vol 9, sample 7), wait 2, song volume 8, wait 8, end. Track 5: note (vol 8, sample 11), wait 8,
    // end. Track 6: rest, wait 6, loop the song. The other tracks end at once.
    Image m(0x200);
    constexpr uint32_t kTable = kRomBase + 0x100;
    constexpr uint32_t kBase = kTable + kSongEntrySize;
    m.Song(kTable, 0, kBase, 0x30, {{4, 0x00}, {5, 0x10}, {6, 0x20}});
    m.PutBytes(kBase + 0x00, {0x00, 0xA9, 0x07, 0x02, 0xFB, 0x08, 0x08, 0xFD});
    m.PutBytes(kBase + 0x10, {0x00, 0xA8, 0x0B, 0x08, 0xFD});
    m.PutBytes(kBase + 0x20, {0x00, 0xE0, 0x06, 0xFE});
    m.PutBytes(kBase + 0x30, {0x00, 0xFD});
    const Rom rom = m.ToRom();
    DriverInfo info = SongsAt(kTable);
    info.revision = Revision::kMinnaNoOujisama;
    Wct2004Sequencer seq(rom, info, 0);

    std::vector<std::array<TrackOutput, kTracks>> frames;
    for (int f = 0; f < 8; f++)
    {
        frames.push_back(seq.Step());
    }

    // At full song volume, the notes play at their volumes, and the frames after a note's start have no retrigger.
    SEQLIB_CHECK(frames[0][4].flags == kOutNoteOn && frames[0][4].vol == 9);
    SEQLIB_CHECK(frames[1][4].vol == 9 && frames[1][4].trig == 0);
    SEQLIB_CHECK(frames[1][5].vol == 8 && frames[1][5].trig == 0);

    // Track 4's FB halves its volume in the same frame, with a retrigger. Track 5 runs before track 4 and gets the
    // change a frame later. Each frame then retriggers both tracks.
    SEQLIB_CHECK(frames[2][4].vol == 4 && frames[2][4].trig == 1);
    SEQLIB_CHECK(frames[2][5].vol == 8 && frames[2][5].trig == 0);
    SEQLIB_CHECK(frames[3][5].vol == 4 && frames[3][5].trig == 1);
    SEQLIB_CHECK(frames[5][4].vol == 4 && frames[5][4].trig == 1);

    // The loop sets the song volume back to full.
    SEQLIB_CHECK(seq.LoopsDone() == 1);
    SEQLIB_CHECK(frames[6][4].flags == kOutNoteOn && frames[6][4].vol == 9);
    SEQLIB_CHECK(frames[6][5].flags == kOutNoteOn && frames[6][5].vol == 8);
    SEQLIB_CHECK(frames[7][4].vol == 9 && frames[7][4].trig == 0);
}

void TestHunterXHunterSequencer()
{
    // A song of 12 tracks, with 28-byte song table entries. Track 4: F6 05, note (vol 12, sample 7), wait 4, end. Track
    // 5: F7, note (vol 8, sample 11), wait 4, end. Track 6: pan left 8 right 15, rest, wait 8, loop the song. The other
    // tracks end at once.
    Image m(0x200);
    constexpr uint32_t kTable = kRomBase + 0x100;
    constexpr uint32_t kBase = kTable + 28;
    m.Song(kTable, 0, kBase, 0x30, {{4, 0x00}, {5, 0x10}, {6, 0x20}}, Revision::kHunterXHunter);
    m.PutBytes(kBase + 0x00, {0x00, 0xF6, 0x05, 0x00, 0xAC, 0x07, 0x04, 0xFD});
    m.PutBytes(kBase + 0x10, {0x00, 0xF7, 0x00, 0xA8, 0x0B, 0x04, 0xFD});
    m.PutBytes(kBase + 0x20, {0x00, 0xF0, 0x8F, 0x00, 0xE0, 0x08, 0xFE});
    m.PutBytes(kBase + 0x30, {0x00, 0xFD});
    const Rom rom = m.ToRom();
    DriverInfo info = SongsAt(kTable);
    info.revision = Revision::kHunterXHunter;
    Wct2004Sequencer seq(rom, info, 0);

    std::vector<std::array<TrackOutput, kTracks>> frames;
    for (int f = 0; f < 10; f++)
    {
        frames.push_back(seq.Step());
    }

    // Only the song's 12 tracks run. A request without the game's table leaves each note its sample.
    SEQLIB_CHECK(seq.Valid());
    SEQLIB_CHECK(frames[0][11].active && !frames[0][12].active);
    SEQLIB_CHECK(frames[0][4].flags == kOutNoteOn && frames[0][4].key == 7 && frames[0][4].vol == 12);
    SEQLIB_CHECK(frames[0][5].flags == kOutNoteOn && frames[0][5].key == 11 && frames[0][5].vol == 8);

    // A loop keeps the pan, as in WCT 2004, where Rave Master centres it again.
    SEQLIB_CHECK(seq.LoopsDone() == 1);
    SEQLIB_CHECK(frames[8][6].pan_start == 0x8F && frames[8][6].pan == 0x8F);
}

void TestCaptainTsubasaConversion()
{
    // Song 0 of the Captain Tsubasa revision. Voice 0: note (vol 8, sample 6), wait 4, volume 3, wait 4, rest, wait 4,
    // end. Sample 6 plays at twice the step of sample 0. A Captain Tsubasa command reader is at kCode.
    Image m = SyntheticImage();
    constexpr uint32_t kCode = kRomBase + 0x800;
    std::vector<uint16_t> reader = CommandReader(0x2DFE, 0x2DF5);
    reader.insert(reader.end(), {0x2DF4, 0xD100, 0x2DF3, 0xD100, 0x2DF2, 0xD100, 0x2DF0, 0xD100});
    m.PutHalfwords(kCode, reader);
    m.Song(kSongTable, 0, kSongBase, 0x20, {{4, 0x00}}, Revision::kCaptainTsubasa);
    m.PutBytes(kSongBase + 0x00, {0x00, 0xA8, 0x06, 0x04, 0xC3, 0x04, 0xE0, 0x04, 0xFD});
    m.PutBytes(kSongBase + 0x20, {0x00, 0xFD});

    // Sample 0 is sample 5, and sample 6 has sample 5's data at twice its step.
    constexpr uint32_t kHeader6 = kRomBase + 0x600;
    m.Put32(kSampleTable, kSampleHeader);
    m.Put32(kSampleTable + 4 * 6, kHeader6);
    m.Put32(kHeader6, 0x2000);
    m.Put32(kHeader6 + 4, 64);
    m.Put32(kHeader6 + 8, 0xFFFFFFFF);
    for (uint32_t i = 0; i < 64; i++)
    {
        m.Put8(kHeader6 + 12 + i, m.Get8(kSampleHeader + 12 + i));
    }

    const Rom rom = m.ToRom();
    DriverInfo info;
    SEQLIB_CHECK(DetectGiven(rom, kSongTable, 1, info) && info.revision == Revision::kCaptainTsubasa);

    const fs::path dir = TempPath("captain_tsubasa");
    std::error_code ec;
    fs::create_directories(dir, ec);
    ConvertSettings opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);
    int notes = 0, bends = 0;
    for (const MidiEvent& e : ReadEvents(sum.midi_path))
    {
        if (e.track != "Voice 0")
        {
            continue;
        }

        notes += (e.status & 0xF0) == 0x90 && e.data2 > 0;
        bends += (e.status & 0xF0) == 0xE0 && e.frame > 0;
    }

    // The volume command's record has a flag, but not the pitch flag, and its sample is 0. The voice keeps its pitch.
    SEQLIB_CHECK(sum.ok);
    SEQLIB_CHECK_EQ(notes, 1);
    SEQLIB_CHECK_EQ(bends, 0);

    fs::remove_all(dir, ec);
}

void TestEternalDuelistConversion()
{
    // Song 0 of the Eternal Duelist revision. Square 1: PSG note (vol 8, note 0x30), wait 4, notes 0x10 and 0x50 at the
    // same volume, 4 frames each, rest, wait 4, end. Wave: wave 2, note (vol 8, note 0x20), wait 4, note 0x24 at the
    // same volume, wait 4, rest, wait 4, end. Voice 0: note (vol 8, sample 5), wait 8, note (vol 8, sample 5, +7), a
    // bend of 0 in the same frame, wait 4, rest, wait 4, end. Voice 1 does the same, but its second note plays sample
    // 6, at semitone 0. An Eternal Duelist command reader is at kCode, and the tables are given by hand.
    Image m = SyntheticImage();
    constexpr uint32_t kCode = kRomBase + 0x800;
    constexpr uint32_t kWaveTable = kRomBase + 0x900;
    m.PutHalfwords(kCode, CommandReader(0x2DFE, 0x2DF3));
    m.Song(kSongTable, 0, kSongBase, 0x60, {{0, 0x00}, {2, 0x20}, {4, 0x40}, {5, 0x70}}, Revision::kEternalDuelist);
    m.PutBytes(kSongBase + 0x00, {0x00, 0xD8, 0x30, 0x04, 0xD8, 0x10, 0x04, 0xD8, 0x50, 0x04, 0xE0, 0x04, 0xFD});
    m.PutBytes(kSongBase + 0x20, {0x00, 0x86, 0x00, 0xD8, 0x20, 0x04, 0xD8, 0x24, 0x04, 0xE0, 0x04, 0xFD});
    m.PutBytes(kSongBase + 0x40, {0x00, 0xA8, 0x05, 0x08, 0xB8, 0x05, 0x07, 0x00, 0xF2, 0x40, 0x04, 0xE0, 0x04, 0xFD});
    m.PutBytes(kSongBase + 0x60, {0x00, 0xFD});
    m.PutBytes(kSongBase + 0x70, {0x00, 0xA8, 0x05, 0x08, 0xA8, 0x06, 0x00, 0xF2, 0x40, 0x04, 0xE0, 0x04, 0xFD});

    // Sample 6: sample 5's data, at twice its step.
    constexpr uint32_t kHeader6 = kRomBase + 0x600;
    m.Put32(kSampleTable + 4 * 6, kHeader6);
    m.Put32(kHeader6, 0x2000);
    m.Put32(kHeader6 + 4, 64);
    m.Put32(kHeader6 + 8, 0xFFFFFFFF);
    for (uint32_t i = 0; i < 64; i++)
    {
        m.Put8(kHeader6 + 12 + i, m.Get8(kSampleHeader + 12 + i));
    }

    // Waves 0 and 2 at volume 8: a square and a sawtooth.
    for (int i = 0; i < 16; i++)
    {
        m.Put8(kWaveTable + 8 * 16 + uint32_t(i), i < 8 ? 0x88 : 0x00);
        m.Put8(kWaveTable + 40 * 16 + uint32_t(i), uint8_t(i * 0x11));
    }

    const Rom rom = m.ToRom();
    DriverInfo info;
    SEQLIB_CHECK(DetectGiven(rom, kSongTable, 1, info) && info.revision == Revision::kEternalDuelist);
    info.wave_table = kWaveTable;

    const fs::path dir = TempPath("eternal_duelist");
    std::error_code ec;
    fs::create_directories(dir, ec);
    ConvertSettings opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    // Each track's note-ons (frame, program), bends (frame, value) and bend range.
    std::map<std::string, std::vector<std::pair<uint32_t, int>>> notes, bends;
    std::map<std::string, int> program, range;
    for (const MidiEvent& e : ReadEvents(sum.midi_path))
    {
        const int kind = e.status & 0xF0;
        if (kind == 0xC0)
        {
            program[e.track] = e.data1;
        }
        if (kind == 0x90 && e.data2 > 0)
        {
            notes[e.track].emplace_back(e.frame, program[e.track]);
        }
        if (kind == 0xE0 && e.frame > 0)
        {
            bends[e.track].emplace_back(e.frame, e.data1 | (e.data2 << 7));
        }
        if (kind == 0xB0 && e.data1 == cc::kDataEntry)
        {
            range[e.track] = e.data2;
        }
    }

    // Square 1's legato notes bend its first note by 32 semitones in either direction. The bend range must cover both.
    SEQLIB_CHECK(sum.ok);
    SEQLIB_REQUIRE_EQ(notes["Square 1"].size(), 1);
    SEQLIB_CHECK_EQ(range["Square 1"], 32);
    SEQLIB_CHECK(bends["Square 1"] == (std::vector<std::pair<uint32_t, int>>{{4, 0}, {8, 16383}}));

    // The wave's legato note reloads wave 0, which MIDI plays as a new note.
    SEQLIB_REQUIRE_EQ(notes["Wave"].size(), 2);
    SEQLIB_CHECK_EQ(notes["Wave"][1].first, 4);
    SEQLIB_CHECK(notes["Wave"][1].second != notes["Wave"][0].second);

    // Voice 0's second note doesn't start, and its first plays on at the second's pitch, 7 semitones up.
    SEQLIB_REQUIRE_EQ(notes["Voice 0"].size(), 1);
    SEQLIB_CHECK_EQ(range["Voice 0"], 7);
    SEQLIB_CHECK(bends["Voice 0"] == (std::vector<std::pair<uint32_t, int>>{{8, 16383}}));

    // Voice 1's first note plays on as well, an octave up, at the step of sample 6 at semitone 0.
    SEQLIB_REQUIRE_EQ(notes["Voice 1"].size(), 1);
    SEQLIB_CHECK_EQ(range["Voice 1"], 12);
    SEQLIB_CHECK(bends["Voice 1"] == (std::vector<std::pair<uint32_t, int>>{{8, 16383}}));

    fs::remove_all(dir, ec);
}

int RunTests()
{
    TestWct2004Decoding();
    TestRaveMasterSequencer();
    TestWct2004Sequencer();
    TestCallDump();
    TestWct2004Panning();
    TestEternalDuelistDecoding();
    TestEternalDuelistSequencer();
    TestEternalDuelistConversion();
    TestCaptainTsubasaDecoding();
    TestCaptainTsubasaSequencer();
    TestCaptainTsubasaConversion();
    TestHunterXHunterDecoding();
    TestHunterXHunterSequencer();
    TestKitaroDecoding();
    TestKitaroSequencer();
    TestMinnaNoOujisamaDecoding();
    TestMinnaNoOujisamaSequencer();

    return seqlib::testing::Summary("konami_revisions");
}

} // namespace
} // namespace ntdlib::sequence_export::konami

int main()
{
    return ntdlib::sequence_export::konami::RunTests();
}
