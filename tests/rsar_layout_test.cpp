// SPDX-License-Identifier: MIT

// Tests of the Wii's sound archives in pieces (ntdlib/rsar/layout.h) and of their sequence files' reader and writer
// (ntdlib/rsar/sequence_file.h), on archives laid out as NintendoWare for Revolution's tools lay them out
// (ntdlib/testing/rsar_tools.h).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/layout.h"
#include "ntdlib/rsar/sequence_file.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/sound_build.h"
#include "ntdlib/testing/rsar_tools.h"

namespace
{

namespace rsar_tools = ntdlib::testing::rsar_tools;
using ntdlib::SndBuild;
using ntdlib::commands::kOne;
using ntdlib::rsar::Layout;
using ntdlib::rsar::SequenceFile;
using ntdlib::sar::Field;
using ntdlib::sar::Piece;
using ntdlib::sar::SequencePiece;
using Bytes = std::vector<uint8_t>;

// The build of 2007 and the build of June 2008, whose sequences take time prefixes.
constexpr SndBuild kBuild2007{.date = 20070608};
constexpr SndBuild kBuild2008{.date = 20080620};

// A sequence file with `bytecode` and labels at the offsets given.
Bytes Sequence(const Bytes& bytecode, const std::vector<std::pair<std::string, uint32_t>>& labels)
{
    SequenceFile file;
    file.data = bytecode;
    for (const auto& [name, offset] : labels)
    {
        file.labels.push_back({name, offset});
    }

    return file.Write();
}

// The first song: a volume, a note, a wait and the end. The second sound starts at the wait.
const Bytes kSong = {0xC1, 0x64, 0x3C, 0x64, 0x30, 0x80, 0x30, 0xFF};

// The second song: forty notes, each with a wait.
Bytes LongSong()
{
    Bytes song;
    for (int i = 0; i < 40; ++i)
    {
        song.insert(song.end(), {0x3C, 0x50, 0x18, 0x80, 0x18});
    }
    song.push_back(0xFF);

    return song;
}

// Bytes of a file that isn't a sequence, such as a bank, or its wave data: `size` bytes that count up from `first`.
Bytes Other(std::size_t size, uint8_t first)
{
    Bytes bytes(size);
    for (std::size_t i = 0; i < size; ++i)
    {
        bytes[i] = static_cast<uint8_t>(first + i);
    }

    return bytes;
}

// An archive of version `version` with the two songs, a bank with its wave data, and three groups: the first song and
// the bank, then the second song and a copy of the first, then the bank again. The second song can take other bytes.
Bytes Archive(uint16_t version = 0x0104, const Bytes& second = Sequence(LongSong(), {{"SEQ_B", 0}}))
{
    const std::vector<rsar_tools::File> files = {
        {Sequence(kSong, {{"SEQ_A", 0}, {"SEQ_A_WAIT", 5}})},
        {second},
        {Other(0x60, 1), Other(0x40, 7)},
    };
    const std::vector<rsar_tools::SequenceSound> sounds = {{"SEQ_A", 0, 0}, {"SEQ_A_WAIT", 0, 5}, {"SEQ_B", 1, 0}};

    return rsar_tools::MakeArchive(version, files, sounds, {{0, 2}, {1, 0}, {2}});
}

ntdlib::rsar::SoundArchive Read(const Bytes& archive)
{
    return ntdlib::rsar::SoundArchive::Load(archive);
}

Bytes FileOf(const ntdlib::rsar::SoundArchive& archive, uint32_t id)
{
    const std::span<const uint8_t> data = archive.FileData(id);

    return {data.begin(), data.end()};
}

Bytes WaveDataOf(const ntdlib::rsar::SoundArchive& archive, uint32_t id)
{
    const std::span<const uint8_t> data = archive.WaveData(id);

    return {data.begin(), data.end()};
}

std::vector<uint32_t> SequencesOfPieces(const Layout& layout)
{
    std::vector<uint32_t> sequences;
    for (const Piece& piece : layout.pieces)
    {
        if (const auto* file = std::get_if<SequencePiece>(&piece))
        {
            sequences.push_back(file->sequence);
        }
    }

    return sequences;
}

Bytes Join(const Layout& layout, const SndBuild& build = kBuild2008)
{
    return ntdlib::rsar::Join(build, layout.sequences, layout.pieces, layout.fields);
}

// Inserts `count` copies of item `copied` of sequence `sequence` before item `at`. Moves the labels and the places at
// or after it the way an editor moves them.
void Insert(Layout& layout, uint32_t sequence, uint32_t at, uint32_t count, uint32_t copied)
{
    std::vector<ntdlib::commands::Command>& items = layout.sequences[sequence].items;
    const ntdlib::commands::Command command = items[copied];
    items.insert(items.begin() + at, count, command);
    for (ntdlib::commands::Label& label : layout.sequences[sequence].labels)
    {
        label.item += label.item >= at ? count : 0;
    }

    for (Field& field : layout.fields)
    {
        for (ntdlib::sar::Place* place : {&field.from, &field.to})
        {
            const auto* piece = place->item ? std::get_if<SequencePiece>(&layout.pieces[place->piece]) : nullptr;
            if (piece != nullptr && piece->sequence == sequence && place->offset >= at)
            {
                place->offset += count;
            }
        }
    }
}

void TestSequenceFile()
{
    // Labels whose names pad to 4 bytes in three ways. Each block pads to 32 bytes.
    const Bytes file = Sequence(kSong, {{"Song", 0}, {"Track_1", 5}, {"Another_label", 2}});
    XSFLIB_CHECK(file.size() == 0x20 + 0x20 + 0x60);
    XSFLIB_CHECK(xsflib::LoadBe32(&file[0x08]) == file.size() && xsflib::LoadBe32(&file[0x2C - 4]) == 0xC);
    const SequenceFile s = SequenceFile::Read(file);
    XSFLIB_CHECK(s.version == 0x0100 && s.data.size() == 0x14);
    XSFLIB_CHECK(s.labels.size() == 3 && s.labels[1].name == "Track_1" && s.labels[1].offset == 5);
    XSFLIB_CHECK(s.labels[2].name == "Another_label" && s.labels[2].offset == 2);
    XSFLIB_CHECK(s.Write() == file);

    // NW4R's reader reads the same bytecode and labels.
    const ntdlib::rsar::Sequence read = ntdlib::rsar::Sequence::Parse(file, {.playback_fields = true});
    XSFLIB_CHECK(read.data.size() == 0x14 && read.labels.size() == 3 && read.labels[2].offset == 2);

    // A file without LABL, and one of another console, are refused.
    Bytes no_labels = file;
    xsflib::StoreBe16(&no_labels[0x0E], 1);
    XSFLIB_CHECK(!xsflib::testing::ErrorOf<xsflib::FormatError>([&] { SequenceFile::Read(no_labels); }).empty());
    Bytes other = file;
    other[0] = 'C';
    XSFLIB_CHECK(!xsflib::testing::ErrorOf<xsflib::FormatError>([&] { SequenceFile::Read(other); }).empty());
}

void TestCut()
{
    for (const uint16_t version : {uint16_t{0x0101}, uint16_t{0x0104}})
    {
        const Bytes archive = Archive(version);
        const Layout layout = ntdlib::rsar::Cut(archive, kBuild2008);

        // Both songs decode, and the copy of the first song is a piece of the same sequence.
        XSFLIB_CHECK(layout.sequences.size() == 2);
        XSFLIB_CHECK((SequencesOfPieces(layout) == std::vector<uint32_t>{0, 1, 0}));
        XSFLIB_CHECK(Join(layout) == archive);
        XSFLIB_CHECK(layout.sequences[0].labels.size() == 2 && layout.sequences[0].labels[1].name == "SEQ_A_WAIT");
    }
}

void TestEdits()
{
    const Bytes archive = Archive();
    const ntdlib::rsar::SoundArchive tables = Read(archive);
    const Layout layout = ntdlib::rsar::Cut(archive, kBuild2008);

    // A new volume keeps the file's size.
    Layout louder = layout;
    louder.sequences[0].items[0].value.value = 120 * kOne;
    const Bytes loud = Join(louder);
    XSFLIB_CHECK(loud.size() == archive.size() && loud != archive);
    XSFLIB_CHECK(ntdlib::rsar::Cut(loud, kBuild2008).sequences[0].items[0].value.value == 120 * kOne);

    // Ten more volumes before the wait make both copies of the first file 32 bytes larger. The files after them move,
    // and the second sound's start follows the wait.
    Layout more = layout;
    Insert(more, 0, 2, 10, 0);
    const Bytes grown = Join(more);
    XSFLIB_CHECK(grown.size() == archive.size() + 2 * 32);
    const ntdlib::rsar::SoundArchive grown_tables = Read(grown);
    XSFLIB_CHECK(!grown_tables.Truncated());
    XSFLIB_CHECK(FileOf(grown_tables, 0).size() == FileOf(tables, 0).size() + 32);
    XSFLIB_CHECK(FileOf(grown_tables, 1) == FileOf(tables, 1) && FileOf(grown_tables, 2) == FileOf(tables, 2));
    XSFLIB_CHECK(WaveDataOf(grown_tables, 2) == WaveDataOf(tables, 2));
    XSFLIB_CHECK(grown_tables.Sounds()[1].start_offset == 25 && grown_tables.Sounds()[0].start_offset == 0);
    XSFLIB_CHECK(grown_tables.Sounds()[2].start_offset == 0);
    const Layout cut = ntdlib::rsar::Cut(grown, kBuild2008);
    XSFLIB_CHECK(cut.sequences == more.sequences && cut.fields == more.fields);

    // Each group and each item hold their new offsets and sizes, as the tools lay the grown archive out.
    const Bytes laid = rsar_tools::MakeArchive(
        0x0104, {{FileOf(grown_tables, 0)}, {FileOf(tables, 1)}, {FileOf(tables, 2), WaveDataOf(tables, 2)}},
        {{"SEQ_A", 0, 0}, {"SEQ_A_WAIT", 0, 25}, {"SEQ_B", 1, 0}}, {{0, 2}, {1, 0}, {2}});
    XSFLIB_CHECK(grown == laid);

    // Without most of its notes, the second song's file shrinks, and so does the archive.
    Layout fewer = layout;
    std::vector<ntdlib::commands::Command>& items = fewer.sequences[1].items;
    items.erase(items.begin() + 2, items.begin() + 80);
    const Bytes shrunk = Join(fewer);
    const Bytes short_song = Sequence({0x3C, 0x50, 0x18, 0x80, 0x18, 0xFF}, {{"SEQ_B", 0}});
    XSFLIB_CHECK(shrunk == Archive(0x0104, short_song));
}

void TestAlignment()
{
    // Every file of this archive starts at a multiple of 64 bytes. NW4R's tools align the files to 32 bytes, and so
    // does a file that grows.
    const auto archive = [](const Bytes& song)
    {
        return rsar_tools::MakeArchive(0x0104, {{song}, {Other(0x40, 3), Other(0x20, 9)}}, {{"SEQ_SONG", 0, 0}},
                                       {{0, 1}});
    };
    const Bytes before = archive(Sequence(kSong, {{"SEQ_SONG", 0}}));
    Layout layout = ntdlib::rsar::Cut(before, kBuild2008);
    Insert(layout, 0, 1, 20, 0);
    Bytes longer = kSong;
    for (int i = 0; i < 20; ++i)
    {
        longer.insert(longer.begin() + 2, {0xC1, 0x64});
    }
    const Bytes after = Join(layout);
    XSFLIB_CHECK(after.size() == before.size() + 32 && after == archive(Sequence(longer, {{"SEQ_SONG", 0}})));
}

void TestStarts()
{
    Layout layout = ntdlib::rsar::Cut(Archive(), kBuild2008);
    Insert(layout, 0, 2, 10, 0);
    const Bytes archive = Join(layout);
    const std::vector<std::size_t> starts = ntdlib::rsar::Starts(archive, layout.pieces);

    // Each sequence piece starts with its file. Each run holds its bytes, but for the fields that Join writes.
    XSFLIB_CHECK(starts.size() == layout.pieces.size() + 1 && starts.back() == archive.size());
    std::size_t sequences = 0;
    for (std::size_t i = 0; i + 1 < starts.size(); ++i)
    {
        if (std::holds_alternative<SequencePiece>(layout.pieces[i]))
        {
            XSFLIB_CHECK(std::memcmp(&archive[starts[i]], "RSEQ", 4) == 0);
            sequences += 1;
        }
        else if (const auto* run = std::get_if<Bytes>(&layout.pieces[i]))
        {
            XSFLIB_CHECK(starts[i + 1] - starts[i] == run->size());
        }
    }
    XSFLIB_CHECK(sequences == 3);

    // The tables hold each sequence sound's start.
    const std::vector<std::optional<std::size_t>> fields = ntdlib::rsar::StartFields(archive);
    XSFLIB_CHECK(fields.size() == 3 && fields[1].has_value());
    if (fields.size() == 3 && fields[1])
    {
        XSFLIB_CHECK(xsflib::LoadBe32(&archive[*fields[1]]) == 25);
    }
}

void TestStaysInBytes()
{
    // A file with bytes that no command reaches stays in the bytes, and so does its copy. The archive comes back.
    const Bytes odd = Sequence({0xFF, 0x12, 0x34}, {{"SEQ_B", 0}});
    const Bytes archive = Archive(0x0104, odd);
    const Layout layout = ntdlib::rsar::Cut(archive, kBuild2008);
    XSFLIB_CHECK(layout.sequences.size() == 1 && (SequencesOfPieces(layout) == std::vector<uint32_t>{0, 0}));
    XSFLIB_CHECK(Join(layout) == archive);

    // An archive cut short comes back too.
    const Bytes whole = Archive();
    const Bytes cut_short(whole.begin(), whole.end() - 0x50);
    const Layout short_layout = ntdlib::rsar::Cut(cut_short, kBuild2008);
    XSFLIB_CHECK(Join(short_layout) == cut_short);

    // So does an archive whose tables the cutter can't follow, as one run: its header places FILE past its end.
    Bytes damaged = whole;
    xsflib::StoreBe32(&damaged[0x20], 0xFFFFFF00);
    const Layout one = ntdlib::rsar::Cut(damaged, kBuild2008);
    XSFLIB_CHECK(one.pieces.size() == 1 && Join(one) == damaged);
}

void TestBuilds()
{
    // A time prefix (0xA3) before a volume: a command of the builds from March 2008 on. The build of 2007 reads the
    // song as data, and keeps the file in the bytes. Each build gives the archive back.
    const Bytes timed = Sequence({0xA3, 0xC1, 0x64, 0x00, 0x30, 0xFF}, {{"SEQ_B", 0}});
    const Bytes archive = Archive(0x0104, timed);
    const Layout late = ntdlib::rsar::Cut(archive, kBuild2008);
    XSFLIB_CHECK(late.sequences.size() == 2 && Join(late) == archive);
    XSFLIB_CHECK(late.sequences.size() == 2 && late.sequences[1].items[0].time.has_value());
    const Layout early = ntdlib::rsar::Cut(archive, kBuild2007);
    XSFLIB_CHECK(Join(early, kBuild2007) == archive);
    for (const SndBuild& build : {SndBuild{.october_2006 = true}, kBuild2007, SndBuild{.date = 20080308}, kBuild2008,
                                  SndBuild{.date = 20090924}})
    {
        XSFLIB_CHECK(Join(ntdlib::rsar::Cut(Archive(), build), build) == Archive());
    }
}

} // namespace

int main()
{
    TestSequenceFile();
    TestCut();
    TestEdits();
    TestAlignment();
    TestStarts();
    TestStaysInBytes();
    TestBuilds();

    return xsflib::testing::Summary("RSAR layout");
}
