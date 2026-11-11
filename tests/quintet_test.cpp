// SPDX-License-Identifier: MIT

// Unit tests for Quintet's driver. They build small synthetic cartridges in memory, so no game ROM is needed.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "seqlib/midi/reader.h"
#include "seqlib/testing/check.h"

#include "ntdlib/agb/files.h"
#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/song_plan.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/quintet/archive_player.h"
#include "ntdlib/quintet/commands.h"
#include "ntdlib/quintet/driver.h"
#include "ntdlib/quintet/instruments.h"
#include "ntdlib/quintet/sequencer.h"
#include "ntdlib/quintet/song.h"
#include "ntdlib/sequence_export/agb/midi.h"
#include "ntdlib/sequence_export/agb/psg.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/agb/sf2.h"
#include "ntdlib/sequence_export/quintet/convert.h"
#include "ntdlib/sequence_export/quintet/soundfont.h"
#include "ntdlib/testing/agb_cart.h"
#include "ntdlib/testing/agb_files.h"
#include "ntdlib/testing/quintet_cart.h"

namespace ntdlib::sequence_export::quintet
{
namespace
{

using ntdlib::agb::kRomBase;
using ntdlib::agb::RegisterWrite;
using ntdlib::agb::Rom;
using ntdlib::agb::Utf8;
using ntdlib::quintet::ArchivePlayer;
using ntdlib::quintet::Command;
using ntdlib::quintet::DecodeCommand;
using ntdlib::quintet::DetectDriver;
using ntdlib::quintet::DriverInfo;
using ntdlib::quintet::DriverOverrides;
using ntdlib::quintet::DumpSong;
using ntdlib::quintet::EffectIndex;
using ntdlib::quintet::Event;
using ntdlib::quintet::FifoFrames;
using ntdlib::quintet::FileOfSong;
using ntdlib::quintet::Instrument;
using ntdlib::quintet::LookUpInstrument;
using ntdlib::quintet::ReadMusicFiles;
using ntdlib::quintet::ReadSample;
using ntdlib::quintet::ReadWave;
using ntdlib::quintet::Revision;
using ntdlib::quintet::Sequencer;
using ntdlib::quintet::SongAddress;
using ntdlib::quintet::SongCount;
using ntdlib::sequence_export::agb::ConvertSettings;
using ntdlib::sequence_export::agb::kMiddleC;
using ntdlib::sequence_export::agb::SongNumber;
using ntdlib::sequence_export::agb::SongReport;
using ntdlib::sequence_export::agb::SongStem;
using ntdlib::testing::agb_files::HasGlobalZoneModulators;
using ntdlib::testing::agb_files::LastWrite;
using ntdlib::testing::agb_files::Le16;
using ntdlib::testing::agb_files::Le32;
using ntdlib::testing::agb_files::MidiEvents;
using ntdlib::testing::agb_files::ReadAll;
using ntdlib::testing::agb_files::ReadMidi;
using ntdlib::testing::agb_files::ReadSf2;
using ntdlib::testing::agb_files::Sf2Records;
using ntdlib::testing::agb_files::TempPath;
using ntdlib::testing::agb_files::TempSettings;
using ntdlib::testing::quintet_cart::Cart;
using ntdlib::testing::quintet_cart::Channel;
using ntdlib::testing::quintet_cart::FileAddress;
using ntdlib::testing::quintet_cart::kEffectsFile;
using ntdlib::testing::quintet_cart::kFrequencyTable;
using ntdlib::testing::quintet_cart::kLengthTable;
using ntdlib::testing::quintet_cart::kLfoTable;
using ntdlib::testing::quintet_cart::kMacroFile;
using ntdlib::testing::quintet_cart::kMusicFile;
using ntdlib::testing::quintet_cart::kPcmPitchTable;
using ntdlib::testing::quintet_cart::kPlay;
using ntdlib::testing::quintet_cart::kPlayEffect;
using ntdlib::testing::quintet_cart::kSampleFile;
using ntdlib::testing::quintet_cart::kSfxMacroFile;
using ntdlib::testing::quintet_cart::kSfxSampleFile;
using ntdlib::testing::quintet_cart::kWaveFile;
using ntdlib::testing::quintet_cart::Saw;
using ntdlib::testing::quintet_cart::Song;

namespace cc = seqlib::midi::cc;

// Returns the driver info of a cartridge, failing the test if detection fails.
DriverInfo Detect(const Rom& rom)
{
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, DriverOverrides(), info, error));
    SEQLIB_CHECK(error.empty());

    return info;
}

// Runs a song for `frames` frames and returns each frame's register writes, with the play routine's first.
std::vector<std::vector<RegisterWrite>> Writes(const Rom& rom, const DriverInfo& info, int song, int frames)
{
    Sequencer seq(rom, info, song);
    std::vector<std::vector<RegisterWrite>> out = {seq.StartWrites()};
    for (int f = 0; f < frames; f++)
    {
        out.push_back(seq.Step());
    }

    return out;
}

// Returns the tick of the last marker in `midi` whose text is `name`, or `missing` if there's none.
uint32_t MarkerTick(const MidiEvents& midi, const std::string& name, uint32_t missing)
{
    uint32_t found = missing;
    for (const auto& [tick, bytes] : midi.Find(0xFF))
    {
        if (bytes.size() > 3 && bytes[1] == 0x06 && std::string(bytes.begin() + 3, bytes.end()) == name)
        {
            found = uint32_t(tick);
        }
    }

    return found;
}

void TestDetection()
{
    // The A revision: the driver's tables, and the game's files from its code.
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel().Rest(4).End()}), Song({Channel().Rest(4).End()})});
    cart.Samples(kSampleFile, {{Saw(200), 8000}, {Saw(300), 11025}});
    const Rom rom = cart.ToRom();

    const DriverInfo info = Detect(rom);

    SEQLIB_CHECK(info.revision == Revision::kA);
    SEQLIB_CHECK_EQ(info.play, kPlay);
    SEQLIB_CHECK_EQ(info.length_table, kLengthTable);
    SEQLIB_CHECK_EQ(info.frequency_table, kFrequencyTable);
    SEQLIB_CHECK_EQ(info.pcm_pitch_table, kPcmPitchTable);
    SEQLIB_CHECK_EQ(info.lfo_table, kLfoTable);
    SEQLIB_CHECK_EQ(info.waves, FileAddress(kWaveFile));
    SEQLIB_CHECK_EQ(info.macros[0], FileAddress(kMacroFile));
    SEQLIB_CHECK_EQ(info.macros[1], 0);
    SEQLIB_CHECK_EQ(info.samples[0], FileAddress(kSampleFile));
    SEQLIB_CHECK_EQ(info.samples[1], 0);
    SEQLIB_CHECK_EQ(info.music_file, FileAddress(kMusicFile));
    SEQLIB_REQUIRE_EQ(info.song_addresses.size(), 2);
    SEQLIB_REQUIRE_EQ(info.sample_addresses[0].size(), 2);
    SEQLIB_CHECK(info.warnings.empty());

    // Detection starts from nothing. A game without the driver keeps none of an earlier game's tables.
    DriverInfo reused = info;
    std::string reused_error;
    Rom blank;
    blank.Assign(std::vector<uint8_t>(0x1000, 0));

    const bool found_blank = DetectDriver(blank, DriverOverrides(), reused, reused_error);

    SEQLIB_CHECK(!found_blank && reused.play == 0 && reused.song_addresses.empty());

    // The J revision has the sound effects' noise macros and samples too.
    Cart cart_j(Revision::kJ);
    cart_j.Songs({Song({Channel().Rest(4).End()})});
    cart_j.Samples(kSampleFile, {{Saw(200), 8000}});
    cart_j.Samples(kSfxSampleFile, {{Saw(100), 8000}, {Saw(100), 8000}, {Saw(100), 8000}});
    const Rom rom_j = cart_j.ToRom();

    const DriverInfo info_j = Detect(rom_j);

    SEQLIB_CHECK(info_j.revision == Revision::kJ);
    SEQLIB_CHECK_EQ(info_j.macros[0], FileAddress(kMacroFile));
    SEQLIB_CHECK_EQ(info_j.macros[1], FileAddress(kSfxMacroFile));
    SEQLIB_CHECK_EQ(info_j.samples[1], FileAddress(kSfxSampleFile));
    SEQLIB_REQUIRE_EQ(info_j.sample_addresses[1].size(), 3);
    SEQLIB_REQUIRE_EQ(info_j.song_addresses.size(), 1);

    // An override of the song count.
    DriverOverrides one;
    one.song_count = 1;
    DriverInfo first;
    std::string error;

    SEQLIB_CHECK(DetectDriver(rom, one, first, error));
    SEQLIB_REQUIRE_EQ(first.song_addresses.size(), 1);

    // A cartridge without the driver shows no sign of it, unless the music file is given.
    Cart bare(Revision::kA, false);
    bare.Songs({Song({Channel().Rest(4).End()})});
    const Rom bare_rom = bare.ToRom();
    DriverOverrides table;
    table.song_table = FileAddress(kMusicFile);
    DriverInfo none, given;

    SEQLIB_CHECK(!DetectDriver(bare_rom, DriverOverrides(), none, error));
    SEQLIB_CHECK(error.empty());
    SEQLIB_CHECK(!DetectDriver(bare_rom, table, given, error));
    SEQLIB_CHECK(error == "no Quintet sound driver found, so it has no length table");
}

void TestDecoding()
{
    Cart cart(Revision::kA);
    const uint32_t kAt = FileAddress(kMusicFile) + 0x800;
    const std::vector<uint8_t> kBytes = {0x4A, 0x3D, 0xD2, 0x64, 0x50, 0x3C, 0x03,
                                         0x0F, 0xE7, 0x00, 0x62, 0x00, 0xF0, 0xFF};
    for (size_t i = 0; i < kBytes.size(); i++)
    {
        cart.Put8(kAt + uint32_t(i), kBytes[i]);
    }
    const Rom rom = cart.ToRom();
    DriverInfo info;
    info.length_table = kLengthTable;
    DriverInfo info_j = info;
    info_j.revision = Revision::kJ;

    const Command note = DecodeCommand(rom, info, kAt, 0);
    const Command rest = DecodeCommand(rom, info, kAt + 1, 0);
    const Command envelope = DecodeCommand(rom, info, kAt + 2, 0);
    const Command range = DecodeCommand(rom, info, kAt + 8, 4);
    const Command bank_a = DecodeCommand(rom, info, kAt + 12, 4);
    const Command bank_j = DecodeCommand(rom, info_j, kAt + 12, 4);
    const Command macros_j = DecodeCommand(rom, info_j, kAt + 12, 3);
    const Command end = DecodeCommand(rom, info, kAt + 13, 0);

    SEQLIB_CHECK(note.note && note.ticks == 64 && note.text == "note A#, 64 ticks (1/6)");
    SEQLIB_CHECK(rest.note && rest.ticks == 96 && rest.text == "rest, 96 ticks (1/4)");
    SEQLIB_CHECK(envelope.size == 6 &&
                 envelope.text == "volume envelope: 100% to 80% over 3 frames, then to 60% over 15 frames");
    SEQLIB_CHECK(range.size == 4 && range.text == "play 0% to 98% of the sample, then loop from 0%");
    SEQLIB_CHECK(bank_a.text == "nothing");
    SEQLIB_CHECK(bank_j.text == "switch to the other bank of samples");
    SEQLIB_CHECK(macros_j.text == "switch to the other bank of noise macros");
    SEQLIB_CHECK(end.end && end.text == "end, or back to the loop point");
}

void TestTiming()
{
    // At tempo 90, a quarter note of 96 ticks takes 96 × 37 / 90 = 39.47 frames, and the driver plays each note at the
    // start of the frame its tick falls in. A loop starts the count again where the driver is, so the passes drift from
    // the song's ticks: the fourth note plays at frame 117, where tick 288 would fall in frame 118.
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel().Bytes({0x7F, 90, 0x5E, 15, 0xBF}).Note(0, 3).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    std::vector<uint32_t> frames;
    for (int f = 0; f < 160; f++)
    {
        seq.Step();
        for (const Event& e : seq.Events())
        {
            if (e.kind == Event::kNote)
            {
                frames.push_back(e.frame);
            }
        }
    }

    SEQLIB_CHECK(frames.size() >= 4 && frames[1] == 39 && frames[2] == 78 && frames[3] == 117);

    // The MIDI file follows: a frame at tempo 90 is 90 ticks, and each note after a loop starts its frame.
    ConvertSettings opt;
    opt.loops = 4;
    opt.track_mask = 1;
    opt.out_dir = Utf8(ntdlib::testing::agb_files::TempFolderPath());
    opt.base_name = "timing";

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    const MidiEvents midi = ReadMidi(sum.midi_path);
    std::vector<uint32_t> ons;
    for (const auto& [tick, bytes] : midi.Find(0x90))
    {
        ons.push_back(uint32_t(tick));
    }

    SEQLIB_CHECK(sum.ok);
    SEQLIB_CHECK_EQ(midi.division, 3552);
    SEQLIB_CHECK(ons.size() == 4 && ons[0] == 0 && ons[1] == 3510 && ons[2] == 7020 && ons[3] == 10530);
}

// A channel that stops at FF on its loop point holds its note, and counts as finished. Here the only channel holds
// after a quarter note at tempo 90, 96 × 37 / 90 = 39.47 frames or 0.66 s, and the song ends there without a loop, with
// the LFO's bends in those frames and no more.
void TestHold()
{
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel().Bytes({0x7F, 90, 0x5E, 15, 0xDF, 0, 8, 64}).Note(0, 3).Bytes({0xBF}).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("hold");

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    const MidiEvents midi = ReadMidi(sum.midi_path);
    SEQLIB_CHECK(sum.ok && sum.seconds > 0.65 && sum.seconds < 0.67 && sum.loop_start < 0);
    SEQLIB_CHECK(!midi.Find(0xE0).empty() && midi.Find(0xE0).size() <= 41);
}

// Square 2 repeats a command 255 times 255 times, without a note, and the guard ends it after 10000 commands. Square 1
// loops a quarter note. The plan takes square 2 as ended. The song therefore loops, without running for the hour that a
// song cut off there takes.
void TestCommandGuard()
{
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel().Bytes({0x7F, 90, 0xBF}).Note(0, 3).End(),
                      Channel().Note(0, 3).Bytes({0xD6, 0xD6, 0xAF, 0x00, 0xD7, 0xFF, 0xD7, 0xFF}).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("guard");

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    const auto has = [&sum](const std::string& text)
    {
        return std::any_of(sum.warnings.begin(), sum.warnings.end(),
                           [&text](const std::string& w) { return w.find(text) != std::string::npos; });
    };
    SEQLIB_CHECK(sum.ok && sum.seconds < 2);
    SEQLIB_CHECK(has("without reaching a note") && !has("cut off"));
}

void TestWaveSwitch()
{
    // A wave note that switches from a wave whose halves match (wave 0, made a 16-step saw played twice) to one whose
    // halves don't (wave 1, a square) plays both zones from the same root key, so the second sounds at the note's pitch
    // and not an octave higher. Each run of keys that the frequency table tunes alike has a pair of zones.
    Cart cart(Revision::kA);
    for (uint32_t i = 0; i < 16; i++)
    {
        cart.Put8(FileAddress(kWaveFile) + 2 + i, uint8_t((2 * (i % 8)) << 4 | (2 * (i % 8) + 1)));
    }
    cart.Songs({Song({{0xFF}, {0xFF}, Channel().Bytes({0xDA, 0x00, 0xDA, 0x81, 0x5E, 3, 0xEA, 10}).Note(0, 2).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("wave_switch");

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    // The instrument's zones after its global zone.
    const Sf2Records sf2 = ReadSf2(sum.sf2_path);
    const size_t first = Le16(sf2.Record("inst", 22, 0) + 20) + 1;
    const size_t zones = Le16(sf2.Record("inst", 22, 1) + 20) - first;

    // A zone's root key: its overriding root key, or else its sample's.
    const auto root = [&sf2](size_t zone)
    {
        const std::map<uint16_t, uint16_t> gens = sf2.ZoneGens(zone);
        const auto key = gens.find(sf2::kOverridingRootKey);
        return key != gens.end() ? int(key->second) : int(sf2.Record("shdr", 46, gens.at(sf2::kSampleId))[40]);
    };

    SEQLIB_CHECK(HasGlobalZoneModulators(sf2));
    SEQLIB_CHECK(sum.ok && zones >= 2 && zones % 2 == 0);
    for (size_t z = first; z < first + zones; z++)
    {
        SEQLIB_CHECK_EQ(root(z), 72);
    }
}

void TestPsgTuning()
{
    // A square note plays its entry of the frequency table. The entries can depart from equal temperament. Entry 24,
    // key 60's, is set about 19 cents flat. The zone that plays key 60 has the entry's tuning against middle C. The
    // zone of key 72 has the tuning of entry 36. The cart works that entry out from equal temperament. A wave note on
    // entry 24 with a wave of 32 steps plays an octave lower, on key 48. Its zone has the entry's tuning too.
    Cart cart(Revision::kA);
    const int flat = int(std::lround(2048 - 131072 / (kMiddleC * std::pow(2.0, -19 / 1200.0))));
    const int octave = cart.ToRom().U16(kFrequencyTable + 72);
    cart.Put16(kFrequencyTable + 48, uint16_t(flat));
    cart.Songs({Song({Channel().Bytes({0x5E, 12, 0x2F, 1}).Note(0, 3).End()}),
                Song({{0xFF}, {0xFF}, Channel().Bytes({0xDA, 0x01, 0x5E, 12}).Note(0, 3).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const SongReport square = ConvertSong(rom, info, 0, TempSettings("psg_tuning"), nullptr);
    const SongReport wave = ConvertSong(rom, info, 1, TempSettings("psg_tuning_wave"), nullptr);

    // The fine tune of the zone of the first instrument of `sf2` that plays `key`, or -1000 if no zone does.
    const auto fine = [](const Sf2Records& sf2, int key)
    {
        const size_t first = Le16(sf2.Record("inst", 22, 0) + 20) + 1;
        const size_t end = Le16(sf2.Record("inst", 22, 1) + 20);
        for (size_t z = first; z < end; z++)
        {
            const std::map<uint16_t, uint16_t> gens = sf2.ZoneGens(z);
            const int range = gens.at(sf2::kKeyRange);
            if (key >= (range & 0xFF) && key <= range >> 8)
            {
                const auto tune = gens.find(sf2::kFineTune);
                return tune != gens.end() ? int(int16_t(tune->second)) : 0;
            }
        }

        return -1000;
    };
    const auto cents = [](int setting, int key)
    {
        return int(std::lround(1200 * std::log2(131072.0 / (2048 - setting) / kMiddleC) - 100 * (key - 60)));
    };

    SEQLIB_CHECK(square.ok && wave.ok);
    const Sf2Records square_sf2 = ReadSf2(square.sf2_path);
    SEQLIB_CHECK_EQ(fine(square_sf2, 60), cents(flat, 60));
    SEQLIB_CHECK(cents(flat, 60) >= -21 && cents(flat, 60) <= -17);
    SEQLIB_CHECK_EQ(fine(square_sf2, 72), cents(octave, 72));

    // The wave plays on MIDI channel 3.
    const auto ons = ReadMidi(wave.midi_path).Find(0x92);
    SEQLIB_REQUIRE_EQ(ons.size(), size_t(1));
    SEQLIB_CHECK_EQ(int(ons[0].bytes[1]), 48);
    SEQLIB_CHECK_EQ(fine(ReadSf2(wave.sf2_path), 48), cents(flat, 60));
}

void TestLoopStarts()
{
    // Square 1 loops two notes from its start, and square 2 plays a note and then loops the same two notes. The loop
    // starts after square 2's first note, within a frame, where both channels are looping, lasts two notes, and plays
    // twice before the song ends.
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel().Bytes({0x7F, 90, 0x5E, 15, 0xBF}).Note(0, 3).Note(4, 3).End(),
                      Channel().Bytes({0x7F, 90, 0x5E, 15}).Note(2, 3).Bytes({0xBF}).Note(0, 3).Note(4, 3).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("loop_starts");

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    const double length = sum.loop_end - sum.loop_start;
    SEQLIB_CHECK(sum.ok && sum.loop_start > 0);
    SEQLIB_CHECK(std::fabs(sum.loop_start - length / 2) < 0.02);
    SEQLIB_CHECK(std::fabs(sum.seconds - (sum.loop_start + 2 * length)) < 0.001);
}

void TestLoopAfterTempoChange()
{
    // Square 1 plays two notes at tempo 200, slows to tempo 60 and loops two notes, and square 2 loops four notes from
    // its start, so that its first pass starts at the faster tempo. The loop starts at the second pass, a loop after
    // square 1's loop point, and the song still plays it twice in full.
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel()
                          .Bytes({0x7F, 200, 0x5E, 15})
                          .Note(0, 3)
                          .Note(2, 3)
                          .Bytes({0x7F, 60, 0xBF})
                          .Note(4, 3)
                          .Note(5, 3)
                          .End(),
                      Channel().Bytes({0x5E, 15, 0xBF}).Note(0, 3).Note(2, 3).Note(4, 3).Note(5, 3).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("loop_tempo");

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    const double length = sum.loop_end - sum.loop_start;
    SEQLIB_CHECK(sum.ok && sum.loop_start > length);
    SEQLIB_CHECK(std::fabs(sum.seconds - (sum.loop_start + 2 * length)) < 0.001);
}

void TestLoopLengths()
{
    // Square 1 loops three notes and square 2 four, both from the start, and the wave channel plays two notes and ends.
    // The loop starts where the wave channel ends, after two notes, and lasts until both squares are back where they
    // started, twelve notes, and the song plays it twice. The driver rounds the notes to frames, which the checks allow
    // for.
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel().Bytes({0x7F, 90, 0x5E, 15, 0xBF}).Note(0, 3).Note(2, 3).Note(4, 3).End(),
                      Channel().Bytes({0x7F, 90, 0x5E, 15, 0xBF}).Note(0, 3).Note(2, 3).Note(4, 3).Note(5, 3).End(),
                      Channel().Bytes({0x7F, 90, 0x5E, 15}).Note(7, 3).Note(9, 3).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("loop_lengths");

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    const double note = sum.loop_start / 2;
    SEQLIB_CHECK(sum.ok && sum.loop_start > 0);
    SEQLIB_CHECK(std::fabs(sum.loop_end - sum.loop_start - 12 * note) < 0.1);
    SEQLIB_CHECK(std::fabs(sum.seconds - (sum.loop_start + 24 * note)) < 0.1);
}

// Square 1's loop sets octave 2 after its first note, so the first time through, that note plays at the octave 3 set
// before the loop, and every time after at octave 2. The loop starts at the second pass, which the driver goes on
// repeating, and the song plays the first pass and then the loop twice. A loop that plays the same each time starts at
// its first pass.
void TestLoopAtSecondPass()
{
    for (const bool moves : {true, false})
    {
        Cart cart(Revision::kA);
        cart.Songs({Song({Channel()
                              .Bytes({0x7F, 90, 0x5E, 15, 0x4F, uint8_t(moves ? 3 : 2), 0xBF})
                              .Note(0, 3)
                              .Bytes({0x4F, 2})
                              .Note(4, 3)
                              .End()})});
        const Rom rom = cart.ToRom();
        const DriverInfo info = Detect(rom);
        ConvertSettings opt = TempSettings(moves ? "second_pass" : "first_pass");

        const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

        // The keys of the notes from the loop's start on.
        const MidiEvents midi = ReadMidi(sum.midi_path);
        const uint32_t loop_start = MarkerTick(midi, "loopStart", ~0u);
        std::vector<int> keys;
        for (const auto& [tick, bytes] : midi.Find(0x90))
        {
            if (tick >= loop_start)
            {
                keys.push_back(bytes[1]);
            }
        }

        // At tempo 90, the two quarter notes take 2 × 96 × 37 / 90 = 78.9 frames, which the driver plays in 78.
        const double kPass = 78 * 280896.0 / 16777216;
        SEQLIB_CHECK(sum.ok);
        SEQLIB_CHECK(std::fabs(sum.loop_start - (moves ? kPass : 0)) < 0.001);
        SEQLIB_CHECK(std::fabs(sum.loop_end - sum.loop_start - kPass) < 0.001);
        SEQLIB_CHECK(std::fabs(sum.seconds - sum.loop_start - 2 * kPass) < 0.001);
        SEQLIB_CHECK(keys == std::vector<int>({60, 64, 60, 64}));
    }
}

// Square 1 loops two quarter notes from its start, and square 2 the same after a rest of 56 ticks. At tempo 90, a frame
// is 90 of the MIDI file's ticks and a tick is 37, so square 2's loop point is 2 MIDI ticks into frame 23, and square
// 1's is at the start of frame 0. Each loop drops the part of a frame that the channel has counted, 84 MIDI ticks for
// square 1 and 86 for square 2, so square 2's second pass starts 2 ticks before the end of a loop from its loop point,
// and its first note would play twice at the seam. The loop starts at the second pass, where both squares start each
// pass as the one before.
void TestLoopSeam()
{
    Cart cart(Revision::kA);
    cart.Songs(
        {Song({Channel().Bytes({0x7F, 90, 0x5E, 15, 0x4F, 3, 0xBF}).Note(0, 3).Note(4, 3).End(),
               Channel().Bytes({0x5E, 15, 0x4F, 3}).Rest(5).Rest(10).Bytes({0xBF}).Note(7, 3).Note(11, 3).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("loop_seam");

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    // Each square's notes from the loop's start to its end.
    const MidiEvents midi = ReadMidi(sum.midi_path);
    const uint32_t loop_start = MarkerTick(midi, "loopStart", ~0u);
    const uint32_t loop_end = MarkerTick(midi, "loopEnd", 0);
    std::array<int, 2> notes = {};
    for (const int c : {0, 1})
    {
        for (const auto& [tick, bytes] : midi.Find(uint8_t(0x90 + c)))
        {
            notes[size_t(c)] += tick >= loop_start && tick < loop_end ? 1 : 0;
        }
    }

    // Square 1's first pass ends at frame 78, and the loop starts as much after that as square 2's loop point is into
    // the song, 2072 MIDI ticks, and lasts 78 frames.
    const double kFrame = 280896.0 / 16777216;
    SEQLIB_CHECK(sum.ok);
    SEQLIB_CHECK(std::fabs(sum.loop_start - (78 + 2072 / 90.0) * kFrame) < 1e-6);
    SEQLIB_CHECK(std::fabs(sum.loop_end - sum.loop_start - 78 * kFrame) < 1e-6);
    SEQLIB_CHECK(notes == (std::array<int, 2>{2, 2}));
}

// Square 1 plays a note at volume 15, and then loops a note at volume 15 and one at 8. The first time through, the
// loop's first note has the level that the note before it left, but a player that jumps back to the loop's start comes
// from the note at volume 8, so the level is written again there.
void TestLoopSettingsAgain()
{
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel()
                          .Bytes({0x7F, 90, 0x5E, 15, 0x4F, 3})
                          .Note(0, 3)
                          .Bytes({0xBF, 0x5E, 15})
                          .Note(4, 3)
                          .Bytes({0x5E, 8})
                          .Note(7, 3)
                          .End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("loop_again");

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    const MidiEvents midi = ReadMidi(sum.midi_path);
    const uint32_t loop_start = MarkerTick(midi, "loopStart", ~0u);
    std::vector<std::pair<uint32_t, int>> levels;
    for (const auto& [tick, bytes] : midi.Find(0xB0))
    {
        if (bytes[1] == cc::kExpression)
        {
            levels.emplace_back(uint32_t(tick), bytes[2]);
        }
    }

    // The setup's level of 0, the first note's, the same again at the loop's start, and then the note at volume 8's.
    SEQLIB_CHECK(sum.ok && loop_start > 0);
    SEQLIB_REQUIRE(levels.size() > 3);
    SEQLIB_CHECK(levels[2] == std::make_pair(loop_start, levels[1].second));
    SEQLIB_CHECK(levels[3].first > loop_start && levels[3].second < levels[2].second);
}

void TestHourLimit()
{
    // A song asked to play its loop, a whole note at tempo 6 or 384 × 37 / 6 = 2368 frames, a hundred times stops where
    // the model gives up, after 216000 frames of 280896 cycles at 16.78 MHz, and says so.
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel().Bytes({0x7F, 6, 0x5E, 15, 0xBF}).Note(0, 0).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("hour");
    opt.loops = 100;

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    const auto cut_off = [](const std::string& w)
    {
        return w.find("cut off after an hour") != std::string::npos;
    };
    SEQLIB_CHECK(sum.ok && std::any_of(sum.warnings.begin(), sum.warnings.end(), cut_off));
    SEQLIB_CHECK(std::fabs(sum.seconds - 216000 * 280896.0 / 16777216) < 0.1);
}

void TestRegisters()
{
    // A note on square 1: the frequency table's entry for octave 2's C, and the note's volume, duty and length.
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel().Bytes({0x5E, 12, 0x2F, 1}).Note(0, 3).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const auto writes = Writes(rom, info, 0, 2);

    SEQLIB_CHECK_EQ(LastWrite(writes[1], 0x04000062), 0xC87F);
    SEQLIB_CHECK_EQ(LastWrite(writes[1], 0x04000064), 0x8000 | rom.U16(kFrequencyTable + 48));
}

void TestWaveVolumes()
{
    // The wave channel at volume 3 plays at 100% in the A revision, and at 75% in the J revision, which has 4 for 100%.
    // NR32's volume code is the top bits of NR31's halfword, which the driver fills with the length × 4.
    for (Revision revision : {Revision::kA, Revision::kJ})
    {
        Cart cart(revision);
        cart.Songs(
            {Song({{0xFF}, {0xFF}, Channel().Bytes({0xDA, 0x00, 0x5E, 3}).Note(0, 3).Bytes({0x5F}).Note(0, 3).End()})});
        const Rom rom = cart.ToRom();
        const DriverInfo info = Detect(rom);

        const auto writes = Writes(rom, info, 0, 60);
        const long long first = LastWrite(writes[1], 0x04000072);
        long long second = -1;
        for (size_t f = 2; f < writes.size() && second < 0; f++)
        {
            second = LastWrite(writes[f], 0x04000072);
        }

        if (revision == Revision::kA)
        {
            SEQLIB_CHECK_EQ(first, 0x20FC);
            SEQLIB_CHECK_EQ(second, 0x20FC);
        }
        else
        {
            SEQLIB_CHECK_EQ(first, 0x80FC);
            SEQLIB_CHECK_EQ(second, 0x20FC);
        }
    }
}

void TestPcm()
{
    // A PCM note points FIFO A's DMA 32 bytes into the sample, the 32 before going into the FIFO, and sets timer 0 for
    // the sample's rate. The J revision sets the timer at the end of the frame.
    for (Revision revision : {Revision::kA, Revision::kJ})
    {
        Cart cart(revision);
        cart.Songs({Song({{0xFF}, {0xFF}, {0xFF}, {0xFF}, Channel().Bytes({0xDC, 0x00, 0x5E, 1}).Note(12, 3).End()})});
        const std::vector<uint32_t> data = cart.Samples(kSampleFile, {{Saw(4000), 10000}});
        const Rom rom = cart.ToRom();
        const DriverInfo info = Detect(rom);

        const auto writes = Writes(rom, info, 0, 2);
        const std::vector<RegisterWrite>& frame = writes[1];

        SEQLIB_CHECK_EQ(LastWrite(frame, 0x040000BC), data[0] + 32);
        SEQLIB_CHECK_EQ(LastWrite(frame, 0x040000C6), 0xB640);
        SEQLIB_CHECK_EQ(LastWrite(frame, 0x04000100), 0x800000 | (0x10000 - 16780000 / 10000));
        SEQLIB_CHECK(!frame.empty() && (frame.back().address == 0x04000100) == (revision == Revision::kJ));
    }

    // The driver checks a FIFO once a frame, and stops or loops it when the next frame would reach the end: from 0, at
    // 600 60ths of a byte a frame and an end of 100 bytes, it does so at the 9th check, after 8 frames.
    SEQLIB_CHECK_EQ(FifoFrames(0, 600, 100), 8);
    SEQLIB_CHECK_EQ(FifoFrames(5000, 600, 100), 0);
    SEQLIB_CHECK_EQ(FifoFrames(0, 0, 100), 0);
}

// An audition plays one note with the instrument, the pitch and the volume given, as the channel's commands would: on
// a FIFO, the sample at index 24 plays at its rate. Its release ends the channel. On the player, a square's audition
// sounds until its release, and the player then stops.
void TestAudition()
{
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel().Bytes({0x7F, 90}).Rest(3).End()})});
    const std::vector<uint32_t> data = cart.Samples(kSampleFile, {{Saw(4000), 10000}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    seq.Audition(4, 0, 24, 1);
    const std::vector<RegisterWrite> frame = seq.Step();
    const bool playing = !seq.Ended();
    seq.ReleaseAudition();
    seq.Step();

    SEQLIB_CHECK_EQ(LastWrite(frame, 0x040000BC), data[0] + 32);
    SEQLIB_CHECK_EQ(LastWrite(frame, 0x040000C6), 0xB640);
    SEQLIB_CHECK_EQ(LastWrite(frame, 0x04000100), 0x800000 | (0x10000 - 16780000 / 10000));
    SEQLIB_CHECK(playing && seq.Ended());

    ArchivePlayer player(rom, std::span<const DriverInfo>(&info, 1));
    player.StartNote(0, 0, 2, 24, 15);
    std::vector<int16_t> out;
    for (int f = 0; f < 4; f++)
    {
        player.RunFrame(out);
    }
    const bool sounds = std::any_of(out.begin(), out.end(), [&out](int16_t v) { return v != out.front(); });
    const bool busy = player.Busy();
    player.ReleaseNote();
    int frames = 0;
    for (; frames < 10 && player.Busy(); frames++)
    {
        player.RunFrame(out);
    }

    SEQLIB_CHECK(sounds && busy);
    SEQLIB_CHECK(frames > 0 && frames < 10);

    // An audition that can't start leaves the music file of the song that plays.
    DriverInfo second = info;
    second.first_song = SongCount(info);
    const std::vector<DriverInfo> files = {info, second};
    ArchivePlayer two(rom, files);
    two.Start(0);
    bool refused = false;
    try
    {
        two.StartNote(SongCount(info), 9, 2, 24, 15);
    }
    catch (const std::out_of_range&)
    {
        refused = true;
    }
    SEQLIB_CHECK(refused && two.Info().first_song == 0);
}

// A FIFO's instrument is a sample of the bank, the wave channel's a wave of the wave table, and a square's a duty. A
// sample of 106 bytes with its header plays (106 - 64) × 0.994 = 41 of them. The second wave is a square, and bit 7 of
// its number picks the wave RAM's bank.
void TestInstruments()
{
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel().Bytes({0x7F, 90}).Rest(3).End()})});
    const std::vector<uint32_t> data = cart.Samples(kSampleFile, {{Saw(100), 10000}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const Instrument pcm = LookUpInstrument(rom, info, 5, 0);
    const Instrument missing = LookUpInstrument(rom, info, 4, 1);
    const Instrument wave = LookUpInstrument(rom, info, 2, 0x81);
    const Instrument square = LookUpInstrument(rom, info, 1, 3);
    const auto sample = ReadSample(rom, info, 0, 0);
    const auto shape = ReadWave(rom, info, 0x81);

    SEQLIB_CHECK(pcm.kind == Instrument::Kind::kPcm && pcm.address == data[0] - 6 && missing.address == 0);
    SEQLIB_CHECK(wave.kind == Instrument::Kind::kWave && wave.address == FileAddress(kWaveFile) + 18);
    SEQLIB_CHECK(square.kind == Instrument::Kind::kSquare && square.number == 3);
    SEQLIB_REQUIRE(sample && shape);
    SEQLIB_CHECK(sample->data == data[0] && sample->size == 100 && sample->played == 41 && sample->rate == 10000);
    SEQLIB_CHECK((*shape)[0] == 0xFF && (*shape)[8] == 0);
    SEQLIB_CHECK(!ReadSample(rom, info, 0, 1));
}

void TestBanks()
{
    // In the J revision, F0 switches a PCM channel to the sound effects' samples, which the samples it then selects
    // come from.
    Cart cart(Revision::kJ);
    cart.Songs({Song({{0xFF},
                      {0xFF},
                      {0xFF},
                      {0xFF},
                      Channel()
                          .Bytes({0xDC, 0x00, 0x5E, 1})
                          .Note(12, 3)
                          .Bytes({0xF0, 0xDC, 0x01})
                          .Note(12, 3)
                          .Bytes({0xDC, 0x00})
                          .Note(12, 3)
                          .End()})});
    const std::vector<uint32_t> music = cart.Samples(kSampleFile, {{Saw(1500), 10000}, {Saw(1500), 10000}});
    const std::vector<uint32_t> effects = cart.Samples(kSfxSampleFile, {{Saw(1500), 10000}, {Saw(1500), 10000}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const auto writes = Writes(rom, info, 0, 200);
    std::vector<long long> sources;
    for (size_t f = 1; f < writes.size(); f++)
    {
        const long long source = LastWrite(writes[f], 0x040000BC);
        if (source >= 0)
        {
            sources.push_back(source - 32);
        }
    }

    SEQLIB_REQUIRE(sources.size() >= 3);
    SEQLIB_CHECK_EQ(sources[0], music[0]);
    SEQLIB_CHECK_EQ(sources[1], effects[1]);
    SEQLIB_CHECK_EQ(sources[2], effects[0]);
}

void TestNoiseDrums()
{
    // D2 fades the noise channel from volume 15 to 0 over 3 frames, restarting it at each new volume, and the drum's
    // sample has those volumes in it. Two notes with the same writes share a drum.
    Cart cart(Revision::kA);
    cart.Songs({Song(
        {{0xFF}, {0xFF}, {0xFF}, Channel().Bytes({0x5E, 15, 0xD2, 100, 0, 0, 3, 1}).Note(12, 3).Note(12, 3).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("noise");

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    const Sf2Records sf2 = ReadSf2(sum.sf2_path);
    const MidiEvents midi = ReadMidi(sum.midi_path);
    const auto ons = midi.Find(0x93);
    SEQLIB_CHECK(sum.ok);
    SEQLIB_CHECK(ons.size() == 2 && ons[0].bytes[1] == ons[1].bytes[1]);
    SEQLIB_CHECK_EQ(sf2.Count("shdr", 46), 2);

    // The sample's peak in each of its first 4 frames of 1097 points.
    const std::vector<uint8_t>& pcm = sf2.chunks.at("smpl");
    const uint32_t start = Le32(sf2.Record("shdr", 46, 0) + 20);
    std::vector<int> volumes;
    for (uint32_t f = 0; f < 4; f++)
    {
        int peak = 0;
        for (uint32_t i = 100; i < 1000; i++)
        {
            const uint32_t at = 2 * (start + f * 1097 + i);
            peak = std::max(peak, std::abs(int(int16_t(Le16(&pcm[at])))));
        }
        volumes.push_back(int(std::lround(peak * 15.0 / 32767)));
    }

    SEQLIB_CHECK(volumes == std::vector<int>({15, 10, 5, 0}));
}

void TestNoiseMacros()
{
    // A noise macro sets NR43 a frame at a time, restarting the channel with the note, and its end silences the
    // channel. In the J revision, F0 switches the noise channel to the sound effects' macros.
    for (Revision revision : {Revision::kA, Revision::kJ})
    {
        const bool j = revision == Revision::kJ;
        Cart cart(revision);
        std::vector<uint8_t> commands = {0x5E, 15, 0xDB, 0x01};
        if (j)
        {
            commands.insert(commands.begin(), 0xF0);
        }
        cart.Songs({Song({{0xFF}, {0xFF}, {0xFF}, Channel().Bytes(commands).Note(12, 3).End()})});
        cart.Macros(kMacroFile, {{0x27, 0x35, 0xFF}});
        cart.Macros(kSfxMacroFile, {{0x45, 0x56, 0xFF}});
        const Rom rom = cart.ToRom();
        const DriverInfo info = Detect(rom);

        const auto writes = Writes(rom, info, 0, 4);

        SEQLIB_CHECK_EQ(LastWrite(writes[1], 0x0400007C), j ? 0x8045 : 0x8027);
        SEQLIB_CHECK_EQ(LastWrite(writes[2], 0x0400007C), j ? 0x0056 : 0x0035);
        SEQLIB_CHECK_EQ(LastWrite(writes[3], 0x0400007C), 0x8000);
        SEQLIB_CHECK_EQ(LastWrite(writes[3], 0x04000078) >> 12, 0);
    }
}

void TestConversion()
{
    // A song with a loop: the tempo, the loop markers, and the level of a square that plays on the left only, in CC10
    // and CC11. A sweep down lowers the pitch within each frame, which the MIDI file bends to between the frame's
    // changes.
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel()
                          .Bytes({0x7F, 90, 0x5E, 15, 0xD0, 0xEE, 0x10, 0xBF, 0x1F, 1, 0x0E, 3, 0x1E, 0x4F, 5})
                          .Note(0, 3)
                          .Note(4, 3)
                          .End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("conversion");

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    const MidiEvents midi = ReadMidi(sum.midi_path);
    const auto tempos = midi.Find(0xFF);
    const auto controllers = midi.Find(0xB0);
    const auto bends = midi.Find(0xE0);

    int start_cc10 = -1, cc10 = -1, cc11 = -1;
    for (const auto& [tick, bytes] : controllers)
    {
        start_cc10 = bytes[1] == cc::kPan && tick == 0 ? bytes[2] : start_cc10;
        cc10 = bytes[1] == cc::kPan && tick > 0 ? bytes[2] : cc10;
        cc11 = bytes[1] == cc::kExpression && tick == 0 && bytes[2] ? bytes[2] : cc11;
    }

    bool between = false;
    for (const auto& [tick, bytes] : bends)
    {
        between = between || tick % 90 != 0;
    }

    const auto is_tempo = [](const auto& e)
    {
        return e.bytes.size() > 5 && e.bytes[1] == 0x51 && (e.bytes[3] << 16 | e.bytes[4] << 8 | e.bytes[5]) == 660779;
    };
    const auto is_marker = [](const auto& e)
    {
        return e.bytes.size() > 1 && e.bytes[1] == 0x06;
    };

    // A level of 30 of 128 on the left alone is panned hard left, and CC11 is 127 × sqrt(30/128 / (sqrt(2) × 127/128)),
    // about 52.
    SEQLIB_CHECK(sum.ok && sum.loop_start == 0 && sum.loop_end > 0);
    SEQLIB_CHECK_EQ(cc11, 52);
    SEQLIB_CHECK_EQ(start_cc10, 0);
    SEQLIB_CHECK_EQ(cc10, -1);
    SEQLIB_CHECK(between);
    SEQLIB_CHECK(std::any_of(tempos.begin(), tempos.end(), is_tempo));
    SEQLIB_CHECK(std::any_of(tempos.begin(), tempos.end(), is_marker));
}

// ConvertSongFiles gives the bytes of the files that ConvertSong writes, and names no paths. A song without notes gives
// no bytes.
void TestSongFiles()
{
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel().Bytes({0x7F, 90, 0x5E, 15}).Note(0, 3).Note(4, 3).End()}),
                Song({Channel().Bytes({0x7F, 90}).Rest(3).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const ConvertSettings opt = TempSettings("song_files");

    const SongReport written = ConvertSong(rom, info, 0, opt, nullptr);
    const SongFiles files = ConvertSongFiles(rom, info, 0, opt);
    const SongFiles none = ConvertSongFiles(rom, info, 1, opt);

    SEQLIB_REQUIRE(written.ok && files.report.ok && !files.report.silent);
    SEQLIB_CHECK(files.report.midi_path.empty() && files.report.sf2_path.empty());
    SEQLIB_CHECK(!files.midi.empty() && files.midi == ReadAll(written.midi_path));
    SEQLIB_CHECK(!files.sf2.empty() && files.sf2 == ReadAll(written.sf2_path));
    SEQLIB_CHECK(files.report.warnings == written.warnings && files.report.seconds == written.seconds);
    SEQLIB_CHECK(none.report.silent && none.midi.empty() && none.sf2.empty());
}

// The listing follows the driver's repeats. A count of 0 makes 256 passes there. The listing also says what a range of
// 0, 0 and 0 plays.
void TestDump()
{
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel().Bytes({0x7F, 90, 0xD6}).Note(0, 3).Bytes({0xD7, 2}).End(),
                      Channel().Bytes({0xE7, 0, 0, 0, 0xD6}).Note(0, 3).Bytes({0xD7, 0}).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const std::string path = Utf8(TempPath("dump.txt"));
    std::string error;

    SEQLIB_CHECK(DumpSong(rom, info, 0, path, error));

    const std::vector<uint8_t> text = ReadAll(path);
    const std::string s(text.begin(), text.end());
    SEQLIB_CHECK(s.find("tempo 90, for every channel") != std::string::npos);
    SEQLIB_CHECK(s.find("0x08007011       96  30                 note C, 96 ticks (1/4)") != std::string::npos);
    SEQLIB_CHECK(s.find("0x08007014      192  FF                 end, or back to the loop point") != std::string::npos);
    SEQLIB_CHECK(s.find("play the sample once, from the last range's start, or from its start after DC or DD") !=
                 std::string::npos);
    SEQLIB_CHECK(s.find("play the repeat 256 times") != std::string::npos);
    SEQLIB_CHECK(s.find("   24576  FF ") != std::string::npos);
}

// The sound effects come after the songs. A sound effect starts on the sound effect channels whose data doesn't start
// with FF, with no music playing. It reads the sound effects' samples and noise macros. When a PSG channel ends, the
// driver writes its level of 0 straight to the envelope register. The output stage then writes the music's image of
// 0s. The channels that a sound effect leaves to the music don't keep the conversion from finding its loop.
void TestEffects()
{
    for (Revision revision : {Revision::kA, Revision::kJ})
    {
        const bool j = revision == Revision::kJ;
        Cart cart(revision);
        cart.Songs({Song({Channel().Rest(4).End()})});
        cart.Effects({Song({Channel().Bytes({0x5E, 15}).Note(0, 3).End()}),
                      Song({{0xFF}, {0xFF}, {0xFF}, {0xFF}, Channel().Bytes({0xDC, 0x01, 0x5E, 1}).Note(12, 3).End()}),
                      Song({{0xFF}, {0xFF}, {0xFF}, Channel().Bytes({0x5E, 15, 0xDB, 0x01}).Note(12, 3).End()}),
                      Song({{0xFF}, Channel().Bytes({0x5E, 15, 0xBF}).Note(0, 3).End()})});
        cart.Samples(kSampleFile, {{Saw(1500), 10000}, {Saw(1500), 10000}});
        const std::vector<uint32_t> effect_samples =
            cart.Samples(kSfxSampleFile, {{Saw(1500), 10000}, {Saw(1500), 10000}});
        cart.Macros(kMacroFile, {{0x27, 0x35, 0xFF}});
        cart.Macros(kSfxMacroFile, {{0x45, 0x56, 0xFF}});
        const Rom rom = cart.ToRom();
        const DriverInfo info = Detect(rom);

        SEQLIB_CHECK_EQ(info.play_effect, kPlayEffect);
        SEQLIB_CHECK_EQ(info.effects_file, FileAddress(kEffectsFile));
        SEQLIB_REQUIRE_EQ(info.effect_addresses.size(), 4);
        SEQLIB_CHECK_EQ(SongCount(info), 5);
        SEQLIB_CHECK_EQ(EffectIndex(info, 0), -1);
        SEQLIB_CHECK_EQ(EffectIndex(info, 2), 1);
        SEQLIB_CHECK_EQ(SongAddress(info, 1), FileAddress(kEffectsFile));
        SEQLIB_CHECK_EQ(info.macros[1], FileAddress(kSfxMacroFile));
        SEQLIB_CHECK_EQ(info.samples[1], FileAddress(kSfxSampleFile));

        // Square 1 plays at level 15 from the first frame. In the frame that it ends, the driver writes NR12 with a
        // level of 0, and the music's image clears NR10 to NR14.
        Sequencer square(rom, info, 1);
        SEQLIB_CHECK(square.StartWrites().empty());
        SEQLIB_CHECK(square.GetChannel(0).position != 0 && square.GetChannel(1).position == 0);
        SEQLIB_CHECK_EQ(LastWrite(square.Step(), 0x04000062) >> 12, 15);
        std::vector<RegisterWrite> last;
        for (int f = 0; f < 100 && !square.Ended(); f++)
        {
            last = square.Step();
        }
        std::vector<uint32_t> nr12;
        for (const RegisterWrite& w : last)
        {
            if (w.address == 0x04000062)
            {
                nr12.push_back(w.value);
            }
        }

        SEQLIB_CHECK(square.Ended());
        SEQLIB_REQUIRE_EQ(nr12.size(), 2);
        SEQLIB_CHECK_EQ(nr12[0], 0x00BF);
        SEQLIB_CHECK_EQ(nr12[1], 0);
        SEQLIB_CHECK_EQ(LastWrite(last, 0x04000064), 0);

        // The PCM channel plays the sound effects' sample 1, and the noise channel their macro 1.
        const auto pcm = Writes(rom, info, 2, 2);
        const auto noise = Writes(rom, info, 3, 2);

        SEQLIB_CHECK_EQ(LastWrite(pcm[1], 0x040000BC), effect_samples[1] + 32);
        SEQLIB_CHECK_EQ(LastWrite(noise[1], 0x0400007C), 0x8045);

        // Square 2 loops, and the plan doesn't wait for the channels that play nothing.
        const SongReport report = InspectSong(rom, info, 4, TempSettings(j ? "effect_j" : "effect_a"));

        SEQLIB_CHECK(report.ok && report.tracks == 1 && report.warnings.empty());
        SEQLIB_CHECK(report.loop_end > 0 && report.seconds < 10);
    }
}

// Detection finds Quintet's driver from its code. A game without the driver's code isn't read, even with the music file
// given.
void TestDetectionFromCode()
{
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel().Note(0, 3).End()})});
    const Rom rom = cart.ToRom();
    Cart bare(Revision::kA, false);
    bare.Songs({Song({Channel().Note(0, 3).End()})});
    const Rom bare_rom = bare.ToRom();
    DriverOverrides table;
    table.song_table = FileAddress(kMusicFile);
    DriverInfo info, bare_info;
    std::string error, table_error;

    SEQLIB_CHECK(DetectDriver(rom, DriverOverrides(), info, error) && info.play != 0);
    SEQLIB_CHECK(SongCount(info) == 1);
    SEQLIB_CHECK(!DetectDriver(bare_rom, table, bare_info, table_error));
    SEQLIB_CHECK(bare_info.play == 0 && table_error == "no Quintet sound driver found, so it has no length table");
}

// A game whose driver of the J revision loads a music file gives the driver its tables in the file. The game's code
// passes the loader one file, and a scan of the ROM finds another. ReadMusicFiles numbers the second file's songs after
// the first file's songs and sound effects, and the conversion names their files by those numbers.
void TestLoadedFiles()
{
    Cart cart(Revision::kJ);
    const std::vector<uint8_t> song = Song({Channel().Bytes({0x5E, 15}).Note(0, 3).End()});
    const std::array<uint32_t, 7> first = cart.MusicFile(8, {song, song}, {song});
    const std::array<uint32_t, 7> second = cart.MusicFile(10, {song}, {});
    cart.Loader(8);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const std::vector<DriverInfo> files = ReadMusicFiles(rom, info);
    const ConvertSettings opt = TempSettings("loaded_files");
    SEQLIB_REQUIRE(files.size() == 2);
    SEQLIB_REQUIRE(FileOfSong(files, 3) == 1 && files[1].first_song == 3);

    const SongReport last = ConvertSong(rom, files[1], 3 - files[1].first_song, opt, nullptr);

    SEQLIB_CHECK(info.music_files == (std::vector<uint32_t>{FileAddress(8), FileAddress(10)}));
    SEQLIB_CHECK_EQ(info.music_file, first[0]);
    SEQLIB_CHECK_EQ(info.effects_file, first[1]);
    SEQLIB_CHECK_EQ(info.waves, first[2]);
    SEQLIB_CHECK(info.macros == (std::array<uint32_t, 2>{first[3], first[4]}));
    SEQLIB_CHECK(info.samples == (std::array<uint32_t, 2>{first[5], first[6]}));
    SEQLIB_CHECK_EQ(info.song_addresses.size(), 2);
    SEQLIB_CHECK_EQ(info.effect_addresses.size(), 1);
    SEQLIB_CHECK_EQ(info.sample_addresses[0].size(), 1);
    SEQLIB_CHECK_EQ(info.sample_addresses[1].size(), 1);
    SEQLIB_CHECK_EQ(FileOfSong(files, 2), 0);
    SEQLIB_CHECK_EQ(SongAddress(files[0], 2), first[1]);
    SEQLIB_CHECK_EQ(SongAddress(files[1], 0), second[0]);
    SEQLIB_CHECK(last.ok && !last.silent);
    SEQLIB_CHECK(last.midi_path == SongStem(opt, SongNumber(3, 0)) + ".mid");
}

// The S revision: the A revision's play routine with notes of two bytes and the commands numbered from 80. The game
// passes its files to the driver from literals. The channel sets tempo 90 (8F = 7F), volume 15 (8A = 5E) and an
// envelope step of 3 (86 = 3E). Then C plays for an eighth note, with a dot (9B = D1) and a tie (9E = D4) to another
// eighth: 48 + 24 + 48 = 120 ticks. B0 skips the byte after it. That byte would otherwise set the volume. The note
// played again lasts a quarter note, and so does the rest. Pitch 12 only waits for an eighth, and C follows at tick
// 360.
void TestRevisionS()
{
    Cart cart(Revision::kS);
    cart.Songs({Song({Channel()
                          .Bytes({0x8F, 90,   0x8A, 15, 0x86, 3,  0x00, 8,  0x9B, 0x9E, 0x00,
                                  8,    0xB0, 0x8A, 14, 4,    15, 4,    12, 8,    0x00, 4})
                          .End()})});
    cart.Effects({Song({Channel().Bytes({0x00, 4}).End()})});
    cart.Samples(kSampleFile, {{Saw(300), 8000}});
    cart.Samples(kSfxSampleFile, {{Saw(300), 8000}, {Saw(300), 8000}});
    const Rom rom = cart.ToRom();

    const DriverInfo info = Detect(rom);

    SEQLIB_CHECK(info.revision == Revision::kS);
    SEQLIB_CHECK_EQ(info.play, kPlay);
    SEQLIB_CHECK_EQ(info.play_effect, kPlayEffect);
    SEQLIB_CHECK_EQ(info.length_table, 0);
    SEQLIB_CHECK_EQ(info.music_file, FileAddress(kMusicFile));
    SEQLIB_CHECK_EQ(info.effects_file, FileAddress(kEffectsFile));
    SEQLIB_CHECK_EQ(info.waves, FileAddress(kWaveFile));
    SEQLIB_CHECK(info.macros == (std::array<uint32_t, 2>{FileAddress(kMacroFile), FileAddress(kSfxMacroFile)}));
    SEQLIB_CHECK(info.samples == (std::array<uint32_t, 2>{FileAddress(kSampleFile), FileAddress(kSfxSampleFile)}));
    SEQLIB_CHECK_EQ(info.sample_addresses[1].size(), 2);
    SEQLIB_CHECK(info.warnings.empty());

    Sequencer seq(rom, info, 0);
    std::vector<Event> events;
    long long step = -1;
    for (int f = 0; f < 200; f++)
    {
        const std::vector<RegisterWrite>& writes = seq.Step();
        for (const Event& e : seq.Events())
        {
            events.push_back(e);
        }
        if (!events.empty() && events.back().kind == Event::kRest && step < 0)
        {
            step = LastWrite(writes, 0x04000062);
        }
    }

    std::vector<std::pair<Event::Kind, uint64_t>> notes;
    for (const Event& e : events)
    {
        if (e.kind == Event::kNote || e.kind == Event::kPlayAgain || e.kind == Event::kRest)
        {
            notes.emplace_back(e.kind, e.tick);
        }
    }

    // After the rest, the square channel keeps its envelope step: the S revision has no flag that clears it.
    SEQLIB_CHECK(notes == (std::vector<std::pair<Event::Kind, uint64_t>>{
                              {Event::kNote, 0}, {Event::kPlayAgain, 120}, {Event::kRest, 216}, {Event::kNote, 360}}));
    SEQLIB_CHECK(step >= 0 && (step >> 8 & 7) == 3 && (step >> 12) == 0);

    // The listing reads the notes and commands as the driver does.
    const uint32_t data = SongAddress(info, 0) + 14;
    const Command tempo = DecodeCommand(rom, info, data, 0);
    const Command note = DecodeCommand(rom, info, data + 6, 0);
    const Command dot = DecodeCommand(rom, info, data + 8, 0);
    const Command skip = DecodeCommand(rom, info, data + 12, 0);
    const Command again = DecodeCommand(rom, info, data + 14, 0);
    const Command wait = DecodeCommand(rom, info, data + 18, 0);

    SEQLIB_CHECK(tempo.size == 2 && tempo.command == 0x7F && tempo.text == "tempo 90, for every channel");
    SEQLIB_CHECK(note.note && note.size == 2 && note.ticks == 48 && note.text == "note C, 48 ticks (1/8)");
    SEQLIB_CHECK(dot.command == 0xD1 && dot.size == 1);
    SEQLIB_CHECK(skip.size == 2 && skip.text == "nothing: skips a byte");
    SEQLIB_CHECK(again.note && again.text == "play again, 96 ticks (1/4)");
    SEQLIB_CHECK(wait.note && wait.text == "wait, 48 ticks (1/8)");
}

// A PCM sound that plays 14 bytes and then loops over 6 gets its loop repeated 6 times: 36 points, the first length of
// kMinLoop points or more.
void TestShortLoop()
{
    ntdlib::testing::agb_files::CartImage image(0x20);
    for (uint32_t i = 0; i < 20; i++)
    {
        image.Put8(kRomBase + i, uint8_t(i * 9));
    }
    const Rom rom = image.ToRom();
    SoundfontBuilder sf(rom);
    PcmSound sound;
    sound.start = kRomBase;
    sound.first = 14;
    sound.loop = 14;
    sound.loop_length = 6;
    sound.looped = true;
    sound.rate = 16384;
    const int index = sf.PcmSample(sound);
    SEQLIB_REQUIRE(index >= 0);
    SEQLIB_CHECK(ntdlib::testing::agb_files::LengthenedLoop(sf.File().samples[size_t(index)], 14, 6));
}

} // namespace

int RunTests()
{
    TestDetection();
    TestDecoding();
    TestTiming();
    TestHold();
    TestCommandGuard();
    TestWaveSwitch();
    TestPsgTuning();
    TestLoopStarts();
    TestLoopAfterTempoChange();
    TestLoopLengths();
    TestLoopAtSecondPass();
    TestLoopSeam();
    TestLoopSettingsAgain();
    TestHourLimit();
    TestRegisters();
    TestWaveVolumes();
    TestPcm();
    TestAudition();
    TestInstruments();
    TestBanks();
    TestNoiseDrums();
    TestNoiseMacros();
    TestConversion();
    TestSongFiles();
    TestDump();
    TestEffects();
    TestDetectionFromCode();
    TestLoadedFiles();
    TestRevisionS();
    TestShortLoop();

    return seqlib::testing::Summary("quintet");
}

} // namespace ntdlib::sequence_export::quintet

int main()
{
    return ntdlib::sequence_export::quintet::RunTests();
}
