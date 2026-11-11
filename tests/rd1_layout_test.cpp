// SPDX-License-Identifier: MIT

// Tests of the cut and the join of a ROM of Nintendo R&D1's GBA driver (ntdlib/rd1/layout.h). The ROMs are written by
// hand: a song table, records, a bank with one sample, and MIDI files.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <variant>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/rd1/commands.h"
#include "ntdlib/rd1/driver.h"
#include "ntdlib/rd1/layout.h"

using xsflib::testing::Check;

namespace
{

namespace rd1 = ntdlib::rd1;
using ntdlib::agb::kRomBase;
using ntdlib::agb::Rom;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;

// The places of a test ROM's data, from its start.
constexpr uint32_t kSongTable = 0x100;
constexpr uint32_t kBankTable = 0x180;
constexpr uint32_t kRecords = 0x200;
constexpr uint32_t kNames = 0x300;
constexpr uint32_t kBank = 0x400;
constexpr uint32_t kInstrument = 0x600;
constexpr uint32_t kSampleHeader = 0x620;
constexpr uint32_t kSampleData = 0x640;
constexpr uint32_t kFirstFile = 0x800;
constexpr uint32_t kSecondFile = 0x900;
constexpr uint32_t kDataEnd = 0xA00;
constexpr uint32_t kRomSize = 0x2000;

// A MIDI file of one track: a note of 24 ticks on `key`.
std::vector<uint8_t> NoteFile(uint8_t key)
{
    return {'M', 'T', 'h', 'd', 0, 0,    0,   6,   0,  0,    0,   1, 0, 48,   'M',  'T', 'r', 'k',
            0,   0,   0,   13,  0, 0x90, key, 100, 24, 0x80, key, 0, 0, 0xFF, 0x2F, 0,   0};
}

void Store32(std::vector<uint8_t>& image, uint32_t at, uint32_t value)
{
    for (uint32_t b = 0; b < 4; b++)
    {
        image[at + b] = uint8_t(value >> (8 * b));
    }
}

// A ROM of four song entries: records of the first file, the second file, none, and the first file again. Its data
// ends at kDataEnd, and 0xFF fills it from there.
std::vector<uint8_t> TestImage()
{
    std::vector<uint8_t> image(kRomSize, 0xFF);
    std::fill(image.begin(), image.begin() + kDataEnd, 0);

    const uint32_t kFiles[] = {kFirstFile, kSecondFile, 0, kFirstFile};
    for (uint32_t s = 0; s < 4; s++)
    {
        if (kFiles[s])
        {
            const uint32_t record = kRecords + 20 * s;
            Store32(image, kSongTable + 8 * s, kRomBase + record);
            Store32(image, record, kRomBase + kFiles[s]);
            Store32(image, record + 12, kRomBase + kNames);
        }
    }
    image[kNames] = 'm';

    // A bank whose first program plays a sample of 16 points.
    Store32(image, kBankTable, kRomBase + kBank);
    Store32(image, kBank, kRomBase + kInstrument);
    image[kInstrument] = 'A';
    image[kInstrument + 1] = 60;
    Store32(image, kInstrument + 4, kRomBase + kSampleHeader);
    Store32(image, kSampleHeader, 16);
    Store32(image, kSampleHeader + 4, 8000);
    Store32(image, kSampleHeader + 8, 60);
    Store32(image, kSampleHeader + 20, kRomBase + kSampleData);
    std::fill(image.begin() + kSampleData, image.begin() + kSampleData + 16, 0x40);

    const std::vector<uint8_t> first = NoteFile(60);
    const std::vector<uint8_t> second = NoteFile(64);
    std::copy(first.begin(), first.end(), image.begin() + kFirstFile);
    std::copy(second.begin(), second.end(), image.begin() + kSecondFile);

    return image;
}

rd1::DriverInfo TestInfo()
{
    rd1::DriverInfo info;
    info.song_table = kRomBase + kSongTable;
    info.song_count = 4;
    info.players.resize(1);
    info.bank_table = kRomBase + kBankTable;
    info.bank_count = 1;

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

    const rd1::Layout layout = rd1::Cut(rom, TestInfo());

    Check(rd1::Join(layout) == image, "the pieces join to give the ROM back");
    Check(layout.sequences.size() == 2 && layout.pieces.size() == 5, "a piece for each file, between runs");
    Check(layout.fields.size() == 3, "a field for each record");
    Check(layout.fields[0].at == ntdlib::sar::Place{0, kRecords} && layout.fields[2].to.piece == 1 &&
              layout.fields[2].to.item,
          "a record's field names its file's first item");
    Check(layout.free.start == 0xB00 && layout.free.padding == 0xFF, "the free space is in the run at the ROM's end");
}

void TestGrowth()
{
    const Rom rom = RomOf(TestImage());
    rd1::Layout layout = rd1::Cut(rom, TestInfo());

    // The first file takes a text event of 300 bytes, and outgrows its place.
    Command text;
    text.kind = Kind::kOther;
    text.encoding.opcode = rd1::kMetaEvent | 0x01;
    text.bytes.assign(300, 'x');
    std::vector<Command>& items = layout.sequences[0].items;
    Command wait;
    wait.kind = Kind::kWait;
    items.insert(items.begin() + 2, {wait, text});

    const std::vector<uint8_t> grown = rd1::Join(layout);

    const Rom grown_rom = RomOf(grown);
    Check(grown.size() == kRomSize, "the file moves to the free space, inside the ROM");
    Check(grown_rom.U32(kRomBase + kRecords) == kRomBase + 0xB00 &&
              grown_rom.U32(kRomBase + kRecords + 60) == kRomBase + 0xB00,
          "both of its records name its new place");
    Check(grown_rom.U32(kRomBase + kRecords + 20) == kRomBase + kSecondFile, "the other file stays");
    Check(std::all_of(grown.begin() + kFirstFile, grown.begin() + kFirstFile + 35, [](uint8_t b) { return b == 0; }),
          "zeros fill the place it left");

    const rd1::Decoded moved = rd1::DecodeRomSequence(grown_rom, TestInfo(), 0);
    Check(!moved.opaque && moved.sequence == layout.sequences[0], "the moved file reads as its items");
    Check(rd1::Join(rd1::Cut(grown_rom, TestInfo())) == grown, "the grown ROM cuts and joins back");

    // A file that fits its place stays there, with zeros after it.
    rd1::Layout shrunk = rd1::Cut(rom, TestInfo());
    std::vector<Command>& second = shrunk.sequences[1].items;
    second.erase(second.begin() + 4, second.begin() + 6);

    const std::vector<uint8_t> joined = rd1::Join(shrunk);

    Check(joined.size() == kRomSize && RomOf(joined).U32(kRomBase + kRecords + 20) == kRomBase + kSecondFile,
          "a smaller file keeps its place");
    Check(joined[kSecondFile + 31] == 0 && joined[kSecondFile + 33] == 0, "zeros follow the smaller file");
}

void TestRejectedRecord()
{
    // The fourth record names the first file and a player that the driver doesn't have. ReadSongRecord rejects it.
    std::vector<uint8_t> image = TestImage();
    image[kSongTable + 8 * 3 + 4] = 5;
    const Rom rom = RomOf(image);
    rd1::Layout layout = rd1::Cut(rom, TestInfo());

    Check(layout.fields.size() == 3, "a rejected record that names a file gets a field");

    // The first file outgrows its place and moves.
    Command text;
    text.kind = Kind::kOther;
    text.encoding.opcode = rd1::kMetaEvent | 0x01;
    text.bytes.assign(300, 'x');
    Command wait;
    wait.kind = Kind::kWait;
    std::vector<Command>& items = layout.sequences[0].items;
    items.insert(items.begin() + 2, {wait, text});

    const Rom grown = RomOf(rd1::Join(layout));

    Check(grown.U32(kRomBase + kRecords) == kRomBase + 0xB00 && grown.U32(kRomBase + kRecords + 60) == kRomBase + 0xB00,
          "the rejected record names the file's new place too");
}

void TestFilesThatStay()
{
    // The second file starts off a multiple of 4, and the first overlaps the sample's points.
    std::vector<uint8_t> image = TestImage();
    const std::vector<uint8_t> second = NoteFile(64);
    std::fill(image.begin() + kSecondFile, image.begin() + kSecondFile + 40, 0);
    std::copy(second.begin(), second.end(), image.begin() + kSecondFile + 1);
    Store32(image, kRecords + 20, kRomBase + kSecondFile + 1);
    const std::vector<uint8_t> first = NoteFile(60);
    std::copy(first.begin(), first.end(), image.begin() + kSampleHeader + 24);
    Store32(image, kRecords, kRomBase + kSampleHeader + 24);
    Store32(image, kRecords + 60, kRomBase + kSampleHeader + 24);
    const Rom rom = RomOf(image);

    const rd1::Layout layout = rd1::Cut(rom, TestInfo());

    Check(layout.sequences.empty() && layout.pieces.size() == 1 && layout.fields.empty(),
          "a file off a multiple of 4, and a file over a sample, stay among the bytes");
    Check(rd1::Join(layout) == image, "the ROM comes back");
}

void TestFillerRuns()
{
    // A run of 0xFF inside the ROM, and data after it.
    std::vector<uint8_t> image = TestImage();
    image.resize(0x4000);
    std::fill(image.begin() + kDataEnd, image.end(), 0x11);
    std::fill(image.begin() + 0x1000, image.begin() + 0x3000, 0xFF);
    const Rom rom = RomOf(image);

    const std::vector<rd1::Span> runs = rd1::FillerRuns(rom, TestInfo());

    Check(runs.size() == 1 && runs[0].start == 0x1100 && runs[0].size == 0x3000 - 0x100 - 0x1100,
          "the run of 0xFF inside the ROM, clear of its ends");

    const rd1::Layout layout = rd1::Cut(rom, TestInfo());

    Check(layout.free.start == 0x4000, "without a run at its end, the free space starts at the ROM's end");
}

void TestJoinErrors()
{
    const Rom rom = RomOf(TestImage());
    const rd1::Layout layout = rd1::Cut(rom, TestInfo());

    rd1::Layout padded = layout;
    padded.pieces.emplace_back(ntdlib::sar::Padding{4});
    Check(Throws<std::invalid_argument>([&] { rd1::Join(padded); }), "padding");

    rd1::Layout misplaced = layout;
    misplaced.fields[0].at = {1, 0};
    Check(Throws<std::invalid_argument>([&] { rd1::Join(misplaced); }), "a field in a sequence piece");

    rd1::Layout missing = layout;
    std::get<ntdlib::sar::SequencePiece>(missing.pieces[1]).sequence = 9;
    Check(Throws<std::invalid_argument>([&] { rd1::Join(missing); }), "a piece of a sequence missing from the layout");

    rd1::Layout broken = layout;
    broken.sequences[0].items.pop_back();
    broken.sequences[0].items.pop_back();
    Check(Throws<std::invalid_argument>([&] { rd1::Join(broken); }), "a file that the encoder can't write");
}

} // namespace

int main()
{
    TestRoundTrip();
    TestGrowth();
    TestRejectedRecord();
    TestFilesThatStay();
    TestFillerRuns();
    TestJoinErrors();

    return xsflib::testing::Summary("rd1_layout");
}
