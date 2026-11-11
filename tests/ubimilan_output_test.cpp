// SPDX-License-Identifier: MIT

// Tests of ubimilan::Output on small synthetic cartridges: the stores of the driver's strict mode and the bytes of its
// mixer. Each expected value follows a rule of the driver's code, worked out by hand. The comments give the rule.
//
// A run of the mixer takes 8 points of each voice. Points p at volume 64 give a sum of 64p. With a point halfway
// between each pair of sums, and each point averaged with the one before, a point's two bytes come to (3 * last + p) /
// 4 and (last + 3 * p) / 4. The tests' points are multiples of 4, and the sums then round nowhere.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/testing/agb_files.h"
#include "ntdlib/testing/ubimilan_image.h"
#include "ntdlib/ubimilan/driver.h"
#include "ntdlib/ubimilan/output.h"
#include "ntdlib/ubimilan/sequencer.h"

namespace
{

namespace ubimilan = ntdlib::ubimilan;
using ntdlib::agb::RegisterWrite;
using ntdlib::agb::Rom;
using ntdlib::testing::ubimilan_image::TestImage;
using ubimilan::DriverInfo;
using ubimilan::Output;
using Instrument = TestImage::Instrument;

constexpr uint32_t kNr10 = 0x04000060;
constexpr uint32_t kNr11 = 0x04000062;
constexpr uint32_t kNr13 = 0x04000064;
constexpr uint32_t kNr21 = 0x04000068;
constexpr uint32_t kNr23 = 0x0400006C;
constexpr uint32_t kNr30 = 0x04000070;
constexpr uint32_t kNr31 = 0x04000072;
constexpr uint32_t kNr33 = 0x04000074;
constexpr uint32_t kNr41 = 0x04000078;
constexpr uint32_t kNr43 = 0x0400007C;

// The bytes of each of Timer 0's frames: 16 bytes in a run, and (frame + 1) * 280,896 / (16 * 1,025) runs by a frame's
// end.
constexpr size_t kRunBytes = 16;

DriverInfo Detect(const Rom& rom)
{
    DriverInfo info;
    std::string error;
    XSFLIB_CHECK(ubimilan::DetectDriver(rom, {}, info, error));
    return info;
}

std::vector<int8_t> Constant(size_t points, int point)
{
    return std::vector<int8_t>(points, int8_t(point));
}

// The piece that TestImage's `image` holds as its only piece, played for `frames` frames. Holds the bytes.
struct Played
{
    std::vector<int8_t> bytes;
    std::vector<std::vector<RegisterWrite>> writes;
    bool reset = false;
};

Played Play(const TestImage& image, int frames)
{
    const Rom rom = image.ToRom();
    const DriverInfo info = Detect(rom);
    Output output(rom, info, 0);
    Played played;
    XSFLIB_CHECK(output.Valid());
    for (int frame = 0; frame < frames; frame++)
    {
        output.Frame();
        played.bytes.insert(played.bytes.end(), output.Fifo().begin(), output.Fifo().end());
        played.writes.push_back(output.SoundWrites());
    }

    played.reset = output.Song().Reset();
    return played;
}

// The value of the store to `address` in `writes`, or -1.
long long StoreTo(const std::vector<RegisterWrite>& writes, uint32_t address)
{
    long long value = -1;
    for (const RegisterWrite& write : writes)
    {
        if (write.address == address)
        {
            value = write.value;
        }
    }

    return value;
}

void TestRuns()
{
    // Timer 0's reload gives 1,025 cycles a point, and 8 frames hold 137.02 runs: 17 runs in each of the first 7
    // frames, and 18 in the eighth.
    TestImage image;
    const int sequence = image.AddSequence({0x20, 0x00, 0xFC});
    image.AddPiece(0, {sequence});
    const Rom rom = image.ToRom();
    const DriverInfo info = Detect(rom);
    XSFLIB_CHECK(ubimilan::OutputCycles(info) == 1025);
    XSFLIB_CHECK(info.warnings.empty());
    XSFLIB_CHECK(info.fine_table == ntdlib::testing::ubimilan_image::kFineTable);
    Output output(rom, info, 0);
    for (int frame = 0; frame < 7; frame++)
    {
        output.Frame();
        XSFLIB_CHECK(output.Fifo().size() == 17 * kRunBytes);
    }

    output.Frame();
    XSFLIB_CHECK(output.Fifo().size() == 18 * kRunBytes);
}

void TestVoiceBytes()
{
    // Key 40 on the first frame: the constant points of 64 at volume 64 come in over a point's two bytes.
    TestImage image;
    image.SetKey(40, image.AddSample(Constant(4000, 64)));
    const int sequence = image.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x20, 0x00, 0xFC});
    image.AddPiece(0, {sequence});
    const Played played = Play(image, 1);
    SEQLIB_REQUIRE(played.bytes.size() == 17 * kRunBytes);
    XSFLIB_CHECK(played.bytes[0] == 16); // (3 * 0 + 64) / 4
    XSFLIB_CHECK(played.bytes[1] == 48); // (0 + 3 * 64) / 4
    for (size_t i = 2; i < played.bytes.size(); i++)
    {
        XSFLIB_CHECK(played.bytes[i] == 64);
    }
}

void TestVolume()
{
    // A byte is the sum over 64: a point of 64 at volume 16 gives 16. A negative sum rounds down: -33 at volume 1 gives
    // -33 / 64. An arithmetic shift of each half takes that to -1.
    TestImage image;
    image.SetKey(40, image.AddSample(Constant(4000, 64)));
    image.SetKey(41, image.AddSample(Constant(4000, -33)));
    const int sequence = image.AddSequence(
        {0x00, 0x00, 0x99, 40, 16, 0x08, 0x00, 0x89, 40, 0, 0x00, 0x00, 0x99, 41, 1, 0x20, 0x00, 0xFC});
    image.AddPiece(0, {sequence});
    const Played played = Play(image, 20);
    XSFLIB_CHECK(played.bytes[100] == 16);
    XSFLIB_CHECK(played.bytes.back() == -1);

    // Each half rounds down: a sum of -65 halves to -33, and -33 and -33 over 64 give -2. A division towards 0 would
    // give -32 and -32, and -1.
    TestImage halves;
    halves.SetKey(40, halves.AddSample(Constant(4000, -65)));
    halves.AddPiece(0, {halves.AddSequence({0x00, 0x00, 0x99, 40, 1, 0x20, 0x00, 0xFC})});
    XSFLIB_CHECK(Play(halves, 2).bytes.back() == -2);

    // A point in between rounds down too. Between sums of -126 and -43, it's -85, whose half is -43. Every byte then
    // gives -2: (-63 - 43) / 64 and (-43 - 22) / 64. A division towards 0 would give -84 and -42, and bytes of -1.
    std::vector<int8_t> points;
    for (int i = 0; i < 2000; i++)
    {
        points.push_back(-126);
        points.push_back(-43);
    }

    TestImage between;
    between.SetKey(40, between.AddSample(points));
    between.AddPiece(0, {between.AddSequence({0x00, 0x00, 0x99, 40, 1, 0x20, 0x00, 0xFC})});
    const std::vector<int8_t> between_bytes = Play(between, 2).bytes;
    for (size_t i = between_bytes.size() - kRunBytes; i < between_bytes.size(); i++)
    {
        XSFLIB_CHECK(between_bytes[i] == -2);
    }
}

void TestLastPointsLeftOut()
{
    // A run moves a voice on 8 points, and adds its points only if the move leaves it playing. A sample of 16 points
    // stops in its second run, whose points stay out: only 8 are heard. After them, the sums fall in the same two steps
    // as they rose.
    TestImage image;
    image.SetKey(40, image.AddSample(Constant(16, 64)));
    const int sequence = image.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x20, 0x00, 0xFC});
    image.AddPiece(0, {sequence});
    const Played played = Play(image, 1);
    XSFLIB_CHECK(played.bytes[15] == 64);
    XSFLIB_CHECK(played.bytes[16] == 48);
    XSFLIB_CHECK(played.bytes[17] == 16);
    XSFLIB_CHECK(played.bytes[18] == 0);

    // One more point takes the voice through a second run: 16 points are heard.
    TestImage longer;
    longer.SetKey(40, longer.AddSample(Constant(17, 64)));
    const int same = longer.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x20, 0x00, 0xFC});
    longer.AddPiece(0, {same});
    const Played played_longer = Play(longer, 1);
    XSFLIB_CHECK(played_longer.bytes[31] == 64);
    XSFLIB_CHECK(played_longer.bytes[32] == 48);
    XSFLIB_CHECK(played_longer.bytes[33] == 16);
    XSFLIB_CHECK(played_longer.bytes[34] == 0);
}

void TestLoop()
{
    // A voice goes back to its loop's start once a run takes it to the loop's end. A DMA word transfer reads from the
    // word that holds the position: a loop that starts at point 10 plays from point 8.
    std::vector<int8_t> points;
    for (int i = 0; i < 16; i++)
    {
        points.push_back(int8_t(4 * i));
    }

    TestImage image;
    image.SetKey(40, image.AddSample(points, true, 10, 16));
    const int sequence = image.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x20, 0x00, 0xFC});
    image.AddPiece(0, {sequence});
    const Played played = Play(image, 1);

    // The first run plays points 0 to 7, the second points 8 to 15, and each run after them points 8 to 15 again.
    const auto point_bytes = [&](size_t run, int point, int last)
    {
        const size_t at = run * kRunBytes + 2 * size_t(point);
        XSFLIB_CHECK(played.bytes[at] == int8_t((3 * 4 * last + 4 * (point + (run ? 8 : 0))) / 4));
        XSFLIB_CHECK(played.bytes[at + 1] == int8_t((4 * last + 3 * 4 * (point + (run ? 8 : 0))) / 4));
    };

    for (int i = 1; i < 8; i++)
    {
        point_bytes(0, i, i - 1);
    }

    point_bytes(1, 0, 7);
    for (int i = 1; i < 8; i++)
    {
        point_bytes(1, i, 8 + i - 1);
        point_bytes(2, i, 8 + i - 1);
    }

    point_bytes(2, 0, 15);
}

void TestClip()
{
    // Each byte clips to 8 bits after the voices' sum: 4 voices of 100 at volume 64 give 400.
    TestImage image;
    image.SetKey(40, image.AddSample(Constant(4000, 100)));
    const std::vector<uint8_t> four_high = {0x00, 0x00, 0x99, 40,   64,   0x00, 0x00, 0x99, 40,   64,   0x00, 0x00,
                                            0x99, 40,   64,   0x00, 0x00, 0x99, 40,   64,   0x20, 0x00, 0xFC};
    image.AddPiece(0, {image.AddSequence(four_high)});
    XSFLIB_CHECK(Play(image, 1).bytes[100] == 127);

    TestImage other;
    other.SetKey(41, other.AddSample(Constant(4000, -100)));
    const std::vector<uint8_t> four_low = {0x00, 0x00, 0x99, 41,   64,   0x00, 0x00, 0x99, 41,   64,   0x00, 0x00,
                                           0x99, 41,   64,   0x00, 0x00, 0x99, 41,   64,   0x20, 0x00, 0xFC};
    other.AddPiece(0, {other.AddSequence(four_low)});
    XSFLIB_CHECK(Play(other, 1).bytes[100] == -128);
}

void TestVoices()
{
    // The kit's notes take the last voice that no sound has. A note-off stops the voice of the kit's last note, and the
    // next note takes it. Levels of 8, 16 and 32 tell the voices apart.
    TestImage image;
    image.SetKey(40, image.AddSample(Constant(40000, 8)));
    image.SetKey(41, image.AddSample(Constant(40000, 16)));
    image.SetKey(42, image.AddSample(Constant(40000, 32)));
    const int sequence = image.AddSequence({
        0x00, 0x00, 0x99, 40,   64,   0x00, 0x00, 0x99, 41, 64, 0x02, 0x00, // 2 voices, 3 frames
        0x89, 41,   0,    0x02, 0x00,                                       // stops key 41's voice, 3 frames
        0x99, 42,   64,   0x20, 0x00,                                       // takes it
        0xFC,
    });
    image.AddPiece(0, {sequence});
    const Rom rom = image.ToRom();
    const DriverInfo info = Detect(rom);
    Output output(rom, info, 0);
    output.Frame();
    XSFLIB_CHECK(output.Fifo().back() == 24);
    output.Frame();
    output.Frame();
    output.Frame();
    XSFLIB_CHECK(output.Fifo().back() == 8);
    output.Frame();
    output.Frame();
    output.Frame();
    XSFLIB_CHECK(output.Fifo().back() == 40);
}

void TestVoiceChoices()
{
    // A note-off frees its voice at once: a note in the same frame takes the voice again.
    TestImage same;
    same.SetKey(40, same.AddSample(Constant(40000, 8)));
    same.AddPiece(0, {same.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x00, 0x00, 0x89, 40, 0, 0x00, 0x00, 0x99, 40, 64,
                                        0x20, 0x00, 0xFC})});
    const Rom same_rom = same.ToRom();
    const DriverInfo same_info = Detect(same_rom);
    Output same_output(same_rom, same_info, 0);
    same_output.Frame();
    XSFLIB_CHECK(same_output.Song().VoicePlays(3));
    XSFLIB_CHECK(!same_output.Song().VoicePlays(2));

    // A sample that has ended leaves its voice free for the next frame's sound update. 64 points end in the first
    // frame.
    TestImage ended;
    ended.SetKey(40, ended.AddSample(Constant(64, 8)));
    ended.SetKey(41, ended.AddSample(Constant(40000, 16)));
    ended.AddPiece(0, {ended.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x01, 0x00, 0x99, 41, 64, 0x20, 0x00, 0xFC})});
    const Rom ended_rom = ended.ToRom();
    const DriverInfo ended_info = Detect(ended_rom);
    Output ended_output(ended_rom, ended_info, 0);
    ended_output.Frame();
    XSFLIB_CHECK(!ended_output.Song().VoicePlays(3));
    ended_output.Frame();
    ended_output.Frame();
    XSFLIB_CHECK(ended_output.Song().VoicePlays(3));
    XSFLIB_CHECK(!ended_output.Song().VoicePlays(2));

    // Channel 9's programs from 12 up move its keys on through the kit. Program 11 goes to the PSG.
    const auto level_after = [](int program)
    {
        TestImage image;
        image.SetKey(40, image.AddSample(Constant(40000, 8)));
        image.SetKey(40 + program, image.AddSample(Constant(40000, 16)));
        image.AddPiece(
            0, {image.AddSequence({0x00, 0x00, 0xC9, uint8_t(program), 0x00, 0x00, 0x99, 40, 64, 0x20, 0x00, 0xFC})});
        return int(Play(image, 1).bytes.back());
    };

    XSFLIB_CHECK(level_after(12) == 16);
    XSFLIB_CHECK(level_after(11) == 8);

    // A sample without a loop keeps the last loop's points in the voice's record.
    TestImage kept;
    kept.SetKey(40, kept.AddSample(Constant(4000, 8), true, 16, 3200));
    kept.SetKey(41, kept.AddSample(Constant(40000, 16)));
    kept.AddPiece(8, {kept.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x01, 0x00, 0x99, 41, 64, 0x20, 0x00, 0xFC})});
    const Rom kept_rom = kept.ToRom();
    const DriverInfo kept_info = Detect(kept_rom);
    Output kept_output(kept_rom, kept_info, 0);
    kept_output.Frame();
    const std::array<uint32_t, 6> looping = kept_output.Song().VoiceState(3);
    kept_output.Frame();
    kept_output.Frame();
    const std::array<uint32_t, 6> once = kept_output.Song().VoiceState(3);
    XSFLIB_CHECK(looping[5] == 1u);
    XSFLIB_CHECK(once[5] == 0u);
    XSFLIB_CHECK(once[1] == looping[1]);
    XSFLIB_CHECK(once[3] == looping[3]);

    // Only a byte of 1 makes a sample loop.
    TestImage byte;
    const int sample = byte.AddSample(Constant(64, 8), true, 0, 64);
    byte.Put8(byte.Resource(sample) + 1, 2);
    byte.SetKey(40, sample);
    byte.AddPiece(0, {byte.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x20, 0x00, 0xFC})});
    const Rom byte_rom = byte.ToRom();
    const DriverInfo byte_info = Detect(byte_rom);
    Output byte_output(byte_rom, byte_info, 0);
    byte_output.Frame();
    XSFLIB_CHECK(!byte_output.Song().VoicePlays(3));
}

void TestVoiceMinusOne()
{
    // A fifth note finds no voice. The game takes voice -1, whose second record is voice 3's first: the note replaces
    // the sample of the first note's voice 3. A note-off then has no voice to stop, whatever its key.
    TestImage image;
    image.SetKey(40, image.AddSample(Constant(40000, 4)));
    image.SetKey(41, image.AddSample(Constant(40000, 8)));
    image.SetKey(42, image.AddSample(Constant(40000, 16)));
    image.SetKey(43, image.AddSample(Constant(40000, 32)));
    image.SetKey(44, image.AddSample(Constant(40000, 40)));
    const int sequence = image.AddSequence({
        0x00, 0x00, 0x99, 40,   64,   0x00, 0x00, 0x99, 41,   64,   0x00, 0x00, 0x99, 42,   64,   0x00, 0x00,
        0x99, 43,   64,   0x00, 0x00, 0x99, 44,   64,   0x02, 0x00, 0x89, 44,   0,    0x20, 0x00, 0xFC,
    });
    image.AddPiece(0, {sequence});
    const Rom rom = image.ToRom();
    const DriverInfo info = Detect(rom);
    Output output(rom, info, 0);
    output.Frame();
    XSFLIB_CHECK(output.Fifo().back() == 8 + 16 + 32 + 40);
    XSFLIB_CHECK(output.Song().VoicePlays(3));
    for (int frame = 0; frame < 6; frame++)
    {
        output.Frame();
    }

    XSFLIB_CHECK(output.Fifo().back() == 8 + 16 + 32 + 40);
}

void TestKeptVoice()
{
    // A piece with bit 3 keeps voice 3 for the kit: each note replaces the last, and the voice is never free.
    TestImage image;
    image.SetKey(40, image.AddSample(Constant(40000, 8)));
    image.SetKey(41, image.AddSample(Constant(40000, 16)));
    const int sequence = image.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x00, 0x00, 0x99, 41, 64, 0x20, 0x00, 0xFC});
    image.AddPiece(8, {sequence});
    const Rom rom = image.ToRom();
    const DriverInfo info = Detect(rom);
    Output output(rom, info, 0);
    output.Frame();
    XSFLIB_CHECK(output.Fifo().back() == 16);
    XSFLIB_CHECK(output.Song().VoicePlays(3));
    XSFLIB_CHECK(!output.Song().VoicePlays(2));
}

void TestSquares()
{
    // The instrument's bytes, the velocity ORed into the envelope, and the frequency of the key's place: 36 keys down.
    // TestImage's frequency table gives place i the value 1000 + i.
    TestImage image;
    image.SetInstrument(0, 1, {{0x11, 0x80, 0x05, 0x80}});
    image.SetInstrument(1, 2, {{0x40, 0x03, 0xC0}});
    const int sequence = image.AddSequence({
        0x00, 0x00, 0xC0, 1,    0x00, 0x00, 0xC1, 2, 0x00, 0x00, // programs
        0x90, 60,   0x70, 0x00, 0x00,                            // square 1's key 60
        0x91, 72,   0x60, 0x02, 0x00,                            // square 2's key 72
        0x80, 61,   0,    0x02, 0x00,                            // another key's note-off does nothing
        0x80, 60,   0,    0x20, 0x00,                            // its key's note-off silences the channel
        0xFC,
    });
    image.AddPiece(0, {sequence});
    const Played played = Play(image, 8);
    const std::vector<RegisterWrite>& first = played.writes[0];

    // The piece's start silences the PSG's envelopes.
    SEQLIB_REQUIRE(first.size() >= 4);
    XSFLIB_CHECK(first[0].address == kNr11 && first[0].value == 0);
    XSFLIB_CHECK(first[1].address == kNr21 && first[1].value == 0);
    XSFLIB_CHECK(first[2].address == kNr31 && first[2].value == 0);
    XSFLIB_CHECK(first[3].address == kNr41 && first[3].value == 0);
    XSFLIB_CHECK(StoreTo(first, kNr10) == 0x11);
    XSFLIB_CHECK(first[5].address == kNr11 && first[5].value == (0x80 | 0x75 << 8));
    XSFLIB_CHECK(StoreTo(first, kNr13) == (0x80 << 8 | (1000 + 24)));
    XSFLIB_CHECK(first[7].address == kNr21 && first[7].value == (0x40 | 0x63 << 8));
    XSFLIB_CHECK(StoreTo(first, kNr23) == (0xC0 << 8 | (1000 + 36)));
    XSFLIB_CHECK(StoreTo(played.writes[3], kNr11) == -1);
    XSFLIB_CHECK(StoreTo(played.writes[6], kNr11) == 0);

    // A key's place is a byte: key 30 of square 1 reads place 250.
    TestImage low;
    low.SetInstrument(0, 0, {{0, 0, 0, 0x80}});
    low.Put16(ntdlib::testing::ubimilan_image::kFrequencyTable + 2 * 250, 0x123);
    low.AddPiece(0, {low.AddSequence({0x00, 0x00, 0x90, 30, 0x70, 0x20, 0x00, 0xFC})});
    XSFLIB_CHECK(StoreTo(Play(low, 1).writes[0], kNr13) == (0x80 << 8 | 0x123));
}

void TestWave()
{
    // The wave channel's keys lie 12 down. A program that changes loads the wave's 2 banks of 16 bytes: 32 bytes for
    // each program from the wave table. A program that stays loads nothing.
    TestImage image;
    image.SetInstrument(2, 0, {{0x20, 0x80}});
    image.SetInstrument(2, 1, {{0x30, 0xC0}});
    const int sequence = image.AddSequence({
        0x00, 0x00, 0x92, 48,   0x40, 0x00, 0x00, // program 0's key 48
        0xC2, 1,    0x00, 0x00,                   // program 1 loads its wave
        0xC2, 1,    0x00, 0x00,                   // and doesn't again
        0x92, 50,   0x20, 0x20, 0x00,             // program 1's key 50
        0xFC,
    });
    image.AddPiece(0, {sequence});
    const Played played = Play(image, 1);
    std::vector<RegisterWrite> writes = played.writes[0];
    writes.erase(writes.begin(), writes.begin() + 4);
    SEQLIB_REQUIRE(writes.size() == 3 + 2 * 17 + 3);
    XSFLIB_CHECK(writes[0].address == kNr30 && writes[0].value == 0xA0);
    XSFLIB_CHECK(writes[1].address == kNr31 && writes[1].value == (0x20 | 0x40 << 8));
    XSFLIB_CHECK(writes[2].address == kNr33 && writes[2].value == (0x80 << 8 | (1000 + 36)));
    XSFLIB_CHECK(writes[3].address == kNr30 && writes[3].value == 0x20);
    XSFLIB_CHECK(writes[20].address == kNr30 && writes[20].value == 0x60);

    // TestImage's wave table gives byte i the value 8 * i, and program 1's wave lies 32 bytes on: past that table.
    XSFLIB_CHECK(writes[4].address == 0x04000090 && writes[4].size == 1);
    XSFLIB_CHECK(writes[37].address == kNr30 && writes[37].value == 0xA0);
    XSFLIB_CHECK(writes[38].address == kNr31 && writes[38].value == (0x30 | 0x20 << 8));
    XSFLIB_CHECK(writes[39].address == kNr33 && writes[39].value == (0xC0 << 8 | (1000 + 38)));
}

void TestNoise()
{
    // Channel 9's keys up to 11 play the noise from the noise table, with the velocity ORed into the entry's second
    // byte. A note-off silences nothing. TestImage's entry of key 0 is 0xC000F01E.
    TestImage image;
    const int sequence = image.AddSequence({0x00, 0x00, 0x99, 0, 0x03, 0x00, 0x00, 0x89, 0, 0, 0x20, 0x00, 0xFC});
    image.AddPiece(0, {sequence});
    const Played played = Play(image, 1);
    const std::vector<RegisterWrite>& writes = played.writes[0];
    XSFLIB_CHECK(StoreTo(writes, kNr43) == 0xC000);
    SEQLIB_REQUIRE(writes.size() == 6);
    XSFLIB_CHECK(writes[4].address == kNr41 && writes[4].value == (0x1E | 0xF3 << 8));

    // Each key's entry takes 4 bytes. A note-off of the noise stores nothing.
    TestImage other;
    other.Put32(ntdlib::testing::ubimilan_image::kNoiseTable + 4 * 3, 0x8042A010);
    other.AddPiece(0, {other.AddSequence({0x00, 0x00, 0x99, 3, 0x05, 0x01, 0x00, 0x89, 3, 0, 0x20, 0x00, 0xFC})});
    const Played other_played = Play(other, 3);
    XSFLIB_CHECK(StoreTo(other_played.writes[0], kNr41) == (0x10 | 0xA5 << 8));
    XSFLIB_CHECK(StoreTo(other_played.writes[0], kNr43) == 0x8042);
    XSFLIB_CHECK(other_played.writes[2].empty());
}

// Each store to `address`, in order, with the frame that makes it.
struct FrameStore
{
    bool operator==(const FrameStore&) const = default;

    int frame = 0;
    uint32_t value = 0;
};

std::vector<FrameStore> StoresTo(const Played& played, uint32_t address)
{
    std::vector<FrameStore> stores;
    for (size_t frame = 0; frame < played.writes.size(); frame++)
    {
        for (const RegisterWrite& write : played.writes[frame])
        {
            if (write.address == address)
            {
                stores.push_back({int(frame), write.value});
            }
        }
    }

    return stores;
}

void TestModulation()
{
    // A modulation waits for its delay, then goes up and down in steps of depth * 65,536 / (period * 2) a frame, up
    // past its depth and down past minus its depth. Each frame of the track runs it, the note's frame too. A frame's
    // frequency comes from the fine table, at the key's place times 100 plus the modulation's whole part. TestImage's
    // fine table gives each place its number. With a depth of 2 and a period of 1, the step is 1 a frame. Key 60 lies
    // at place 24.
    TestImage image;
    Instrument instrument;
    instrument.bytes = {0, 0, 0, 0x80};
    instrument.modulation_delay = 1;
    instrument.modulation_depth = 2;
    instrument.modulation_period = 1;
    instrument.flags = 1;
    image.SetInstrument(0, 0, instrument);
    image.AddPiece(0, {image.AddSequence({0x00, 0x00, 0x90, 60, 0x70, 0x40, 0x00, 0xFC})});
    const std::vector<FrameStore> expected = {
        {0, 0x8000 | (1000 + 24)}, // the note. The delay takes frames 0 and 1
        {2, 2401},
        {3, 2402},
        {4, 2403}, // past the depth: it turns
        {5, 2402},
        {6, 2401},
        // Frame 7: a modulation of 0 without a slide sets nothing
        {8, 2399},
        {9, 2398},
        {10, 2397}, // past minus the depth: it turns
        {11, 2398},
        {12, 2399},
        // Frame 13: nothing
        {14, 2401},
    };
    XSFLIB_CHECK(StoresTo(Play(image, 15), kNr13) == expected);

    // A note-off stops no modulation. A note of an instrument without one does. With a delay of 0, the note's frame
    // starts the wave at 0.
    TestImage other;
    instrument.modulation_delay = 0;
    other.SetInstrument(0, 0, instrument);
    Instrument plain;
    plain.bytes = {0, 0, 0, 0x80};
    other.SetInstrument(0, 1, plain);
    const int sequence = other.AddSequence({
        0x00, 0x00, 0x90, 60,   0x70, 0x01, 0x00, // the note, on frame 0
        0x80, 60,   0,    0x01, 0x00,             // its note-off, on frame 2
        0xC0, 1,    0x00, 0x00,                   // the plain instrument, on frame 4
        0x90, 62,   0x70, 0x20, 0x00,             // its note, on frame 4
        0xFC,
    });
    other.AddPiece(0, {sequence});
    const std::vector<FrameStore> stopped = {
        {0, 0x8000 | (1000 + 24)}, {1, 2401}, {2, 2402}, {3, 2403}, {4, 0x8000 | (1000 + 26)},
    };
    XSFLIB_CHECK(StoresTo(Play(other, 7), kNr13) == stopped);
}

void TestSlide()
{
    // A pitch mode's first note starts a slide at the key's place times 100. From then on, each frame of the track sets
    // the frequency from the fine table at the slide's place, the note's frame too. The next note starts at the last
    // key's frequency, and moves by (new place - place) / frames a frame up to the new place. A note in the slide's
    // middle starts from the fine table.
    TestImage image;
    Instrument instrument;
    instrument.bytes = {0, 0, 0, 0x80};
    instrument.slide_frames = 4;
    instrument.flags = 2;
    image.SetInstrument(0, 0, instrument);
    const int sequence = image.AddSequence({
        0x00,
        0x00,
        0x90,
        60,
        0x70,
        0x01,
        0x00, // key 60 on frame 0, the next command on frame 2
        0x90,
        62,
        0x70,
        0x01,
        0x00, // key 62 on frame 2, the next command on frame 4
        0x90,
        73,
        0x70,
        0x20,
        0x00, // key 73 on frame 4
        0xFC,
    });
    image.AddPiece(0, {sequence});
    const std::vector<FrameStore> expected = {
        {0, 0x8000 | (1000 + 24)}, // the first note's frequency
        {0, 2400},                 // and its slide's place
        {1, 2400},
        {2, 0x8000 | (1000 + 24)}, // the second note starts at the last key's frequency
        {2, 2450},                 // a move of 200 / 4 a frame
        {3, 2500},
        {4, 0x8000 | 2500}, // the third note starts at the slide's place
        {4, 2500 + 300},    // a move of (3700 - 2500) / 4 a frame
        {5, 2500 + 2 * 300},
        {6, 2500 + 3 * 300},
        {7, 2500 + 4 * 300},
        {8, 3700}, // the place stays there
    };
    XSFLIB_CHECK(StoresTo(Play(image, 9), kNr13) == expected);

    // A move rounds towards 0. A place short of the target moves on by the whole step, and the next frame goes back to
    // the target. From key 60 to key 61, in 3 frames: 33 a frame, past the target on the fourth.
    TestImage rounded;
    instrument.slide_frames = 3;
    rounded.SetInstrument(0, 0, instrument);
    rounded.AddPiece(0,
                     {rounded.AddSequence({0x00, 0x00, 0x90, 60, 0x70, 0x00, 0x00, 0x90, 61, 0x70, 0x20, 0x00, 0xFC})});
    const std::vector<FrameStore> rounded_stores = {
        {0, 0x8000 | (1000 + 24)}, {0, 0x8000 | (1000 + 24)}, {0, 2433}, {1, 2466}, {2, 2499}, {3, 2532}, {4, 2500},
    };
    XSFLIB_CHECK(StoresTo(Play(rounded, 5), kNr13) == rounded_stores);

    // The game's division by a slide's 0 frames gives 0: the place goes to the target at once.
    TestImage at_once;
    instrument.slide_frames = 0;
    at_once.SetInstrument(0, 0, instrument);
    at_once.AddPiece(0,
                     {at_once.AddSequence({0x00, 0x00, 0x90, 60, 0x70, 0x01, 0x00, 0x90, 62, 0x70, 0x20, 0x00, 0xFC})});
    const std::vector<FrameStore> at_once_stores = {
        {0, 0x8000 | (1000 + 24)}, {0, 2400}, {1, 2400}, {2, 0x8000 | (1000 + 24)}, {2, 2600}, {3, 2600},
    };
    XSFLIB_CHECK(StoresTo(Play(at_once, 4), kNr13) == at_once_stores);
}

void TestSlideRepeats()
{
    Instrument instrument;
    instrument.bytes = {0, 0, 0, 0x80};
    instrument.slide_frames = 4;
    instrument.flags = 2;
    const auto play = [&instrument](int first, int second, int third)
    {
        TestImage image;
        image.SetInstrument(0, 0, instrument);
        const std::vector<uint8_t> commands = {
            0x00, 0x00,
            0x90, uint8_t(first),
            0x70, 0x01,
            0x00, // on frame 0
            0x90, uint8_t(second),
            0x70, 0x01,
            0x00, // on frame 2
            0x90, uint8_t(third),
            0x70, 0x20,
            0x00, // on frame 4
            0xFC,
        };
        image.AddPiece(0, {image.AddSequence(commands)});
        return StoresTo(Play(image, 7), kNr13);
    };

    // The slide's target again, on the way up: a move of 0, and the place goes to the target at once.
    const std::vector<FrameStore> up = {
        {0, 0x8000 | (1000 + 24)},
        {0, 2400},
        {1, 2400},
        {2, 0x8000 | (1000 + 24)},
        {2, 2450},
        {3, 2500},
        {4, 0x8000 | 2500},
        {4, 2600},
        {5, 2600},
        {6, 2600},
    };
    XSFLIB_CHECK(play(60, 62, 62) == up);

    // The slide's target again, on the way down: a move of 0, and the place stays short of the target.
    const std::vector<FrameStore> down = {
        {0, 0x8000 | (1000 + 26)},
        {0, 2600},
        {1, 2600},
        {2, 0x8000 | (1000 + 26)},
        {2, 2550},
        {3, 2500},
        {4, 0x8000 | 2500},
        {4, 2500},
        {5, 2500},
        {6, 2500},
    };
    XSFLIB_CHECK(play(62, 60, 60) == down);

    // The first note's key again: the slide has arrived. The next note starts at the place, from the fine table.
    const std::vector<FrameStore> arrived = {
        {0, 0x8000 | (1000 + 24)},
        {0, 2400},
        {1, 2400},
        {2, 0x8000 | (1000 + 24)},
        {2, 2400},
        {3, 2400},
        {4, 0x8000 | 2400},
        {4, 2450},
        {5, 2500},
        {6, 2550},
    };
    XSFLIB_CHECK(play(60, 60, 62) == arrived);
}

void TestSlideEnds()
{
    // A note of an instrument without a pitch mode ends the slide. The next pitch mode's note starts a new one.
    Instrument sliding;
    sliding.bytes = {0, 0, 0, 0x80};
    sliding.slide_frames = 4;
    sliding.flags = 2;
    Instrument plain;
    plain.bytes = {0, 0, 0, 0x80};
    TestImage image;
    image.SetInstrument(0, 0, sliding);
    image.SetInstrument(0, 1, plain);
    const int sequence = image.AddSequence({
        0x00, 0x00, 0x90, 60,   0x70, 0x01, 0x00, // the slide's note, on frame 0
        0xC0, 1,    0x00, 0x00,                   // the plain instrument, on frame 2
        0x90, 62,   0x70, 0x01, 0x00,             // its note
        0xC0, 0,    0x00, 0x00,                   // the slide's instrument, on frame 4
        0x90, 64,   0x70, 0x20, 0x00,             // its note
        0xFC,
    });
    image.AddPiece(0, {sequence});
    const std::vector<FrameStore> expected = {
        {0, 0x8000 | (1000 + 24)}, {0, 2400}, {1, 2400}, {2, 0x8000 | (1000 + 26)},
        {4, 0x8000 | (1000 + 28)}, {4, 2800}, {5, 2800},
    };
    XSFLIB_CHECK(StoresTo(Play(image, 6), kNr13) == expected);
}

void TestLegato()
{
    // A legato's note on a sounding channel keeps the channel going: a store of the new key's frequency without the
    // bytes that restart it. After a note-off, the next note restarts it.
    TestImage image;
    Instrument instrument;
    instrument.bytes = {0x11, 0x80, 0x05, 0x80};
    instrument.flags = 4;
    image.SetInstrument(0, 0, instrument);
    const int sequence = image.AddSequence({
        0x00, 0x00, 0x90, 60,   0x70, 0x01, 0x00, // the note, on frame 0
        0x90, 62,   0x70, 0x01, 0x00,             // goes on, on frame 2
        0x80, 62,   0,    0x01, 0x00,             // stops, on frame 4
        0x90, 64,   0x70, 0x20, 0x00,             // restarts, on frame 6
        0xFC,
    });
    image.AddPiece(0, {sequence});
    const Played played = Play(image, 8);
    XSFLIB_CHECK(StoreTo(played.writes[0], kNr13) == (0x8000 | (1000 + 24)));
    XSFLIB_CHECK(StoreTo(played.writes[2], kNr13) == 1000 + 26);
    XSFLIB_CHECK(StoreTo(played.writes[2], kNr11) == -1);
    XSFLIB_CHECK(StoreTo(played.writes[4], kNr11) == 0);
    XSFLIB_CHECK(StoreTo(played.writes[6], kNr13) == (0x8000 | (1000 + 28)));
    XSFLIB_CHECK(StoreTo(played.writes[6], kNr10) == 0x11);

    // With a pitch mode, the note goes on by a move of the slide, and stores no note's frequency.
    TestImage sliding;
    instrument.flags = 4 | 2;
    instrument.slide_frames = 4;
    sliding.SetInstrument(0, 0, instrument);
    sliding.AddPiece(0,
                     {sliding.AddSequence({0x00, 0x00, 0x90, 60, 0x70, 0x01, 0x00, 0x90, 62, 0x70, 0x20, 0x00, 0xFC})});
    const std::vector<FrameStore> moved = {
        {0, 0x8000 | (1000 + 24)}, {0, 2400}, {1, 2400}, {2, 2450}, {3, 2500},
    };
    XSFLIB_CHECK(StoresTo(Play(sliding, 4), kNr13) == moved);

    // The wave channel's test of the pitch mode takes any flag: without a slide, the note stores nothing at all. The
    // note's key still counts for a note-off.
    TestImage wave;
    Instrument wave_instrument;
    wave_instrument.bytes = {0x10, 0x80};
    wave_instrument.flags = 4;
    wave.SetInstrument(2, 0, wave_instrument);
    const int wave_sequence = wave.AddSequence({
        0x00,
        0x00,
        0x92,
        60,
        0x20,
        0x01,
        0x00, // the note, on frame 0
        0x92,
        62,
        0x20,
        0x01,
        0x00, // goes on, on frame 2
        0x82,
        62,
        0,
        0x20,
        0x00, // stops, on frame 4
        0xFC,
    });
    wave.AddPiece(0, {wave_sequence});
    const Played wave_played = Play(wave, 5);
    XSFLIB_CHECK(StoreTo(wave_played.writes[0], kNr33) == (0x8000 | (1000 + 48)));
    XSFLIB_CHECK(wave_played.writes[2].empty());
    XSFLIB_CHECK(StoreTo(wave_played.writes[4], kNr31) == 0);
}

void TestPieces()
{
    // A looping piece of two sequences plays them in turn. Each sequence's first countdown follows the last command's
    // frame. Levels of 8 and 16 tell the sequences apart.
    TestImage image;
    image.SetKey(40, image.AddSample(Constant(40000, 8)));
    image.SetKey(41, image.AddSample(Constant(40000, 16)));
    const int first = image.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x02, 0x00, 0x89, 40, 0, 0x02, 0x00, 0xFC});
    const int second = image.AddSequence({0x00, 0x00, 0x99, 41, 64, 0x02, 0x00, 0x89, 41, 0, 0x02, 0x00, 0xFC});
    image.AddPiece(1, {first, second});
    const Rom rom = image.ToRom();
    const DriverInfo info = Detect(rom);
    Output output(rom, info, 0);
    std::vector<int> levels;
    for (int frame = 0; frame < 25; frame++)
    {
        output.Frame();
        levels.push_back(output.Fifo().back());
    }

    XSFLIB_CHECK(!output.Song().Reset());
    XSFLIB_CHECK(levels ==
                 std::vector<int>({8, 8, 8, 0, 0, 0, 16, 16, 16, 0, 0, 0, 8, 8, 8, 0, 0, 0, 16, 16, 16, 0, 0, 0, 8}));
}

void TestResets()
{
    // The game resets on a status byte that it doesn't take.
    TestImage unknown;
    unknown.AddPiece(0, {unknown.AddSequence({0x00, 0x00, 0xA0, 0, 0, 0xFC})});
    XSFLIB_CHECK(Play(unknown, 1).reset);

    // On a voice's volume above 64, in the sound update of the frame that starts the note.
    TestImage loud;
    loud.SetKey(40, loud.AddSample(Constant(4000, 64)));
    loud.AddPiece(0, {loud.AddSequence({0x00, 0x00, 0x99, 40, 65, 0x20, 0x00, 0xFC})});
    const Played played_loud = Play(loud, 1);
    XSFLIB_CHECK(played_loud.reset);
    for (const int8_t byte : played_loud.bytes)
    {
        XSFLIB_CHECK(byte == 0);
    }

    // A volume of 64 doesn't.
    TestImage fine;
    fine.SetKey(40, fine.AddSample(Constant(4000, 64)));
    fine.AddPiece(0, {fine.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x20, 0x00, 0xFC})});
    XSFLIB_CHECK(!Play(fine, 1).reset);

    // A sample among the piece's sequences leaves the resource that the game's callback gave the track. The first end
    // therefore passes. The second end takes the sample's bytes for a sequence: its kind for a countdown of 3, then a
    // status of 0x40.
    TestImage listed;
    const int listed_sample = listed.AddSample(Constant(64, 8));
    const int listed_first = listed.AddSequence({0x00, 0x00, 0xE0, 0x01, 0x00, 0xFC});
    const int listed_second = listed.AddSequence({0x00, 0x00, 0xE0, 0x10, 0x00, 0xFC});
    listed.AddPiece(1, {listed_first, listed_second, listed_sample});
    XSFLIB_CHECK(!Play(listed, 23).reset);
    XSFLIB_CHECK(Play(listed, 24).reset);

    // And at the first end of a piece of two sequences that doesn't loop: the game reads the kind of the sequence after
    // the next, and finds none.
    TestImage two;
    const int first = two.AddSequence({0x00, 0x00, 0xE0, 0x01, 0x00, 0xFC});
    const int second = two.AddSequence({0x08, 0x00, 0xFC});
    two.AddPiece(0, {first, second});
    XSFLIB_CHECK(!Play(two, 2).reset);
    XSFLIB_CHECK(Play(two, 3).reset);

    // A piece of one sequence just ends.
    TestImage one;
    one.AddPiece(0, {one.AddSequence({0x00, 0x00, 0xFC})});
    const Rom rom = one.ToRom();
    const DriverInfo info = Detect(rom);
    Output output(rom, info, 0);
    output.Frame();
    XSFLIB_CHECK(!output.Song().Reset());
    XSFLIB_CHECK(!output.Song().TrackActive());
}

void TestInvalid()
{
    // A piece whose first resource isn't a sequence can't play.
    TestImage image;
    const int sample = image.AddSample(Constant(16, 0));
    image.AddPiece(0, {sample});
    const Rom rom = image.ToRom();
    const DriverInfo info = Detect(rom);
    XSFLIB_CHECK(!Output(rom, info, 0).Valid());
    XSFLIB_CHECK(!Output(rom, info, 5).Valid());
}

void TestVoicesSilent()
{
    // A voice that a run stops counts as silent. So do voices that a note-off stops.
    TestImage image;
    image.SetKey(40, image.AddSample(Constant(64, 64)));
    image.AddPiece(0, {image.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x20, 0x00, 0xFC})});
    const Rom rom = image.ToRom();
    const DriverInfo info = Detect(rom);
    Output output(rom, info, 0);
    XSFLIB_CHECK(output.VoicesSilent());
    output.Frame();
    XSFLIB_CHECK(output.VoicesSilent());

    TestImage looped;
    looped.SetKey(40, looped.AddSample(Constant(64, 64), true, 0, 64));
    looped.AddPiece(0, {looped.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x01, 0x00, 0x89, 40, 0, 0x20, 0x00, 0xFC})});
    const Rom rom_looped = looped.ToRom();
    const DriverInfo info_looped = Detect(rom_looped);
    Output output_looped(rom_looped, info_looped, 0);
    output_looped.Frame();
    XSFLIB_CHECK(!output_looped.VoicesSilent());
    output_looped.Frame();
    output_looped.Frame();
    XSFLIB_CHECK(output_looped.VoicesSilent());

    // The sequencer plays on until its track has ended and its voices have stopped. 1,600 points last 200 of the
    // mixer's runs, and 12 frames hold 205.
    TestImage tail;
    tail.SetKey(40, tail.AddSample(Constant(1600, 64)));
    tail.AddPiece(0, {tail.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x00, 0x00, 0xFC})});
    const Rom rom_tail = tail.ToRom();
    const DriverInfo info_tail = Detect(rom_tail);
    Output output_tail(rom_tail, info_tail, 0);
    for (int frame = 0; frame < 11; frame++)
    {
        output_tail.Frame();
    }

    XSFLIB_CHECK(!output_tail.Song().TrackActive());
    XSFLIB_CHECK(output_tail.Song().Playing());
    output_tail.Frame();
    XSFLIB_CHECK(!output_tail.Song().Playing());
}

} // namespace

int main()
{
    TestRuns();
    TestVoiceBytes();
    TestVolume();
    TestLastPointsLeftOut();
    TestLoop();
    TestClip();
    TestVoices();
    TestVoiceChoices();
    TestVoiceMinusOne();
    TestKeptVoice();
    TestSquares();
    TestWave();
    TestNoise();
    TestModulation();
    TestSlide();
    TestSlideRepeats();
    TestSlideEnds();
    TestLegato();
    TestPieces();
    TestResets();
    TestInvalid();
    TestVoicesSilent();
    return xsflib::testing::Summary("Ubisoft Milan's output");
}
