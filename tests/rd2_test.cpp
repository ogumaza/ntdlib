// SPDX-License-Identifier: MIT

// Unit tests for Nintendo R&D2's driver. They build small synthetic cartridges in memory. No game ROM is needed.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "seqlib/testing/check.h"
#include "xsflib/text.h"

#include "ntdlib/agb/files.h"
#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/song_plan.h"
#include "ntdlib/rd2/driver.h"
#include "ntdlib/rd2/sequencer.h"
#include "ntdlib/rd2/song.h"
#include "ntdlib/sequence_export/agb/midi.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/agb/sf2.h"
#include "ntdlib/sequence_export/rd2/convert.h"
#include "ntdlib/sequence_export/rd2/soundfont.h"
#include "ntdlib/testing/agb_cart.h"
#include "ntdlib/testing/agb_files.h"
#include "ntdlib/testing/rd2_cart.h"

namespace ntdlib::sequence_export::rd2
{
namespace
{

using namespace ntdlib::testing::rd2_cart;

namespace cc = seqlib::midi::cc;
using ntdlib::agb::RegisterWrite;
using ntdlib::agb::Rom;
using ntdlib::agb::Utf8;
using ntdlib::rd2::DetectDriver;
using ntdlib::rd2::DriverInfo;
using ntdlib::rd2::DriverOverrides;
using ntdlib::rd2::DumpSong;
using ntdlib::rd2::Event;
using ntdlib::rd2::kEffectEcho;
using ntdlib::rd2::kEffectPriority;
using ntdlib::rd2::Revision;
using ntdlib::rd2::Sequencer;
using ntdlib::rd2::SongCount;
using ntdlib::sequence_export::agb::ConvertSettings;
using ntdlib::sequence_export::agb::SongReport;
using ntdlib::sequence_export::agb::SongVersion;
using ntdlib::testing::agb_files::HasGlobalZoneModulators;
using ntdlib::testing::agb_files::Le32;
using ntdlib::testing::agb_files::MidiEvents;
using ntdlib::testing::agb_files::ReadAll;
using ntdlib::testing::agb_files::ReadMidi;
using ntdlib::testing::agb_files::ReadSf2;
using ntdlib::testing::agb_files::Sf2Records;
using ntdlib::testing::agb_files::TempPath;
using ntdlib::testing::agb_files::TempSettings;

// Returns the driver's tables in `rom`. The ROM must have them.
DriverInfo Detect(const Rom& rom)
{
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, DriverOverrides(), info, error));

    return info;
}

// Runs version `version` of a sequence for `frames` frames, and returns each frame's events.
std::vector<std::vector<Event>> Run(const Rom& rom, const DriverInfo& info, int sequence, int frames, int version = 0)
{
    Sequencer seq(rom, info, sequence, version);
    std::vector<std::vector<Event>> out;
    for (int f = 0; f < frames; f++)
    {
        seq.Step();
        out.push_back(seq.Events());
    }

    return out;
}

void TestDetection()
{
    // The driver's code gives its tables, and the game's call to its init gives its settings.
    Cart cart;
    cart.Sequences({{TrackData().End()}, {TrackData().End()}});
    const Rom rom = cart.ToRom();
    DriverInfo info;
    std::string error;

    const bool found = DetectDriver(rom, DriverOverrides(), info, error);

    SEQLIB_CHECK(found);
    SEQLIB_CHECK_EQ(info.init, kInit);
    SEQLIB_CHECK_EQ(info.settings, kSettings);
    SEQLIB_CHECK_EQ(info.pitch_table, kPitchTable);
    SEQLIB_CHECK_EQ(info.frequency_table, kFrequencyTable);
    SEQLIB_CHECK_EQ(info.noise_table, kNoiseTable);
    SEQLIB_CHECK_EQ(info.lfo_table, kLfoTable);
    SEQLIB_CHECK_EQ(info.wave_volumes, kWaveVolumes);
    SEQLIB_CHECK_EQ(info.voice_classes, kVoiceClasses);
    SEQLIB_CHECK_EQ(info.key_envelope, kKeyEnvelope);
    SEQLIB_REQUIRE_EQ(info.sequence_addresses.size(), 2);
    SEQLIB_CHECK_EQ(info.sequence_addresses[0], kSequenceData);

    // A game without the driver's code has no sign of it, unless it's given the settings.
    Cart bare(false);
    bare.Sequences({{TrackData().End()}});
    const Rom none = bare.ToRom();
    DriverOverrides given;
    given.settings = kSettings;
    DriverInfo nothing, tables;

    SEQLIB_CHECK(!DetectDriver(none, DriverOverrides(), nothing, error));
    SEQLIB_CHECK(error.empty());
    SEQLIB_CHECK(!DetectDriver(none, given, tables, error));
    SEQLIB_CHECK(!error.empty());

    // Detection starts from nothing. A game without the driver keeps none of an earlier game's tables.
    SEQLIB_CHECK(!DetectDriver(none, DriverOverrides(), info, error));
    SEQLIB_CHECK(info.init == 0 && info.sequence_addresses.empty());
}

void TestSuperMarioAdvance2()
{
    // The Super Mario Advance 2 revision's code gives its tables too. Its E4 takes a byte. A tempo of 150 plays a
    // tick a frame. Its EA does nothing and takes no operand.
    Cart cart(true, Revision::kSuperMarioAdvance2);
    cart.Sequences({{TrackData()
                         .Bytes({0xE4, 150, 0xEA, 0xC2, kLooped, 0xE2, 2, 0xE1, 64})
                         .Note(60, 4, 127)
                         .Wait(4)
                         .Bytes({0xE1, uint8_t(-64)})
                         .Note(60, 4, 127)
                         .Wait(4)
                         .Bytes({0xE1, 0, 0xC2, kPlain, 0xE0, 0x55})
                         .Note(48, 4, 100)
                         .Wait(4)
                         .End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Sequencer seq(rom, info, 0);

    std::vector<std::pair<int, int>> notes;
    std::vector<uint32_t> pitches, volumes;
    for (int f = 0; f < 9; f++)
    {
        seq.Step();
        for (const Event& e : seq.Events())
        {
            if (e.kind == Event::kNote)
            {
                notes.emplace_back(f, e.voice);
            }
        }
        pitches.push_back(notes.empty() ? 0 : seq.Voices()[size_t(notes.back().second)].pitch);
        volumes.push_back(notes.empty() ? 0 : seq.Voices()[size_t(notes.back().second)].volume);
    }

    SEQLIB_CHECK(info.revision == Revision::kSuperMarioAdvance2);
    SEQLIB_CHECK_EQ(info.pitch_table, kPitchTable);
    SEQLIB_CHECK_EQ(info.frequency_table, kFrequencyTable);
    SEQLIB_CHECK_EQ(info.noise_table, kNoiseTable);
    SEQLIB_CHECK_EQ(info.lfo_table, kLfoTable);
    SEQLIB_CHECK_EQ(info.voice_classes, kVoiceClasses);
    SEQLIB_REQUIRE_EQ(notes.size(), 3);
    SEQLIB_CHECK_EQ(notes[1].first, 4);
    SEQLIB_CHECK_EQ(notes[2].first, 8);

    // A bend of 64 with a range of 2 scales the pitch by (4013 * 64 + 0x400000) >> 14 = 271 256ths, and a bend of -64
    // by 240 256ths, from 0x8000 for the looped sample's note 60.
    const uint32_t range = rom.U32(kPitchTable + 4 * 0x32);
    SEQLIB_CHECK_EQ(range, 36781);
    SEQLIB_CHECK_EQ(pitches[0], (0x8000u * 271) >> 8);
    SEQLIB_CHECK_EQ(pitches[4], (0x8000u * 240) >> 8);

    // A velocity of 100 at a track volume of 0x55 and the envelope's full level gives a level of 16999, without the
    // player's second volume. A Link to the Past's revision gives 16991.
    SEQLIB_CHECK_EQ(volumes[8], 16999);
}

void TestTiming()
{
    // At a tempo of 75, a tick takes two frames, and a note of 3 ticks lasts 6 frames before its release. With C8,
    // notes wait for their length.
    Cart cart;
    cart.Sequences({{TrackData()
                         .Bytes({0xE4, 75, 0xC2, kLooped})
                         .Note(60, 3, 127)
                         .Wait(2)
                         .Note(62, 1, 127)
                         .Bytes({0xC8})
                         .Note(64, 2, 127)
                         .Note(65, 2, 127)
                         .End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const auto frames = Run(rom, info, 0, 16);

    std::vector<std::pair<int, uint64_t>> ons, releases;
    for (size_t f = 0; f < frames.size(); f++)
    {
        for (const Event& e : frames[f])
        {
            if (e.kind == Event::kNote)
            {
                ons.emplace_back(int(f), e.units);
            }
            if (e.kind == Event::kRelease && e.id == 1)
            {
                releases.emplace_back(int(f), e.units);
            }
        }
    }
    SEQLIB_REQUIRE_EQ(ons.size(), 4);
    SEQLIB_CHECK_EQ(ons[0].first, 0);
    SEQLIB_CHECK_EQ(ons[1].first, 4);
    SEQLIB_CHECK_EQ(ons[1].second, 300);
    SEQLIB_CHECK_EQ(ons[2].first, 4);
    SEQLIB_CHECK_EQ(ons[3].first, 8);
    SEQLIB_CHECK_EQ(ons[3].second, 600);
    SEQLIB_REQUIRE_EQ(releases.size(), 1);
    SEQLIB_CHECK_EQ(releases[0].first, 5);
}

void TestSongFiles()
{
    // The files in memory hold the bytes that a conversion writes, and the report names no paths.
    Cart cart;
    cart.Sequences(
        {{TrackData().Bytes({0xC2, kLooped}).Note(60, 20, 100).Wait(20).End()}, {TrackData().Wait(20).End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const ConvertSettings opt = TempSettings("rd2_song_files");
    const SongReport written = ConvertSong(rom, info, 0, opt, nullptr);
    const SongFiles files = ConvertSongFiles(rom, info, 0, opt);
    SEQLIB_REQUIRE(written.ok && files.report.ok);
    SEQLIB_CHECK(!written.midi_path.empty() && !written.sf2_path.empty());
    SEQLIB_CHECK(files.report.midi_path.empty() && files.report.sf2_path.empty());
    SEQLIB_CHECK(!files.midi.empty() && files.midi == ReadAll(written.midi_path));
    SEQLIB_CHECK(!files.sf2.empty() && files.sf2 == ReadAll(written.sf2_path));
    SEQLIB_CHECK(files.report.seconds == written.seconds);
    SEQLIB_CHECK_EQ(files.report.tracks, written.tracks);

    // A silent sequence gives a report and no files.
    const SongFiles silent = ConvertSongFiles(rom, info, 1, opt);
    SEQLIB_CHECK(silent.report.ok && silent.report.silent);
    SEQLIB_CHECK(silent.midi.empty() && silent.sf2.empty());

    // A sequence that the game lacks gives neither.
    const SongFiles missing = ConvertSongFiles(rom, info, SongCount(info), opt);
    SEQLIB_CHECK(!missing.report.ok && missing.midi.empty() && missing.sf2.empty());
}

void TestVoices()
{
    // The mixer has 7 voices. An eighth note takes the oldest of the lowest priority, and a note of a lower priority
    // than every voice's gets none.
    Cart cart;
    TrackData t;
    t.Bytes({0xC2, kLooped});
    for (uint8_t n = 0; n < 8; n++)
    {
        t.Note(uint8_t(50 + n), 20, 100);
    }
    t.Wait(1).Bytes({0xC4, 1}).Note(70, 20, 100);
    cart.Sequences({{t.End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const auto frames = Run(rom, info, 0, 2);

    int voiced = 0, stopped = -1, eighth = -1, last = 0;
    for (const auto& frame : frames)
    {
        for (const Event& e : frame)
        {
            voiced += e.kind == Event::kNote && e.voice >= 0 ? 1 : 0;
            stopped = e.kind == Event::kStop ? int(e.id) : stopped;
            eighth = e.kind == Event::kNote && e.key == 57 ? e.voice : eighth;
            last = e.kind == Event::kNote && e.key == 70 ? e.voice : last;
        }
    }
    SEQLIB_CHECK_EQ(voiced, 8);
    SEQLIB_CHECK_EQ(stopped, 1);
    SEQLIB_CHECK_EQ(eighth, 0);
    SEQLIB_CHECK_EQ(last, -1);
}

// Returns a track that plays `notes` notes at once on the plain sample, from note 50 up, and ends once they have.
std::vector<uint8_t> Chord(int notes)
{
    TrackData t;
    t.Bytes({0xC2, kPlain});
    for (int n = 0; n < notes; n++)
    {
        t.Note(uint8_t(50 + n), 20, 127);
    }

    return t.Wait(20).End();
}

// Returns the first CC11 of a sequence's conversion, or -1 if it has none.
int FirstExpression(const Rom& rom, const DriverInfo& info, int sequence)
{
    ConvertSettings opt = TempSettings("rd2_music_settings");
    const SongReport sum = ConvertSong(rom, info, sequence, opt, nullptr);
    SEQLIB_CHECK(sum.ok);
    if (!sum.ok)
    {
        return -1;
    }

    for (const auto& e : ReadMidi(sum.midi_path).Find(0xB0))
    {
        if (e.bytes[1] == 11)
        {
            return e.bytes[2];
        }
    }

    return -1;
}

void TestMusicSettings()
{
    // The game's play-music routine gives the driver 6 sample voices for music 0, whose entry has bit 10 set, and the
    // player a second volume of 0x37 for music 2. Music 3 plays sequence 1 too, but music 0 played it first. Music 4's
    // sequence isn't in the game. The table ends there, and music 5 isn't read.
    Cart cart;
    cart.Sequences({{Chord(7)}, {Chord(7)}, {Chord(1)}, {Chord(1)}});
    cart.PlayMusic({0x0401, 0x0000, 0x0002, 0x0001, 0x0009, 0x0403}, {{2, 0x37}});
    const Rom rom = cart.ToRom();

    const DriverInfo info = Detect(rom);

    SEQLIB_REQUIRE_EQ(info.music_settings.size(), 4);
    const int kVoiceCounts[4] = {7, 6, 7, 7};
    const int kSecondVolumes[4] = {0x80, 0x80, 0x37, 0x80};
    for (int s = 0; s < 4; s++)
    {
        SEQLIB_REQUIRE_EQ(info.music_settings[size_t(s)].size(), 1);
        SEQLIB_CHECK_EQ(info.music_settings[size_t(s)][0].sample_voices, kVoiceCounts[s]);
        SEQLIB_CHECK_EQ(info.music_settings[size_t(s)][0].volume2, kSecondVolumes[s]);
        SEQLIB_CHECK_EQ(SongVersions(info, s).size(), 1);
    }

    // With 7 voices, seven notes at once each have one. With 6, the seventh takes the first note's voice.
    for (int s = 0; s < 2; s++)
    {
        int voiced = 0, stopped = 0;
        for (const auto& frame : Run(rom, info, s, 2))
        {
            for (const Event& e : frame)
            {
                voiced += e.kind == Event::kNote && e.voice >= 0 ? 1 : 0;
                stopped += e.kind == Event::kStop ? 1 : 0;
            }
        }
        SEQLIB_CHECK_EQ(voiced, 7);
        SEQLIB_CHECK_EQ(stopped, s);
    }

    // CC11 scales the amplitude by its square. The second volume of 0x37 takes it down to sqrt(0x37 / 0x80) of the
    // driver's default.
    const int full = FirstExpression(rom, info, 3);
    const int lower = FirstExpression(rom, info, 2);
    SEQLIB_CHECK(full > 0);
    SEQLIB_CHECK(std::abs(lower - full * std::sqrt(0x37 / 128.0)) <= 1);
}

void TestMusicSettingsWithCount()
{
    // An override converts 2 of the game's 3 sequences. Music 0 plays sequence 2. That sequence isn't converted, but
    // the game has it. The table therefore goes on. Music 1 gives sequence 0 6 sample voices and a second volume of
    // 0x37.
    Cart cart;
    cart.Sequences({{Chord(1)}, {Chord(1)}, {Chord(1)}});
    cart.PlayMusic({0x0002, 0x0400, 0x0009}, {{1, 0x37}});
    const Rom rom = cart.ToRom();
    DriverOverrides overrides;
    overrides.sequence_count = 2;
    DriverInfo info;
    std::string error;

    SEQLIB_CHECK(DetectDriver(rom, overrides, info, error));

    SEQLIB_REQUIRE_EQ(info.music_settings.size(), 2);
    SEQLIB_REQUIRE_EQ(info.music_settings[0].size(), 1);
    SEQLIB_CHECK_EQ(info.music_settings[0][0].sample_voices, 6);
    SEQLIB_CHECK_EQ(info.music_settings[0][0].volume2, 0x37);
}

// Returns the keys of the note-ons on every channel of a MIDI file, from the lowest.
std::vector<int> NoteKeys(const std::string& path)
{
    const MidiEvents midi = ReadMidi(path);
    std::vector<int> keys;
    for (int channel = 0; channel < 16; channel++)
    {
        for (const auto& e : midi.Find(uint8_t(0x90 | channel)))
        {
            if (e.bytes[2])
            {
                keys.push_back(e.bytes[1]);
            }
        }
    }
    std::sort(keys.begin(), keys.end());

    return keys;
}

void TestMutedTracks()
{
    // The game's play-music routine mutes tracks of sequence 1: track 2 for music 2, and track 1 for the other music
    // numbers that play it. Music 1 plays the sequence first and gives the first version. Music 2 makes a second
    // version. Music 3 mutes the same track as music 1 and makes no version.
    Cart cart;
    cart.Sequences({{Chord(1)},
                    {TrackData().Bytes({0xC2, kPlain}).Note(60, 6, 127).Wait(6).End(),
                     TrackData().Bytes({0xC2, kPlain}).Note(62, 6, 127).Wait(6).End(),
                     TrackData().Bytes({0xC2, kPlain}).Note(64, 6, 127).Wait(6).End()}});
    cart.MutingPlayMusic({0x0000, 0x0001, 0x0001, 0x0001, 0x0009}, 1, 2, {1, 1}, {1, 2});
    const Rom rom = cart.ToRom();

    const DriverInfo info = Detect(rom);

    SEQLIB_REQUIRE_EQ(info.music_settings.size(), 2);
    SEQLIB_REQUIRE_EQ(info.music_settings[0].size(), 1);
    SEQLIB_CHECK_EQ(info.music_settings[0][0].music, 0);
    SEQLIB_CHECK_EQ(info.music_settings[0][0].muted_tracks, 0);
    SEQLIB_CHECK_EQ(info.music_settings[0][0].sample_voices, 7);
    SEQLIB_REQUIRE_EQ(info.music_settings[1].size(), 2);
    SEQLIB_CHECK_EQ(info.music_settings[1][0].music, 1);
    SEQLIB_CHECK_EQ(info.music_settings[1][0].muted_tracks, 0x2);
    SEQLIB_CHECK_EQ(info.music_settings[1][1].music, 2);
    SEQLIB_CHECK_EQ(info.music_settings[1][1].muted_tracks, 0x4);
    SEQLIB_CHECK(std::find(info.log.begin(), info.log.end(),
                           "the game's music table: 0x08000780; muted tracks: in sequence 1, track 1 as music 1 and "
                           "track 2 as music 2") != info.log.end());

    const std::vector<SongVersion> versions = SongVersions(info, 1);
    SEQLIB_REQUIRE_EQ(versions.size(), 2);
    SEQLIB_CHECK(versions[0].name.empty() && versions[0].suffix.empty());
    SEQLIB_CHECK(versions[1].name == "as music 2" && versions[1].suffix == "_music2");
    SEQLIB_CHECK_EQ(SongVersions(info, 0).size(), 1);

    // A muted track plays no notes. Each version's files have its suffix after the sequence's number.
    for (int v = 0; v < 2; v++)
    {
        int notes = 0;
        for (const auto& frame : Run(rom, info, 1, 4, v))
        {
            for (const Event& e : frame)
            {
                notes += e.kind == Event::kNote ? 1 : 0;
            }
        }
        SEQLIB_CHECK_EQ(notes, 2);

        ConvertSettings opt = TempSettings("rd2_muted_tracks");
        opt.version = v;
        const SongReport r = ConvertSong(rom, info, 1, opt, nullptr);
        SEQLIB_REQUIRE(r.ok);
        SEQLIB_CHECK_EQ(r.tracks, 2);
        SEQLIB_CHECK(r.midi_path.ends_with(v == 0 ? "_01.mid" : "_01_music2.mid"));
        SEQLIB_CHECK(NoteKeys(r.midi_path) == (v == 0 ? std::vector<int>({60, 64}) : std::vector<int>({60, 62})));
    }

    // SongVersions() names the versions.
    SEQLIB_CHECK_EQ(SongVersions(info, 1).size(), 2);
    SEQLIB_CHECK(SongVersions(info, 1)[1].name == "as music 2");
}

void TestYoshiTracks()
{
    // Unless the player rides Yoshi, the game mutes track 1 of the music whose entry has bit 9 set. Music 1 plays
    // sequence 1 with the bit. The first version mutes the track, and a second version, riding Yoshi, plays it. Music 0
    // plays sequence 0 without the bit, in one version.
    Cart cart;
    cart.Sequences({{Chord(1)},
                    {TrackData().Bytes({0xC2, kPlain}).Note(60, 6, 127).Wait(6).End(),
                     TrackData().Bytes({0xC2, kPlain}).Note(62, 6, 127).Wait(6).End(),
                     TrackData().Bytes({0xC2, kPlain}).Note(64, 6, 127).Wait(6).End()}});
    cart.MutingPlayMusic({0x0000, 0x0201, 0x0009}, 5, 9, {1, 0}, {1, 1});
    cart.YoshiMute(0x02);
    const Rom rom = cart.ToRom();

    const DriverInfo info = Detect(rom);

    SEQLIB_REQUIRE_EQ(info.music_settings.size(), 2);
    SEQLIB_REQUIRE_EQ(info.music_settings[0].size(), 1);
    SEQLIB_CHECK_EQ(info.music_settings[0][0].muted_tracks, 0);
    SEQLIB_REQUIRE_EQ(info.music_settings[1].size(), 2);
    SEQLIB_CHECK_EQ(info.music_settings[1][0].music, 1);
    SEQLIB_CHECK_EQ(info.music_settings[1][0].muted_tracks, 0x2);
    SEQLIB_CHECK(!info.music_settings[1][0].riding_yoshi);
    SEQLIB_CHECK_EQ(info.music_settings[1][1].music, 1);
    SEQLIB_CHECK_EQ(info.music_settings[1][1].muted_tracks, 0);
    SEQLIB_CHECK(info.music_settings[1][1].riding_yoshi);
    SEQLIB_CHECK(std::find(info.log.begin(), info.log.end(),
                           "the game's music table: 0x08000780; muted tracks: none; muted unless riding Yoshi: track 1 "
                           "of sequence 1") != info.log.end());

    const std::vector<SongVersion> versions = SongVersions(info, 1);
    SEQLIB_REQUIRE_EQ(versions.size(), 2);
    SEQLIB_CHECK(versions[0].name.empty() && versions[0].suffix.empty());
    SEQLIB_CHECK(versions[1].name == "riding Yoshi" && versions[1].suffix == "_yoshi");

    // The first version leaves out the track's notes. The second has them.
    for (int v = 0; v < 2; v++)
    {
        ConvertSettings opt = TempSettings("rd2_yoshi_tracks");
        opt.version = v;
        const SongReport r = ConvertSong(rom, info, 1, opt, nullptr);
        SEQLIB_REQUIRE(r.ok);
        SEQLIB_CHECK(r.midi_path.ends_with(v == 0 ? "_01.mid" : "_01_yoshi.mid"));
        SEQLIB_CHECK(NoteKeys(r.midi_path) == (v == 0 ? std::vector<int>({60, 64}) : std::vector<int>({60, 62, 64})));
    }
}

void TestEnvelope()
{
    // The envelope rises to 0x7FFF in 4 frames, a quarter each frame. The release then takes the voice's volume down to
    // 300/512 of itself each frame until it's silent, and the voice is freed 10 frames after the release starts.
    Cart cart;
    cart.Sequences({{TrackData().Bytes({0xC2, kLooped}).Note(60, 6, 127).Wait(40).End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Sequencer seq(rom, info, 0);

    std::vector<int32_t> levels;
    std::vector<uint32_t> volumes;
    std::vector<uint8_t> states;
    for (int f = 0; f < 30; f++)
    {
        seq.Step();
        levels.push_back(seq.Voices()[0].level);
        volumes.push_back(seq.Voices()[0].volume);
        states.push_back(seq.Voices()[0].state);
    }

    SEQLIB_CHECK_EQ(levels[0], 0x7FFF / 4);
    SEQLIB_CHECK_EQ(levels[3], 4 * (0x7FFF / 4));
    SEQLIB_CHECK_EQ(levels[4], 0x7FFF + (0x3F80 - 0x7FFF) / 8);
    SEQLIB_CHECK_EQ(states[4], 1);
    SEQLIB_CHECK_EQ(states[5], 2);
    SEQLIB_CHECK(volumes[5] > 0 && volumes[6] == (300 * volumes[5]) >> 9);
    SEQLIB_CHECK_EQ(states[14], 2);
    SEQLIB_CHECK_EQ(states[15], 0);
    SEQLIB_CHECK_EQ(states[29], 0);
}

void TestPsg()
{
    // A square 2 note starts the channel at its pitch, with its envelope and duty, on both sides. With a tuning of
    // 0x30, note 0x30 plays middle C.
    Cart cart;
    cart.Sequences({{TrackData().Bytes({0xC2, kSquare}).Note(0x30, 4, 127).Wait(10).End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Sequencer seq(rom, info, 0);

    seq.Step();

    std::vector<std::pair<uint32_t, uint32_t>> writes;
    for (const RegisterWrite& w : seq.Writes())
    {
        writes.emplace_back(w.address, w.value);
    }
    const uint16_t middle_c = rom.U16(kFrequencyTable + 2 * 0x30);
    SEQLIB_CHECK(std::find(writes.begin(), writes.end(), std::make_pair(0x04000081u, 0xFFu)) != writes.end());
    SEQLIB_CHECK(std::find(writes.begin(), writes.end(), std::make_pair(0x04000069u, 0xF8u)) != writes.end());
    SEQLIB_CHECK(std::find(writes.begin(), writes.end(), std::make_pair(0x0400006Cu, 0x8000u | middle_c)) !=
                 writes.end());
    SEQLIB_CHECK(std::find(writes.begin(), writes.end(), std::make_pair(0x04000068u, 0x40u)) != writes.end());
}

void TestInstruments()
{
    // A drum kit plays each region at the region's pitch and pan. An instrument with a sample for each key picks one. A
    // key split plays one region below its split and another above.
    Cart cart;
    cart.Sequences({{TrackData()
                         .Bytes({0xC2, kDrums})
                         .Note(36, 4, 100)
                         .Wait(1)
                         .Bytes({0xC2, kKeyed})
                         .Note(40, 4, 100)
                         .Note(41, 4, 100)
                         .Wait(1)
                         .Bytes({0xC2, kSplit})
                         .Note(59, 4, 100)
                         .Note(60, 4, 100)
                         .End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    std::vector<Event> notes;
    std::vector<uint32_t> pitches;
    for (int f = 0; f < 4; f++)
    {
        seq.Step();
        for (const Event& e : seq.Events())
        {
            if (e.kind == Event::kNote)
            {
                notes.push_back(e);
                pitches.push_back(e.voice >= 0 ? seq.Voices()[size_t(e.voice)].note_pitch : 0);
            }
        }
    }

    SEQLIB_REQUIRE_EQ(notes.size(), 5);
    SEQLIB_CHECK(notes[0].lookup.drum);
    SEQLIB_CHECK_EQ(notes[0].lookup.drum_pan, 0x20);
    SEQLIB_CHECK_EQ(pitches[0], 0x8000);
    SEQLIB_CHECK(notes[1].lookup.key_sample && notes[2].lookup.key_sample);
    SEQLIB_CHECK(notes[1].sample != notes[2].sample);
    SEQLIB_CHECK_EQ(notes[3].sample, notes[2].sample);
    SEQLIB_CHECK_EQ(notes[4].sample, notes[1].sample);
    SEQLIB_CHECK_EQ(pitches[3], rom.U32(kPitchTable + 4 * (59 + 0x30 - 48)));
}

void TestConversion()
{
    // The conversion of a sequence that loops has the tempo, the loop markers, the notes' keys and velocities, and the
    // SoundFont's zone for the instrument. The zone plays the sample at its tuning. The first track goes back to its
    // start, just after the sequence's header of 6 bytes.
    Cart cart;
    cart.Sequences({{TrackData()
                         .Bytes({0xE4, 120, 0xC2, kPlain})
                         .Note(60, 6, 127)
                         .Wait(6)
                         .Note(64, 6, 64)
                         .Wait(6)
                         .Bytes({0xF0, 0x06, 0x00})
                         .End(),
                     TrackData().Bytes({0xC2, kLooped}).Note(48, 12, 100).Wait(48).End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.loops = 1;
    opt.out_dir = Utf8(ntdlib::testing::agb_files::TempFolderPath());
    opt.base_name = "rd2";

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(sum.ok);
    SEQLIB_CHECK_EQ(sum.tracks, 2);
    SEQLIB_CHECK(sum.loop_start >= 0 && sum.loop_end > sum.loop_start);

    const MidiEvents midi = ReadMidi(sum.midi_path);
    SEQLIB_CHECK_EQ(midi.division, 3600);
    const auto first = midi.Find(0x90);
    SEQLIB_REQUIRE(first.size() >= 2);
    SEQLIB_CHECK_EQ(first[0].tick, 0);
    SEQLIB_CHECK_EQ(first[0].bytes[1], 60);
    SEQLIB_CHECK_EQ(first[0].bytes[2], 127);
    SEQLIB_CHECK_EQ(first[1].tick, 900);
    SEQLIB_CHECK_EQ(first[1].bytes[1], 64);
    SEQLIB_CHECK_EQ(first[1].bytes[2], int(std::lround(127 * std::sqrt(64 / 127.0))));

    const auto metas = midi.Find(0xFF);
    const auto is_tempo = [](const auto& e)
    {
        return e.bytes.size() > 5 && e.bytes[1] == 0x51 && (e.bytes[3] << 16 | e.bytes[4] << 8 | e.bytes[5]) == 502281;
    };
    const auto is_marker = [](const auto& e)
    {
        return e.bytes.size() > 1 && e.bytes[1] == 0x06;
    };
    SEQLIB_CHECK(std::any_of(metas.begin(), metas.end(), is_tempo));
    SEQLIB_CHECK(std::any_of(metas.begin(), metas.end(), is_marker));

    const Sf2Records sf = ReadSf2(sum.sf2_path);
    SEQLIB_CHECK(HasGlobalZoneModulators(sf));
    SEQLIB_CHECK_EQ(sf.Count("phdr", 38), 3);
    SEQLIB_CHECK_EQ(sf.Count("shdr", 46), 3);
    const auto zone = sf.ZoneGens(1); // the first instrument's zone after its global zone
    SEQLIB_CHECK_EQ(zone.at(58), 48);
    SEQLIB_CHECK_EQ(Le32(sf.Record("shdr", 46, 0) + 36), 21024);
}


void TestLoopLengths()
{
    // Two tracks loop 12 and 18 ticks from their starts, after the sequence's header of 8 bytes, and a third, without a
    // loop, plays a note of 9 ticks and ends after 24. The loop starts at tick 9, where that note ends, and lasts until
    // both loops are back where they started, 36 ticks, and the sequence plays it twice. A tick is 150 of the MIDI
    // file's.
    Cart cart;
    cart.Sequences({{TrackData().Bytes({0xE4, 120, 0xC2, kLooped}).Note(60, 6, 127).Wait(12).Bytes({0xF0, 8, 0}).End(),
                     TrackData().Bytes({0xC2, kLooped}).Note(64, 6, 100).Wait(18).Bytes({0xF0, 21, 0}).End(),
                     TrackData().Bytes({0xC2, kLooped}).Note(67, 9, 100).Wait(24).End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("loop_lengths");

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    const MidiEvents midi = ReadMidi(sum.midi_path);
    std::vector<std::pair<uint32_t, std::string>> markers;
    for (const auto& e : midi.tracks[0].events)
    {
        if (e.bytes[0] == 0xFF && e.bytes[1] == 0x06)
        {
            markers.push_back({uint32_t(e.tick), std::string(e.bytes.begin() + 3, e.bytes.end())});
        }
    }
    const std::vector<std::pair<uint32_t, std::string>> kMarkers = {{1350, "loopStart"}, {6750, "loopEnd"}};
    SEQLIB_CHECK(sum.ok);
    SEQLIB_CHECK(markers == kMarkers);
    SEQLIB_CHECK(sum.loop_end > 0 && std::fabs(sum.seconds / sum.loop_end - 12150.0 / 6750) < 0.01);
}

// A track sets volume 200 and plays a note, and then loops: volume 200 and a note, and volume 100, pan 100 and a note.
// The game keeps pan 100 when the track jumps back. The loop's first note plays in the middle the first time through
// and at pan 100 after that. The marked loop therefore starts at the second pass, at the MIDI file's tick 2700, and a
// player that jumps back there from the loop's end has the loudness and pan that the file's next pass starts with.
void TestLoopSettingsAgain()
{
    Cart cart;
    cart.Sequences({{TrackData()
                         .Bytes({0xE4, 120, 0xC2, kLooped, 0xE0, 200})
                         .Note(60, 6, 127)
                         .Wait(6)
                         .Bytes({0xE0, 200})
                         .Note(64, 6, 127)
                         .Wait(6)
                         .Bytes({0xE0, 100, 0xC3, 100})
                         .Note(67, 6, 127)
                         .Wait(6)
                         .Bytes({0xF0, 15, 0})
                         .End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("loop_again");

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    const ntdlib::testing::agb_files::MidiEvents midi = ReadMidi(sum.midi_path);
    std::vector<std::pair<uint32_t, std::string>> markers;
    for (const auto& [tick, bytes] : midi.Find(0xFF))
    {
        if (bytes[1] == 0x06)
        {
            markers.emplace_back(uint32_t(tick), std::string(bytes.begin() + 3, bytes.end()));
        }
    }

    // Returns a controller's value after the events up to and including `tick`.
    const auto value_at = [&](int controller, uint32_t tick)
    {
        int value = -1;
        for (const auto& [t, bytes] : midi.Find(0xB0))
        {
            if (t <= tick && bytes[1] == controller)
            {
                value = bytes[2];
            }
        }

        return value;
    };

    const std::vector<std::pair<uint32_t, std::string>> kMarkers = {{2700, "loopStart"}, {4500, "loopEnd"}};
    SEQLIB_CHECK(sum.ok);
    SEQLIB_CHECK(markers == kMarkers);
    SEQLIB_CHECK(value_at(cc::kPan, 900) == 64 && value_at(cc::kPan, 2700) > 64);
    SEQLIB_CHECK_EQ(value_at(cc::kPan, 2700), value_at(cc::kPan, 4500));
    SEQLIB_CHECK_EQ(value_at(cc::kExpression, 2700), value_at(cc::kExpression, 4500));
}

// The listing follows each track in the sequence's header, and then each track that F8 starts.
void TestDump()
{
    // The track starts track 3 at the note after its end. The track's data starts at offset 4, after the sequence's
    // header and the track's offset. F8 takes 4 bytes.
    const std::vector<uint8_t> first =
        TrackData().Bytes({0xE4, 120, 0xC2, kPlain, 0xC3, 0x20}).Note(60, 6, 127).Wait(6).End();
    const std::vector<uint8_t> started = TrackData().Note(62, 6, 127).Wait(6).End();
    const uint16_t offset = uint16_t(4 + 4 + first.size());
    std::vector<uint8_t> track = {0xF8, 3, uint8_t(offset), uint8_t(offset >> 8)};
    track.insert(track.end(), first.begin(), first.end());
    track.insert(track.end(), started.begin(), started.end());
    Cart cart;
    cart.Sequences({{track}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    std::string error;
    const std::string path = Utf8(TempPath("rd2_dump.txt"));

    const bool ok = DumpSong(rom, info, 0, path, error);

    const std::vector<uint8_t> data = ReadAll(path);
    const std::string text(data.begin(), data.end());
    SEQLIB_CHECK(ok);
    SEQLIB_CHECK(text.find("tempo 120") != std::string::npos);
    SEQLIB_CHECK(text.find("pan 32") != std::string::npos);
    SEQLIB_CHECK(text.find("note 60 (C4), length 6, velocity 127") != std::string::npos);
    SEQLIB_CHECK(text.find("      6  FF") != std::string::npos);
    SEQLIB_CHECK(text.find("start track 3 at 0x") != std::string::npos);
    SEQLIB_CHECK(text.find("; ---- track 3 at 0x") != std::string::npos);
    SEQLIB_CHECK(text.find(", started by F8 at 0x") != std::string::npos);
    SEQLIB_CHECK(text.find("note 62 (D4), length 6, velocity 127") != std::string::npos);
}

void TestEffects()
{
    // Two sequences of sound effects. The first has three tracks. The second of them is empty. The second sequence has
    // one track. The first sequence's table of three tracks takes 6 bytes. Its first track starts at offset 6. The
    // third track starts another track with F8. That track comes after the third track's end. The third track's C4 and
    // C2 take 2 bytes each, its F8 4, its note 3 and its end 1.
    const std::vector<uint8_t> first = TrackData().Bytes({0xC2, kPlain}).Note(60, 3, 127).Note(64, 2, 127).End();
    const uint16_t other = uint16_t(6 + first.size() + 12);
    std::vector<uint8_t> third =
        TrackData().Bytes({0xC4, 5, 0xC2, kPlain, 0xF8, 1, uint8_t(other), uint8_t(other >> 8)}).Note(67, 2, 127).End();
    const std::vector<uint8_t> started = TrackData().Note(69, 2, 127).Note(71, 2, 127).End();
    third.insert(third.end(), started.begin(), started.end());
    Cart cart;
    cart.Sequences({{TrackData().End()}});
    cart.Effects({{first, {}, third}, {TrackData().Bytes({0xC2, kPlain}).Note(72, 1, 127).End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    // Songs 1 to 4 are the sound effects. Song 0 is the sequence.
    SEQLIB_CHECK_EQ(SongCount(info), 5);
    SEQLIB_CHECK(SongEffect(info, 0) == nullptr && SongEffect(info, 5) == nullptr);
    SEQLIB_REQUIRE(SongEffect(info, 1) != nullptr && SongEffect(info, 2) != nullptr && SongEffect(info, 4) != nullptr);
    SEQLIB_CHECK_EQ(SongEffect(info, 1)->address, kEffectData + 6);
    SEQLIB_CHECK_EQ(SongEffect(info, 2)->address, 0);
    SEQLIB_CHECK_EQ(SongEffect(info, 4)->sequence, 1);
    SEQLIB_CHECK_EQ(SongEffect(info, 4)->track, 0);
    SEQLIB_CHECK(std::any_of(info.log.begin(), info.log.end(), [](const std::string& line)
                             { return line.find("(2 sequences, 4 effects: songs 1-4)") != std::string::npos; }));
    SEQLIB_CHECK(!Sequencer(rom, info, 2).Valid());

    // The first track plays at a priority of 12 with an echo send of 7F. Its second note waits for the first.
    Sequencer effect(rom, info, 1);
    effect.Step();
    const int slot = effect.GetPlayer().tracks[0];
    SEQLIB_REQUIRE(slot >= 0);
    SEQLIB_CHECK(effect.GetPlayer().effect);
    SEQLIB_CHECK_EQ(effect.Tracks()[size_t(slot)].priority, kEffectPriority);
    SEQLIB_CHECK_EQ(effect.Tracks()[size_t(slot)].echo, kEffectEcho);
    SEQLIB_CHECK_EQ(effect.Voices()[0].priority, kEffectPriority);
    const auto frames = Run(rom, info, 1, 8);
    std::vector<int> ons;
    for (size_t f = 0; f < frames.size(); f++)
    {
        for (const Event& e : frames[f])
        {
            ons.push_back(e.kind == Event::kNote ? int(f) : -1);
        }
    }
    ons.erase(std::remove(ons.begin(), ons.end(), -1), ons.end());
    SEQLIB_REQUIRE_EQ(ons.size(), 2);
    SEQLIB_CHECK_EQ(ons[0], 0);
    SEQLIB_CHECK_EQ(ons[1], 3);

    // F8 starts a track at the sound effect's defaults. Then it copies the starter's priority. The new track's notes
    // wait for their length too.
    Sequencer starter(rom, info, 3);
    starter.Step();
    const int second = starter.GetPlayer().tracks[1];
    SEQLIB_REQUIRE(second >= 0);
    SEQLIB_CHECK_EQ(starter.Tracks()[size_t(second)].priority, 5);
    SEQLIB_CHECK_EQ(starter.Tracks()[size_t(second)].echo, kEffectEcho);
    SEQLIB_CHECK_EQ(starter.Tracks()[size_t(second)].timed_notes, 1);

    // The sound effects count as songs. The empty entry gives no track.
    SEQLIB_CHECK_EQ(SongCount(info), 5);
    SEQLIB_REQUIRE(SongEffect(info, 1) != nullptr && SongEffect(info, 2) != nullptr && SongEffect(info, 3) != nullptr);
    SEQLIB_CHECK(SongEffect(info, 1)->address != 0 && SongEffect(info, 2)->address == 0 &&
                 SongEffect(info, 3)->address != 0);
    SEQLIB_CHECK_EQ(SongEffect(info, 1)->address, kEffectData + 6);

    // The conversion names the sound effect. The listing gives the times of its notes.
    const SongReport sum = ConvertSong(rom, info, 1, TempSettings("rd2_effect"), nullptr);
    SEQLIB_CHECK(sum.ok);
    const std::vector<uint8_t> midi = ReadAll(sum.midi_path);
    SEQLIB_CHECK(std::string(midi.begin(), midi.end()).find("track 0 of the effects' sequence 0") != std::string::npos);
    const std::string path = Utf8(TempPath("rd2_effect.txt"));
    std::string error;
    SEQLIB_CHECK(DumpSong(rom, info, 3, path, error));
    const std::vector<uint8_t> data = ReadAll(path);
    const std::string text(data.begin(), data.end());
    SEQLIB_CHECK(text.find("sound effect 3: track 2 of the effects' sequence 0") != std::string::npos);
    SEQLIB_CHECK(text.find(", started by F8 at 0x") != std::string::npos);
    SEQLIB_CHECK(text.find("      2  A7 02 7F") != std::string::npos);

    // The track that F8 starts ends after the track that starts it. The sound effect lasts until the later end. The
    // track that F8 starts plays on MIDI channel 1.
    const SongReport layered = ConvertSong(rom, info, 3, TempSettings("rd2_effect_f8"), nullptr);
    SEQLIB_REQUIRE(layered.ok);
    std::vector<int> keys;
    for (const auto& e : ReadMidi(layered.midi_path).Find(0x91))
    {
        keys.push_back(e.bytes[1]);
    }
    SEQLIB_CHECK(keys == std::vector<int>({69, 71}));
}

} // namespace

int RunTests()
{
    TestDetection();
    TestSuperMarioAdvance2();
    TestTiming();
    TestSongFiles();
    TestVoices();
    TestMusicSettings();
    TestMusicSettingsWithCount();
    TestMutedTracks();
    TestYoshiTracks();
    TestEnvelope();
    TestPsg();
    TestInstruments();
    TestConversion();
    TestLoopLengths();
    TestLoopSettingsAgain();
    TestDump();
    TestEffects();

    return seqlib::testing::Summary("rd2");
}

} // namespace ntdlib::sequence_export::rd2

int main()
{
    return ntdlib::sequence_export::rd2::RunTests();
}
