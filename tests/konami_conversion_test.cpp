// SPDX-License-Identifier: MIT

// Unit tests for the conversion of Konami's songs: the MIDI, SoundFont and SFZ files, the echo, the loops, the PSG and
// the samples' keys. They build small synthetic cartridges in memory. No game ROM is needed.

#include "konami_test_util.h"

namespace ntdlib::sequence_export::konami
{
namespace
{

void TestConversion()
{
    const Rom rom = SyntheticRom();
    DriverInfo info;
    SEQLIB_CHECK(DetectGiven(rom, kSongTable, 1, info));
    SEQLIB_CHECK_EQ(info.song_count, 1);

    const fs::path dir = TempPath("out");
    fs::create_directories(dir);
    ConvertSettings opt;
    opt.frame_timing = true;
    opt.loops = 1;
    opt.out_dir = Utf8(dir);
    opt.base_name = "synthetic";

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(sum.ok);
    SEQLIB_CHECK(sum.loop_start == 0.0);
    SEQLIB_CHECK(sum.loop_end == FrameSeconds(15));
    SEQLIB_CHECK(sum.seconds == FrameSeconds(15));
    SEQLIB_CHECK_EQ(sum.tracks, 3);
    SEQLIB_CHECK(fs::exists(PathFromUtf8(sum.sf2_path)));

    const std::vector<MidiEvent> events = ReadEvents(sum.midi_path);

    auto has = [&](const std::string& track, uint32_t frame, uint8_t type)
    {
        for (const MidiEvent& e : events)
        {
            if (e.track == track && e.frame == frame && (e.status & 0xF0) == type)
            {
                return true;
            }
        }

        return false;
    };

    SEQLIB_CHECK(has("Voice 0", 0, 0x90));
    SEQLIB_CHECK(has("Voice 0", 10, 0x80));
    SEQLIB_CHECK(has("Voice 1", 7, 0x90));
    SEQLIB_CHECK(has("Voice 1", 10, 0x80));
    SEQLIB_CHECK(has("Square 1", 0, 0x90));
    SEQLIB_CHECK(has("Square 1", 4, 0x80));

    for (const MidiEvent& e : events)
    {
        if (e.track == "Square 1" && (e.status & 0xF0) == 0x90)
        {
            SEQLIB_CHECK_EQ(e.data1, 60); // PSG note 24 = C4
        }
    }

    opt.base_name = "\xE9\x9F\xB3\xE6\xA5\xBD:demo. ";

    const SongReport renamed = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(renamed.ok);
    SEQLIB_CHECK(PathFromUtf8(renamed.midi_path) == dir / PathFromUtf8("\xE9\x9F\xB3\xE6\xA5\xBD_demo_00.mid"));
    SEQLIB_CHECK(ReadAll(renamed.midi_path) == ReadAll(sum.midi_path));
    SEQLIB_CHECK(ReadAll(renamed.sf2_path) == ReadAll(sum.sf2_path));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// The programs that each MIDI track of the file at `path` selects.
std::map<std::string, std::set<int>> TrackPrograms(const std::string& path)
{
    std::map<std::string, std::set<int>> programs;
    for (const MidiEvent& e : ReadEvents(path))
    {
        if ((e.status & 0xF0) == 0xC0)
        {
            programs[e.track].insert(e.data1);
        }
    }

    return programs;
}

// The programs that the <master> headers of SFZ file `text` select.
std::set<int> SfzPrograms(const std::string& text)
{
    std::set<int> programs;
    for (size_t at = text.find("<master> loprog="); at != std::string::npos; at = text.find("<master> loprog=", at + 1))
    {
        programs.insert(std::stoi(text.substr(at + 16)));
    }

    return programs;
}

void TestSfzFiles()
{
    // ConvertSongFiles makes an SFZ file for each MIDI track, with a <master> for each program that the track selects,
    // and a WAV file for each sample that the regions name.
    const Rom rom = SyntheticRom();
    DriverInfo info;
    SEQLIB_CHECK(DetectGiven(rom, kSongTable, 1, info));
    ConvertSettings opt;
    opt.frame_timing = true;
    opt.loops = 1;
    opt.program = "test";

    const SongFiles files = ConvertSongFiles(rom, info, 0, opt);

    SEQLIB_CHECK(files.report.ok && !files.report.silent);
    const fs::path midi = TempPath("sfz.mid");
    std::string error;
    SEQLIB_CHECK(ntdlib::agb::WriteFile(Utf8(midi), files.midi, error));
    const std::map<std::string, std::set<int>> programs = TrackPrograms(Utf8(midi));
    SEQLIB_CHECK_EQ(files.sfz.files.size(), size_t(3));
    SEQLIB_CHECK_EQ(programs.size(), size_t(3));
    for (const seqlib::sfz::File& file : files.sfz.files)
    {
        const std::string track = file.name.substr(0, file.name.size() - 4);
        SEQLIB_CHECK(programs.contains(track) && SfzPrograms(file.text) == programs.at(track));
        SEQLIB_CHECK(file.text.find("<region> sample=") != std::string::npos);
    }

    // Square 1 plays PSG note 24 on key 60, at its zone's root key.
    const auto square = std::ranges::find(files.sfz.files, std::string("Square 1.sfz"), &seqlib::sfz::File::name);
    SEQLIB_CHECK(square != files.sfz.files.end() && square->text.find("pitch_keycenter=60") != std::string::npos);

    // Every region's sample is among the WAV files.
    std::set<std::string> named;
    for (const seqlib::sfz::File& file : files.sfz.files)
    {
        for (size_t at = file.text.find("sample="); at != std::string::npos; at = file.text.find("sample=", at + 1))
        {
            if (file.text.compare(at - 1, 1, " ") == 0)
            {
                named.insert(file.text.substr(at + 7, file.text.find(' ', at) - at - 7));
            }
        }
    }
    std::set<std::string> wavs;
    for (const seqlib::sfz::Sample& sample : files.sfz.samples)
    {
        SEQLIB_CHECK(sample.wav.size() > 44 && sample.wav[0] == 'R');
        wavs.insert(sample.name);
    }
    SEQLIB_CHECK(!named.empty() && named == wavs);

    std::error_code ec;
    fs::remove(midi, ec);
}

void TestSilentSongs()
{
    const Rom rom = SyntheticRom();
    DriverInfo info;
    SEQLIB_CHECK(DetectGiven(rom, kSongTable, 2, info));

    std::error_code ec;
    const fs::path dir = TempPath("silent");
    fs::create_directories(dir, ec);
    SEQLIB_CHECK(!ec);
    if (ec)
    {
        return;
    }

    ConvertSettings opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);
    opt.base_name = "silent";

    // Song 1 plays no notes, so nothing is written for it.
    SongReport sum = ConvertSong(rom, info, 1, opt, nullptr);

    SEQLIB_CHECK(sum.ok && sum.silent);
    SEQLIB_CHECK_EQ(sum.tracks, 0);
    SEQLIB_CHECK(sum.midi_path.empty() && sum.sf2_path.empty());

    // Song 0 has notes, but none on track 15.
    opt.track_mask = 1u << 15;

    sum = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(sum.ok && sum.silent);
    SEQLIB_CHECK(fs::is_empty(dir));
    fs::remove_all(dir, ec);
}

void TestSharedSoundfont()
{
    // A shared SoundFont puts each song's presets in the bank numbered after the song. MIDI can't select a bank above
    // 127. Song 128's presets therefore take the free programs after the presets of song 0 in bank 0. This table has
    // the synthetic song as both song 0 and song 128.
    Image m = SyntheticImage();
    m.Resize(0x3000);
    constexpr uint32_t kTable = kRomBase + 0x1000;
    m.Song(kTable, 0, kSongBase, kRestOffset, kTrackOffsets);
    m.Song(kTable, 128, kSongBase, kRestOffset, kTrackOffsets);
    const Rom rom = m.ToRom();

    DriverInfo info;
    SEQLIB_CHECK(DetectGiven(rom, kTable, 129, info));

    std::error_code ec;
    const fs::path dir = TempPath("shared");
    fs::create_directories(dir, ec);
    ConvertSettings opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);
    SoundfontBuilder shared(rom, info, 129);

    const SongReport first = ConvertSong(rom, info, 0, opt, &shared);
    const size_t first_count = shared.File().presets.size();
    const SongReport last = ConvertSong(rom, info, 128, opt, &shared);

    const std::vector<sf2::Preset>& presets = shared.File().presets;
    SEQLIB_CHECK(first.ok && last.ok && first.sf2_path.empty());
    SEQLIB_CHECK(first_count > 0 && presets.size() == 2 * first_count);
    for (size_t i = 0; i < presets.size(); i++)
    {
        SEQLIB_CHECK(presets[i].bank == 0 && presets[i].program == int(i));
    }
    fs::remove_all(dir, ec);
}

void TestEchoRouting()
{
    // Song 2 routes voice 0 to bus 0, then to bus 1 as well, then off both, then to bus 0 again, 5 frames apart. The
    // mixer takes a voice routed to buses 0 and 1 as routed to bus 1 alone, so the echo is only sent while the voice is
    // on bus 0 alone.
    Image m = SyntheticImage();
    constexpr uint32_t kBase = kRomBase + 0x600;
    m.Song(kSongTable, 2, kBase, 0x40, {{4, 0}});
    m.PutBytes(kBase, {0x00, 0xF7, 0x7F, 0x00, 0xF9, 0x01, 0x00, 0xA8, 0x10, 0x05, 0x05,
                       0xF9, 0x02, 0x05, 0xF9, 0x00, 0x05, 0xF9, 0x01, 0x05, 0xFD});
    m.PutBytes(kBase + 0x40, {0x00, 0xFD});
    const Rom rom = m.ToRom();

    DriverInfo info;
    SEQLIB_CHECK(DetectGiven(rom, kSongTable, 3, info));

    std::error_code ec;
    const fs::path dir = TempPath("echo");
    fs::create_directories(dir, ec);
    ConvertSettings opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);

    const SongReport sum = ConvertSong(rom, info, 2, opt, nullptr);

    std::vector<std::pair<uint32_t, int>> sends;
    for (const MidiEvent& e : ReadEvents(sum.midi_path))
    {
        if (e.track == "Voice 0" && (e.status & 0xF0) == 0xB0 && e.data1 == cc::kReverb)
        {
            sends.emplace_back(e.frame, e.data2);
        }
    }

    const std::vector<std::pair<uint32_t, int>> kExpected = {{0, 0}, {0, 127}, {5, 0}, {15, 127}};
    SEQLIB_CHECK(sum.ok);
    SEQLIB_CHECK(sends == kExpected);
    fs::remove_all(dir, ec);
}

// Song 2 routes voices 0 and 5 to bus 0 alone. The mixer of the Live Start revision takes only voices 5 to 11 from bus
// 0. Voice 0 then sends no echo there. The Ultimate Masters revision takes every voice.
void TestEchoFromVoice5()
{
    Image m = SyntheticImage();
    constexpr uint32_t kBase = kRomBase + 0x600;
    m.Song(kSongTable, 2, kBase, 0x40, {{4, 0}, {9, 0}});
    m.PutBytes(kBase, {0x00, 0xF7, 0x7F, 0x00, 0xF9, 0x01, 0x00, 0xF9, 0x51, 0x00, 0xA8, 0x10, 0x05, 0x05, 0xFD});
    m.PutBytes(kBase + 0x40, {0x00, 0xFD});
    const Rom rom = m.ToRom();

    DriverInfo info;
    SEQLIB_CHECK(DetectGiven(rom, kSongTable, 3, info));

    std::error_code ec;
    const fs::path dir = TempPath("echo_from_5");
    fs::create_directories(dir, ec);
    ConvertSettings opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);

    // The highest echo that each voice's track sends.
    const auto highest = [&](Revision revision)
    {
        info.revision = revision;
        const SongReport sum = ConvertSong(rom, info, 2, opt, nullptr);
        SEQLIB_CHECK(sum.ok);
        std::pair<int, int> most = {-1, -1};
        for (const MidiEvent& e : ReadEvents(sum.midi_path))
        {
            if ((e.status & 0xF0) == 0xB0 && e.data1 == cc::kReverb)
            {
                int& voice = e.track == "Voice 0" ? most.first : most.second;
                voice = std::max(voice, int(e.data2));
            }
        }
        return most;
    };

    SEQLIB_CHECK(highest(Revision::kUltimateMasters) == std::pair(127, 127));
    SEQLIB_CHECK(highest(Revision::kLiveStart) == std::pair(0, 127));
    fs::remove_all(dir, ec);
}

// Song 2's sample track plays a note at volume 16, and then from its loop point a note at 16 and one at 8. The first
// time through, the loop's first note has the level and program that the note before it left, but a player that jumps
// back to the loop's start comes from the note at volume 8, so they're written again there, and so is the tempo.
void TestLoopSettingsAgain()
{
    Image m = SyntheticImage();
    constexpr uint32_t kBase = kRomBase + 0x600;
    m.Song(kSongTable, 2, kBase, 0x40, {{4, 0}});
    m.PutBytes(kBase, {0x00, 0xA8, 0x10, 0x05, 0x0A, 0xF3, 0x00, 0x00, 0x00, 0xA8,
                       0x10, 0x05, 0x0A, 0xA8, 0x08, 0x05, 0x0A, 0xFF, 0x01, 0x00});
    m.PutBytes(kBase + 0x40, {0x00, 0xFD});
    const Rom rom = m.ToRom();

    DriverInfo info;
    SEQLIB_CHECK(DetectGiven(rom, kSongTable, 3, info));

    std::error_code ec;
    const fs::path dir = TempPath("loop_again");
    fs::create_directories(dir, ec);
    ConvertSettings opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);

    const SongReport sum = ConvertSong(rom, info, 2, opt, nullptr);

    std::vector<std::pair<uint32_t, int>> levels, programs;
    for (const MidiEvent& e : ReadEvents(sum.midi_path))
    {
        if (e.track == "Voice 0" && (e.status & 0xF0) == 0xB0 && e.data1 == cc::kExpression)
        {
            levels.emplace_back(e.frame, e.data2);
        }
        if (e.track == "Voice 0" && (e.status & 0xF0) == 0xC0)
        {
            programs.emplace_back(e.frame, e.data1);
        }
    }

    SEQLIB_CHECK(sum.ok);
    SEQLIB_CHECK(sum.loop_start == FrameSeconds(10));
    SEQLIB_CHECK(
        ntdlib::testing::agb_files::RepeatsTempoAtLoopStart(ntdlib::testing::agb_files::ReadMidi(sum.midi_path)));
    SEQLIB_REQUIRE_EQ(levels.size(), 6);
    SEQLIB_REQUIRE_EQ(programs.size(), 2);
    SEQLIB_CHECK(levels[2] == std::make_pair(10u, levels[1].second));
    SEQLIB_CHECK(levels[3].second != levels[2].second && levels[4] == std::make_pair(30u, levels[1].second));
    SEQLIB_CHECK(programs[1] == std::make_pair(10u, programs[0].second));
    fs::remove_all(dir, ec);
}

// Song 2's sample track pans to the left before its loop point, plays a note and then pans to the right, which the
// driver keeps when every track goes back to its loop point. The first pass's note plays on the left and the later
// passes' on the right, so the marked loop starts at the second pass, and the song plays the first pass before it.
void TestLoopStartsAtSecondPass()
{
    Image m = SyntheticImage();
    constexpr uint32_t kBase = kRomBase + 0x600;
    m.Song(kSongTable, 2, kBase, 0x40, {{4, 0}});
    m.PutBytes(kBase, {0x00, 0xF0, 0x00, 0x00, 0xF3, 0x00, 0x00, 0x00, 0xA8, 0x10, 0x05, 0x0A, 0xF0, 0x7F, 0x0A, 0xFF,
                       0x01, 0x00});
    m.PutBytes(kBase + 0x40, {0x00, 0xFD});
    const Rom rom = m.ToRom();

    DriverInfo info;
    SEQLIB_CHECK(DetectGiven(rom, kSongTable, 3, info));

    std::error_code ec;
    const fs::path dir = TempPath("loop_second");
    fs::create_directories(dir, ec);
    ConvertSettings opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);

    const SongReport sum = ConvertSong(rom, info, 2, opt, nullptr);

    // The first pass runs from frame 0 to 20, so the loop runs from 20 to 40, and the song ends after its second pass
    // through it.
    SEQLIB_CHECK(sum.ok);
    SEQLIB_CHECK(sum.loop_start == FrameSeconds(20));
    SEQLIB_CHECK(sum.loop_end == FrameSeconds(40));
    SEQLIB_CHECK(sum.seconds == FrameSeconds(60));
    fs::remove_all(dir, ec);
}

void TestNoiseKeys()
{
    // A noise track that plays notes 0 to 129, more than one preset has keys for. Notes 0 to 91 go on keys 36 to 127
    // and 92 to 127 on the keys below 36, and 128 and 129 need a second preset. Each note gets a key of its own.
    Image m = SyntheticImage();
    m.Resize(0x2000);
    constexpr uint32_t kBase = kRomBase + 0x1000;
    m.Song(kSongTable, 3, kBase, 0x400, {{3, 0}});
    std::vector<uint8_t> track = {0x00};
    for (int n = 0; n < 130; n++)
    {
        track.insert(track.end(), {0xD8, 0x0F, uint8_t(n), 0x01}); // note n at volume 15, then wait a frame
    }
    track.insert(track.end(), {0xE0, 0x00, 0xFD});
    m.PutBytes(kBase, track);
    m.PutBytes(kBase + 0x400, {0x00, 0xFD});
    const Rom rom = m.ToRom();

    DriverInfo info;
    SEQLIB_CHECK(DetectGiven(rom, kSongTable, 4, info));

    const fs::path dir = TempPath("noise");
    std::error_code ec;
    fs::create_directories(dir, ec);
    ConvertSettings opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);

    const SongReport sum = ConvertSong(rom, info, 3, opt, nullptr);

    std::vector<std::pair<int, int>> sounds; // (program, key) of each note
    int program = -1;
    for (const MidiEvent& e : ReadEvents(sum.midi_path))
    {
        if (e.track == "Noise" && (e.status & 0xF0) == 0xC0)
        {
            program = e.data1;
        }
        if (e.track == "Noise" && (e.status & 0xF0) == 0x90)
        {
            sounds.emplace_back(program, e.data1);
        }
    }

    SEQLIB_CHECK(sum.ok);
    SEQLIB_REQUIRE_EQ(sounds.size(), 130);
    SEQLIB_CHECK(sounds.front() == std::make_pair(sounds.front().first, 36));
    SEQLIB_CHECK(sounds[92] == std::make_pair(sounds.front().first, 35));
    SEQLIB_CHECK(sounds[128].first != sounds.front().first && sounds[128].second == 127);

    std::sort(sounds.begin(), sounds.end());
    SEQLIB_CHECK(std::unique(sounds.begin(), sounds.end()) == sounds.end());
    fs::remove_all(dir, ec);
}

void TestPsgPastTable()
{
    // Square 1 plays note 9, note FF, a rest and note FF again, 5 frames apart. The code at kFreqCode loads the address
    // of the PSG frequency table, which has an entry for note 9. Note FF reads the word at 0x3FC0 past its start.
    Image m = SyntheticImage();
    m.Resize(0x6000);
    constexpr uint32_t kBase = kRomBase + 0x1000;
    constexpr uint32_t kFreqCode = kRomBase + 0x1100;
    constexpr uint32_t kFreqTable = kRomBase + 0x1200;
    m.Song(kSongTable, 2, kBase, 0x80, {{0, 0}});
    m.PutBytes(kBase, {0x00, 0x82, 0x00, 0xD5, 0x09, 0x05, 0xD5, 0xFF, 0x05, 0xE0, 0x05, 0xD5, 0xFF, 0x05, 0xFD});
    m.PutBytes(kBase + 0x80, {0x00, 0xFD});
    m.PutHalfwords(kFreqCode, {0x4803, 0x2700, 0x5FE9, 0x0049, 0x1809, 0x880D});
    m.Put32(kFreqCode + 0x10, kFreqTable);
    m.Put16(kFreqTable, 0x802C);
    m.Put16(kFreqTable + 2 * 9 * 32, 0x8358);

    const fs::path dir = TempPath("past_table");
    std::error_code ec;
    fs::create_directories(dir, ec);
    ConvertSettings opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);

    // The note-ons (frame, key) and pitch bends (frame, value) of Square 1.
    struct Square1
    {
        std::vector<std::pair<uint32_t, int>> notes, bends;
    };

    // Returns the events of Square 1 when the word past the table is `word`.
    auto convert = [&](uint16_t word)
    {
        m.Put16(kFreqTable + 0x3FC0, word);
        const Rom rom = m.ToRom();
        DriverInfo info;
        SEQLIB_CHECK(DetectGiven(rom, kSongTable, 3, info) && info.psg_freq_table == kFreqTable);

        const SongReport sum = ConvertSong(rom, info, 2, opt, nullptr);
        SEQLIB_CHECK(sum.ok);

        Square1 square;
        for (const MidiEvent& e : ReadEvents(sum.midi_path))
        {
            if (e.track == "Square 1" && (e.status & 0xF0) == 0x90 && e.data2 > 0)
            {
                square.notes.emplace_back(e.frame, e.data1);
            }
            if (e.track == "Square 1" && (e.status & 0xF0) == 0xE0 && e.frame > 0)
            {
                square.bends.emplace_back(e.frame, e.data1 | (e.data2 << 7));
            }
        }

        return square;
    };

    const Square1 plain = convert(0x0000);

    // Register 0 without the restart bit: note 9 goes on at 64 Hz, 12/32 semitone below note 0, and the second note FF,
    // after the rest, plays nothing. The bend range is 10 semitones, enough for the 300/32 between them.
    SEQLIB_REQUIRE_EQ(plain.notes.size(), 1);
    SEQLIB_CHECK(plain.bends == (std::vector<std::pair<uint32_t, int>>{{5, 8192 - 7680}}));

    const Square1 restarting = convert(0x8000);

    // With the restart bit, each note FF starts a note of its own, on the key of note 0, bent down by 12/32 semitone of
    // the 2-semitone bend range. The bend is already there for the second one.
    SEQLIB_REQUIRE_EQ(restarting.notes.size(), 3);
    SEQLIB_CHECK(restarting.bends == (std::vector<std::pair<uint32_t, int>>{{5, 8192 - 1536}}));
    const int key0 = plain.notes[0].second - 9;
    SEQLIB_CHECK(restarting.notes[1] == std::make_pair(uint32_t(5), key0));
    SEQLIB_CHECK(restarting.notes[2] == std::make_pair(uint32_t(15), key0));

    fs::remove_all(dir, ec);
}

// Returns the key range and the tuning in cents, {lo, hi, cents}, of each instrument zone of the SoundFont at `path`,
// in the file's order. Each zone starts with its key range.
std::vector<std::array<int, 3>> ReadZoneTuning(const std::string& path)
{
    const std::vector<uint8_t> igen = ReadSf2(path).chunks["igen"];
    std::vector<std::array<int, 3>> zones;
    for (size_t g = 0; g + 4 <= igen.size(); g += 4)
    {
        const int op = igen[g] | (igen[g + 1] << 8);
        const int amount = int16_t(igen[g + 2] | (igen[g + 3] << 8));
        if (op == sf2::kKeyRange)
        {
            zones.push_back({igen[g + 2], igen[g + 3], 0});
        }
        else if (op == sf2::kCoarseTune && !zones.empty())
        {
            zones.back()[2] += 100 * amount;
        }
        else if (op == sf2::kFineTune && !zones.empty())
        {
            zones.back()[2] += amount;
        }
    }

    return zones;
}

void TestPsgTuning()
{
    // Square 1 plays note 0 and then note 12, 5 frames apart. The PSG frequency table gives note 0 setting 44. That
    // setting plays C2 at 65.4 Hz. Note 12 gets setting 1060. That one plays 132.7 Hz, 24 cents above C3. The table's
    // other entries are 0.
    Image m = SyntheticImage();
    m.Resize(0x6000);
    constexpr uint32_t kBase = kRomBase + 0x1000;
    constexpr uint32_t kFreqCode = kRomBase + 0x1100;
    constexpr uint32_t kFreqTable = kRomBase + 0x1200;
    m.Song(kSongTable, 2, kBase, 0x80, {{0, 0}});
    m.PutBytes(kBase, {0x00, 0x82, 0x00, 0xD5, 0x00, 0x05, 0xD5, 0x0C, 0x05, 0xFD});
    m.PutBytes(kBase + 0x80, {0x00, 0xFD});
    m.PutHalfwords(kFreqCode, {0x4803, 0x2700, 0x5FE9, 0x0049, 0x1809, 0x880D});
    m.Put32(kFreqCode + 0x10, kFreqTable);
    m.Put16(kFreqTable, 0x802C);
    m.Put16(kFreqTable + 2 * 12 * 32, 0x8424);
    const Rom rom = m.ToRom();
    DriverInfo info;
    SEQLIB_REQUIRE(DetectGiven(rom, kSongTable, 3, info) && info.psg_freq_table == kFreqTable);

    const fs::path dir = TempPath("psg_tuning");
    std::error_code ec;
    fs::create_directories(dir, ec);
    ConvertSettings opt;
    opt.out_dir = Utf8(dir);
    const SongReport sum = ConvertSong(rom, info, 2, opt, nullptr);
    SEQLIB_CHECK(sum.ok);

    // The notes play on keys 36 and 48. The square's zones tune key 48 up by 24 cents, and key 36 not at all. The keys
    // below the table take note 0's tuning.
    std::vector<int> keys;
    for (const MidiEvent& e : ReadEvents(sum.midi_path))
    {
        if (e.track == "Square 1" && (e.status & 0xF0) == 0x90 && e.data2 > 0)
        {
            keys.push_back(e.data1);
        }
    }
    SEQLIB_CHECK((keys == std::vector<int>{36, 48}));

    const std::vector<std::array<int, 3>> zones = ReadZoneTuning(sum.sf2_path);
    auto cents_of = [&](int key)
    {
        for (const auto& [lo, hi, cents] : zones)
        {
            if (lo <= key && key <= hi)
            {
                return cents;
            }
        }

        return -9999;
    };
    SEQLIB_CHECK_EQ(cents_of(36), 0);
    SEQLIB_CHECK_EQ(cents_of(48), 24);
    SEQLIB_CHECK_EQ(cents_of(0), 0);

    fs::remove_all(dir, ec);
}

void TestSampleKeys()
{
    // Voice 0 plays samples 5 and 6 (the same data) at two pitches each, so each gets a zone that spans them. Voices 1
    // and 2 play sample 5 at a single pitch, 50 semitones up and 128 down, and each of those gets a key of its own. The
    // sample has no clear pitch, so it's laid out as if its own pitch were C4.
    Image m = SyntheticImage();
    m.Put32(kSampleTable + 4 * 6, kSampleHeader);
    constexpr uint32_t kBase = kRomBase + 0x600;
    m.Song(kSongTable, 2, kBase, 0x40, {{4, 0x00}, {5, 0x20}, {6, 0x30}});
    m.PutBytes(kBase, {0x00, 0xB7, 0x05, 0x00, 0x02, 0xB7, 0x05, 0x02, 0x02, 0xB7,
                       0x06, 0x0A, 0x02, 0xB7, 0x06, 0x0C, 0x02, 0xE0, 0x00, 0xFD});
    m.PutBytes(kBase + 0x20, {0x00, 0xB7, 0x05, 0x32, 0x02, 0xE0, 0x00, 0xFD});
    m.PutBytes(kBase + 0x30, {0x00, 0xB7, 0x05, 0x80, 0x02, 0xE0, 0x00, 0xFD});
    m.PutBytes(kBase + 0x40, {0x00, 0xFD});
    const Rom rom = m.ToRom();

    DriverInfo info;
    SEQLIB_CHECK(DetectGiven(rom, kSongTable, 3, info));

    const fs::path dir = TempPath("keys");
    std::error_code ec;
    fs::create_directories(dir, ec);
    ConvertSettings opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);

    const SongReport sum = ConvertSong(rom, info, 2, opt, nullptr);

    // Voice 0's zones start out on keys 60-62 and 70-72. The keys between them are split halfway, and the outer ends
    // grow by an octave. Voice 1's key is one whose root key, 50 below it, is a MIDI key too. No key has a root key 128
    // above it, so voice 2 gets the nearest: key 0, with root key 127.
    const std::vector<std::array<int, 3>> kExpected = {{48, 66, 60}, {67, 84, 60}, {50, 50, 0}, {0, 0, 127}};
    SEQLIB_CHECK(sum.ok);
    SEQLIB_CHECK(ReadZoneKeys(sum.sf2_path) == kExpected);
    SEQLIB_CHECK(HasGlobalZoneModulators(ReadSf2(sum.sf2_path)));
    fs::remove_all(dir, ec);
}

void TestLoopWithoutDelay()
{
    // Song 2 loops before a frame has passed, and song 3 after a note of 20 frames. Neither waits a frame after its
    // loop point, which would hang the driver. Each song stops there, with a warning that says why, however many times
    // the loop is to be played.
    Image m = SyntheticImage();
    constexpr uint32_t kBase = kRomBase + 0x600;
    m.Song(kSongTable, 2, kBase, 0x40, {{4, 0}});
    m.Song(kSongTable, 3, kBase, 0x40, {{4, 0x20}});
    m.PutBytes(kBase, {0x00, 0xA7, 0x05, 0x00, 0xFF, 0x01});
    m.PutBytes(kBase + 0x20, {0x00, 0xA7, 0x05, 0x14, 0xF3, 0x00, 0x00, 0x00, 0xFF, 0x01});
    m.PutBytes(kBase + 0x40, {0x00, 0xFD});
    const Rom rom = m.ToRom();

    DriverInfo info;
    SEQLIB_CHECK(DetectGiven(rom, kSongTable, 4, info));

    const fs::path dir = TempPath("hang");
    std::error_code ec;
    fs::create_directories(dir, ec);
    ConvertSettings opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);
    ConvertSettings looped = opt;
    looped.loops = 10;

    const SongReport at_start = ConvertSong(rom, info, 2, opt, nullptr);
    const SongReport at_start_looped = ConvertSong(rom, info, 2, looped, nullptr);
    const SongReport later = ConvertSong(rom, info, 3, looped, nullptr);

    auto hangs = [](const SongReport& s)
    {
        return s.warnings.size() == 1 && s.warnings[0].find("song loops without waiting a frame") != std::string::npos;
    };

    // Song 2 plays nothing, so it's skipped. Song 3 plays its note for 20 frames and stops on the next, without a loop.
    SEQLIB_CHECK(at_start.ok && at_start.silent && hangs(at_start));
    SEQLIB_CHECK(at_start_looped.ok && at_start_looped.silent && hangs(at_start_looped));
    SEQLIB_CHECK(later.ok && !later.silent && hangs(later));
    SEQLIB_CHECK(later.seconds == FrameSeconds(21));
    SEQLIB_CHECK(later.loop_end == -1.0);
    fs::remove_all(dir, ec);
}

void TestSilentEndAfterLastSound()
{
    // Song 2's sample track plays a note for 10 frames, rests for 500 and stops the song. The stop comes about 8.4 s
    // after the note falls silent, and the song ends there. Song 3 rests for 700 frames, about 11.7 s, more than
    // kMaxSilentEndSeconds. It then ends two frames after the note's end, and its MIDI file holds nothing later.
    Image m = SyntheticImage();
    constexpr uint32_t kBase = kRomBase + 0x600;
    m.Song(kSongTable, 2, kBase, 0x40, {{4, 0}});
    m.Song(kSongTable, 3, kBase, 0x40, {{4, 0x20}});
    m.PutBytes(kBase, {0x00, 0xA8, 0x10, 0x05, 0x0A, 0xE0, 0xE1, 0xF4, 0xFF, 0x00});
    m.PutBytes(kBase + 0x20, {0x00, 0xA8, 0x10, 0x05, 0x0A, 0xE0, 0xE2, 0xBC, 0xFF, 0x00});
    m.PutBytes(kBase + 0x40, {0x00, 0xFD});
    const Rom rom = m.ToRom();

    DriverInfo info;
    SEQLIB_CHECK(DetectGiven(rom, kSongTable, 4, info));

    const fs::path dir = TempPath("silent_end");
    std::error_code ec;
    fs::create_directories(dir, ec);
    ConvertSettings opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);

    const SongReport near = ConvertSong(rom, info, 2, opt, nullptr);
    const SongReport far = ConvertSong(rom, info, 3, opt, nullptr);
    SEQLIB_CHECK(near.ok && near.seconds > FrameSeconds(510) && near.seconds < FrameSeconds(513));
    SEQLIB_CHECK(far.ok && far.seconds == FrameSeconds(12));
    SEQLIB_CHECK(far.loop_end == -1.0);

    uint32_t last = 0;
    int ons = 0;
    for (const MidiEvent& e : ReadEvents(far.midi_path))
    {
        last = std::max(last, e.frame);
        ons += (e.status & 0xF0) == 0x90 && e.data2 > 0 ? 1 : 0;
    }
    SEQLIB_CHECK_EQ(ons, 1);
    SEQLIB_CHECK(last <= 12);
    fs::remove_all(dir, ec);
}

void TestFrameLimit()
{
    // Song 2 plays a note and loops every 2048 frames, and song 3 plays a note and waits 65535 frames twice before it
    // ends. Neither finishes within the frame limit, and song 2 only gets to play its loop 52 of the 100 times asked
    // for.
    Image m = SyntheticImage();
    constexpr uint32_t kBase = kRomBase + 0x600;
    m.Song(kSongTable, 2, kBase, 0x20, {{4, 0}});
    m.Song(kSongTable, 3, kBase, 0x20, {{4, 0x10}});
    m.PutBytes(kBase, {0x00, 0xA7, 0x05, 0xF0, 0x00, 0x08, 0xFF, 0x01});
    m.PutBytes(kBase + 0x10, {0x00, 0xA7, 0x05, 0xF0, 0xFF, 0xFF, 0xE0, 0xF0, 0xFF, 0xFF, 0xFE});
    m.PutBytes(kBase + 0x20, {0x00, 0xFD});
    const Rom rom = m.ToRom();

    DriverInfo info;
    SEQLIB_CHECK(DetectGiven(rom, kSongTable, 4, info));

    const fs::path dir = TempPath("limit");
    std::error_code ec;
    fs::create_directories(dir, ec);
    ConvertSettings opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);
    opt.loops = 100;

    const SongReport looped = ConvertSong(rom, info, 2, opt, nullptr);
    const SongReport long_song = ConvertSong(rom, info, 3, opt, nullptr);

    // Both are cut off at the limit, and the warnings say how far each got.
    SEQLIB_CHECK(looped.ok && looped.seconds == FrameSeconds(kMaxFrames));
    SEQLIB_CHECK(looped.warnings ==
                 std::vector<std::string>{
                     "song reached the frame limit after playing its loop 52 of 100 times; output truncated"});
    SEQLIB_CHECK(long_song.ok && long_song.seconds == FrameSeconds(kMaxFrames));
    SEQLIB_CHECK(long_song.warnings ==
                 std::vector<std::string>{"song didn't end or loop within the frame limit; output truncated"});
    fs::remove_all(dir, ec);
}

void TestTrackPastEnd()
{
    // Track 0 plays a note and waits 2 frames, and that delay is the ROM's last byte. The other tracks end at once.
    Image m(0x12A);
    constexpr uint32_t kTable = kRomBase + 0x100;
    m.Song(kTable, 0, kTable + kSongEntrySize, 0, {{0, 2}});
    m.PutBytes(kTable + kSongEntrySize, {0x00, 0xFD, 0x00, 0xD7, 0x30, 0x02});
    const Rom rom = m.ToRom();
    UltimateMastersSequencer seq(rom, SongsAt(kTable), 0);

    for (int f = 0; f < 4; f++)
    {
        seq.Step();
    }

    // The track stops where its data runs out, and the warning says so.
    const std::vector<std::string>& warnings = seq.Warnings();
    SEQLIB_CHECK(warnings.size() == 1 && warnings[0].find("runs past the end of the ROM") != std::string::npos);
}

void TestManyPresets()
{
    // A song that needs 149 presets. The wave track plays 9 waves at 15 volumes, which makes 135 wave presets, and
    // every other track plays a note, so the song uses all 16 MIDI channels, channel 10 included.
    Image m = SyntheticImage();
    m.Resize(0x2000);
    constexpr uint32_t kBase = kRomBase + 0x1000;
    std::vector<uint8_t> waves = {0x00};
    for (int w = 0; w < 9; w++)
    {
        waves.insert(waves.end(), {uint8_t(0x04 + w), 0x00}); // wave w
        for (int v = 1; v <= 15; v++)
        {
            waves.insert(waves.end(), {0xD8, uint8_t(v), 0x18, 0x01}); // note 24 at volume v, then wait a frame
        }
    }
    waves.insert(waves.end(), {0xE0, 0x00, 0xFD});
    m.PutBytes(kBase, waves);
    m.PutBytes(kBase + 0x300, {0x00, 0x81, 0x00, 0xD3, 0x18, 0x04, 0xE0, 0x00, 0xFD}); // the squares
    m.PutBytes(kBase + 0x310, {0x00, 0xD3, 0x05, 0x04, 0xE0, 0x00, 0xFD});             // noise
    m.PutBytes(kBase + 0x320, {0x00, 0xA8, 0x10, 0x05, 0x0A, 0xE0, 0x05, 0xFD});       // every sample voice
    m.Song(kSongTable, 3, kBase, 0x320, {{0, 0x300}, {1, 0x300}, {2, 0x000}, {3, 0x310}});
    const Rom rom = m.ToRom();

    DriverInfo info;
    SEQLIB_CHECK(DetectGiven(rom, kSongTable, 4, info));

    const fs::path dir = TempPath("presets");
    std::error_code ec;
    fs::create_directories(dir, ec);
    ConvertSettings opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);

    // In a SoundFont of its own, the presets go on into bank 1. Only the preset of the track on channel 10, the last
    // one, gets a copy in the drum bank, so no two presets have the same bank and program.
    const SongReport own = ConvertSong(rom, info, 3, opt, nullptr);

    const std::vector<PresetHeader> presets = ReadPresets(own.sf2_path);
    std::set<std::pair<int, int>> ids;
    for (const PresetHeader& p : presets)
    {
        ids.insert({p.bank, p.program});
    }

    SEQLIB_CHECK(own.ok);
    SEQLIB_REQUIRE_EQ(presets.size(), 150);
    SEQLIB_REQUIRE_EQ(ids.size(), presets.size());
    SEQLIB_CHECK(presets[128].bank == 1 && presets[128].program == 0);
    SEQLIB_CHECK(presets.back().bank == 128 && presets.back().name == "Voice 11");

    // In a shared SoundFont, the song's bank takes the first 128 presets, and the other 21 the free programs after song
    // 0's in bank 0. Banks 1 and 2 stay free for songs 1 and 2.
    SoundfontBuilder shared(rom, info, 4);
    SEQLIB_CHECK(ConvertSong(rom, info, 0, opt, &shared).ok);
    const int song0 = int(shared.File().presets.size());

    const SongReport in_shared = ConvertSong(rom, info, 3, opt, &shared);

    const std::vector<sf2::Preset>& shared_presets = shared.File().presets;
    std::set<std::pair<int, int>> shared_ids;
    for (const sf2::Preset& p : shared_presets)
    {
        shared_ids.insert({p.bank, p.program});
    }
    SEQLIB_CHECK(in_shared.ok);
    SEQLIB_REQUIRE_EQ(shared_presets.size(), size_t(song0) + 149);
    SEQLIB_REQUIRE_EQ(shared_ids.size(), shared_presets.size());
    const sf2::Preset& first = shared_presets[size_t(song0)];
    const sf2::Preset& spilled = shared_presets[size_t(song0) + 128];
    SEQLIB_CHECK(first.bank == 3 && first.program == 0);
    SEQLIB_CHECK(spilled.bank == 0 && spilled.program == song0);
    fs::remove_all(dir, ec);
}

int RunTests()
{
    TestConversion();
    TestSfzFiles();
    TestSilentSongs();
    TestSharedSoundfont();
    TestEchoRouting();
    TestEchoFromVoice5();
    TestLoopSettingsAgain();
    TestLoopStartsAtSecondPass();
    TestNoiseKeys();
    TestPsgPastTable();
    TestPsgTuning();
    TestSampleKeys();
    TestLoopWithoutDelay();
    TestSilentEndAfterLastSound();
    TestFrameLimit();
    TestTrackPastEnd();
    TestManyPresets();

    return seqlib::testing::Summary("konami_conversion");
}

} // namespace
} // namespace ntdlib::sequence_export::konami

int main()
{
    return ntdlib::sequence_export::konami::RunTests();
}
