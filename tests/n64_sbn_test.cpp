// SPDX-License-Identifier: MIT

// Tests of the reader of the sound archive of Intelligent Systems' engine (ntdlib/n64/sbn.h). Each test builds its
// bytes by hand in the layouts that the header describes, and the expected values were worked out by hand from the same
// layouts.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/n64/sbn.h"

using xsflib::testing::ErrorOf;

namespace
{

namespace n64 = ntdlib::n64;

using Bytes = std::vector<uint8_t>;

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

void PutText(Bytes& out, std::string_view text)
{
    out.insert(out.end(), text.begin(), text.end());
}

void Append(Bytes& out, std::initializer_list<uint8_t> bytes)
{
    out.insert(out.end(), bytes);
}

// A BGM file with one composition, one phrase and one track, and a table of one drum and one instrument.
//
//     0x00  the header, 0x24 bytes
//     0x24  the composition: play the phrase at +0x10, start a loop with label 1, end the loop with label 1, 3 times,
//     end 0x34  the phrase's 16 words, 0x40 bytes 0x74  the track's commands 0x90  the detour's bytes 0x98  the
//     branch's table: two entries 0xA0  the branch's places 0xA8  the drum, then the instrument
Bytes MakeBgm()
{
    Bytes bgm;
    PutText(bgm, "BGM ");
    Put32(bgm, 0xBC);
    PutText(bgm, "TEST");
    Put32(bgm, 0);
    Append(bgm, {4, 0, 0, 0});
    Put16(bgm, 0x24 >> 2);
    Put16(bgm, 0);
    Put16(bgm, 0);
    Put16(bgm, 0);
    Put16(bgm, 0xA8 >> 2);
    Put16(bgm, 1);
    Put16(bgm, 0xB4 >> 2);
    Put16(bgm, 1);

    Put32(bgm, 0x10000000 | (0x10 >> 2));
    Put32(bgm, 0x30000001);
    Put32(bgm, 0x50000000 | (3 << 5) | 1);
    Put32(bgm, 0);

    // The track starts 0x40 bytes after the phrase. It's a drum track with a polyphony of 2 voices (index 5).
    Put32(bgm, (0x40U << 16) | (5U << 13) | 0x80);
    for (int i = 1; i < 16; i++)
    {
        Put32(bgm, 0);
    }

    // 0x74: tempo; a wait of 16; a note of key 0x10 with a length of 1; a note with a long length; a long wait; a
    // detour of 3 bytes to 0x90; a branch; the end.
    Append(bgm, {0xE0, 0x00, 0x78});
    Append(bgm, {0x10});
    Append(bgm, {0x90, 0x60, 0x01});
    Append(bgm, {0x91, 0x61, 0xC1, 0x02});
    Append(bgm, {0x79, 0x03});
    Append(bgm, {0xFE, 0x00, 0x90, 0x03});
    Append(bgm, {0xFC, 0x00, 0x98, 0x02});
    Append(bgm, {0x00});
    bgm.resize(0x90, 0);

    // 0x90: a note inside the detour.
    Append(bgm, {0x92, 0x62, 0x05});
    bgm.resize(0x98, 0);

    Append(bgm, {0x00, 0xA0, 0x00, 0x00, 0xA4, 0x01});
    bgm.resize(0xA0, 0);

    // 0xA0: a wait, then the return. 0xA4: a note, then the return.
    Append(bgm, {0x05, 0x00});
    bgm.resize(0xA4, 0);
    Append(bgm, {0x93, 0x63, 0x07, 0x00});

    // 0xA8: the drum.
    Put16(bgm, 0x3004);
    Put16(bgm, 6000);
    Append(bgm, {100, 0xC0, 7, 1, 2, 3, 4, 9});

    // 0xB4: the instrument.
    Put16(bgm, 0x3105);
    Append(bgm, {90, 40, 8, 0xFE, 0x05, 0x0A});

    return bgm;
}

void TestBgmFile()
{
    const Bytes data = MakeBgm();
    const n64::BgmFile file = n64::BgmFile::Parse(data);
    XSFLIB_CHECK(file.name == "TEST" && file.timing_preset == 4);
    XSFLIB_CHECK(file.CompositionOffset(0) == 0x24 && file.CompositionOffset(1) == 0);
    XSFLIB_CHECK(file.drums.size() == 1 && file.instruments.size() == 1);
    XSFLIB_CHECK(file.drums[0].bank_patch == 0x3004 && file.drums[0].key_base == 6000 && file.drums[0].pan == -64);
    XSFLIB_CHECK(file.drums[0].random_reverb == 4 && file.drums[0].padding == 9);
    XSFLIB_CHECK(file.instruments[0].coarse_tune == -2 && file.instruments[0].fine_tune == 5);
    XSFLIB_CHECK(file.Write() == data);

    const auto commands = file.Composition(0x24);
    XSFLIB_CHECK(commands.size() == 4);
    XSFLIB_CHECK(commands[0].Op() == n64::BgmCompositionOp::kPlayPhrase && commands[0].PhraseOffset(0x24) == 0x34);
    XSFLIB_CHECK(commands[1].Op() == n64::BgmCompositionOp::kStartLoop && commands[1].Label() == 1);
    XSFLIB_CHECK(commands[2].Op() == n64::BgmCompositionOp::kEndLoop && commands[2].LoopCount() == 3);
    XSFLIB_CHECK(commands[3].Op() == n64::BgmCompositionOp::kEnd);

    const auto tracks = file.Phrase(0x34);
    XSFLIB_CHECK(tracks[0].offset == 0x74 && tracks[0].polyphony == 5 && tracks[0].drums);
    XSFLIB_CHECK(tracks[1].offset == 0);

    // A changed field lands at its place.
    n64::BgmFile edited = file;
    edited.drums[0].volume = 1;
    edited.instruments[0].pan = -1;
    const Bytes written = edited.Write();
    XSFLIB_CHECK(written[0xA8 + 4] == 1 && written[0xB4 + 3] == 0xFF);

    Bytes bad = data;
    bad[0] = 'X';
    XSFLIB_CHECK(!n64::BgmFile::Matches(bad));
    XSFLIB_CHECK(!ErrorOf<xsflib::FormatError>([&] { n64::BgmFile::Parse(bad); }).empty());
}

void TestBgmCommands()
{
    const Bytes data = MakeBgm();
    n64::BgmCursor cursor;
    cursor.position = 0x74;

    n64::BgmCommand command = n64::ReadBgmCommand(data, cursor);
    XSFLIB_CHECK(command.op == n64::BgmOp::kTempo && command.args[1] == 0x78 && cursor.position == 0x77);

    command = n64::ReadBgmCommand(data, cursor);
    XSFLIB_CHECK(command.op == n64::BgmOp::kWait && command.length == 16);

    command = n64::ReadBgmCommand(data, cursor);
    XSFLIB_CHECK(command.op == n64::BgmOp::kNote && command.key == 0x10 && command.velocity == 0x60);
    XSFLIB_CHECK(command.length == 1 && command.offset == 0x78);

    // A first length byte of 0xC1 with 2 after it: (1 << 8) + 2 + 0xC0.
    command = n64::ReadBgmCommand(data, cursor);
    XSFLIB_CHECK(command.op == n64::BgmOp::kNote && command.length == 0x1C2);

    // A first wait byte of 0x79 with 3 after it: (1 << 8) + 3 + 0x78.
    command = n64::ReadBgmCommand(data, cursor);
    XSFLIB_CHECK(command.op == n64::BgmOp::kWait && command.length == 0x17B);

    // The detour takes the cursor to its place, and its last byte takes the cursor back.
    command = n64::ReadBgmCommand(data, cursor);
    XSFLIB_CHECK(command.op == n64::BgmOp::kDetour && cursor.position == 0x90 && cursor.detour_left == 3);
    const uint32_t saved = cursor.saved;
    command = n64::ReadBgmCommand(data, cursor);
    XSFLIB_CHECK(command.op == n64::BgmOp::kNote && command.key == 0x12 && command.length == 5);
    XSFLIB_CHECK(cursor.position == saved && cursor.detour_left == 0);

    // The branch takes a place from its table. A value past the table takes the first place.
    command = n64::ReadBgmCommand(data, cursor);
    XSFLIB_CHECK(command.op == n64::BgmOp::kBranch && command.args[2] == 2);
    XSFLIB_CHECK(n64::ReadBgmBranch(data, command, 0).position == 0xA0);
    XSFLIB_CHECK(!n64::ReadBgmBranch(data, command, 0).drums);
    XSFLIB_CHECK(n64::ReadBgmBranch(data, command, 1).position == 0xA4);
    XSFLIB_CHECK(n64::ReadBgmBranch(data, command, 1).drums);
    XSFLIB_CHECK(n64::ReadBgmBranch(data, command, 9).position == 0xA0);

    // The zero that ends a branch returns. The next one ends the phrase.
    const uint32_t after = cursor.position;
    cursor.branch_end = after;
    cursor.position = 0xA1;
    command = n64::ReadBgmCommand(data, cursor);
    XSFLIB_CHECK(command.op == n64::BgmOp::kReturn && cursor.position == after && cursor.branch_end == 0);
    command = n64::ReadBgmCommand(data, cursor);
    XSFLIB_CHECK(command.op == n64::BgmOp::kEnd);

    // A code that no track holds, and commands that leave the file.
    const Bytes invalid = {0xD4};
    n64::BgmCursor at_invalid;
    XSFLIB_CHECK(n64::ReadBgmCommand(invalid, at_invalid).op == n64::BgmOp::kInvalid);
    const Bytes cut = {0xE0, 0x00};
    n64::BgmCursor at_cut;
    XSFLIB_CHECK(!ErrorOf<xsflib::FormatError>([&] { n64::ReadBgmCommand(cut, at_cut); }).empty());
}

void TestBgmWalk()
{
    const n64::BgmFile file = n64::BgmFile::Parse(MakeBgm());
    const n64::BgmWalk walk = n64::WalkBgm(file);
    XSFLIB_CHECK(walk.compositions == 1 && walk.phrases == 1 && walk.tracks == 1);
    XSFLIB_CHECK(walk.detours == 1 && walk.branches == 1);

    // The track's notes: two, one in the detour, and one in the second branch's place.
    XSFLIB_CHECK(walk.notes == 4);
    XSFLIB_CHECK(walk.bad_offsets.empty());
}

// A BK file with one instrument at slot 2.
Bytes MakeBk()
{
    Bytes bank;
    Append(bank, {'B', 'K', ' ', ' '});
    Put32(bank, 0x40 + 0x30 + 0x20 + 0x20 + 9);
    Append(bank, {'N', 'A', 'M', 'E'});
    Append(bank, {'C', 'R', 0, 0, 0, 0});

    // The offsets of the instruments, from 0x12.
    Put16(bank, 0);
    Put16(bank, 0);
    Put16(bank, 0x40);
    for (int i = 3; i < 16; i++)
    {
        Put16(bank, 0);
    }

    bank.resize(0x40, 0);
    Put32(bank, 0xB0); // the wave
    Put32(bank, 9);    // its size
    Put32(bank, 0x70); // the loop's state
    Put32(bank, 0);    // the loop's start
    Put32(bank, 16);   // its end
    Put32(bank, 0xFFFFFFFF);
    Put32(bank, 0x90); // the book
    Put16(bank, 32);
    Put16(bank, 6000);
    Put32(bank, 32000);
    Append(bank, {0, 1, 2, 3, 4, 5, 6, 7});
    Put32(bank, 0);

    bank.resize(0x70, 0);
    for (uint16_t i = 0; i < 16; i++)
    {
        Put16(bank, i);
    }

    bank.resize(0x90, 0);
    for (uint16_t i = 0; i < 16; i++)
    {
        Put16(bank, static_cast<uint16_t>(i * 2));
    }

    Append(bank, {0x10, 1, 2, 3, 4, 5, 6, 7, 8});

    return bank;
}

void TestBkFile()
{
    const Bytes data = MakeBk();
    const n64::BkFile bank = n64::BkFile::Parse(data);
    XSFLIB_CHECK(bank.name == "NAME");
    XSFLIB_CHECK(bank.slots[2] == 0 && bank.slots[0] == -1 && bank.instruments.size() == 1);
    const n64::BkInstrument& instrument = bank.instruments[0];
    XSFLIB_CHECK(instrument.sample_rate == 32000 && instrument.key_base == 6000 && instrument.unused[6] == 7);
    XSFLIB_CHECK(bank.Book(instrument).predictor_count == 1 && bank.Book(instrument).Coefficient(0, 1, 7) == 30);
    XSFLIB_CHECK(bank.LoopState(instrument)[15] == 15);
    XSFLIB_CHECK(bank.Wave(instrument).size() == 9 && bank.Wave(instrument)[0] == 0x10);
    XSFLIB_CHECK(bank.Write() == data);

    n64::BkFile edited = bank;
    edited.instruments[0].sample_rate = 22050;
    const Bytes written = edited.Write();
    XSFLIB_CHECK(xsflib::LoadBe32(written.data() + 0x40 + 0x20) == 22050);
}

void TestArchive()
{
    Bytes archive;
    PutText(archive, "SBN ");
    Put32(archive, 0); // the size, filled in below
    Put32(archive, 0);
    Put32(archive, 0);
    Put32(archive, 0x40);
    Put32(archive, 3); // files
    Put32(archive, 0);
    Put32(archive, 0);
    Put32(archive, 0);
    Put32(archive, 0x60); // INIT
    archive.resize(0x40, 0);

    // The table: the files at 0x90, 0x150 and 0x210.
    const Bytes bgm = MakeBgm();
    const Bytes bank = MakeBk();
    Put32(archive, 0x90);
    Put32(archive, 0x10000000 | static_cast<uint32_t>(bgm.size()));
    Put32(archive, 0x150);
    Put32(archive, 0x30000040);
    Put32(archive, 0x210);
    Put32(archive, 0x40000010);
    archive.resize(0x60, 0);

    // INIT: one bank from 0x20, one song from 0x24, two other files from 0x2C.
    PutText(archive, "INIT");
    Put32(archive, 0x30);
    Put16(archive, 0x20);
    Put16(archive, 4);
    Put16(archive, 0x24);
    Put16(archive, 8);
    Put16(archive, 0x2C);
    Put16(archive, 4);
    archive.resize(0x80, 0);
    Put16(archive, 1);
    Append(archive, {2, 3});
    Put16(archive, 0);
    Put16(archive, 1);
    Put16(archive, 0);
    Put16(archive, 0);
    Put16(archive, 7);
    Put16(archive, 8);

    archive.resize(0x90, 0);
    archive.insert(archive.end(), bgm.begin(), bgm.end());
    archive.resize(0x150, 0);
    archive.insert(archive.end(), bank.begin(), bank.end());
    archive.resize(0x210, 0);
    PutText(archive, "PER ");
    Put32(archive, 0x10 + 12);
    archive.resize(0x220, 0);
    Put16(archive, 0x2001);
    Put16(archive, 7200);
    Append(archive, {50, 64, 0, 0, 0, 0, 0, 0});
    xsflib::StoreBe32(archive.data() + 4, static_cast<uint32_t>(archive.size()));

    Bytes image(0x100, 0xFF);
    const std::size_t place = image.size();
    image.insert(image.end(), archive.begin(), archive.end());
    XSFLIB_CHECK(n64::FindSbnArchive(image) == place);
    XSFLIB_CHECK(n64::FindSbnArchive(bgm) == bgm.size());

    const n64::SbnArchive parsed = n64::SbnArchive::Parse(archive);
    XSFLIB_CHECK(parsed.Entries().size() == 3);
    XSFLIB_CHECK(parsed.Entries()[1].format == n64::kSbnFormatBank && parsed.Entries()[1].size == 0x40);
    XSFLIB_CHECK(parsed.File(0).size() == bgm.size());
    XSFLIB_CHECK(std::equal(bank.begin(), bank.end(), parsed.File(1).begin(), parsed.File(1).end()));
    XSFLIB_CHECK(parsed.InitBanks().size() == 1 && parsed.InitBanks()[0].file == 1 &&
                 parsed.InitBanks()[0].bank_set == 3);
    XSFLIB_CHECK(parsed.Songs().size() == 1 && parsed.Songs()[0].banks[0] == 1);
    XSFLIB_CHECK(parsed.OtherFiles() == std::vector<uint16_t>({7, 8}));

    const std::vector<n64::BgmDrum> drums = n64::ParsePerFile(parsed.File(2));
    XSFLIB_CHECK(drums.size() == 1 && drums[0].key_base == 7200 && drums[0].pan == 64);
    XSFLIB_CHECK(!ErrorOf<xsflib::FormatError>([&] { n64::ParsePrgFile(parsed.File(2)); }).empty());
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("n64_sbn",
                                    []
                                    {
                                        TestBgmFile();
                                        TestBgmCommands();
                                        TestBgmWalk();
                                        TestBkFile();
                                        TestArchive();
                                    });
}
