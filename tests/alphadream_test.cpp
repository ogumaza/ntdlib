// SPDX-License-Identifier: MIT

// Unit tests for AlphaDream's driver. They build a small synthetic cartridge in memory, so no game ROM is needed.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "seqlib/sf2/soundfont.h"
#include "seqlib/testing/check.h"

#include "ntdlib/agb/files.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/alphadream/commands.h"
#include "ntdlib/alphadream/driver.h"
#include "ntdlib/sequence_export/agb/midi.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/alphadream/convert.h"
#include "ntdlib/sequence_export/alphadream/performance.h"
#include "ntdlib/sequence_export/alphadream/sequencer.h"
#include "ntdlib/sequence_export/alphadream/soundfont.h"
#include "ntdlib/testing/agb_cart.h"
#include "ntdlib/testing/agb_files.h"

namespace ntdlib::sequence_export::alphadream
{

namespace sf2 = seqlib::sf2;

using ntdlib::agb::kRomBase;
using ntdlib::agb::Utf8;
using ntdlib::alphadream::SampleHeader;
using ntdlib::alphadream::Version;
using ntdlib::sequence_export::agb::MelodicChannel;

namespace
{

using ntdlib::testing::agb_files::CartImage;
using ntdlib::testing::agb_files::MidiEvents;
using ntdlib::testing::agb_files::ReadAll;
using ntdlib::testing::agb_files::ReadMidi;
using ntdlib::testing::agb_files::ReadSf2;
using ntdlib::testing::agb_files::Sf2Records;
using ntdlib::testing::agb_files::TempPath;
using ntdlib::testing::agb_files::TempSettings;

// Addresses in a test cartridge: the routines that detection finds, then the tables.
constexpr uint32_t kStart = kRomBase + 0x100;
constexpr uint32_t kLookup = kRomBase + 0x200;
constexpr uint32_t kVoice = kRomBase + 0x280;
constexpr uint32_t kStep = kRomBase + 0x300;
constexpr uint32_t kPsgPitch = kRomBase + 0x400;
constexpr uint32_t kNoise = kRomBase + 0x480;
constexpr uint32_t kWave = kRomBase + 0x4C0;
constexpr uint32_t kNote = kRomBase + 0x500;
constexpr uint32_t kCommands = kRomBase + 0x540;
constexpr uint32_t kCommandTable = kRomBase + 0x580;
constexpr uint32_t kEffectHandler = kRomBase + 0x5C0;
constexpr uint32_t kEffectStart = kRomBase + 0x600;
constexpr uint32_t kWaveLevel = kRomBase + 0x640;
constexpr uint32_t kSongTable = kRomBase + 0x800;
constexpr uint32_t kInstruments = kRomBase + 0x900;
constexpr uint32_t kSamples = kRomBase + 0xA00;
constexpr uint32_t kPsgPitchTable = kRomBase + 0xB00;
constexpr uint32_t kNoiseTable = kRomBase + 0xC00;
constexpr uint32_t kWaveTable = kRomBase + 0xC80;
constexpr uint32_t kWaveLevels = kRomBase + 0xD00;
constexpr uint32_t kSong = kRomBase + 0x1000;

// The routines' code, as driver.cpp's patterns give it, with each x as 0.
constexpr const char* kStartCode =
    "B5F0 4647 B480 1C04 1C0D F000 F800 1C20 214B F000 F800 4E00 6831 4B00 18C9 1909 "
    "780A 2001 4310 7008 6831 18C9 1909 780A 20FD 4010 7008 4800";

// The Hamtaro games' version of the routine that starts a song: it takes the song in r4 and the player in r5, and it
// makes one call before it sets the player's flags (0x0814AC70 in A84J).
constexpr const char* kHamtaroStartCode =
    "B5F0 4647 B480 1C05 1C0C F000 F800 4E00 6831 4B00 18C9 1949 780A 2001 4310 7008 6831 18C9 1949 780A 20FD 4010 "
    "7008 4800";

constexpr const char* kLookupCode =
    "B500 1C0B 4A00 0041 1889 8809 1889 3001 0040 1880 8800 1882 4291 D000 7808 4298 "
    "DC00 7848 4298 DB00 1C08 E000";
constexpr const char* kVoiceCode = "B5F0 4657 464D 4643 B4A8 8872 4900 0450 0BC0 580F 187F 0452 D300 6878 213C 2200";
constexpr const char* kStepCode = "4684 A31A 5CC9 A30C 0708 0E80 18C0 6803 6840 1AC0 4350 0A00 181B 0909 40CB 4A00";
constexpr const char* kPsgPitchCode =
    "B510 0400 0E03 21FF 0409 4001 0C0C 3B24 2B00 DB00 4A00 0058 1880 8801 1C58 0040 "
    "1880 8800 1A40 4360 0A00 1809 2280 0112";
constexpr const char* kNoiseCode = "B500 1C01 2914 DC00 20D7 E000 2950 DC00 4800 3915";
constexpr const char* kWaveCode = "0100 4900 1840 4B00 21C0 7019";
constexpr const char* kWaveLevelCode = "4900 7918 0880 1840 7800 0200 8010";

// The start of a note in the routine that runs a track: A88E's, then B85A's and A84J's, which read a volume after the
// key. A84J's then calls the track's volume handler.
constexpr const char* kNoteCode = "8928 1C41 8129 0400 0C00 1810 7804 207F 4020 72E8 8829 2040 4008";
constexpr const char* kHamSportsNoteCode =
    "8928 1C41 8129 0400 0C00 1810 7804 207F 4020 72E8 1C48 8128 0409 0C09 1851 7808 7368 8829 2040 4008";
constexpr const char* kHamtaro4NoteCode =
    "8928 1C41 8129 0400 0C00 1810 7804 207F 4020 72E8 1C48 8128 0409 0C09 1851 7808 7368 6879 1C28";

// The routine that runs the game's commands, command 5's handler, which calls the routine that starts a sound effect
// at +6, and the start of that routine.
constexpr const char* kCommandCode = "0B18 2809 D800 0080 4900 1840 6800 4687";
constexpr const char* kEffectHandlerCode = "4800 4003 1C18";
constexpr const char* kEffectStartCode = "B5F0 4800";

class Cartridge
{
public:
    Cartridge() : image_(0x2000, 0)
    {
    }

    void U8(uint32_t at, uint8_t v)
    {
        image_[at - kRomBase] = v;
    }

    void U16(uint32_t at, uint16_t v)
    {
        U8(at, uint8_t(v));
        U8(at + 1, uint8_t(v >> 8));
    }

    void U32(uint32_t at, uint32_t v)
    {
        U16(at, uint16_t(v));
        U16(at + 2, uint16_t(v >> 16));
    }

    void Bytes(uint32_t at, std::initializer_list<int> bytes)
    {
        for (int b : bytes)
        {
            U8(at++, uint8_t(b));
        }
    }

    // Writes code at `at`. Returns the address after it.
    uint32_t Code(uint32_t at, const char* code)
    {
        std::istringstream words(code);
        std::string word;
        uint32_t end = at;
        while (words >> word)
        {
            U16(end, uint16_t(std::stoul(word, nullptr, 16)));
            end += 2;
        }

        return end;
    }

    // Writes a Thumb bl at `at` that calls `target`.
    void Bl(uint32_t at, uint32_t target)
    {
        const uint32_t offset = target - (at + 4);
        U16(at, uint16_t(0xF000 | ((offset >> 12) & 0x7FF)));
        U16(at + 2, uint16_t(0xF800 | ((offset >> 1) & 0x7FF)));
    }

    // Writes a routine's code at `at`, with its literal load at offset `literal` loading `value` from a pool after the
    // code.
    void Routine(uint32_t at, const char* code, uint32_t literal, uint32_t value)
    {
        const uint32_t end = Code(at, code);
        const uint32_t pool = (end + 3) & ~3u;
        const uint32_t load = at + literal;
        U16(load, uint16_t(ReadU16(load) | ((pool - ((load + 4) & ~3u)) / 4)));
        U32(pool, value);
    }

    uint16_t ReadU16(uint32_t at) const
    {
        return uint16_t(image_[at - kRomBase] | (image_[at - kRomBase + 1] << 8));
    }

    Rom Build() const
    {
        Rom rom;
        rom.Assign(image_);
        return rom;
    }

private:
    std::vector<uint8_t> image_;
};

// A cartridge with the driver, one instrument of one region that plays one looping sample, and song 1, whose track 2
// plays the sample and whose track 8 plays square 1. Song 0 plays no track. `start_code` is the routine that starts a
// song, whose song table's literal load is at `start_literal`.
Cartridge TestCartridge(std::initializer_list<int> track2, const char* start_code = kStartCode,
                        uint32_t start_literal = 0x36)
{
    Cartridge c;
    c.Routine(kStart, start_code, start_literal, kSongTable);
    c.Routine(kLookup, kLookupCode, 4, kInstruments);
    c.Routine(kVoice, kVoiceCode, 0xC, kSamples);
    c.Routine(kStep, kStepCode, 0x1E, 0x0A080000);
    c.Routine(kPsgPitch, kPsgPitchCode, 0x14, kPsgPitchTable);
    c.Routine(kNoise, kNoiseCode, 0x10, kNoiseTable);
    c.Routine(kWave, kWaveCode, 2, kWaveTable);
    c.Routine(kWaveLevel, kWaveLevelCode, 0, kWaveLevels);
    c.Code(kNote, kNoteCode);

    // The NR32 settings of the three games: 25, 50, 75 and 100%.
    c.Bytes(kWaveLevels, {0x60, 0x40, 0x80, 0x20});

    // The step routine's tables, where its adr instructions point: 13 steps, then a byte for each key.
    for (int n = 0; n <= 12; n++)
    {
        c.U32(kStep + 0x38 + 4 * uint32_t(n), uint32_t(std::lround(8388608 * std::pow(2.0, n / 12.0))));
    }
    for (int key = 0; key < 128; key++)
    {
        c.U8(kStep + 0x6C + uint32_t(key), uint8_t((10 - key / 12) << 4 | key % 12));
    }
    for (int key = 0; key < 92; key++)
    {
        c.U16(kPsgPitchTable + 2 * uint32_t(key),
              uint16_t(std::lround(131072 / (65.40639 * std::pow(2.0, key / 12.0)))));
    }

    // Two songs, then the table's end.
    c.U32(kSongTable, kSong);
    c.U32(kSongTable + 4, kSong + 0x10);
    c.U32(kSongTable + 8, 0);
    c.U16(kSong, 0);
    c.U16(kSong + 0x10, 0x104); // tracks 2 and 8
    c.U16(kSong + 0x12, 0x10);
    c.U16(kSong + 0x14, 0x80);

    uint32_t at = kSong + 0x20;
    for (int b : track2)
    {
        c.U8(at++, uint8_t(b));
    }
    c.Bytes(kSong + 0x90, {0xF0, 0x01, 0xF1, 0xFF, 0xF2, 0x7F, 0x08, 0x48, 0xFF});

    // Instrument 0: keys 40-80 play sample 0, attack 0x40, decay 0x10, sustain 0x80, release 0x20. Instrument 1: a
    // square of duty 2 over every key.
    c.U16(kInstruments, 6);
    c.U16(kInstruments + 2, 14);
    c.U16(kInstruments + 4, 22);
    c.Bytes(kInstruments + 6, {40, 80, 0, 0, 0x40, 0x10, 0x80, 0x20});
    c.Bytes(kInstruments + 14, {0, 127, 2, 0, 0x10, 0x10, 0xF0, 0x10});

    // Sample 0: 64 points at 8000 Hz, looping from point 32.
    c.U32(kSamples, 4);
    c.U32(kSamples + 4, 0x40000000);
    c.U32(kSamples + 8, 8000 * 1024);
    c.U32(kSamples + 12, 32);
    c.U32(kSamples + 16, 64);
    for (uint32_t i = 0; i < 64; i++)
    {
        c.U8(kSamples + 20 + i, uint8_t(i % 2 ? 0xC0 : 0x40));
    }

    return c;
}

// Detection finds each table through its routine's literal.
void TestDetection()
{
    const Rom rom = TestCartridge({0xFF}).Build();
    DriverInfo info;
    std::string error;

    SEQLIB_CHECK(DetectDriver(rom, info, error));

    SEQLIB_CHECK(error.empty());
    SEQLIB_CHECK(info.song_table == kSongTable);
    SEQLIB_CHECK(info.song_count == 2);
    SEQLIB_CHECK(info.instrument_table == kInstruments);
    SEQLIB_CHECK(info.instrument_count == 2);
    SEQLIB_CHECK(info.sample_table == kSamples);
    SEQLIB_CHECK(info.sample_count == 1);
    SEQLIB_CHECK(info.step_table == kStep + 0x38);
    SEQLIB_CHECK(info.key_table == kStep + 0x6C);
    SEQLIB_CHECK(info.psg_pitch_table == kPsgPitchTable);
    SEQLIB_CHECK(info.noise_table == kNoiseTable);
    SEQLIB_CHECK(info.wave_table == kWaveTable);
    SEQLIB_CHECK(info.wave_levels == kWaveLevels);

    Song song;

    SEQLIB_CHECK(!ReadSong(rom, info, 0, song));
    SEQLIB_CHECK(ReadSong(rom, info, 1, song));

    SEQLIB_CHECK(song.mask == 0x104);
    SEQLIB_CHECK(song.tracks[2] == kSong + 0x20 && song.tracks[8] == kSong + 0x90);

    Region r;

    SEQLIB_CHECK(FindRegion(rom, info, 0, 60, r) && r.sample == 0 && r.envelope[2] == 0x80);
    SEQLIB_CHECK(!FindRegion(rom, info, 0, 30, r));

    Rom empty;
    empty.Assign(std::vector<uint8_t>(0x1000, 0));

    SEQLIB_CHECK(!DetectDriver(empty, info, error) && error.empty());

    // The Hamtaro games' routine that starts a song gives the same tables.
    const Rom hamtaro = TestCartridge({0xFF}, kHamtaroStartCode, 0x2E).Build();

    SEQLIB_CHECK(DetectDriver(hamtaro, info, error));

    SEQLIB_CHECK(info.start_routine == kStart && info.song_table == kSongTable && info.song_count == 2);
}

// The start of a note gives the driver's version, and the table of sound effects ends the song table.
void TestVersions()
{
    DriverInfo info;
    std::string error;

    SEQLIB_CHECK(DetectDriver(TestCartridge({0xFF}).Build(), info, error));

    SEQLIB_CHECK(info.version == Version::kSuperstarSaga && info.effect_table == 0 && info.song_count == 2);

    Cartridge sports = TestCartridge({0xFF});
    sports.Code(kNote, kHamSportsNoteCode);
    SEQLIB_CHECK(DetectDriver(sports.Build(), info, error) && info.version == Version::kHamSports);

    Cartridge older = TestCartridge({0xFF});
    older.Code(kNote, kHamtaro4NoteCode);
    SEQLIB_CHECK(DetectDriver(older.Build(), info, error) && info.version == Version::kHamtaro4);

    // The table of sound effects follows the song table's first entry, so song 1 is an effect.
    Cartridge effects = TestCartridge({0xFF});
    effects.Routine(kCommands, kCommandCode, 8, kCommandTable);
    effects.U32(kCommandTable + 4 * 5, kEffectHandler | 1);
    effects.Code(kEffectHandler, kEffectHandlerCode);
    effects.Bl(kEffectHandler + 6, kEffectStart);
    effects.Routine(kEffectStart, kEffectStartCode, 2, kSongTable + 4);

    SEQLIB_CHECK(DetectDriver(effects.Build(), info, error));

    SEQLIB_CHECK(info.effect_table == kSongTable + 4 && info.song_count == 1);
}

// B85A's and A84J's notes have a third byte, the track's volume. For a tied note, B85A's driver keeps the volume for
// later, and A84J's driver works the levels out at once. F1 is a note's length in both. In B85A, FA keeps a priority.
void TestHamtaroNotes()
{
    DriverInfo info;
    std::string error;
    Song song;

    // Pan 0x40, then a note at volume 0x80 tied to a note of 0xF1 ticks at volume 0x20.
    Cartridge sports = TestCartridge({0xF9, 0x4B, 0xF2, 0x40, 0xFA, 0x03, 0x00, 0x3C, 0x80, 0xF1, 0x3E, 0x20, 0xFF});
    sports.Code(kNote, kHamSportsNoteCode);
    const Rom b85a = sports.Build();
    SEQLIB_CHECK(DetectDriver(b85a, info, error) && ReadSong(b85a, info, 1, song));
    Sequencer b(b85a, info, song);
    SEQLIB_CHECK(b.Track(2).flags == (flag::kMuted | flag::kUsed));

    b.Step();

    SEQLIB_CHECK(b.Track(2).wait == 0xF1 && b.Track(2).volume == 0x20 && b.Channel(2).pitch == 0x3E * 256);
    // The first note's volume: the right side at 129 × 127 / 256 and the left at 129 × 64 / 256.
    SEQLIB_CHECK(b.Channel(2).right == 63 && b.Channel(2).left == 32);

    Cartridge older = TestCartridge({0xF9, 0x4B, 0xF2, 0x40, 0x00, 0x3C, 0x80, 0xF1, 0x3E, 0x20, 0xFF});
    older.Code(kNote, kHamtaro4NoteCode);
    const Rom a84j = older.Build();
    SEQLIB_CHECK(DetectDriver(a84j, info, error) && ReadSong(a84j, info, 1, song));
    Sequencer a(a84j, info, song);

    a.Step();

    SEQLIB_CHECK(a.Track(2).wait == 0xF1 && a.Channel(2).pitch == 0x3E * 256);
    // The tied note's volume: the right side at 33 × 127 / 256 and the left at 33 × 64 / 256.
    SEQLIB_CHECK(a.Channel(2).right == 16 && a.Channel(2).left == 8);
}

// A note's timing, levels and envelope. At tempo 75 and the divisor 0x4B, the count of 45 that the first frame leaves
// passes the divisor each frame, so a tick runs on each frame.
void TestSequencer()
{
    const Rom rom = TestCartridge({0xF9, 0x4B, 0xF1, 0x80, 0xF2, 0x40, 0x08, 0x3C, 0xF6, 0x02, 0xFF}).Build();
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, info, error));
    Song song;
    SEQLIB_CHECK(ReadSong(rom, info, 1, song));
    Sequencer seq(rom, info, song);

    // Frame 0: the restore and the first note-on with key 0, which no region holds. The tempo is still 0x78, whose
    // count of 120 passes 75: a tick runs, the wait of 1 ends, and the track reads its tempo, volume, pan and note.
    seq.Step();

    SEQLIB_CHECK(seq.Track(2).tempo == 0x4B);
    SEQLIB_CHECK(seq.Track(2).key == 60);
    SEQLIB_CHECK(seq.Track(2).wait == 8);
    const SampleChannel& c = seq.Channel(2);
    SEQLIB_CHECK(c.status == 0x81);
    SEQLIB_CHECK(c.level == 0x40);
    SEQLIB_CHECK(c.pitch == 60 * 256);
    // Volume 0x80 and pan 0x40: the right side at 129 × 127 / 256 and the left at 129 × 64 / 256.
    SEQLIB_CHECK(c.right == 63 && c.left == 32);

    // The attack rises by 0x40 a frame to 255, then the level drops to the sustain level.
    seq.Step();
    SEQLIB_CHECK(c.level == 0x80 && c.status == 0x81);
    seq.Step();
    SEQLIB_CHECK(c.level == 0xC0);
    seq.Step();
    SEQLIB_CHECK(c.level == 0xFF && c.status == 0x82);
    seq.Step();
    SEQLIB_CHECK(c.level == 0x80 && c.status == 0x83);

    // The note's 8 ticks end on frame 8. The release takes 0x20 a frame off the sustain level of 0x80, and the
    // channel stops when the level would reach 0.
    for (int i = 0; i < 4; i++)
    {
        seq.Step();
    }
    SEQLIB_CHECK(c.status == 0xC3 && c.level == 0x60);
    seq.Step();
    seq.Step();
    SEQLIB_CHECK(c.status == 0xC3 && c.level == 0x20);
    seq.Step();
    SEQLIB_CHECK(c.status == 0);
    SEQLIB_CHECK(!(seq.Track(2).flags & flag::kUsed));

    // Track 8's square note comes after track 2's, on the same frame.
    SEQLIB_CHECK(seq.Notes().size() == 2);
    const Note& n = seq.Notes().front();
    SEQLIB_CHECK(n.track == 2 && n.key == 60 && n.on == 0 && n.off == 8);
    SEQLIB_CHECK(seq.Notes()[1].track == 8 && seq.Notes()[1].key == 0x48 && seq.Notes()[1].instrument == 1);
}

// The rests' state. A note that isn't tied, with bit 7 in its key, sets it. From then on, a rest follows each note that
// isn't tied, where the note ends (0x0819B4C0). The F6 rest, the tied note's wait, the rest after a note and the F7 end
// without one. At tempo 75, a tick passes each frame.
void TestRests()
{
    const Rom rom = TestCartridge({0xF9, 0x4B, 0xF0, 0x00, 0xF1, 0xC0, 0x04, 0xBC, 0x03, 0xF6, 0x05,
                                   0x00, 0xBE, 0x02, 0x02, 0x40, 0x01, 0xF7, 0x03, 0x3C, 0x02, 0xFF})
                        .Build();
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, info, error));
    Song song;
    SEQLIB_CHECK(ReadSong(rom, info, 1, song));
    Sequencer seq(rom, info, song);

    std::vector<int> waits;
    std::vector<int> keys;
    for (int f = 0; f < 22; f++)
    {
        seq.Step();
        waits.push_back(seq.Track(2).wait);
        keys.push_back(seq.Track(2).key);
    }
    const bool playing = seq.Track(2).flags & flag::kUsed;
    seq.Step();

    // The note of 4 ticks ends on frame 4, and its rest of 3 on frame 7. The F6 rest of 5 runs to frame 12. The tied
    // note waits 2 ticks, and the note of key 64 then goes on from it for 2 more. Its rest of 1 ends on frame 17, where
    // the F7 finds no note and the last note starts. Its rest of 2 ends on frame 22, at the track's end.
    const std::vector<int> kExpectedWaits = {4, 3, 2, 1, 3, 2, 1, 5, 4, 3, 2, 1, 2, 1, 2, 1, 1, 3, 2, 1, 2, 1};
    SEQLIB_CHECK(waits == kExpectedWaits);
    SEQLIB_CHECK(keys[0] == 60 && keys[12] == 62 && keys[14] == 64 && keys[17] == 60);
    SEQLIB_CHECK(playing && !(seq.Track(2).flags & flag::kUsed));
    SEQLIB_CHECK(seq.Notes().size() >= 3);
    const Note& first = seq.Notes().front();
    const Note& last = seq.Notes().back();
    SEQLIB_CHECK(first.key == 60 && first.on == 0 && first.released == 4);
    SEQLIB_CHECK(last.key == 60 && last.on == 17 && last.released == 20);
}

// A square's envelope, as the driver works the hardware's envelope (0x0819AC84) and the PSG plays it. Square 1 plays
// key 0x48 for 8 ticks at tempo 75 and volume 0x80, with the envelope bytes C8 FF FF C8 that most of A88E's rising
// squares have: a rise from 0 and a fall to 0, each at a step of 1.
void TestPsgEnvelope()
{
    Cartridge c = TestCartridge({0xFF});
    c.Bytes(kInstruments + 18, {0xC8, 0xFF, 0xFF, 0xC8});
    c.Bytes(kSong + 0x90, {0xF9, 0x4B, 0xF0, 0x01, 0xF1, 0x80, 0xF2, 0x7F, 0x08, 0x48, 0xFF});
    const Rom rom = c.Build();
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, info, error));
    Song song;
    SEQLIB_CHECK(ReadSong(rom, info, 1, song));
    Sequencer seq(rom, info, song);
    const PsgChannel& p = seq.Psg(0);

    // Frame 0: the note starts the rise at volume 0 and a step of 1 (0x09). The channel sounds from volume 0.
    seq.Step();

    SEQLIB_CHECK(p.volume == 8 && p.state == 2 && p.written == 0x09 && p.count == 0);
    SEQLIB_CHECK(seq.PsgVolume(0) == 0);

    // The driver counts a step a frame. The hardware takes 64 steps a second.
    for (int f = 1; f < 8; f++)
    {
        seq.Step();
        SEQLIB_CHECK(p.state == 2 && p.count == f);
        SEQLIB_CHECK(seq.PsgVolume(0) == f);
    }

    // Frame 8: the note ends before the rise does, at the driver's volume of 8. The fall starts from it (0x81).
    seq.Step();

    SEQLIB_CHECK(p.state == 6 && p.written == 0x81 && p.count == 0);
    SEQLIB_CHECK(seq.PsgVolume(0) == 8);

    // The hardware reaches 0 on frame 15. The driver silences the channel on frame 16, once its count reaches 8.
    const int kFalls[] = {7, 6, 5, 4, 2, 1, 0};
    for (int v : kFalls)
    {
        seq.Step();
        SEQLIB_CHECK(seq.PsgVolume(0) == v);
    }
    SEQLIB_CHECK(p.state == 6);
    seq.Step();
    SEQLIB_CHECK(p.state == 0 && p.written == 0 && seq.PsgVolume(0) == -1);
    SEQLIB_CHECK(!seq.Playing());

    // The note lasts while the channel sounds. Each frame's level is the mean of the hardware's volumes at the frame's
    // start and end. Frames 7 and 8 have the same level, and the changes keep frame 7's alone.
    SEQLIB_CHECK(seq.Notes().size() == 1);
    const Note& n = seq.Notes().front();
    SEQLIB_CHECK(n.track == 8 && n.on == 0 && n.off == 15);
    // The track releases the note on frame 8. The beat uses that frame.
    SEQLIB_CHECK(n.released == 8);

    std::vector<double> levels;
    for (const Change& ch : seq.Changes())
    {
        levels.push_back(ch.left * 15 / 63);
    }

    const std::vector<double> kExpected = {0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 6.5, 5.5, 4.5, 3, 1.5, 0.5};
    SEQLIB_CHECK(levels.size() == kExpected.size());
    for (size_t i = 0; i < levels.size() && i < kExpected.size(); i++)
    {
        SEQLIB_CHECK(std::abs(levels[i] - kExpected[i]) < 1e-9);
    }
}

// The PSG channels' records on a song's first frames. In each version, the restore of a PSG channel uses the squares'
// pitch routine, for the noise too (0x0819ABF8 in A88E). With key 0, the noise's setting stays 0. A84J's driver counts
// the frames since the last setting even when it has written none (0x0814A844). A88E's driver counts them only after a
// setting.
void TestPsgRecords()
{
    DriverInfo info;
    std::string error;
    Song song;

    // Song 1 also plays track 11, the noise, which ends at once. The offsets count from the song's start.
    Cartridge sports = TestCartridge({0xFF});
    sports.Code(kNote, kHamSportsNoteCode);
    sports.U16(kSong + 0x10, 0x904);
    sports.U16(kSong + 0x16, 0xA0);
    sports.U8(kSong + 0x10 + 0xA0, 0xFF);
    const Rom b85a = sports.Build();
    SEQLIB_CHECK(DetectDriver(b85a, info, error) && ReadSong(b85a, info, 1, song));
    Sequencer b(b85a, info, song);

    b.Step();

    SEQLIB_CHECK(b.Psg(3).frequency == 0);
    SEQLIB_CHECK(b.Psg(1).count == 0);

    Cartridge older = TestCartridge({0xFF});
    older.Code(kNote, kHamtaro4NoteCode);
    const Rom a84j = older.Build();
    SEQLIB_CHECK(DetectDriver(a84j, info, error) && ReadSong(a84j, info, 1, song));
    Sequencer a(a84j, info, song);

    for (int f = 1; f <= 3; f++)
    {
        a.Step();
        SEQLIB_CHECK(a.Psg(1).count == f && a.Psg(1).written == 0);
    }
}

// The wave (0x0819AF18). Track 10 plays key 72 for 4 ticks at volume 94, then again at volume 200, at tempo 75. A
// note's start sets NR32 by the channel's volume over 4: 5 gives 50% and 12 gives 100%. The note's end mutes the wave.
// The wave's sample plays an octave below a square's at the same key.
void TestWave()
{
    Cartridge c = TestCartridge({0xFF});
    c.U16(kSong + 0x10, 0x404); // tracks 2 and 10
    c.Bytes(kSong + 0x90, {0xF9, 0x4B, 0xF0, 0x01, 0xF1, 0x5E, 0x04, 0x48, 0xF1, 0xC8, 0x04, 0x48, 0xFF});
    const Rom rom = c.Build();
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, info, error));
    Song song;
    SEQLIB_CHECK(ReadSong(rom, info, 1, song));
    Sequencer seq(rom, info, song);
    const PsgChannel& p = seq.Psg(2);

    seq.Step();
    SEQLIB_CHECK(p.volume == 5 && p.state == 2 && p.duty == 0);
    for (int f = 1; f < 4; f++)
    {
        seq.Step();
        SEQLIB_CHECK(p.state == 2);
    }

    // Frame 4: the second note restarts the wave without a frame of silence.
    seq.Step();
    SEQLIB_CHECK(p.volume == 12 && p.state == 2);
    for (int f = 5; f < 9; f++)
    {
        seq.Step();
    }
    SEQLIB_CHECK(p.state == 0);

    SEQLIB_REQUIRE_EQ(seq.Notes().size(), 2);
    SEQLIB_CHECK(seq.Notes()[0].track == 10 && seq.Notes()[0].on == 0 && seq.Notes()[0].off == 4);
    SEQLIB_CHECK(seq.Notes()[1].track == 10 && seq.Notes()[1].on == 4 && seq.Notes()[1].off == 8);

    std::vector<double> levels;
    for (const Change& ch : seq.Changes())
    {
        levels.push_back(ch.left * 15 / 63);
    }

    SEQLIB_REQUIRE_EQ(levels.size(), 2);
    SEQLIB_CHECK(std::abs(levels[0] - 7.5) < 1e-9 && std::abs(levels[1] - 15) < 1e-9);

    // The wave's preset: a cycle of 32 points at middle C on key 72, with an instant release.
    InstrumentSet set;
    Region r;
    SEQLIB_CHECK(FindRegion(rom, info, 1, 72, r));
    set.Add(Kind::kWave, 1, 72, r);
    Sf2File file;

    set.Build(rom, info, file);

    SEQLIB_REQUIRE_EQ(file.instruments.size(), 1);

    int waves = 0;
    for (const sf2::Sample& s : file.samples)
    {
        if (s.name == "Wave 2")
        {
            waves++;
            SEQLIB_CHECK(s.original_key == 72);
            SEQLIB_CHECK(std::abs(double(s.sample_rate) - 32 * 261.6256) < 1);
        }
    }
    SEQLIB_CHECK(waves == 1);

    for (const sf2::Zone& z : file.instruments[0].zones)
    {
        for (const sf2::Generator& g : z.generators)
        {
            SEQLIB_CHECK(g.oper != sf2::kReleaseVolEnv);
        }
    }
}

// A square plays the frequency setting that the driver writes from its table, 131072 / (2048 - setting) Hz, and the
// setting 0 below key 36 (0x0819A96C). The test's table rounds 131072 over each key's frequency. Key 72 gets 250, and
// key 24 gets 0, which plays 64 Hz.
void TestPsgPitch()
{
    Cartridge c = TestCartridge({0xFF});
    c.Bytes(kSong + 0x90, {0xF9, 0x4B, 0xF0, 0x01, 0xF1, 0x80, 0x04, 0x48, 0x04, 0x18, 0xFF});
    const Rom rom = c.Build();
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, info, error));
    Song song;
    SEQLIB_CHECK(ReadSong(rom, info, 1, song));
    Sequencer seq(rom, info, song);

    for (int f = 0; f < 8; f++)
    {
        seq.Step();
        if (f == 0)
        {
            SEQLIB_CHECK(seq.Psg(0).frequency == 2048 - 250);
        }
        if (f == 4)
        {
            SEQLIB_CHECK(seq.Psg(0).frequency == 0);
        }
    }

    double first = 1000;
    double second = 1000;
    for (const Change& ch : seq.Changes())
    {
        (ch.frame < 4 ? first : second) = ch.pitch;
    }

    SEQLIB_CHECK(std::abs(first - 12 * std::log2(131072.0 / 250 / 523.2511306)) < 1e-6);
    SEQLIB_CHECK(std::abs(second - 12 * std::log2(64 / 32.70319566)) < 1e-6);
}

// An entry of 0 in the sample table points the header at the table itself. The driver plays those bytes. The SoundFont
// leaves the sample out. Here sample 1 is empty, and the table's first 16 bytes read as a header of 8 points.
void TestEmptySample()
{
    Cartridge c = TestCartridge({0xFF});
    c.U32(kSamples, 16);
    c.U32(kSamples + 4, 0);
    c.U32(kSamples + 8, 0);
    c.U32(kSamples + 12, 8);
    c.U32(kSamples + 16, 0x40000000);
    c.U32(kSamples + 20, 8000 * 1024);
    c.U32(kSamples + 24, 32);
    c.U32(kSamples + 28, 64);
    const Rom rom = c.Build();
    DriverInfo info;
    std::string error;

    SEQLIB_CHECK(DetectDriver(rom, info, error) && info.sample_count == 4);
    SampleHeader h;
    SEQLIB_CHECK(ReadSample(rom, info, 1, h) && h.empty && h.address == kSamples && h.end == 8);
    SEQLIB_CHECK(ReadSample(rom, info, 0, h) && !h.empty && h.rate == 8000 * 1024);

    Region r;
    SEQLIB_CHECK(FindRegion(rom, info, 0, 60, r));
    r.sample = 1;
    InstrumentSet empty;
    empty.Add(Kind::kSample, 0, 60, r);
    Sf2File without;

    SEQLIB_CHECK(empty.Build(rom, info, without).size() == 1); // "Instrument 0 has nothing to play"

    SEQLIB_CHECK(without.samples.empty() && without.instruments.empty());

    r.sample = 0;
    InstrumentSet full;
    full.Add(Kind::kSample, 0, 60, r);
    Sf2File with;

    SEQLIB_CHECK(full.Build(rom, info, with).empty());

    SEQLIB_CHECK(with.samples.size() == 1 && with.instruments.size() == 1);
}

// The listing reads each version's commands as its driver does.
void TestDump()
{
    const std::initializer_list<int> kTrack = {0xFA, 0x03, 0x00, 0x3C, 0x80, 0xF1, 0x3E, 0x20, 0xFF};
    DriverInfo info;
    std::string error;
    const std::string path = Utf8(TempPath("alphadream_dump.txt"));

    const auto dump = [&](const Rom& rom)
    {
        SEQLIB_CHECK(DetectDriver(rom, info, error) && DumpSong(rom, info, 1, path, error));
        const std::vector<uint8_t> text = ReadAll(path);
        return std::string(text.begin(), text.end());
    };

    // In B85A, FA sets the priority, a note's third byte is the volume, and F1 is a note's length.
    Cartridge sports = TestCartridge(kTrack);
    sports.Code(kNote, kHamSportsNoteCode);

    const std::string b85a = dump(sports.Build());

    SEQLIB_CHECK(b85a.find("  +0000  priority 3\n") != std::string::npos);
    SEQLIB_CHECK(b85a.find("  +0002  tie key 60 volume 128\n") != std::string::npos);
    SEQLIB_CHECK(b85a.find("  +0005  note key 62 length 241 volume 32\n") != std::string::npos);
    SEQLIB_CHECK(b85a.find("  +0008  end\n") != std::string::npos);

    // In A84J, FA is a note's length too.
    Cartridge older = TestCartridge(kTrack);
    older.Code(kNote, kHamtaro4NoteCode);

    const std::string a84j = dump(older.Build());

    SEQLIB_CHECK(a84j.find("  +0000  note key 3 length 250 volume 0\n") != std::string::npos);

    // In A88E, F1 is the volume and a note has two bytes.
    const std::string a88e = dump(TestCartridge({0xF1, 0x80, 0x08, 0x3C, 0xFF}).Build());

    SEQLIB_CHECK(a88e.find("  +0000  volume 128\n") != std::string::npos);
    SEQLIB_CHECK(a88e.find("  +0002  note key 60 length 8\n") != std::string::npos);
    SEQLIB_CHECK(a88e.find("  +0004  end\n") != std::string::npos);
}

// The conversion writes a track for each track that plays notes, and a preset for each instrument.
void TestConversion()
{
    const Rom rom = TestCartridge({0xF1, 0x80, 0x08, 0x3C, 0x08, 0x40, 0xF8, 0xF9, 0xFF}).Build();
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, info, error));
    ConvertSettings settings = TempSettings("alphadream");

    const SongReport report = ConvertSong(rom, info, 1, settings, nullptr);

    SEQLIB_CHECK(report.ok);
    SEQLIB_CHECK(report.tracks == 2);
    SEQLIB_CHECK(report.loop_start >= 0 && report.loop_end > report.loop_start);

    const MidiEvents midi = ReadMidi(report.midi_path);
    SEQLIB_CHECK(midi.tracks.size() == 3);
    const Sf2Records sf2 = ReadSf2(report.sf2_path);
    SEQLIB_CHECK(sf2.Count("phdr", 38) == 3); // two presets and the closing record

    // At the starting tempo of 0x78 and the divisor 0x4B, 8 ticks last 5 frames. Track 2 plays keys 60 and 64 and goes
    // back to key 60 on frame 10. The note that follows the goto starts on the same frame. The loop runs from frame 10
    // to frame 20, and a note of key 60 starts on each marker.
    settings.frame_timing = true;

    const SongReport frames = ConvertSong(rom, info, 1, settings, nullptr);

    SEQLIB_CHECK(frames.ok);
    SEQLIB_CHECK(std::fabs(frames.loop_start - 10 / ntdlib::agb::kFrameRate) < 1e-9);
    SEQLIB_CHECK(std::fabs(frames.loop_end - 20 / ntdlib::agb::kFrameRate) < 1e-9);

    const MidiEvents timed = ReadMidi(frames.midi_path);
    std::vector<uint64_t> markers;
    for (const auto& e : timed.tracks[0].events)
    {
        if (e.bytes[0] == 0xFF && e.bytes[1] == 0x06)
        {
            markers.push_back(e.tick);
        }
    }

    std::vector<uint64_t> starts;
    for (const auto& e : timed.tracks[1].events)
    {
        if ((e.bytes[0] & 0xF0) == 0x90 && e.bytes[1] == 60 && e.bytes[2] > 0)
        {
            starts.push_back(e.tick);
        }
    }

    SEQLIB_REQUIRE_EQ(markers.size(), 2);
    SEQLIB_CHECK(std::count(starts.begin(), starts.end(), markers[0]) == 1);
    SEQLIB_CHECK(std::count(starts.begin(), starts.end(), markers[1]) == 1);
}

// A note that its track releases on the frame it starts plays through the release, from the level that the channel had.
// At tempo 255, three or four ticks run each frame. Track 2 plays key 60 for 23 ticks, which reaches the sustain level
// of 0x80, then key 60 for 1 tick with a rest of 16. Frame 7 runs ticks 21 to 24, so the second note starts and ends on
// it. The release takes 0x20 a frame: the second note plays at 0x60, 0x40 and 0x20, and the channel stops on the fourth
// frame.
void TestReleasedNote()
{
    const Rom rom = TestCartridge({0xF9, 0xFF, 0x17, 0x3C, 0x01, 0xBC, 0x10, 0xFF}).Build();
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, info, error));
    Song song;
    SEQLIB_CHECK(ReadSong(rom, info, 1, song));
    Sequencer seq(rom, info, song);

    std::vector<int> levels;
    for (int f = 0; f < 20; f++)
    {
        seq.Step();
        levels.push_back(seq.Channel(2).level);
    }

    std::vector<Note> notes;
    for (const Note& n : seq.Notes())
    {
        if (n.track == 2)
        {
            notes.push_back(n);
        }
    }

    SEQLIB_REQUIRE_EQ(notes.size(), 2);
    const Note& n = notes[1];
    SEQLIB_CHECK(n.released == n.on && n.ended && n.off == n.on + 4);
    SEQLIB_CHECK(levels[n.on] == 0x60 && levels[n.on + 1] == 0x40 && levels[n.on + 2] == 0x20);

    std::vector<double> shares;
    for (const Change& c : seq.Changes())
    {
        if (c.track == 2 && c.frame >= n.on)
        {
            shares.push_back(c.left / (seq.Channel(2).left * 63.0 / 127));
        }
    }

    SEQLIB_REQUIRE_EQ(shares.size(), 3);
    SEQLIB_CHECK(std::abs(shares[0] - 0x60 / 255.0) < 1e-9 && std::abs(shares[2] - 0x20 / 255.0) < 1e-9);

    // The conversion keeps the note.
    ConvertSettings settings = TempSettings("alphadream_released");

    const SongReport report = ConvertSong(rom, info, 1, settings, nullptr);

    SEQLIB_CHECK(report.ok);
    const MidiEvents midi = ReadMidi(report.midi_path);
    SEQLIB_CHECK(midi.Find(0x90 | MelodicChannel(2)).size() >= 2);
}

// A song loops for the period that the most tracks fill. At tempo 75, tracks 2 and 3 go back every 10 frames and track
// 8 every 15. The loop starts on frame 15, where track 8 first goes back, and lasts 10 frames.
void TestLoopPeriod()
{
    Cartridge c = TestCartridge({0xF9, 0x4B, 0x0A, 0x3C, 0xF8, 0xFB, 0xFF});
    c.U16(kSong + 0x10, 0x10C); // tracks 2, 3 and 8
    c.U16(kSong + 0x14, 0x40);
    c.U16(kSong + 0x16, 0x80);
    c.Bytes(kSong + 0x50, {0xF9, 0x4B, 0x0A, 0x40, 0xF8, 0xFB, 0xFF});
    c.Bytes(kSong + 0x90, {0xF9, 0x4B, 0xF0, 0x01, 0x0F, 0x48, 0xF8, 0xFB, 0xFF});
    const Rom rom = c.Build();
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, info, error));
    ConvertSettings settings = TempSettings("alphadream_period");

    const SongReport report = ConvertSong(rom, info, 1, settings, nullptr);

    SEQLIB_CHECK(report.ok && report.tracks == 3);
    SEQLIB_CHECK(std::fabs(report.loop_start - 15 / ntdlib::agb::kFrameRate) < 1e-9);
    SEQLIB_CHECK(std::fabs(report.loop_end - 25 / ntdlib::agb::kFrameRate) < 1e-9);
    SEQLIB_CHECK(std::fabs(report.seconds - 35 / ntdlib::agb::kFrameRate) < 1e-9);
}

// The performance of TestConversion's song. Track 2 sets its volume to 0x80, plays keys 60 and 64 for 5 frames each,
// and goes back to key 60 on frames 10, 20 and 30. The song ends on frame 30, after two passes of the loop. Track 8
// sets its volume to 0xFF and the pan to the centre, and plays key 72.
void TestPerformance()
{
    const Rom rom = TestCartridge({0xF1, 0x80, 0x08, 0x3C, 0x08, 0x40, 0xF8, 0xF9, 0xFF}).Build();
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, info, error));

    const Performance perf = PerformSong(rom, info, 1, TempSettings("alphadream_performance"));

    SEQLIB_CHECK(perf.base == kSong + 0x10 && perf.end_tick == 30);
    SEQLIB_CHECK(perf.loop && perf.loop->start == 10 && perf.loop->end == 20);
    SEQLIB_CHECK(perf.track_names[2] == "Track 2" && perf.track_names[8] == "Square 1");

    // Each note leads to its bytes, in the stretch that runs them: the track's start, then each pass of the loop. The
    // pass that would start on frame 30 belongs to the next one.
    std::vector<int> keys;
    std::vector<uint32_t> offsets;
    std::vector<uint32_t> stretches;
    for (const TrackEvent& e : perf.tracks[2])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            keys.push_back(e.key);
            offsets.push_back(perf.note_commands.at(e.note - 1).offset);
            stretches.push_back(perf.note_commands.at(e.note - 1).stretch);
        }
    }

    SEQLIB_CHECK(keys == std::vector<int>({60, 64, 60, 64, 60, 64}));
    SEQLIB_CHECK(offsets == std::vector<uint32_t>({0x12, 0x14, 0x12, 0x14, 0x12, 0x14}));
    SEQLIB_CHECK(stretches == std::vector<uint32_t>({0, 0, 1, 1, 2, 2}));
    SEQLIB_REQUIRE_EQ(perf.stretches[2].size(), 3);
    const Stretch& open = perf.stretches[2][0];
    const Stretch& loop = perf.stretches[2][1];
    SEQLIB_CHECK(open.entry == Entry::kOpen && open.tick == 0 && open.first == 0x10 && open.last == 0x16);
    SEQLIB_CHECK(loop.entry == Entry::kLoop && loop.tick == 10 && loop.first == 0x12 && loop.last == 0x16);
    SEQLIB_CHECK(perf.midi_places.size() == perf.note_commands.size());

    // The offsets name the decoder's items.
    const ntdlib::alphadream::Decoded decoded =
        ntdlib::alphadream::DecodeRomSequence(rom, info, ntdlib::alphadream::SequenceOfSong(rom, info, 1));
    const std::vector<uint32_t> items = ntdlib::alphadream::ItemOffsets(decoded.sequence, info.version);

    const auto item = std::find(items.begin(), items.end(), 0x14u);
    SEQLIB_REQUIRE(item != items.end());
    const ntdlib::commands::Command& note = decoded.sequence.items[size_t(item - items.begin())];
    SEQLIB_CHECK(note.kind == ntdlib::commands::Kind::kNote && note.key == 64);

    // F1 sets the volume, and F2's 0x7F is the centre.
    SEQLIB_CHECK(perf.mix[2] == std::vector<MixChange>({{0, Mix::kVolume, 0x80}}));
    SEQLIB_CHECK(perf.mix[8] == std::vector<MixChange>({{0, Mix::kVolume, 0xFF}, {0, Mix::kPan, 0}}));
}

} // namespace

int RunTests()
{
    if (!ntdlib::testing::agb_files::MakeTempFolder())
    {
        std::fprintf(stderr, "couldn't make a temp folder\n");
        return 1;
    }

    TestDetection();
    TestVersions();
    TestHamtaroNotes();
    TestSequencer();
    TestRests();
    TestPsgEnvelope();
    TestPsgRecords();
    TestWave();
    TestPsgPitch();
    TestEmptySample();
    TestDump();
    TestConversion();
    TestLoopPeriod();
    TestReleasedNote();
    TestPerformance();

    std::error_code ec;
    std::filesystem::remove_all(ntdlib::testing::agb_files::g_temp, ec);

    return seqlib::testing::Summary("alphadream");
}

} // namespace ntdlib::sequence_export::alphadream

int main()
{
    return ntdlib::sequence_export::alphadream::RunTests();
}
