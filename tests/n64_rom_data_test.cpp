// SPDX-License-Identifier: MIT

// Tests of the decoders and scanners of Nintendo 64 images (ntdlib/n64/rom_data.h). Each test builds its bytes by hand
// in the layouts that the header describes, and the expected values were worked out by hand from the same layouts.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/n64/ead_bank.h"
#include "ntdlib/n64/midi.h"
#include "ntdlib/n64/rom_data.h"
#include "ntdlib/n64/seq_file.h"

using xsflib::testing::ErrorOf;

namespace
{

namespace n64 = ntdlib::n64;

using Bytes = std::vector<uint8_t>;

constexpr std::string_view kRepeated = "abcabcabcd";

Bytes Text(std::string_view text)
{
    return Bytes(text.begin(), text.end());
}

void Put16(Bytes& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

void Put32(Bytes& out, uint32_t value)
{
    Put16(out, static_cast<uint16_t>(value >> 16));
    Put16(out, static_cast<uint16_t>(value));
}

// A block's header: the magic, the decoded size, and the places of the references and the literals.
Bytes Header(std::string_view magic, uint32_t size, uint32_t references, uint32_t literals)
{
    Bytes out = Text(magic);
    Put32(out, size);
    Put32(out, references);
    Put32(out, literals);

    return out;
}

void Append(Bytes& out, std::initializer_list<uint8_t> bytes)
{
    out.insert(out.end(), bytes);
}

void TestMio0()
{
    // Three literals, a reference of 6 bytes from 3 bytes back, and a literal: the bits 1110 1000.
    Bytes block = Header("MIO0", 10, 20, 22);
    Append(block, {0xE8, 0, 0, 0});
    Append(block, {0x30, 0x02});
    Append(block, {'a', 'b', 'c', 'd'});
    XSFLIB_CHECK(n64::DecodeMio0(block) == Text(kRepeated));

    // A reference to bytes before the start.
    Bytes early = Header("MIO0", 4, 20, 22);
    Append(early, {0x60, 0, 0, 0});
    Append(early, {0x10, 0x02});
    Append(early, {'a'});
    XSFLIB_CHECK(!ErrorOf<xsflib::FormatError>([&] { n64::DecodeMio0(early); }).empty());

    // Another format's magic.
    Bytes other = block;
    other[0] = 'Y';
    XSFLIB_CHECK(!ErrorOf<xsflib::FormatError>([&] { n64::DecodeMio0(other); }).empty());
}

void TestYay0()
{
    // A count of 6 takes the nibble 4 in this format.
    Bytes block = Header("Yay0", 10, 20, 22);
    Append(block, {0xE8, 0, 0, 0});
    Append(block, {0x40, 0x02});
    Append(block, {'a', 'b', 'c', 'd'});
    XSFLIB_CHECK(n64::DecodeYay0(block) == Text(kRepeated));

    // A nibble of 0 reads the count from the literals: 2 + 18 bytes of the last byte, between the literals 'a' and 'b'.
    // The bits are 101.
    Bytes longer = Header("Yay0", 22, 20, 22);
    Append(longer, {0xA0, 0, 0, 0});
    Append(longer, {0x00, 0x00});
    Append(longer, {'a', 2, 'b'});
    Bytes expected(21, 'a');
    expected.push_back('b');
    XSFLIB_CHECK(n64::DecodeYay0(longer) == expected);

    // A block that ends early.
    Bytes cut = block;
    cut.resize(24);
    XSFLIB_CHECK(!ErrorOf<xsflib::FormatError>([&] { n64::DecodeYay0(cut); }).empty());
}

void TestYaz0()
{
    // The literals and the references lie among the bits.
    Bytes block = Header("Yaz0", 10, 0, 0);
    Append(block, {0xE8, 'a', 'b', 'c', 0x40, 0x02, 'd'});
    XSFLIB_CHECK(n64::DecodeYaz0(block) == Text(kRepeated));

    Bytes longer = Header("Yaz0", 22, 0, 0);
    Append(longer, {0xA0, 'a', 0x00, 0x00, 2, 'b'});
    Bytes expected(21, 'a');
    expected.push_back('b');
    XSFLIB_CHECK(n64::DecodeYaz0(longer) == expected);

    // A second byte of flags after eight items.
    Bytes groups = Header("Yaz0", 9, 0, 0);
    Append(groups, {0xFF, '1', '2', '3', '4', '5', '6', '7', '8', 0x80, '9'});
    XSFLIB_CHECK(n64::DecodeYaz0(groups) == Text("123456789"));
}

// Each stream comes from zlib's deflate, or by hand from RFC 1951 for the stored block, after the two bytes of Rare's
// blocks.
void TestRare()
{
    // A stored block.
    const Bytes stored = {0x11, 0x72, 0x01, 0x03, 0x00, 0xFC, 0xFF, 'a', 'b', 'c'};
    XSFLIB_CHECK(n64::DecodeRare(stored) == Text("abc"));

    // A block with the fixed codes, which repeats from 3 bytes back.
    const Bytes fixed = {0x11, 0x72, 0x4B, 0x4C, 0x4A, 0x4E, 0x04, 0xA3, 0x14, 0x00};
    XSFLIB_CHECK(n64::DecodeRare(fixed) == Text(kRepeated));

    // A block with a table of codes: "aca" 20 times.
    const Bytes dynamic = {0x11, 0x72, 0x05, 0xC1, 0x01, 0x09, 0x00, 0x00, 0x00, 0x80, 0xA0, 0xAD, 0xD2,
                           0xFF, 0x0F, 0xA9, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0xC8};
    Bytes expected;
    for (int i = 0; i < 20; i++)
    {
        Append(expected, {'a', 'c', 'a'});
    }

    XSFLIB_CHECK(n64::DecodeRare(dynamic) == expected);

    // Another magic, a stored block whose sizes disagree, and a stream that ends early.
    XSFLIB_CHECK(!ErrorOf<xsflib::FormatError>([] { n64::DecodeRare(Bytes{0x11, 0x73, 0x01}); }).empty());
    XSFLIB_CHECK(!ErrorOf<xsflib::FormatError>(
                      [] { n64::DecodeRare(Bytes{0x11, 0x72, 0x01, 0x03, 0x00, 0xFC, 0xFE, 'a', 'b', 'c'}); })
                      .empty());
    Bytes cut = fixed;
    cut.resize(6);
    XSFLIB_CHECK(!ErrorOf<xsflib::FormatError>([&] { n64::DecodeRare(cut); }).empty());

    // A reference to bytes before the start: the fixed code for a length of 3, and a distance of 1.
    XSFLIB_CHECK(!ErrorOf<xsflib::FormatError>([] { n64::DecodeRare(Bytes{0x11, 0x72, 0x03, 0x02, 0x00}); }).empty());
}

void TestRareSequenceTable()
{
    // Junk, then a table of two entries with the blocks after it.
    Bytes image(6, 0xFF);
    const Bytes first = {0x11, 0x72, 0x01, 0x03, 0x00, 0xFC, 0xFF, 'a', 'b', 'c'};
    const Bytes second = {0x11, 0x72, 0x01, 0x01, 0x00, 0xFE, 0xFF, 'd'};
    Put16(image, 2);
    Put16(image, 0);
    Put32(image, 20);
    Put16(image, 3);
    Put16(image, static_cast<uint16_t>(first.size()));
    Put32(image, 30);
    Put16(image, 1);
    Put16(image, static_cast<uint16_t>(second.size()));
    image.insert(image.end(), first.begin(), first.end());
    image.insert(image.end(), second.begin(), second.end());

    const auto found = n64::FindRareSequences(image);
    XSFLIB_CHECK(found == std::size_t{6});
    const n64::RareSequenceTable table = n64::RareSequenceTable::Parse(std::span<const uint8_t>(image).subspan(6));
    XSFLIB_CHECK(table.entries.size() == 2);
    XSFLIB_CHECK(table.Sequence(0) == Text("abc"));
    XSFLIB_CHECK(table.Sequence(1) == Text("d"));

    // A block that decodes to another size than its entry's.
    Bytes wrong = image;
    wrong[6 + 4 + 5] = 4;
    const n64::RareSequenceTable wrong_table = n64::RareSequenceTable::Parse(std::span<const uint8_t>(wrong).subspan(6));
    XSFLIB_CHECK(!ErrorOf<xsflib::FormatError>([&] { wrong_table.Sequence(0); }).empty());

    // The second block doesn't start where the first ends.
    Bytes moved = image;
    moved[6 + 4 + 8 + 3] = 31;
    XSFLIB_CHECK(!n64::FindRareSequences(moved).has_value());
}

void PutLaterEntry(Bytes& out, uint32_t address, uint32_t size, uint16_t counts)
{
    Put32(out, address);
    Put32(out, size);
    Append(out, {2, 3});
    Put16(out, 0);
    Put16(out, counts);
    Put16(out, 0);
}

void PutLaterHeader(Bytes& out, uint16_t count)
{
    Put16(out, count);
    out.resize(out.size() + 14, 0);
}

// Tables in the image, as a game without a file table holds them, and files in a row after a stretch of zeros.
void TestFindPlainLaterSoundData()
{
    Bytes image(16, 0xAA);
    PutLaterHeader(image, 2);
    PutLaterEntry(image, 0, 0x30, 0x0100);
    PutLaterEntry(image, 0x30, 0x10, 0);
    PutLaterHeader(image, 2);
    PutLaterEntry(image, 0, 0x20, 0);
    PutLaterEntry(image, 0x20, 0x20, 0);
    PutLaterHeader(image, 2);
    PutLaterEntry(image, 0, 0x10, 0);
    PutLaterEntry(image, 0x10, 0x10, 0);
    image.resize(0x400, 0);

    // The first bank: no drums, no effects, and an instrument at 0x10 with no samples.
    constexpr std::size_t kBanks = 0x200;
    image[kBanks + 11] = 0x10;

    // The sequences start with commands.
    constexpr std::size_t kSequences = kBanks + 0x40 + 0x40;
    image[kSequences] = 0xD3;
    image[kSequences + 0x10] = 0xD3;

    const auto found = n64::FindLaterSoundData(image);
    XSFLIB_CHECK(found.has_value());
    XSFLIB_CHECK(found->banks == kBanks && found->samples == kBanks + 0x40 && found->sequences == kSequences);
    XSFLIB_CHECK(found->code.size() == 3 * 16 + 6 * 16);
    XSFLIB_CHECK(found->bank_table == 0 && found->sample_table == 48 && found->sequence_table == 96);
    XSFLIB_CHECK(n64::EadTable::Parse(found->code).entries.size() == 2);

    // A sequence that doesn't start with a command.
    Bytes without = image;
    without[kSequences + 0x10] = 0;
    XSFLIB_CHECK(!n64::FindLaterSoundData(without).has_value());
}

void TestFindFiles()
{
    // A sequence bank at 0x20 with one entry, and a standard MIDI file after it, at an odd offset.
    Bytes image(0x20, 0xFF);
    Append(image, {'S', '1'});
    Put16(image, 1);
    Put32(image, 12);
    Put32(image, 4);
    Append(image, {1, 2, 3, 4});

    const n64::MidiEvent end = {.delta = 0, .status = 0xFF, .type = 0x2F};
    const std::vector<std::vector<n64::MidiEvent>> tracks = {{end}};
    const n64::StandardMidi midi = n64::StandardMidi::Build(0, 480, tracks);
    const std::size_t midi_offset = image.size() + 1;
    image.push_back(0xFF);
    const std::span<const uint8_t> midi_bytes = midi.Bytes();
    image.insert(image.end(), midi_bytes.begin(), midi_bytes.end());
    image.resize(image.size() + 16, 0xFF);

    const n64::FoundFiles found = n64::FindFiles(image);
    XSFLIB_CHECK(found.sequence_banks == std::vector<std::size_t>{0x20});
    XSFLIB_CHECK(found.standard_midi == std::vector<std::size_t>{midi_offset});
    XSFLIB_CHECK(found.bank_files.empty());
}

// A file of the early engine with two entries of 8 bytes each. A 20-byte table takes it to 36 bytes.
void AppendEarlyFile(Bytes& image, std::size_t offset, uint16_t revision)
{
    image.resize(offset);
    Put16(image, revision);
    Put16(image, 2);
    Put32(image, 20);
    Put32(image, 8);
    Put32(image, 28);
    Put32(image, 8);
    image.resize(offset + 36, 0xAA);
}

void TestFindEarlySoundData()
{
    Bytes image;
    AppendEarlyFile(image, 0x10, n64::kEarlyBankFileRevision);
    AppendEarlyFile(image, 0x40, n64::kEarlySampleFileRevision);
    AppendEarlyFile(image, 0x70, n64::kEarlySequenceFileRevision);

    // The table of the sequences' banks: two offsets, then bank 1 for the first sequence and bank 0 for the second.
    image.resize(0xA0);
    Put16(image, 4);
    Put16(image, 6);
    Append(image, {1, 1, 1, 0});
    image.resize(0x100, 0xFF);

    const auto found = n64::FindEarlySoundData(image);
    XSFLIB_CHECK(found.has_value());
    XSFLIB_CHECK(found->banks == 0x10 && found->samples == 0x40 && found->sequences == 0x70);
    XSFLIB_CHECK(found->bank_sets == 0xA0U);

    // No file of sequences.
    Bytes without = image;
    without[0x71] = 0;
    XSFLIB_CHECK(!n64::FindEarlySoundData(without).has_value());
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("n64_rom_data",
                                    []
                                    {
                                        TestMio0();
                                        TestYay0();
                                        TestYaz0();
                                        TestRare();
                                        TestRareSequenceTable();
                                        TestFindPlainLaterSoundData();
                                        TestFindFiles();
                                        TestFindEarlySoundData();
                                    });
}
