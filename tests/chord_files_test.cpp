// SPDX-License-Identifier: MIT

// Unit tests for the chord files and the chord database (jaudio/chord_files.h), with made-up files.

#include "ntdlib/jaudio/chord_files.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "ntdlib/jaudio/jungle_beat_states.h"

using namespace ntdlib::jaudio;

namespace
{

constexpr uint8_t kNone = kChordNone;

// Whether `read` throws FormatError.
template <typename Read>
bool ThrowsFormatError(Read read)
{
    try
    {
        read();
    }
    catch (const xsflib::FormatError&)
    {
        return true;
    }

    return false;
}

// Builds a big-endian file.
class Bytes
{
public:
    void Tag(const char* tag)
    {
        data_.insert(data_.end(), tag, tag + 4);
    }

    void U8(uint8_t value)
    {
        data_.push_back(value);
    }

    void U16(uint16_t value)
    {
        U8(static_cast<uint8_t>(value >> 8));
        U8(static_cast<uint8_t>(value));
    }

    void U32(uint32_t value)
    {
        U16(static_cast<uint16_t>(value >> 16));
        U16(static_cast<uint16_t>(value));
    }

    // Writes `value` at `at`, over what is there.
    void SetU32(std::size_t at, uint32_t value)
    {
        for (std::size_t i = 0; i < 4; i++)
        {
            data_[at + i] = static_cast<uint8_t>(value >> (24 - 8 * i));
        }
    }

    std::size_t Size() const
    {
        return data_.size();
    }

    const std::vector<uint8_t>& Data() const
    {
        return data_;
    }

private:
    std::vector<uint8_t> data_;
};

// A chord's 16 bytes. `scale` is the scale's offset.
void PutChord(Bytes& b, std::array<uint8_t, 9> bytes, uint32_t scale)
{
    for (const uint8_t value : bytes)
    {
        b.U8(value);
    }

    b.U8(0);
    b.U16(0);
    b.U32(scale);
}

// A chord file with one order (chord 1, then chord 0) and two chords: A minor seventh with C major's scale, and G
// major with G major's scale (F raised).
std::vector<uint8_t> MakeChordFile(const char* tag)
{
    Bytes b;
    b.Tag(tag);
    b.U32(0);
    b.U16(1);
    b.U16(2);
    b.U32(0x18); // the order
    b.U32(0x1c); // chord 0
    b.U32(0x2c); // chord 1
    b.U8(2);
    b.U8(1);
    b.U8(0);
    b.U8(0);
    PutChord(b, {9, 1, 1, kNone, kNone, kNone, 0, kNone, 0}, 0x3c);
    PutChord(b, {7, 0, 0, kNone, kNone, kNone, 0, kNone, 0}, 0x43);
    for (const uint8_t code : {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0})
    {
        b.U8(code);
    }

    return b.Data();
}

void TestChordFile()
{
    const ChordFile file = ReadChordFile(MakeChordFile("CITS"));
    XSFLIB_CHECK(file.has_scales);
    XSFLIB_CHECK(file.orders == (std::vector<std::vector<uint8_t>>{{1, 0}}));
    XSFLIB_CHECK(file.ChordAt(0, 0) == std::optional<uint8_t>(1));
    XSFLIB_CHECK(file.ChordAt(0, 1) == std::optional<uint8_t>(0));
    XSFLIB_CHECK(!file.ChordAt(0, 2));
    XSFLIB_CHECK(!file.ChordAt(1, 0));
    XSFLIB_CHECK(file.chords.size() == 2);
    const Chord& minor = file.chords[0];
    XSFLIB_CHECK(minor.root == 9 && minor.kind == 1 && minor.seventh == 1 && minor.fifth == 0);
    XSFLIB_CHECK(minor.scale_root == kNone && minor.no_third == 0);
    XSFLIB_CHECK(minor.scale == (std::array<uint8_t, 7>{}));
    XSFLIB_CHECK(file.chords[1].scale == (std::array<uint8_t, 7>{0, 0, 0, 1, 0, 0, 0}));

    // A "CITL" file has no scales, and its chords' last words aren't offsets.
    const ChordFile plain = ReadChordFile(MakeChordFile("CITL"));
    XSFLIB_CHECK(!plain.has_scales);
    XSFLIB_CHECK(!plain.chords[0].scale);
    XSFLIB_CHECK(FitToScale(plain.chords[0], 61, false) == kNone);

    XSFLIB_CHECK(ThrowsFormatError([&] { ReadChordFile(MakeChordFile("CITX")); }));
    std::vector<uint8_t> cut = MakeChordFile("CITS");
    cut.resize(0x30);
    XSFLIB_CHECK(ThrowsFormatError([&] { ReadChordFile(cut); }));
}

Chord MakeChord(uint8_t root, ChordKind kind)
{
    return {.root = root, .kind = static_cast<uint8_t>(kind)};
}

using Tones = std::vector<std::pair<uint8_t, uint8_t>>;

// The tones that aren't kChordNone, and the count of those that are.
std::pair<Tones, int> Listed(const ChordTones& tones)
{
    Tones listed;
    int none = 0;
    for (std::size_t i = 0; i < tones.pitch_classes.size(); i++)
    {
        if (tones.pitch_classes[i] == kNone)
        {
            none++;
        }
        else
        {
            listed.emplace_back(tones.pitch_classes[i], tones.intervals[i]);
        }
    }

    return {listed, none};
}

void CheckTones(const Chord& chord, const Tones& expected)
{
    const auto [listed, none] = Listed(TonesOf(chord));
    XSFLIB_CHECK(listed == expected);
    XSFLIB_CHECK(none == static_cast<int>(8 - expected.size()));
}

void TestTones()
{
    CheckTones(MakeChord(0, ChordKind::kMajor), {{0, 0}, {4, 4}, {7, 7}});
    CheckTones(MakeChord(2, ChordKind::kMinor), {{2, 0}, {5, 3}, {9, 7}});
    CheckTones(MakeChord(7, ChordKind::kSuspended), {{7, 0}, {0, 5}, {2, 7}});
    CheckTones(MakeChord(11, ChordKind::kDiminishedSeventh), {{11, 0}, {2, 3}, {5, 6}, {8, 9}});
    CheckTones(MakeChord(0, ChordKind::kAugmented), {{0, 0}, {4, 4}, {8, 8}});
    CheckTones(MakeChord(kNone, ChordKind::kMajor), {});
    CheckTones({.root = 0, .kind = 5}, {});

    // The fifth: raised, lowered, left out. Bytes other than 1, 2 and kChordNone give the perfect fifth.
    Chord chord = MakeChord(0, ChordKind::kMajor);
    chord.fifth = 1;
    CheckTones(chord, {{0, 0}, {4, 4}, {8, 8}});
    chord.fifth = 2;
    CheckTones(chord, {{0, 0}, {4, 4}, {6, 6}});
    chord.fifth = kNone;
    CheckTones(chord, {{0, 0}, {4, 4}});
    chord.fifth = 9;
    CheckTones(chord, {{0, 0}, {4, 4}, {7, 7}});

    // Without the third. A diminished seventh chord keeps its other tones.
    chord = MakeChord(0, ChordKind::kMajor);
    chord.no_third = 1;
    CheckTones(chord, {{0, 0}, {7, 7}});
    Chord diminished = MakeChord(11, ChordKind::kDiminishedSeventh);
    diminished.no_third = 1;
    CheckTones(diminished, {{11, 0}, {5, 6}, {8, 9}});

    // The sevenths.
    chord = MakeChord(0, ChordKind::kMajor);
    chord.seventh = 2;
    CheckTones(chord, {{0, 0}, {4, 4}, {7, 7}, {11, 11}});
    chord.seventh = 3;
    CheckTones(chord, {{0, 0}, {4, 4}, {7, 7}});

    // An added tone goes before the first tone with a higher pitch class: the ninth of C before E, the ninth of A
    // minor seventh after G, where only free tones are higher. Its interval stays as the chord gives it.
    chord = MakeChord(0, ChordKind::kMajor);
    chord.added = {14, kNone, kNone};
    CheckTones(chord, {{0, 0}, {2, 14}, {4, 4}, {7, 7}});
    Chord minor = MakeChord(9, ChordKind::kMinor);
    minor.seventh = 1;
    minor.added = {kNone, 14, kNone};
    CheckTones(minor, {{9, 0}, {0, 3}, {4, 7}, {7, 10}, {11, 14}});

    // The root keeps its byte. The other tones are pitch classes.
    CheckTones(MakeChord(9, ChordKind::kMajor), {{9, 0}, {1, 4}, {4, 7}});

    // Eight tones fill the list.
    Chord full = MakeChord(0, ChordKind::kDiminishedSeventh);
    full.seventh = 2;
    full.added = {1, 2, 4};
    CheckTones(full, {{0, 0}, {1, 1}, {2, 2}, {3, 3}, {4, 4}, {6, 6}, {9, 9}, {11, 11}});
    full.added = {1, 2, 5};
    CheckTones(full, {{0, 0}, {1, 1}, {2, 2}, {3, 3}, {5, 5}, {6, 6}, {9, 9}, {11, 11}});

    const ChordTones tones = TonesOf(MakeChord(0, ChordKind::kMajor));
    XSFLIB_CHECK(tones.IntervalOf(7) == 7);
    XSFLIB_CHECK(tones.IntervalOf(1) == kNone);
}

constexpr std::array<uint8_t, 7> kCMajor{};

void TestScales()
{
    constexpr std::array<int, 7> kDegrees = {0, 2, 4, 5, 7, 9, 11};
    for (int degree = 0; degree < 7; degree++)
    {
        XSFLIB_CHECK(ScaleDegree(kCMajor, degree) == kDegrees[static_cast<std::size_t>(degree)]);
    }

    // Raised, lowered, raised and lowered twice, and left out. B raised stays 12. C lowered becomes 11.
    const std::array<uint8_t, 7> codes = {2, 4, 1, kNone, 3, 0, 1};
    XSFLIB_CHECK(ScaleDegree(codes, 0) == 11);
    XSFLIB_CHECK(ScaleDegree(codes, 1) == 0);
    XSFLIB_CHECK(ScaleDegree(codes, 2) == 5);
    XSFLIB_CHECK(ScaleDegree(codes, 3) == -1);
    XSFLIB_CHECK(ScaleDegree(codes, 4) == 9);
    XSFLIB_CHECK(ScaleDegree(codes, 6) == 12);

    XSFLIB_CHECK(InScale(kCMajor, 60));
    XSFLIB_CHECK(!InScale(kCMajor, 61));
    const std::array<uint8_t, 7> b_sharp = {kNone, 0, 0, 0, 0, 0, 1};
    XSFLIB_CHECK(!InScale(b_sharp, 60));
    XSFLIB_CHECK(!InScale(b_sharp, 71));
}

void TestFitToScale()
{
    Chord chord = MakeChord(9, ChordKind::kMinor);
    chord.scale = kCMajor;
    XSFLIB_CHECK(FitToScale(chord, 60, false) == 60);
    XSFLIB_CHECK(FitToScale(chord, 61, false) == 62);
    XSFLIB_CHECK(FitToScale(chord, 61, true) == 60);

    // The scale root stays, in the scale or not: byte 7, or the root without it.
    std::array<uint8_t, 7> no_c = kCMajor;
    no_c[0] = kNone;
    chord.scale = no_c;
    chord.scale_root = 0;
    XSFLIB_CHECK(FitToScale(chord, 60, false) == 60);
    chord.scale_root = kNone;
    XSFLIB_CHECK(FitToScale(chord, 60, false) == 62);
    chord.root = 0;
    XSFLIB_CHECK(FitToScale(chord, 72, false) == 72);

    // The search stays from 12 to 120.
    chord.root = 9;
    XSFLIB_CHECK(FitToScale(chord, 13, true) == kNone);
    chord.scale = kCMajor;
    XSFLIB_CHECK(FitToScale(chord, 13, true) == 12);
    XSFLIB_CHECK(FitToScale(chord, 119, false) == 119);
    XSFLIB_CHECK(FitToScale(chord, 121, false) == kNone);
    XSFLIB_CHECK(FitToScale(chord, 118, false) == 119);

    // No scale degree within 12 semitones.
    chord.scale = std::array<uint8_t, 7>{kNone, kNone, kNone, kNone, kNone, kNone, kNone};
    XSFLIB_CHECK(FitToScale(chord, 61, false) == kNone);
}

void TestFitToChord()
{
    const Chord major = MakeChord(0, ChordKind::kMajor);
    const ChordTones tones = TonesOf(major);
    XSFLIB_CHECK(tones.Has(64) && tones.Has(12) && !tones.Has(61));
    XSFLIB_CHECK(tones.Nearest(61, false) == 64);
    XSFLIB_CHECK(tones.Nearest(61, true) == 60);
    XSFLIB_CHECK(tones.Nearest(13, true) == 12);
    XSFLIB_CHECK(tones.Nearest(12, true) == kNone);
    XSFLIB_CHECK(tones.Nearest(119, false) == 120);
    XSFLIB_CHECK(tones.Nearest(121, false) == kNone);

    XSFLIB_CHECK(FitToChord(major, 64, false) == 64);
    XSFLIB_CHECK(FitToChord(major, 62, false) == 64);
    XSFLIB_CHECK(FitToChord(major, 62, true) == 60);
    Chord kept = major;
    kept.scale_root = 2;
    XSFLIB_CHECK(FitToChord(kept, 62, false) == 62);
    XSFLIB_CHECK(FitToChord(MakeChord(kNone, ChordKind::kMajor), 62, false) == kNone);
}

void TestStepUpScale()
{
    XSFLIB_CHECK(StepUpScale(kCMajor, 60, 1, false, false) == 62);
    XSFLIB_CHECK(StepUpScale(kCMajor, 60, 7, false, false) == 72);
    XSFLIB_CHECK(StepUpScale(kCMajor, 60, 7, false, true) == 60);
    XSFLIB_CHECK(StepUpScale(kCMajor, 71, 1, false, false) == 72);
    XSFLIB_CHECK(StepUpScale(kCMajor, 61, 3, false, false) == 61);

    // E lowered twice repeats D. A step passes over it, unless the steps count repeats.
    const std::array<uint8_t, 7> repeat = {0, 0, 4, 0, 0, 0, 0};
    XSFLIB_CHECK(StepUpScale(repeat, 60, 2, false, false) == 65);
    XSFLIB_CHECK(StepUpScale(repeat, 60, 2, true, false) == 62);
}

// A chord database with one entry of one part with two blocks, and a record table that gives three records and holds
// two.
std::vector<uint8_t> MakeDatabase()
{
    Bytes b;
    b.U32(0);
    b.Tag("CDSE");
    b.U32(1);
    b.U32(0x40); // the records
    b.U32(0x14); // entry 0
    b.U16(1);
    b.U16(0x1234);
    b.U32(0x20); // the steps
    b.U32(0x28); // part 0
    b.U8(1);
    b.U8(0x05);
    b.U8(0);
    b.U8(0);
    b.U32(0);
    b.U8(2);
    b.U8(2);
    b.U16(0);
    b.U32(0x34);
    b.U32(0x3a);
    b.U32(0);
    while (b.Size() < 0x40)
    {
        b.U8(0);
    }

    b.U32(3);
    for (const uint32_t sound : {0x50019u, 0x5000fu})
    {
        b.U32(0);
        b.U32(sound);
        b.U32(0);
        b.U32(0x8000);
    }

    return b.Data();
}

void TestDatabase()
{
    const ChordDatabase database = ReadChordDatabase(MakeDatabase());
    XSFLIB_CHECK(database.entries.size() == 1);
    const ChordDatabase::Entry& entry = database.entries[0];
    XSFLIB_CHECK(entry.offset == 0x14 && entry.steps == 0x20);
    XSFLIB_CHECK(entry.parts.size() == 1);
    XSFLIB_CHECK(entry.parts[0].offset == 0x28);
    XSFLIB_CHECK(entry.parts[0].blocks == (std::vector<uint32_t>{0x34, 0x3a}));
    XSFLIB_CHECK(database.record_count == 3);
    XSFLIB_CHECK(database.records.size() == 2);
    XSFLIB_CHECK(database.records[1].sound == 0x5000f && database.records[1].fourth == 0x8000);

    std::vector<uint8_t> bad = MakeDatabase();
    bad[7] = 'X';
    XSFLIB_CHECK(ThrowsFormatError([&] { ReadChordDatabase(bad); }));
    bad = MakeDatabase();
    bad[0x1b] = 0xf0; // the steps' offset past the end
    XSFLIB_CHECK(ThrowsFormatError([&] { ReadChordDatabase(bad); }));
}

void TestJungleBeatChordFiles()
{
    XSFLIB_CHECK(std::strcmp(JungleBeatChordFileOf(0x01000005), "chord_s_main01.cit") == 0);
    XSFLIB_CHECK(std::strcmp(JungleBeatChordFileOf(0x0100004c), "chord_bind01b.cit") == 0);
    XSFLIB_CHECK(JungleBeatChordFileOf(0x01000007) == nullptr);
    XSFLIB_CHECK(std::size(kJungleBeatChordFiles) == 53);
}

} // namespace

int main()
{
    TestChordFile();
    TestTones();
    TestScales();
    TestFitToScale();
    TestFitToChord();
    TestStepUpScale();
    TestDatabase();
    TestJungleBeatChordFiles();

    return xsflib::testing::Summary("chord files");
}
