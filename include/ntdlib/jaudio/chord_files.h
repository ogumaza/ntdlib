// SPDX-License-Identifier: MIT

// The chord files of Donkey Kong Jungle Beat, and the chords they describe. The game plays some sound effects in the
// chords of the music that plays. A chord file ("CITS" or "CITL", in the archive JaiChord.arc) gives a piece's chords
// and the order in which they come. The music tells the game which chord plays: the chord's position in that order.
// The chord database (ChordSeDataBase.csd, "CDSE") gives the effects that follow the chords. The addresses are in the
// main.dol of the GameCube release (USA). The Wii release, New Play Control! Donkey Kong Jungle Beat, has the same
// files and the same code at other addresses.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ntdlib::jaudio
{

// A value that a chord or a scale leaves unset, and a key or a pitch class that a lookup doesn't find.
inline constexpr uint8_t kChordNone = 0x7f;

// The kinds of chord: the intervals above the root that byte 1 of a chord adds (0x8024a8a8). Each kind adds a third,
// unless the chord leaves the third out. A major, minor or suspended chord then adds the fifth that byte 6 gives.
enum class ChordKind : uint8_t
{
    kMajor = 0,             // a major third
    kMinor = 1,             // a minor third
    kSuspended = 2,         // a fourth
    kDiminishedSeventh = 3, // a minor third, a diminished fifth and a diminished seventh
    kAugmented = 4,         // a major third and an augmented fifth
};

// A chord of a chord file: 16 bytes. Bytes 9 to 11 are unused.
struct Chord
{
    uint8_t root = kChordNone; // byte 0: the root's pitch class, 0 for C. kChordNone for no chord.
    uint8_t kind = 0;          // byte 1: a ChordKind. Any other value gives no chord.
    uint8_t seventh = 0;       // byte 2: 1 for a minor seventh, 2 for a major seventh, anything else for none
    // Bytes 3 to 5: added tones, each in semitones above the root, or kChordNone.
    std::array<uint8_t, 3> added = {kChordNone, kChordNone, kChordNone};
    uint8_t fifth = 0;               // byte 6: 1 raised, 2 lowered, kChordNone for none, anything else perfect
    uint8_t scale_root = kChordNone; // byte 7: the pitch class that a fit to the scale keeps, kChordNone for the root
    uint8_t no_third = 0;            // byte 8: not 0 to leave the third out
    // Bytes 12 to 15: the offset of the chord's scale in the file, in a "CITS" file only. The scale gives each degree
    // of C major (C, D, E, F, G, A and B) a code: 0 as it is, 1 raised, 2 lowered, 3 raised twice, 4 lowered twice,
    // or kChordNone to leave the degree out.
    std::optional<std::array<uint8_t, 7>> scale;
};

// A chord file. A "CITS" file gives each chord a scale. A "CITL" file gives none.
struct ChordFile
{
    // Each order of the chords: the chords' indices.
    std::vector<std::vector<uint8_t>> orders;
    std::vector<Chord> chords;
    bool has_scales = false;

    // The index of the chord at `position` of order `order`, or nothing past the order's end (0x8024b8a8).
    std::optional<uint8_t> ChordAt(std::size_t order, std::size_t position) const;
};

// Reads a chord file (0x8024b778): "CITS" or "CITL", a word, the count of orders and the count of chords (16 bits
// each), then the offset of each order and of each chord. An order is a count and the chords' indices, a byte each.
// Throws FormatError if the data isn't a chord file or a part lies outside it.
ChordFile ReadChordFile(std::span<const uint8_t> data);

// The tones of a chord, as the game lists them (0x8024a8a8): up to 8 pairs of a pitch class and its interval above
// the root, in semitones. The root comes first with 0, then the third, the fifth and the tones of a diminished
// seventh chord, in that order. The seventh follows them. Each added tone goes before the first tone listed with a
// higher pitch class. The rest are kChordNone.
struct ChordTones
{
    std::array<uint8_t, 8> pitch_classes{};
    std::array<uint8_t, 8> intervals{};

    // The interval of the first tone with pitch class `pitch_class`, or kChordNone if the chord has none
    // (0x8024ae48).
    uint8_t IntervalOf(uint8_t pitch_class) const;

    // Whether the pitch class of `key` is one of the tones (0x8024b1d4).
    bool Has(uint8_t key) const;

    // The nearest key above `key` (0x8024b248), or below it if `downward` (0x8024b2d4), whose pitch class is one of
    // the tones, within 12 semitones and from 12 to 120. kChordNone if there is none.
    uint8_t Nearest(uint8_t key, bool downward) const;
};

ChordTones TonesOf(const Chord& chord);

// `key` fitted to the tones of `chord` (0x8024b0fc): the key itself if its pitch class is the chord's scale root
// (byte 7, or else the root) or one of the tones. Otherwise ChordTones::Nearest.
uint8_t FitToChord(const Chord& chord, uint8_t key, bool downward);

// The pitch class of degree `degree` (0 to 6) of `scale` (0x8024b8e8): the degree's pitch class in C major, moved by
// its code and brought into 0 to 12. B raised stays 12, and so matches no key. -1 for a degree that the scale leaves
// out.
int ScaleDegree(const std::array<uint8_t, 7>& scale, int degree);

// Whether the pitch class of `key` is a degree of `scale` (0x8024b9f8).
bool InScale(const std::array<uint8_t, 7>& scale, int key);

// `key` fitted to the scale of `chord` (0x8024ae98): the key itself if its pitch class is the chord's scale root
// (byte 7, or else the root) or a degree of the scale. Otherwise the nearest key above it in the scale, or below it if
// `downward`, within 12 semitones and from 12 to 120. kChordNone if the chord has no scale or no such key exists.
uint8_t FitToScale(const Chord& chord, uint8_t key, bool downward);

// The nearest key above `key` (0x8024af94), or below it if `downward` (0x8024b048), in the scale of `chord`, within 12
// semitones and from 12 to 120. kChordNone if the chord has no scale or no such key exists.
uint8_t NearestInScale(const Chord& chord, uint8_t key, bool downward);

// `key` moved `steps` degrees up `scale` (0x8024ba68). A key whose pitch class isn't in the scale stays. Unless
// `count_repeats`, a step passes over a degree with the same pitch class as the one before it. Each pass of degree 6
// adds an octave, unless `keep_octave`.
int StepUpScale(const std::array<uint8_t, 7>& scale, int key, int steps, bool count_repeats, bool keep_octave);

// The chord database: the effects that follow the chords. The reader resolves the offsets, as the game does when it
// loads the file (0x8024c114, 0x8024c1d8 and 0x8024c25c). The file starts with a word, "CDSE", the count of entries
// and the offset of the records.
struct ChordDatabase
{
    // A part of an entry: the count of its blocks in its first byte, then each block's offset at 4.
    struct Part
    {
        uint32_t offset = 0;
        std::vector<uint32_t> blocks;
    };

    // An entry: the count of its parts (16 bits), the offset of its steps at 4, then each part's offset at 8.
    struct Entry
    {
        uint32_t offset = 0;
        uint32_t steps = 0;
        std::vector<Part> parts;
    };

    // A record: the effect that the game starts by the record's index (0x8024c4c8).
    struct Record
    {
        uint32_t entry = 0;
        uint32_t sound = 0; // the effect's sound ID
        uint32_t third = 0;
        uint32_t fourth = 0;
    };

    std::vector<Entry> entries;
    // The records up to the count that the table gives, as far as the file holds them. The game reads any record up
    // to the count. Jungle Beat's database gives a count of 218 and ends after 165 records.
    std::vector<Record> records;
    uint32_t record_count = 0;
};

// Reads the chord database. The offsets in the result count from the start of `data`. Throws FormatError if the data
// isn't a chord database or an entry or a part lies outside it.
ChordDatabase ReadChordDatabase(std::span<const uint8_t> data);

} // namespace ntdlib::jaudio
