// SPDX-License-Identifier: MIT

// Tests of the files of the DS's conversion (ntdlib/sequence_export/ntr): the MIDI, SoundFont and SFZ files of a
// sequence and of a bank, the tuning in the files, the frames of a performance, and the conversion of a cartridge
// image that testing/nds_files.h builds.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "seqlib/midi/reader.h"
#include "seqlib/sf2/reader.h"
#include "seqlib/sf2/soundfont.h"
#include "seqlib/testing/check.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sequence_export/frame.h"
#include "ntdlib/sequence_export/ntr/archive_input.h"
#include "ntdlib/sequence_export/ntr/convert.h"
#include "ntdlib/sequence_export/ntr/instruments.h"
#include "ntdlib/sequence_export/ntr/performance.h"
#include "ntdlib/testing/dse_song_files.h"
#include "ntdlib/testing/nds_files.h"
#include "ntdlib/testing/sdat_files.h"
#include "ntdlib/testing/sequence_export_check.h"
#include "ntdlib/testing/universomo_files.h"
#include "sequence_export_ntr_cases.h"
#include "temp_folder.h"

namespace
{

using namespace sequence_export_ntr_cases;
using seqlib::testing::Throws;

namespace dse_song_files = ntdlib::testing::dse_song_files;
namespace fs = std::filesystem;
namespace nds_files = ntdlib::testing::nds_files;
namespace universomo_files = ntdlib::testing::universomo_files;

// The name that the conversions' files give the program.
constexpr char kProgram[] = "xyz2midi";

// A conversion's files pass seqlib's checks, and each tick's frame starts no earlier than the tick, within a frame of
// it: half a tick at a tempo of 120. The driver runs a tick at the start of the frame after the tick's time.
// sequence_export's other consoles run it before (ntdlib::testing::CheckConversion).
void CheckFiles(const Conversion& c, const std::string& name)
{
    const std::vector<std::string> midi = seqlib::midi::Problems(c.midi);
    const std::vector<std::string> sf2 = seqlib::sf2::Check(c.sf2);
    const std::string sfz =
        midi.empty() ? ntdlib::testing::SfzProblem(c.sfz, seqlib::midi::TrackNames(seqlib::midi::Read(c.midi))) : "";
    Check(midi.empty() && sf2.empty() && sfz.empty(),
          name + ": seqlib's checks: " + (midi.empty() ? "" : midi[0]) + (sf2.empty() ? "" : sf2[0]) + sfz);

    const std::vector<double>& frames = c.performance.frame_ticks;
    bool ordered = !frames.empty();
    for (std::size_t k = 0; k < frames.size(); k++)
    {
        ordered = ordered && frames[k] > static_cast<double>(k) - 1e-9 &&
                  frames[k] < static_cast<double>(k) + 0.5 + 1e-9 && (k == 0 || frames[k] >= frames[k - 1]);
    }
    Check(ordered, name + ": each tick's frame");
}

// The conversion of a sequence that plays every kind of instrument, with each kind of LFO and a sweep. Program 7's
// region plays full left, 3 dB above the centre. Every SoundFont's zones are 3 dB quieter, whatever their pans.
void TestConvert()
{
    // clang-format off
    const Bytes every = {
        0x81, 0x00, 0x3C, 0x7F, 0x18, 0x80, 0x18,                               // wave 0
        0x81, 0x01, 0x45, 0x7F, 0x18, 0x80, 0x18,                               // the PSG
        0x81, 0x02, 0x3C, 0x7F, 0x18, 0x80, 0x18,                               // the noise
        0x81, 0x03, 0x24, 0x7F, 0x0C, 0x25, 0x7F, 0x0C, 0x26, 0x7F, 0x0C, 0x80, 0x18, // the drum set
        0x81, 0x04, 0x30, 0x7F, 0x0C, 0x80, 0x0C, 0x48, 0x7F, 0x0C, 0x80, 0x0C, // the key split
        0x81, 0x07, 0xD3, 0x7F, 0xC0, 0x20, 0xCA, 0x40, 0xCB, 0x10, 0xCD, 0x02, // release 127, pan 32, a pitch LFO
        0x3C, 0x7F, 0x30, 0x80, 0x30,
        0xCC, 0x01, 0xCA, 0x7F, 0x3C, 0x7F, 0x30, 0x80, 0x30,                   // a volume LFO
        0xCC, 0x02, 0x3C, 0x7F, 0x30, 0x80, 0x30,                               // a pan LFO
        0xCA, 0x00, 0xE3, 0x40, 0x00, 0x3C, 0x7F, 0x30, 0x80, 0x30,             // a sweep
        0xFF,
    };
    // clang-format on
    const TestArchive test(every);
    const Conversion c = ConvertSequence(test.archive, {0}, "BGM_TEST", {}, kProgram);
    CheckFiles(c, "every instrument");

    // CC11's level is (v / 127)^2. It takes the gain of the track's pan: 127 * sqrt(PanGain(64)) = 106.8 at the
    // centre, and 127 * sqrt(PanGain(32)) = 112.9 at 32. CC10 at 32 is 64 + 64 * PanPosition(32) = 26.2. CC1 holds the
    // LFO's depth, but at 0 while the pan LFO's note sounds alone on its part, from tick 444. Wave 1 plays once, and
    // the note ends in its first tick.
    std::vector<std::pair<uint32_t, int>> controls;
    for (const seqlib::midi::TrackEvent& e : c.performance.tracks[0])
    {
        if (e.kind == EventKind::kExpression || e.kind == EventKind::kPan || e.kind == EventKind::kModulation)
        {
            const int value = e.kind == EventKind::kPan          ? 1000 + e.value
                              : e.kind == EventKind::kModulation ? 2000 + e.value
                                                                 : e.value;
            controls.emplace_back(e.tick, value);
        }
    }
    Check(controls == std::vector<std::pair<uint32_t, int>>{{0, 107},
                                                            {0, 1064},
                                                            {0, 2000},
                                                            {252, 113},
                                                            {252, 1026},
                                                            {252, 2064},
                                                            {348, 2127},
                                                            {444, 2000},
                                                            {445, 2127},
                                                            {540, 2000}},
          "CC11 holds the pan's gain, CC10 the pan, and CC1 the LFO's depth");

    // The LFO at speed 16 steps through 512 / 16 frames a cycle: 5.998625 Hz. At CC1's 127 and range 2, the pitch
    // swings by 127 * 127 * 2 / 16384 semitones, and the volume by 60 times that in 0.1 dB. SoundFont's CC1 gives
    // 127/128 of a modulator's amount. Program 7's region plays full left, 3.01 dB above the centre.
    const std::string& text = c.sfz.files.empty() ? std::string() : c.sfz.files[0].text;
    Check(text.find("lfo1_freq=5.998625 lfo1_pitch_oncc1=196.887207") != std::string::npos &&
              text.find("lfo1_volume_oncc1=11.813232") != std::string::npos &&
              text.find("lfo1_pan_oncc1=196.887207") != std::string::npos,
          "SFZ's LFOs");
    Check(text.find("<region> sample=w0_1.wav lokey=0 hikey=") != std::string::npos &&
              text.find(" pitch_keycenter=60 sample_quality=0 volume=3.01 pan=-100 ") != std::string::npos &&
              text.find(" pitch_keycenter=60 tune=24 sample_quality=0 volume=3.01 pan=-100 ") != std::string::npos,
          "SFZ's regions full left, with the timer's tunings");
    const seqlib::sf2::Records records = seqlib::sf2::Read(c.sf2);
    bool pitch = false;
    bool volume = false;
    for (std::size_t i = 0; i < records.Count("imod", 10); i++)
    {
        const uint8_t* r = records.Record("imod", 10, i);
        const auto field = [r](int at)
        {
            return xsflib::LoadLe16(r + at);
        };
        pitch = pitch || (field(0) == 0x0081 && field(2) == 6 && field(4) == 198);
        volume = volume || (field(0) == 0x0081 && field(2) == 13 && field(4) == 119);
    }
    Check(pitch && volume, "SoundFont's modulators from CC1: 198.4 cents and 119.1 cB");

    // Program 7's zones pan full left (-500). The other zones stay at the centre.
    std::size_t left = 0;
    std::size_t panned = 0;
    for (std::size_t z = 0; z + 1 < records.Count("ibag", 4); z++)
    {
        const std::map<uint16_t, uint16_t> generators = records.ZoneGens(z);
        if (const auto pan = generators.find(seqlib::sf2::kPan); pan != generators.end() && pan->second != 0)
        {
            panned++;
            left += static_cast<int16_t>(pan->second) == -500 ? 1 : 0;
        }
    }
    Check(panned >= 3 && panned % 3 == 0 && left == panned,
          "SoundFont's zones of program 7's three presets pan full left");

    // Wave 0, wave 1, the PSG at duties 3, 1 and 0, and the noise.
    Check(c.samples == 6 && c.sfz.samples.size() == 6, "a sample for each wave, and a WAV file for each");
    Check(c.warnings.empty(), "no warnings");
    Check(c.performance.approximations.empty(), "no approximations: CC10 holds the pan LFO");

    // Every zone of the SoundFont takes 30.1 cB, the gain of a side against the centre: 75 cB in the generator, at 2.5
    // times. Program 7's zones pan full left, and play at full level.
    std::set<uint16_t> attenuations;
    for (std::size_t z = 0; z + 1 < records.Count("ibag", 4); z++)
    {
        const std::map<uint16_t, uint16_t> generators = records.ZoneGens(z);
        if (generators.contains(seqlib::sf2::kSampleId))
        {
            const auto found = generators.find(seqlib::sf2::kInitialAttenuation);
            const auto pan = generators.find(seqlib::sf2::kPan);
            const bool full_left = pan != generators.end() && static_cast<int16_t>(pan->second) == -500;
            attenuations.insert(
                static_cast<uint16_t>((found != generators.end() ? found->second : 0) + (full_left ? 1000 : 0)));
        }
    }
    Check(attenuations == std::set<uint16_t>{75, 1000}, "every zone takes the cut of a zone full left");

    // A sequence that the archive gives a volume of 0 plays silent in the game. The report says why its files are
    // silent.
    const TestArchive muted(kOneNote, 0, 0);
    Check(ConvertSequence(muted.archive, {0}, "BGM_TEST", {}, kProgram).warnings ==
              std::vector<std::string>{
                  "volume 0 in the sound archive. The game plays it silent, and so does the conversion."},
          "a sequence of volume 0: one line in the report");

    const TestArchive loop(kLoop);
    CheckFiles(ConvertSequence(loop.archive, {0}, "BGM_LOOP", {.loops = 2}, kProgram), "a loop");
    const TestArchive one_note(kOneNote);
    CheckFiles(ConvertSequence(one_note.archive, {0}, "BGM_TEST", {}, kProgram), "one note");

    // Program 7 plays wave 1 from root key 60, and the sequence plays only key 84. In the SoundFont, key 84 takes its
    // timer's 3 cents, and the other keys join its run: one zone in place of a zone for each run of keys.
    const TestArchive key_84({0x81, 0x07, 0x54, 0x7F, 0x30, 0x80, 0x30, 0xFF});
    const seqlib::sf2::Records played =
        seqlib::sf2::Read(ConvertSequence(key_84.archive, {0}, "BGM_TEST", {}, kProgram).sf2);
    std::vector<std::tuple<int, int, int>> tunings;
    for (std::size_t z = 0; z + 1 < played.Count("ibag", 4); z++)
    {
        const std::map<uint16_t, uint16_t> generators = played.ZoneGens(z);
        if (generators.contains(seqlib::sf2::kSampleId) && generators.contains(seqlib::sf2::kKeyRange))
        {
            const auto tune = [&generators](uint16_t generator)
            {
                const auto found = generators.find(generator);
                return found != generators.end() ? static_cast<int16_t>(found->second) : 0;
            };
            const uint16_t keys = generators.at(seqlib::sf2::kKeyRange);
            tunings.emplace_back(keys & 0xFF, keys >> 8,
                                 tune(seqlib::sf2::kCoarseTune) * 100 + tune(seqlib::sf2::kFineTune));
        }
    }
    Check(tunings == std::vector<std::tuple<int, int, int>>{{0, 127, 3}},
          "SoundFont's tunings only on the keys that the sequence plays");
}

// The tunings that a conversion writes. Program 0 plays wave 1 from root key 0, and program 1 the PSG from root key 69.
// The sequence plays keys 60 and 120 on program 0 and key 93 on program 1. Key 120 is 10 octaves above the root:
// the timer's period stops at its floor of 0x10, and the key plays thousands of cents flat. The SoundFont writes the
// tuning's hundreds in coarseTune and the rest in fineTune. The SFZ writes them in transpose and tune.
void TestTuningInFiles()
{
    const Bytes bank = files::Bank({
        {.type = 1, .region = {.wave = 1, .root_key = 0}},
        {.type = 2, .region = {.wave = 3, .root_key = 69}},
    });
    const Bytes bytes = TestArchiveBytes(
        {0x81, 0x00, 0x3C, 0x7F, 0x18, 0x78, 0x7F, 0x18, 0x81, 0x01, 0x5D, 0x7F, 0x18, 0x80, 0x18, 0xFF}, "BGM_TEST", 0,
        127, bank);
    const sdat::Archive archive = sdat::Archive::Parse(bytes);
    const Conversion c = ConvertSequence(archive, {0}, "BGM_TEST", {}, kProgram);
    const seqlib::sf2::Records records = seqlib::sf2::Read(c.sf2);
    std::vector<std::tuple<int, int, int>> found;
    for (std::size_t z = 0; z + 1 < records.Count("ibag", 4); z++)
    {
        const std::map<uint16_t, uint16_t> generators = records.ZoneGens(z);
        if (generators.contains(seqlib::sf2::kSampleId) && generators.contains(seqlib::sf2::kKeyRange))
        {
            const auto tune = [&generators](uint16_t generator)
            {
                const auto value = generators.find(generator);
                return value != generators.end() ? static_cast<int16_t>(value->second) : 0;
            };
            const uint16_t keys = generators.at(seqlib::sf2::kKeyRange);
            found.emplace_back(keys & 0xFF, keys >> 8,
                               tune(seqlib::sf2::kCoarseTune) * 100 + tune(seqlib::sf2::kFineTune));
        }
    }
    const int key_60 = KeyTuning(0, 1022, 0, 60);
    const int key_120 = KeyTuning(0, 1022, 0, 120);
    const int psg = KeyTuning(1, 8006, 69, 93);
    Check(key_60 != 0 && key_120 <= -100 && key_120 % 100 != 0 && psg == 1, "the tunings that the files should write");
    Check(found == std::vector<std::tuple<int, int, int>>{{0, 119, key_60}, {120, 127, key_120}, {0, 127, psg}},
          "SoundFont's coarseTune and fineTune: the tunings of the played keys of wave 1 and of the PSG");

    const std::string& text = c.sfz.files.empty() ? std::string() : c.sfz.files[0].text;
    const std::string line_120 = " lokey=120 hikey=120 pitch_keycenter=0 transpose=" + std::to_string(key_120 / 100) +
                                 " tune=" + std::to_string(key_120 % 100) + " ";
    Check(text.find(line_120) != std::string::npos, "SFZ's transpose and tune of key 120");
}

// The phdr records of SoundFont `sf2`: each preset's bank and program, in order, without the terminal record.
std::vector<std::pair<uint16_t, uint16_t>> PresetsOf(const std::vector<uint8_t>& sf2)
{
    const seqlib::sf2::Records records = seqlib::sf2::Read(sf2);
    std::vector<std::pair<uint16_t, uint16_t>> presets;
    for (std::size_t i = 0; i + 1 < records.Count("phdr", 38); i++)
    {
        const uint8_t* r = records.Record("phdr", 38, i);
        presets.emplace_back(xsflib::LoadLe16(r + 22), xsflib::LoadLe16(r + 20));
    }

    return presets;
}

// The conversion of the test bank's instruments by themselves. Programs 5 and 6 play nothing, and neither does a
// program past the bank. Program 7's region plays full left, 3 dB above the centre. The SoundFont's zones are 3 dB
// quieter, as every SoundFont's are. A program past 127 takes a preset in bank 1. Programs given out of order, or
// twice, convert in ascending order, once each.
void TestConvertBank()
{
    const TestArchive test(kOneNote);
    const std::vector<uint32_t> every = {0, 1, 2, 3, 4, 5, 6, 7, 200};
    const ntdlib::sequence_export::BankConversion c =
        ConvertBank(test.archive, 0, every, ntdlib::ntr::snd::Build{}, kProgram);
    Check(c.sound_fonts.size() == 1 && c.sound_fonts[0].first == 0 && c.sound_fonts[0].last == 7,
          "a bank: one SoundFont of programs 0 to 7");
    const std::vector<uint8_t>& sf2 = c.sound_fonts.empty() ? std::vector<uint8_t>() : c.sound_fonts[0].sf2;
    Check(seqlib::sf2::Check(sf2).empty(), "a bank: seqlib's check of the SoundFont");
    Check(PresetsOf(sf2) == std::vector<std::pair<uint16_t, uint16_t>>{{0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}, {0, 7}},
          "a bank: a preset at each program that plays");

    // The zones of programs 0 to 4 play at the centre, 3 dB quieter: 75 cB in the generator. Program 7's zones play
    // full left, at full level. Every zone of the drum set plays one key.
    const seqlib::sf2::Records records = seqlib::sf2::Read(sf2);
    std::set<std::pair<uint16_t, bool>> attenuations;
    std::size_t one_key = 0;
    for (std::size_t z = 0; z + 1 < records.Count("ibag", 4); z++)
    {
        const std::map<uint16_t, uint16_t> generators = records.ZoneGens(z);
        if (generators.contains(seqlib::sf2::kSampleId))
        {
            const auto found = generators.find(seqlib::sf2::kInitialAttenuation);
            const auto pan = generators.find(seqlib::sf2::kPan);
            attenuations.insert({found != generators.end() ? found->second : 0, pan != generators.end()});
            const uint16_t keys = generators.at(seqlib::sf2::kKeyRange);
            one_key += (keys & 0xFF) == keys >> 8 ? 1 : 0;
        }
    }
    Check(attenuations == std::set<std::pair<uint16_t, bool>>{{0, true}, {75, false}} && one_key >= 3,
          "a bank: the SoundFont's zones take the cut, but for the zones full left");

    // Wave 0, wave 1, the PSG at duties 3, 1 and 0, and the noise.
    Check(!c.sound_fonts.empty() && c.sound_fonts[0].samples == 6 && c.sfz.samples.size() == 6,
          "a bank: a sample for each wave, and a WAV file for each");
    std::vector<std::string> names = {"conductor"};
    for (const seqlib::sfz::File& file : c.sfz.files)
    {
        names.push_back(file.name.substr(0, file.name.size() - 4));
    }
    Check(names == std::vector<std::string>{"conductor", "BANK_TEST_0", "BANK_TEST_1", "BANK_TEST_2", "BANK_TEST_3",
                                            "BANK_TEST_4", "BANK_TEST_7"},
          "a bank: an SFZ file of each program that plays");
    Check(ntdlib::testing::SfzProblem(c.sfz, names).empty(), "a bank: the SFZ files' samples and loops");
    const std::string& seventh = c.sfz.files.size() == 6 ? c.sfz.files[5].text : std::string();
    Check(seventh.find("<region> sample=w0_1.wav lokey=0 hikey=") == seventh.find("<region> ") &&
              seventh.find(" pitch_keycenter=60 sample_quality=0 volume=3.01 pan=-100 ") != std::string::npos &&
              seventh.find(" pitch_keycenter=60 tune=24 sample_quality=0 volume=3.01 pan=-100 ") != std::string::npos &&
              seventh.find("bend_up=200 bend_down=-200") != std::string::npos &&
              seventh.find("// Program 7 of BANK_TEST, converted by xyz2midi.") == 0,
          "a bank: program 7's file plays its region full left, with a track's starting bend range");
    const std::string& drums = c.sfz.files.size() == 6 ? c.sfz.files[3].text : std::string();
    std::size_t regions = 0;
    for (std::size_t at = drums.find("<region> "); at != std::string::npos; at = drums.find("<region> ", at + 1))
    {
        regions++;
    }
    Check(regions == 3 && drums.find("lokey=37 hikey=37") != std::string::npos, "a bank: the drum set's three keys");
    Check(
        c.warnings == std::vector<std::string>{"program 5 of bank 0 plays nothing", "program 6 of bank 0 plays nothing",
                                               "program 200 of bank 0 plays nothing"},
        "a bank: the programs that play nothing");

    // Program 7 by itself, then program 130 of a bank of 131 programs.
    const ntdlib::sequence_export::BankConversion seventh_only =
        ConvertBank(test.archive, 0, std::vector<uint32_t>{7}, ntdlib::ntr::snd::Build{}, kProgram);
    Check(seventh_only.sound_fonts.size() == 1 &&
              PresetsOf(seventh_only.sound_fonts[0].sf2) == std::vector<std::pair<uint16_t, uint16_t>>{{0, 7}} &&
              seventh_only.sfz.files.size() == 1 && seventh_only.sfz.files[0].name == "BANK_TEST_7.sfz" &&
              seventh_only.sfz.samples.size() == 1,
          "a bank: one program by itself");
    std::vector<files::Instrument> many(131);
    many.back() = {.type = 1, .region = {.wave = 1}};
    const Bytes large_bytes = TestArchiveBytes(kOneNote, "BGM_TEST", 0, 127, files::Bank(many));
    const sdat::Archive large = sdat::Archive::Parse(large_bytes);
    const ntdlib::sequence_export::BankConversion past =
        ConvertBank(large, 0, std::vector<uint32_t>{9, 130}, ntdlib::ntr::snd::Build{}, kProgram);
    Check(past.sound_fonts.size() == 1 &&
              PresetsOf(past.sound_fonts[0].sf2) == std::vector<std::pair<uint16_t, uint16_t>>{{1, 2}} &&
              past.sfz.files.size() == 1 && past.sfz.files[0].name == "BANK_TEST_130.sfz",
          "a bank: program 130 takes preset 2 of bank 1");
    const ntdlib::sequence_export::BankConversion unsorted =
        ConvertBank(large, 0, std::vector<uint32_t>{130, 9, 130}, ntdlib::ntr::snd::Build{}, kProgram);
    Check(unsorted.sound_fonts.size() == 1 && unsorted.sound_fonts[0].first == 130 &&
              PresetsOf(unsorted.sound_fonts[0].sf2) == std::vector<std::pair<uint16_t, uint16_t>>{{1, 2}} &&
              unsorted.sfz.files.size() == 1 && unsorted.sfz.files[0].name == "BANK_TEST_130.sfz",
          "a bank: a program given twice converts once");
    const ntdlib::sequence_export::BankConversion repeated =
        ConvertBank(test.archive, 0, std::vector<uint32_t>{7, 0, 7}, ntdlib::ntr::snd::Build{}, kProgram);
    std::vector<std::string> repeated_names;
    for (const seqlib::sfz::File& file : repeated.sfz.files)
    {
        repeated_names.push_back(file.name);
    }
    Check(repeated.sound_fonts.size() == 1 && repeated.sound_fonts[0].first == 0 && repeated.sound_fonts[0].last == 7 &&
              PresetsOf(repeated.sound_fonts[0].sf2) == std::vector<std::pair<uint16_t, uint16_t>>{{0, 0}, {0, 7}} &&
              repeated_names == std::vector<std::string>{"BANK_TEST_0.sfz", "BANK_TEST_7.sfz"},
          "a bank: programs out of order convert in ascending order");
    many[7] = many.back();
    const Bytes wide_bytes = TestArchiveBytes(kOneNote, "BGM_TEST", 0, 127, files::Bank(many));
    const sdat::Archive wide = sdat::Archive::Parse(wide_bytes);
    const ntdlib::sequence_export::BankConversion widths =
        ConvertBank(wide, 0, std::vector<uint32_t>{130, 7}, ntdlib::ntr::snd::Build{}, kProgram);
    std::vector<std::string> width_names;
    for (const seqlib::sfz::File& file : widths.sfz.files)
    {
        width_names.push_back(file.name);
    }
    Check(widths.sound_fonts.size() == 1 &&
              PresetsOf(widths.sound_fonts[0].sf2) == std::vector<std::pair<uint16_t, uint16_t>>{{0, 7}, {1, 2}} &&
              width_names == std::vector<std::string>{"BANK_TEST_007.sfz", "BANK_TEST_130.sfz"},
          "a bank: programs of one and three digits, given from the highest");

    const bool threw =
        Throws<xsflib::FormatError>([&] { ConvertBank(test.archive, 1, every, ntdlib::ntr::snd::Build{}, kProgram); });
    Check(threw, "a bank that the archive lacks");
}

// The command line on a cartridge image with two sound archives in one file and a third in another, on a loose sound
// archive, on a file that is neither, and on cartridge images with DSE's songs and sound effects and with
// Universomo's songs.
void TestFrame()
{
    const ntdlib::testing::TempFolder folder("sequence_export_ntr_test");
    const Bytes one_note = TestArchiveBytes(kOneNote);
    const Bytes loop = TestArchiveBytes(kLoop, "BGM_LOOP");
    Bytes pack = {'J', 'U', 'N', 'K', 0, 0, 0, 0};
    pack.insert(pack.end(), loop.begin(), loop.end());
    files::Align(pack, 4);
    pack.insert(pack.end(), one_note.begin(), one_note.end());

    nds_files::Image spec;
    spec.files = {{"sound/sound_data.sdat", one_note}, {"data/pack.bin", pack}, {"data/readme.txt", {'h', 'i'}}};
    const fs::path image = folder.Write("game.nds", nds_files::BuildImage(spec).first);
    const fs::path loose = folder.Write("loose.sdat", one_note);
    const fs::path neither = folder.Write("neither.bin", {1, 2, 3, 4, 5, 6, 7, 8});

    ArchiveConverter converter;
    converter.program = kProgram;
    converter.version_line = "xyz2midi 1.0";
    const auto run = [&converter](const fs::path& input)
    {
        return RunArchiveConverter({kProgram, "--quiet", ntdlib::sequence_export::Utf8(input)}, converter);
    };

    // Each file's archives go to folders named after the file: a second archive in the file takes _1.
    const fs::path out = folder.Path() / "game_xyz2midi";
    Check(run(image) == 0, "the image converts");
    const auto passes = [](const fs::path& stem)
    {
        const std::optional<std::vector<uint8_t>> midi =
            ntdlib::sequence_export::ReadFile(fs::path(stem).replace_extension(".mid"));
        const std::optional<std::vector<uint8_t>> sf2 =
            ntdlib::sequence_export::ReadFile(fs::path(stem).replace_extension(".sf2"));
        return midi && sf2 && seqlib::midi::Problems(*midi).empty() && seqlib::sf2::Check(*sf2).empty();
    };
    Check(passes(out / "sound_data" / "BGM_TEST") && passes(out / "pack" / "BGM_LOOP") &&
              passes(out / "pack_1" / "BGM_TEST"),
          "each archive's files, which pass seqlib's checks");
    Check(fs::exists(out / "pack" / "sfz" / "BGM_LOOP" / "Track 0.sfz") && fs::exists(out / "pack" / "sfz" / "samples"),
          "and its SFZ files");

    Check(run(loose) == 0 && passes(folder.Path() / "loose_xyz2midi" / "BGM_TEST"), "a loose archive converts");

    // DSE's songs and sound effects: each folder of them goes to a folder of its name. A sound effect takes the name
    // of its bank of sound effects and its slot. The image's ARM9 binary holds no build of DSE's driver that a model
    // covers: its songs convert only on the model that --dse-model names.
    nds_files::Image dse;
    dse.files = {
        {"SOUND/BGM/bgm0001.smd", dse_song_files::SongFile()},
        {"SOUND/BGM/bgm0001.swd", dse_song_files::BankFile()},
        {"SOUND/BGM/bgm.swd", dse_song_files::MainBankFile()},
        {"SOUND/SE/fx.sed",
         dse_song_files::EffectsFile(
             {std::nullopt,
              dse_song_files::Effect{.tracks = {dse_song_files::Join({{1, 0, 0, 0},
                                                                      {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F},
                                                                      dse_song_files::Note(0x60, 0, 0, 0x18, 1),
                                                                      {0x92, 0x18, 0x98}})}}},
             5)},
        {"SOUND/SE/fx.swd", dse_song_files::BankFile()}};
    const fs::path dse_image = folder.Write("dse.nds", nds_files::BuildImage(dse).first);
    const fs::path dse_out = folder.Path() / "dse_xyz2midi";
    Check(run(dse_image) == 1 && !fs::exists(dse_out), "DSE's songs don't convert on a build that no model covers");
    Check(RunArchiveConverter({kProgram, "--quiet", "--dse-model", "later", ntdlib::sequence_export::Utf8(dse_image)},
                              converter) == 0 &&
              passes(dse_out / "BGM" / "bgm0001") && passes(dse_out / "SE" / "fx_001") &&
              fs::exists(dse_out / "SE" / "sfz" / "fx_001" / "Track 0.sfz"),
          "DSE's songs and sound effects convert on the model that --dse-model names");

    // Universomo's songs: the songs beside the bank go to the folder universomo, each named after its file. --only
    // picks songs by name.
    nds_files::Image ums;
    ums.files = {{"data/samplebank.usb", universomo_files::BankFile()},
                 {"data/au81.ums", universomo_files::SongFile()},
                 {"data/au82.ums", universomo_files::SongFile()}};
    const Bytes ums_bytes = nds_files::BuildImage(ums).first;
    const fs::path ums_out = folder.Path() / "ums_xyz2midi" / "universomo";
    Check(run(folder.Write("ums.nds", ums_bytes)) == 0 && passes(ums_out / "au81") && passes(ums_out / "au82") &&
              fs::exists(ums_out / "sfz" / "au81"),
          "Universomo's songs convert");
    const fs::path only_out = folder.Path() / "ums_only_xyz2midi" / "universomo";
    Check(RunArchiveConverter({kProgram, "--quiet", "--only", "^au82$",
                               ntdlib::sequence_export::Utf8(folder.Write("ums_only.nds", ums_bytes))},
                              converter) == 0 &&
              passes(only_out / "au82") && !fs::exists(only_out / "au81.mid"),
          "--only picks Universomo's songs by name");
    Check(run(neither) == 1 && !fs::exists(folder.Path() / "neither_xyz2midi"), "a file that is neither fails");

    // A game's profile: its settings give a sequence's program through variable 0, and an arrangement gives another
    // program. Without a value, the program is -1 and the sequence plays nothing.
    const fs::path voices = folder.Write("voices.sdat", TestArchiveBytes(kProgramFromVariable, "SE_VOICE"));
    ArchiveConverter profiled = converter;
    profiled.game_settings = [](const ArchiveInput&, const ntdlib::ntr::SequenceRef&, PerformOptions& perform)
    {
        perform.variables.try_emplace(0, 0);
    };
    profiled.arrangements = [](const ArchiveInput&, const ntdlib::ntr::SequenceRef&)
    {
        return std::vector<Arrangement>{{.suffix = "_PROGRAM7", .variables = {{0, 7}}}};
    };
    const auto convert =
        [&voices, &folder](const ArchiveConverter& c, const std::string& subfolder, const std::string& var = "")
    {
        std::vector<std::string> args = {kProgram, "--quiet", "--output",
                                         ntdlib::sequence_export::Utf8(folder.Path() / subfolder)};
        if (!var.empty())
        {
            args.insert(args.end(), {"--var", var});
        }
        args.push_back(ntdlib::sequence_export::Utf8(voices));
        return RunArchiveConverter(args, c);
    };
    // The samples of a SoundFont that the converter wrote: its smpl chunk.
    const auto samples = [&folder](const std::string& subfolder, const std::string& name)
    {
        const std::optional<std::vector<uint8_t>> sf2 =
            ntdlib::sequence_export::ReadFile(folder.Path() / subfolder / (name + ".sf2"));
        const std::string_view text(reinterpret_cast<const char*>(sf2 ? sf2->data() : nullptr), sf2 ? sf2->size() : 0);
        const std::size_t at = text.find("smpl");
        if (at == std::string_view::npos || at + 8 > text.size())
        {
            return std::string();
        }
        const uint32_t size = xsflib::LoadLe32(sf2->data() + at + 4);
        return std::string(text.substr(at + 8, size));
    };
    // The number of its samples: the records of its shdr chunk but the last. The last record ends the list. A SoundFont
    // without samples still holds the 46 zero points that follow each sample.
    const auto sample_count = [&folder](const std::string& subfolder, const std::string& name)
    {
        const std::optional<std::vector<uint8_t>> sf2 =
            ntdlib::sequence_export::ReadFile(folder.Path() / subfolder / (name + ".sf2"));
        return sf2 ? seqlib::sf2::Read(*sf2).Count("shdr", 46) - 1 : 0;
    };
    Check(convert(converter, "plain") == 0 && convert(profiled, "profiled") == 0 &&
              convert(profiled, "given", "0=7") == 0 && convert(profiled, "below", "0=0") == 0,
          "the profiled archive converts");
    Check(passes(folder.Path() / "profiled" / "SE_VOICE") && passes(folder.Path() / "profiled" / "SE_VOICE_PROGRAM7") &&
              !fs::exists(folder.Path() / "plain" / "SE_VOICE_PROGRAM7.mid"),
          "an arrangement converts beside its sequence");
    Check(sample_count("plain", "SE_VOICE") == 0 && sample_count("profiled", "SE_VOICE") == 1 &&
              sample_count("profiled", "SE_VOICE_PROGRAM7") == 1 &&
              samples("profiled", "SE_VOICE") != samples("profiled", "SE_VOICE_PROGRAM7"),
          "the game's settings and the arrangement give their programs");
    Check(samples("given", "SE_VOICE") == samples("profiled", "SE_VOICE_PROGRAM7") &&
              samples("given", "SE_VOICE_PROGRAM7") == samples("profiled", "SE_VOICE_PROGRAM7") &&
              samples("below", "SE_VOICE") == samples("profiled", "SE_VOICE") &&
              samples("below", "SE_VOICE_PROGRAM7") == samples("profiled", "SE_VOICE"),
          "the command line's value stays over both");
    const std::optional<std::vector<uint8_t>> arranged =
        ntdlib::sequence_export::ReadFile(folder.Path() / "profiled" / "SE_VOICE_PROGRAM7.sf2");
    Check(arranged && std::string_view(reinterpret_cast<const char*>(arranged->data()), arranged->size())
                              .find("from sequence SE_VOICE_PROGRAM7.") != std::string_view::npos,
          "an arrangement's files take its name");

    // A game's profile with commands: its settings give track 0 a volume of 0 once the sequence starts, and an
    // arrangement gives it 127 again. The arrangement's commands follow the settings'.
    ArchiveConverter commanded = converter;
    commanded.game_settings = [](const ArchiveInput&, const ntdlib::ntr::SequenceRef&, PerformOptions& perform)
    {
        perform.commands.push_back(TrackVolume(0x0001, -32768));
    };
    commanded.arrangements = [](const ArchiveInput&, const ntdlib::ntr::SequenceRef&)
    {
        return std::vector<Arrangement>{{.suffix = "_FULL", .commands = {TrackVolume(0x0001, 0)}}};
    };
    const std::vector<std::string> args = {kProgram, "--quiet", "--output",
                                           ntdlib::sequence_export::Utf8(folder.Path() / "commanded"),
                                           ntdlib::sequence_export::Utf8(loose)};
    // The value of the first CC11 in a MIDI file that the converter wrote, or -1.
    const auto first_expression = [&folder](const std::string& name)
    {
        const std::optional<std::vector<uint8_t>> midi =
            ntdlib::sequence_export::ReadFile(folder.Path() / "commanded" / (name + ".mid"));
        if (!midi)
        {
            return -1;
        }
        for (const seqlib::midi::Track& track : seqlib::midi::Read(*midi).tracks)
        {
            for (const seqlib::midi::Event& e : track.events)
            {
                if (e.bytes.size() == 3 && (e.bytes[0] & 0xF0) == 0xB0 && e.bytes[1] == 11)
                {
                    return static_cast<int>(e.bytes[2]);
                }
            }
        }
        return -1;
    };
    Check(RunArchiveConverter(args, commanded) == 0 && first_expression("BGM_TEST") == 0 &&
              first_expression("BGM_TEST_FULL") == 107,
          "the game's settings' commands and an arrangement's after them");
}

// A DSE song that the game starts at a level converts at that level, and a song with several states converts once for
// each. Explorers of Sky (North America, C2SE) starts bgm0086 at 180 first, and at full level as a second state:
// bgm0086 and bgm0086_LEVEL256. DSE's volume is 89 at 180 and 127 at full level. CC11 contains the synth's volume. The
// first CC11 of bgm0086 is therefore 89/127 of bgm0086_LEVEL256's. bgm0001 has no row and converts once, at full level.
// --only selects each state by the name of its file.
void TestDseSongStates()
{
    const ntdlib::testing::TempFolder folder("sequence_export_ntr_dse_states_test");
    nds_files::Image spec;
    spec.game_code = "C2SE";
    spec.files = {{"SOUND/BGM/bgm0001.smd", dse_song_files::SongFile()},
                  {"SOUND/BGM/bgm0001.swd", dse_song_files::BankFile()},
                  {"SOUND/BGM/bgm0086.smd", dse_song_files::SongFile()},
                  {"SOUND/BGM/bgm0086.swd", dse_song_files::BankFile()},
                  {"SOUND/BGM/bgm.swd", dse_song_files::MainBankFile()}};
    const fs::path image = folder.Write("sky.nds", nds_files::BuildImage(spec).first);
    const fs::path out = folder.Path() / "sky_xyz2midi" / "BGM";
    ArchiveConverter converter;
    converter.program = kProgram;
    converter.version_line = "xyz2midi 1.0";
    // The value of the first CC11 in a MIDI file that the converter wrote, or -1.
    const auto first_expression = [&out](const std::string& name)
    {
        const std::optional<std::vector<uint8_t>> midi = ntdlib::sequence_export::ReadFile(out / (name + ".mid"));
        if (!midi)
        {
            return -1;
        }
        for (const seqlib::midi::Track& track : seqlib::midi::Read(*midi).tracks)
        {
            for (const seqlib::midi::Event& e : track.events)
            {
                if (e.bytes.size() == 3 && (e.bytes[0] & 0xF0) == 0xB0 && e.bytes[1] == 11)
                {
                    return static_cast<int>(e.bytes[2]);
                }
            }
        }
        return -1;
    };

    const int converted = RunArchiveConverter(
        {kProgram, "--quiet", "--dse-model", "later", ntdlib::sequence_export::Utf8(image)}, converter);

    const int plain = first_expression("bgm0001");
    const int lower = first_expression("bgm0086");
    const int full = first_expression("bgm0086_LEVEL256");
    Check(converted == 0 && plain > 0 && full == plain && !fs::exists(out / "bgm0001_LEVEL256.mid"),
          "a song without a row converts once, at full level: " + std::to_string(plain));
    Check(lower > 0 && std::abs(lower - full * 89.0 / 127.0) <= 1.0,
          "bgm0086 at level 180 and at full level: CC11 " + std::to_string(lower) + " and " + std::to_string(full));

    // --only selects a state by its file's name: the second state alone, then the first alone.
    fs::remove_all(out);
    const int second = RunArchiveConverter({kProgram, "--quiet", "--dse-model", "later", "--only", "^bgm0086_LEVEL256$",
                                            ntdlib::sequence_export::Utf8(image)},
                                           converter);
    const bool second_alone = fs::exists(out / "bgm0086_LEVEL256.mid") && !fs::exists(out / "bgm0086.mid");
    fs::remove_all(out);
    const int first = RunArchiveConverter(
        {kProgram, "--quiet", "--dse-model", "later", "--only", "^bgm0086$", ntdlib::sequence_export::Utf8(image)},
        converter);
    const bool first_alone = fs::exists(out / "bgm0086.mid") && !fs::exists(out / "bgm0086_LEVEL256.mid");
    Check(second == 0 && second_alone && first == 0 && first_alone, "--only selects one state of a song by its name");
}

// The help names the inputs "input", because an input is a sound archive or a cartridge image.
void TestUsage()
{
    ArchiveConverter converter;
    converter.program = kProgram;

    const std::string text = UsageText(converter);

    Check(text.find("options:\n"
                    "  --output DIR   write to DIR (one input only)\n"
                    "  --only REGEX   convert matching sequence names (for example --only BGM)\n"
                    "  --loops N") != std::string::npos,
          "the --output and --only lines for inputs, before --loops: " + text);
}

} // namespace

int main()
{
    TestConvert();
    TestTuningInFiles();
    TestConvertBank();
    TestFrame();
    TestDseSongStates();
    TestUsage();

    return xsflib::testing::Summary("DS conversion: files");
}
