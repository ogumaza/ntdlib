// SPDX-License-Identifier: MIT

// Unit tests for what the GBA's conversions share: safe file names, the SoundFont writer, the SFZ files and the banks
// of a song's programs.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "seqlib/testing/check.h"
#include "seqlib/wav.h"

#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/agb/sf2.h"
#include "ntdlib/sequence_export/agb/sfz.h"
#include "ntdlib/sequence_export/agb/song_banks.h"
#include "ntdlib/testing/agb_cart.h"
#include "ntdlib/testing/agb_files.h"

namespace fs = std::filesystem;

namespace ntdlib::sequence_export::agb
{
using ntdlib::agb::PathFromUtf8;
using ntdlib::agb::Utf8;
using ntdlib::testing::agb_files::Le16;
using ntdlib::testing::agb_files::Le32;
using ntdlib::testing::agb_files::MakeTempFolder;
using ntdlib::testing::agb_files::ReadAll;
using ntdlib::testing::agb_files::ReadSf2;
using ntdlib::testing::agb_files::Sf2Records;
using ntdlib::testing::agb_files::TempPath;

namespace
{

void TestFileNames()
{
    SEQLIB_CHECK(SafeFileName("game") == "game");
    SEQLIB_CHECK(SafeFileName("\xE9\x9F\xB3\xE6\xA5\xBD caf\xC3\xA9") == "\xE9\x9F\xB3\xE6\xA5\xBD caf\xC3\xA9");
    SEQLIB_CHECK(SafeFileName("<>:\"/\\|?*\x01") == "__________");
    SEQLIB_CHECK(SafeFileName("game. . ") == "game");
    SEQLIB_CHECK(SafeFileName("..") == "output");
    SEQLIB_CHECK(SafeFileName("") == "output");
    SEQLIB_CHECK(SafeFileName("con") == "_con");
    SEQLIB_CHECK(SafeFileName("NuL.backup") == "_NuL.backup");
    SEQLIB_CHECK(SafeFileName("COM1") == "_COM1");
    SEQLIB_CHECK(SafeFileName("lpt9") == "_lpt9");
    SEQLIB_CHECK(SafeFileName("COM\xC2\xB9") == "_COM\xC2\xB9");
    SEQLIB_CHECK(SafeFileName("console") == "console");
    SEQLIB_CHECK(SafeFileName("COM10") == "COM10");
}

void TestSf2Writer()
{
    Sf2File sf;
    sf.name = "Test";

    sf2::Sample s;
    s.name = "Saw";
    for (int i = 0; i < 100; i++)
    {
        s.data.push_back(int16_t(i * 100));
    }
    s.sample_rate = 16000;
    s.loop_start = 10;
    s.loop_end = 90;
    sf.samples.push_back(s);

    // A global zone with a modulator, then a zone whose generators are out of order on purpose.
    sf2::Instrument inst;
    inst.name = "Inst";
    sf2::Zone global;
    global.modulators.push_back(
        {sf2::kNoteOnVelocity | sf2::kNegative | sf2::kConcave, sf2::kInitialAttenuation, 480, 0, 0});
    inst.zones.push_back(global);
    sf2::Zone z;
    z.generators = {sf2::Generator::Value(sf2::kSampleId, 0), sf2::Generator::Range(sf2::kKeyRange, 0, 127)};
    inst.zones.push_back(z);
    sf.instruments.push_back(inst);
    sf.presets.push_back(MakePreset("Preset", 0, 5, 0));

    const std::string path = Utf8(TempPath("test.sf2"));
    std::string err;

    SEQLIB_CHECK(sf.Write(path, err));

    const std::vector<uint8_t> f = ReadAll(path);
    SEQLIB_CHECK(f.size() > 12 && std::memcmp(f.data(), "RIFF", 4) == 0 && std::memcmp(&f[8], "sfbk", 4) == 0);
    SEQLIB_CHECK_EQ(Le32(&f[4]) + 8, f.size());

    const Sf2Records r = ReadSf2(path);
    SEQLIB_CHECK_EQ(r.chunks.at("smpl").size(), (100 + 46) * 2);
    SEQLIB_CHECK_EQ(r.Count("shdr", 46), 2);
    const uint8_t* h = r.Record("shdr", 46, 0);
    SEQLIB_CHECK(std::string(reinterpret_cast<const char*>(h)) == "Saw");
    SEQLIB_CHECK_EQ(Le32(h + 20), 0);
    SEQLIB_CHECK_EQ(Le32(h + 24), 100);
    SEQLIB_CHECK_EQ(Le32(h + 28), 10);
    SEQLIB_CHECK_EQ(Le32(h + 32), 90);
    SEQLIB_CHECK_EQ(Le32(h + 36), 16000);

    // The global zone's bag points at its modulator and no generators, the zone's bag at its two generators after the
    // modulator, and the zone's keyRange comes first.
    SEQLIB_CHECK_EQ(r.Count("imod", 10), 2);
    SEQLIB_CHECK_EQ(Le16(r.Record("imod", 10, 0)), 0x0502);
    SEQLIB_CHECK_EQ(Le16(r.Record("imod", 10, 0) + 2), sf2::kInitialAttenuation);
    SEQLIB_CHECK_EQ(Le16(r.Record("imod", 10, 0) + 4), 480);
    SEQLIB_CHECK_EQ(r.Count("ibag", 4), 3);
    SEQLIB_CHECK_EQ(Le16(r.Record("ibag", 4, 0)), 0);
    SEQLIB_CHECK_EQ(Le16(r.Record("ibag", 4, 0) + 2), 0);
    SEQLIB_CHECK_EQ(Le16(r.Record("ibag", 4, 1)), 0);
    SEQLIB_CHECK_EQ(Le16(r.Record("ibag", 4, 1) + 2), 1);
    SEQLIB_CHECK_EQ(Le16(r.Record("ibag", 4, 2)), 2);
    SEQLIB_CHECK_EQ(Le16(r.Record("igen", 4, 0)), sf2::kKeyRange);
    SEQLIB_CHECK_EQ(Le16(r.Record("igen", 4, 1)), sf2::kSampleId);

    // The converting program's name goes in the comment and in ISFT.
    Sf2File named = sf;
    named.program = "xyz2midi";
    named.SetTitle("Game", " #01");
    const std::string named_path = Utf8(TempPath("named.sf2"));
    SEQLIB_CHECK(named.Write(named_path, err));
    const Sf2Records n = ReadSf2(named_path);
    const auto text = [&](const char* chunk)
    {
        const auto it = n.chunks.find(chunk);
        return it == n.chunks.end() ? std::string() : std::string(reinterpret_cast<const char*>(it->second.data()));
    };
    SEQLIB_CHECK(text("INAM") == "Game");
    SEQLIB_CHECK(text("ISFT") == "xyz2midi");
    SEQLIB_CHECK(text("ICMT") == "Instruments for Game #01, extracted by xyz2midi");

    fs::remove(PathFromUtf8(path));
    fs::remove(PathFromUtf8(named_path));
}

// Whether `text` contains `line` as one of its lines.
bool HasLine(const std::string& text, const std::string& line)
{
    return ("\n" + text).find("\n" + line + "\n") != std::string::npos;
}

// The SoundFont of the SFZ tests: a looped square cycle and a sample without a loop. Preset 0 plays the square on keys
// 0 to 59, a semitone and 25 cents up, and on keys 60 to 127 with key 72 as its root. Preset 1 of bank 1 plays the
// sample with its loop mode on, and preset 2 of bank 128 is the drum bank's copy of preset 0.
Sf2File SfzSoundFont()
{
    Sf2File sf;
    sf2::Sample square;
    square.name = "Square 50%";
    square.sample_rate = 8000;
    square.original_key = 60;
    for (int i = 0; i < 24; i++)
    {
        square.data.push_back(int16_t(i % 8 < 4 ? 8192 : -8192));
    }
    square.loop_start = 8;
    square.loop_end = 16;
    sf2::Sample sample;
    sample.name = "Sample 003";
    sample.sample_rate = 13379;
    sample.original_key = 57;
    sample.data = {0, 100, 200, 300, 400};
    sf.samples = {square, sample};

    sf.instruments.push_back(
        MakeInstrument("Square", {SampleZone(0, true, 0, 59, 125), RootKeyZone(0, true, 60, 127, 72)}));
    sf.instruments.push_back(MakeInstrument("Sample", {SampleZone(1, true)}));
    sf.presets = {MakePreset("Square", 0, 0, 0), MakePreset("Sample", 1, 5, 1), MakePreset("Square", kDrumBank, 0, 0)};

    return sf;
}

void TestTrackSfz()
{
    // The values follow the opcodes of sfzformat.com: a region's keys, its root key (pitch_keycenter), its tuning in
    // semitones (transpose) and cents (tune), and a loop from loop_start to loop_end, the loop's last sample.
    const Sf2File sf = SfzSoundFont();
    const std::vector<SfzTrack> tracks = {{"Square 1", 3, {0}}, {"Voice 0", 2, {1, 0, 1}}, {"Wave", 2, {}}};

    const seqlib::sfz::Instruments sfz = TrackSfz(sf, tracks, "Song 01", "test");

    SEQLIB_CHECK_EQ(sfz.files.size(), size_t(2));
    SEQLIB_CHECK(sfz.files[0].name == "Square 1.sfz" && sfz.files[1].name == "Voice 0.sfz");
    const std::string& square = sfz.files[0].text;
    SEQLIB_CHECK(square.rfind("// Square 1 of Song 01, converted by test.", 0) == 0);
    SEQLIB_CHECK(HasLine(square, "default_path=../samples/"));
    SEQLIB_CHECK(HasLine(square, "bend_up=300 bend_down=-300"));
    SEQLIB_CHECK(HasLine(square, "<master> loprog=0 hiprog=0 locc0=0 hicc0=0"));
    SEQLIB_CHECK(HasLine(square,
                         "<region> sample=Square_50.wav lokey=0 hikey=59 pitch_keycenter=60 sample_quality=0 "
                         "transpose=1 tune=25 ampeg_release=0.000977 loop_mode=loop_continuous loop_start=8 "
                         "loop_end=15"));
    SEQLIB_CHECK(HasLine(square,
                         "<region> sample=Square_50.wav lokey=60 hikey=127 pitch_keycenter=72 "
                         "sample_quality=0 ampeg_release=0.000977 loop_mode=loop_continuous loop_start=8 "
                         "loop_end=15"));
    const std::string& voice = sfz.files[1].text;
    SEQLIB_CHECK(voice.find("<master> loprog=0 hiprog=0 locc0=0 hicc0=0") <
                 voice.find("<master> loprog=5 hiprog=5 locc0=1 hicc0=1"));
    SEQLIB_CHECK(HasLine(voice,
                         "<region> sample=Sample_003.wav lokey=0 hikey=127 pitch_keycenter=57 "
                         "sample_quality=0 ampeg_release=0.000977 loop_mode=loop_continuous loop_start=0 "
                         "loop_end=4"));

    // Each sample's WAV file contains its points at its rate, once.
    SEQLIB_CHECK_EQ(sfz.samples.size(), size_t(2));
    SEQLIB_CHECK(sfz.samples[0].name == "Square_50.wav" &&
                 sfz.samples[0].wav == seqlib::WavFile(sf.samples[0].data, 8000));
    SEQLIB_CHECK(sfz.samples[1].name == "Sample_003.wav" &&
                 sfz.samples[1].wav == seqlib::WavFile(sf.samples[1].data, 13379));
}

void TestTrackSfzRefusals()
{
    // A SoundFont of one bank selects no bank. A zone that plays once has no loop.
    Sf2File sf = SfzSoundFont();
    sf.presets.resize(1);
    sf.instruments[0].zones[2] = RootKeyZone(0, false, 60, 127, 72);
    const std::vector<SfzTrack> tracks = {{"Square 1", 2, {0}}};

    const std::string text = TrackSfz(sf, tracks, "Song 01", "test").files.at(0).text;

    SEQLIB_CHECK(HasLine(text, "<master> loprog=0 hiprog=0"));
    SEQLIB_CHECK(HasLine(text,
                         "<region> sample=Square_50.wav lokey=60 hikey=127 pitch_keycenter=72 "
                         "sample_quality=0 ampeg_release=0.000977 loop_mode=no_loop"));

    // A preset past the SoundFont's, and a zone that contains an envelope, are refused.
    const std::vector<SfzTrack> past = {{"Square 1", 2, {1}}};
    bool refused = false;
    try
    {
        TrackSfz(sf, past, "Song 01", "test");
    }
    catch (const std::invalid_argument&)
    {
        refused = true;
    }
    SEQLIB_CHECK(refused);

    AddVolumeEnvelope(sf.instruments[0].zones[1], {.release = 0});
    refused = false;
    try
    {
        TrackSfz(sf, tracks, "Song 01", "test");
    }
    catch (const std::invalid_argument&)
    {
        refused = true;
    }
    SEQLIB_CHECK(refused);
}

void TestPresetSfz()
{
    // Each preset's file name contains its number: bank 1's preset 5 as 133, padded to the largest's digits.
    Sf2File sf = SfzSoundFont();
    sf.presets.resize(2);

    const seqlib::sfz::Instruments sfz = PresetSfz(sf, "Game: Samples", "test");

    SEQLIB_CHECK_EQ(sfz.files.size(), size_t(2));
    SEQLIB_CHECK(sfz.files[0].name == "Game_ Samples_000.sfz" && sfz.files[1].name == "Game_ Samples_133.sfz");
    SEQLIB_CHECK(sfz.files[1].text.rfind("// Sample of Game: Samples, converted by test.", 0) == 0);
    SEQLIB_CHECK(HasLine(sfz.files[1].text, "bend_up=200 bend_down=-200"));
    SEQLIB_CHECK(sfz.files[1].text.find("<master>") == std::string::npos);
    SEQLIB_CHECK(HasLine(sfz.files[1].text,
                         "<region> sample=Sample_003.wav lokey=0 hikey=127 pitch_keycenter=57 "
                         "sample_quality=0 ampeg_release=0.000977 loop_mode=loop_continuous "
                         "loop_start=0 loop_end=4"));
    SEQLIB_CHECK_EQ(sfz.samples.size(), size_t(2));
}

void TestSongBanks()
{
    // Three songs share a SoundFont. Song 0 takes bank 0, and song 2's 130 presets take bank 2 and then the free
    // programs after song 0's in bank 0. Bank 1 stays free until song 1 takes it.
    SongBanks banks(3);

    const std::vector<BankProgram> song0 = banks.Place(0, 3);
    const std::vector<BankProgram> song2 = banks.Place(2, 130);
    const std::vector<BankProgram> song1 = banks.Place(1, 2);

    SEQLIB_CHECK(song0.size() == 3 && song0[2].bank == 0 && song0[2].program == 2);
    SEQLIB_CHECK(song2.size() == 130);
    if (song2.size() == 130)
    {
        SEQLIB_CHECK(song2[127].bank == 2 && song2[127].program == 127);
        SEQLIB_CHECK(song2[128].bank == 0 && song2[128].program == 3 && song2[129].program == 4);
    }
    SEQLIB_CHECK(song1.size() == 2 && song1[0].bank == 1 && song1[0].program == 0);

    // A SoundFont of one song's goes on from bank 0 into bank 1.
    SongBanks own(1);

    const std::vector<BankProgram> one = own.Place(0, 130);

    SEQLIB_CHECK(one.size() == 130 && one[128].bank == 1 && one[128].program == 0);

    // With one free program left in each of the 128 banks, song 128 finds no room for 129 presets and takes nothing,
    // and then finds room for 128.
    SongBanks full(128);
    for (int s = 0; s < 128; s++)
    {
        full.Place(s, 127);
    }

    const bool too_many = full.Place(128, 129).empty();
    const std::vector<BankProgram> fits = full.Place(128, 128);

    SEQLIB_CHECK(too_many);
    SEQLIB_CHECK(fits.size() == 128 && fits[0].bank == 0 && fits[0].program == 127);
}

// Runs every test. Returns the exit code.
int RunTests()
{
    if (!ntdlib::testing::agb_files::MakeTempFolder())
    {
        std::fprintf(stderr, "can't create a temporary folder for the test output\n");
        return 1;
    }

    TestFileNames();
    TestSf2Writer();
    TestTrackSfz();
    TestTrackSfzRefusals();
    TestPresetSfz();
    TestSongBanks();

    std::error_code ec;
    std::filesystem::remove_all(ntdlib::testing::agb_files::g_temp, ec);
    return seqlib::testing::Summary("sequence_export_agb");
}

} // namespace
} // namespace ntdlib::sequence_export::agb

int main()
{
    return ntdlib::sequence_export::agb::RunTests();
}
