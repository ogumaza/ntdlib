// SPDX-License-Identifier: MIT

// Tests of the cut and the join of a ROM of AlphaDream's GBA driver (ntdlib/alphadream/layout.h). The ROMs are written
// by hand: a song table, a table of sound effects, an instrument with one region, a sample, the driver's tables and
// songs.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <variant>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/agb/rom.h"
#include "ntdlib/alphadream/commands.h"
#include "ntdlib/alphadream/driver.h"
#include "ntdlib/alphadream/layout.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"

using xsflib::testing::Check;

namespace
{

namespace alphadream = ntdlib::alphadream;
using ntdlib::agb::kRomBase;
using ntdlib::agb::Rom;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;

// The places of a test ROM's data, from its start.
constexpr uint32_t kSongTable = 0x100;
constexpr uint32_t kEffectTable = 0x120;
constexpr uint32_t kInstrumentTable = 0x180;
constexpr uint32_t kSampleTable = 0x200;
constexpr uint32_t kSampleData = 0x214;
constexpr uint32_t kStepTable = 0x300;
constexpr uint32_t kKeyTable = 0x340;
constexpr uint32_t kPsgPitchTable = 0x3C0;
constexpr uint32_t kNoiseTable = 0x480;
constexpr uint32_t kWaveLevels = 0x4C0;
constexpr uint32_t kWaveTable = 0x500;
constexpr uint32_t kEffect = 0x700;
constexpr uint32_t kFirstSong = 0x800;
constexpr uint32_t kSecondSong = 0x900;
constexpr uint32_t kDataEnd = 0xA00;
constexpr uint32_t kRomSize = 0x2000;

// A song of track 0: a note of 24 ticks on `key`.
std::vector<uint8_t> NoteSong(uint8_t key)
{
    return {0x01, 0x00, 0x04, 0x00, 0x18, key, 0xFF};
}

// The second song: a pan, then a note of 24 ticks at key 64.
std::vector<uint8_t> PanSong()
{
    return {0x01, 0x00, 0x04, 0x00, 0xF2, 0x40, 0x18, 0x40, 0xFF};
}

void Store16(std::vector<uint8_t>& image, uint32_t at, uint32_t value)
{
    image[at] = uint8_t(value);
    image[at + 1] = uint8_t(value >> 8);
}

void Store32(std::vector<uint8_t>& image, uint32_t at, uint32_t value)
{
    Store16(image, at, value);
    Store16(image, at + 2, value >> 16);
}

void Put(std::vector<uint8_t>& image, uint32_t at, const std::vector<uint8_t>& bytes)
{
    std::copy(bytes.begin(), bytes.end(), image.begin() + at);
}

// A ROM of three songs in the song table: the first song, the second song and the first song again. Its data ends at
// kDataEnd, and 0xFF fills it from there.
std::vector<uint8_t> TestImage()
{
    std::vector<uint8_t> image(kRomSize, 0xFF);
    std::fill(image.begin(), image.begin() + kDataEnd, 0);
    Store32(image, kSongTable, kRomBase + kFirstSong);
    Store32(image, kSongTable + 4, kRomBase + kSecondSong);
    Store32(image, kSongTable + 8, kRomBase + kFirstSong);
    Store32(image, kEffectTable, kRomBase + kEffect);

    // An instrument of one region that plays sample 0, of 16 points.
    Store16(image, kInstrumentTable, 4);
    Store16(image, kInstrumentTable + 2, 12);
    Put(image, kInstrumentTable + 4, {0, 127, 0, 0, 0xFF, 0, 0xFF, 0});
    Store32(image, kSampleTable, 4);
    Store32(image, kSampleTable + 8, 8000 * 1024);
    Store32(image, kSampleTable + 16, 16);
    std::fill(image.begin() + kSampleData, image.begin() + kSampleData + 16, 0x80);

    Put(image, kEffect, {0x00, 0x04, 0x04, 0x00, 0x0C, 0x48, 0xFF});
    Put(image, kFirstSong, NoteSong(60));
    Put(image, kSecondSong, PanSong());

    return image;
}

alphadream::DriverInfo TestInfo()
{
    alphadream::DriverInfo info;
    info.song_table = kRomBase + kSongTable;
    info.song_count = 3;
    info.effect_table = kRomBase + kEffectTable;
    info.instrument_table = kRomBase + kInstrumentTable;
    info.instrument_count = 1;
    info.sample_table = kRomBase + kSampleTable;
    info.sample_count = 1;
    info.step_table = kRomBase + kStepTable;
    info.key_table = kRomBase + kKeyTable;
    info.psg_pitch_table = kRomBase + kPsgPitchTable;
    info.noise_table = kRomBase + kNoiseTable;
    info.wave_table = kRomBase + kWaveTable;
    info.wave_levels = kRomBase + kWaveLevels;

    return info;
}

Rom RomOf(std::vector<uint8_t> image)
{
    Rom rom;
    rom.Assign(std::move(image));

    return rom;
}

// Whether `body` throws an exception of type `E`.
template <typename E, typename Body>
bool Throws(Body body)
{
    try
    {
        body();
    }
    catch (const E&)
    {
        return true;
    }

    return false;
}

void TestRoundTrip()
{
    const std::vector<uint8_t> image = TestImage();
    const Rom rom = RomOf(image);

    const alphadream::Layout layout = alphadream::Cut(rom, TestInfo());

    Check(alphadream::Join(layout) == image, "the pieces join to give the ROM back");
    Check(layout.sequences.size() == 2 && layout.pieces.size() == 5, "a piece for each song, between runs");
    Check(layout.fields.size() == 3, "a field for each word of the song table");
    Check(layout.fields[0].at == ntdlib::sar::Place{0, kSongTable} && layout.fields[2].to.piece == 1 &&
              layout.fields[2].to.item,
          "a word's field names its song's first item");
    Check(layout.free.start == 0xB00 && layout.free.padding == 0xFF, "the free space is in the run at the ROM's end");
}

void TestGrowth()
{
    const Rom rom = RomOf(TestImage());
    alphadream::Layout layout = alphadream::Cut(rom, TestInfo());

    // The first song takes a pan and a tempo before its note, and outgrows its place.
    Command pan;
    pan.kind = Kind::kPan;
    pan.value.value = ntdlib::commands::ToUnit(0x40, {});
    Command tempo = pan;
    tempo.kind = Kind::kTempo;
    std::vector<Command>& items = layout.sequences[0].items;
    items.insert(items.begin() + 2, {pan, tempo});

    const std::vector<uint8_t> grown = alphadream::Join(layout);

    const Rom grown_rom = RomOf(grown);
    Check(grown.size() == kRomSize, "the song moves to the free space, inside the ROM");
    Check(grown_rom.U32(kRomBase + kSongTable) == kRomBase + 0xB00 &&
              grown_rom.U32(kRomBase + kSongTable + 8) == kRomBase + 0xB00,
          "both of its words name its new place");
    Check(grown_rom.U32(kRomBase + kSongTable + 4) == kRomBase + kSecondSong, "the other song stays");
    Check(std::all_of(grown.begin() + kFirstSong, grown.begin() + kFirstSong + 7, [](uint8_t b) { return b == 0; }),
          "zeros fill the place it left");

    const alphadream::Decoded moved = alphadream::DecodeRomSequence(grown_rom, TestInfo(), 0);
    Check(!moved.opaque && moved.sequence == layout.sequences[0], "the moved song reads as its items");
    Check(alphadream::Join(alphadream::Cut(grown_rom, TestInfo())) == grown, "the grown ROM cuts and joins back");

    // A song that fits its place stays there, with zeros after it.
    alphadream::Layout shrunk = alphadream::Cut(rom, TestInfo());
    std::vector<Command>& second = shrunk.sequences[1].items;
    second.erase(second.begin() + 2);

    const std::vector<uint8_t> joined = alphadream::Join(shrunk);

    Check(joined.size() == kRomSize && RomOf(joined).U32(kRomBase + kSongTable + 4) == kRomBase + kSecondSong,
          "a smaller song keeps its place");
    Check(joined[kSecondSong + 4] == 0x18 && joined[kSecondSong + 7] == 0 && joined[kSecondSong + 8] == 0,
          "zeros follow the smaller song");
}

void TestSongsThatStay()
{
    // The first word names a song over the sample's points, the second a song off a multiple of 2, and the third the
    // sound effect.
    std::vector<uint8_t> image = TestImage();
    std::fill(image.begin() + kSecondSong, image.begin() + kSecondSong + 16, 0);
    Put(image, kSecondSong + 1, PanSong());
    Put(image, kSampleData, NoteSong(60));
    Store32(image, kSongTable, kRomBase + kSampleData);
    Store32(image, kSongTable + 4, kRomBase + kSecondSong + 1);
    Store32(image, kSongTable + 8, kRomBase + kEffect);
    const Rom rom = RomOf(image);

    const alphadream::Layout layout = alphadream::Cut(rom, TestInfo());

    Check(
        layout.sequences.empty() && layout.pieces.size() == 1 && layout.fields.empty(),
        "a song over a sample, a song off a multiple of 2 and a song that a sound effect shares stay among the bytes");
    Check(alphadream::Join(layout) == image, "the ROM comes back");

    // The first song's track starts in the second song.
    std::vector<uint8_t> shared = TestImage();
    Store16(shared, kFirstSong + 2, kSecondSong + 6 - kFirstSong);

    const alphadream::Layout both = alphadream::Cut(RomOf(shared), TestInfo());

    Check(both.sequences.empty() && both.fields.empty(), "two songs that share bytes stay among the bytes");
    Check(alphadream::Join(both) == shared, "the ROM of two songs that share bytes comes back");
}

void TestFillerRuns()
{
    // A run of 0xFF inside the ROM, and data after it.
    std::vector<uint8_t> image = TestImage();
    image.resize(0x4000);
    std::fill(image.begin() + kDataEnd, image.end(), 0x11);
    std::fill(image.begin() + 0x1000, image.begin() + 0x3000, 0xFF);
    const Rom rom = RomOf(image);

    const std::vector<alphadream::Span> runs = alphadream::FillerRuns(rom, TestInfo());

    Check(runs.size() == 1 && runs[0].start == 0x1100 && runs[0].size == 0x3000 - 0x100 - 0x1100,
          "the run of 0xFF inside the ROM, clear of its ends");

    const alphadream::Layout layout = alphadream::Cut(rom, TestInfo());

    Check(layout.free.start == 0x4000, "without a run at its end, the free space starts at the ROM's end");
}

void TestJoinErrors()
{
    const Rom rom = RomOf(TestImage());
    const alphadream::Layout layout = alphadream::Cut(rom, TestInfo());

    alphadream::Layout padded = layout;
    padded.pieces.emplace_back(ntdlib::sar::Padding{4});
    Check(Throws<std::invalid_argument>([&] { alphadream::Join(padded); }), "padding");

    alphadream::Layout misplaced = layout;
    misplaced.fields[0].at = {1, 0};
    Check(Throws<std::invalid_argument>([&] { alphadream::Join(misplaced); }), "a field in a sequence piece");

    alphadream::Layout missing = layout;
    std::get<ntdlib::sar::SequencePiece>(missing.pieces[1]).sequence = 9;
    Check(Throws<std::invalid_argument>([&] { alphadream::Join(missing); }),
          "a piece of a sequence missing from the layout");

    alphadream::Layout broken = layout;
    broken.sequences[0].items.pop_back();
    Check(Throws<std::invalid_argument>([&] { alphadream::Join(broken); }), "a song that the encoder can't write");

    // In B85A's version, each note takes a third byte, the track's volume. Both songs outgrow their places.
    alphadream::Layout other = layout;
    other.version = alphadream::Version::kHamSports;

    const Rom sports = RomOf(alphadream::Join(other));

    Check(sports.U32(kRomBase + kSongTable) != kRomBase + kFirstSong &&
              sports.U32(kRomBase + kSongTable + 4) != kRomBase + kSecondSong,
          "the version sets the bytes of the notes");
}

} // namespace

int main()
{
    TestRoundTrip();
    TestGrowth();
    TestSongsThatStay();
    TestFillerRuns();
    TestJoinErrors();

    return xsflib::testing::Summary("alphadream_layout");
}
