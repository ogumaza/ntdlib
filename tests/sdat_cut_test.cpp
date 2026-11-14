// SPDX-License-Identifier: MIT

// Tests of the cut and the join of a DS sound archive (ntdlib/sdat/cut.h), on small synthetic archives. The expected
// places come from the rules in cut.h and from the layout of the SDK's tools: a file on a multiple of 32 bytes from the
// archive's start, and a sequence file or a sequence archive on a multiple of 4 bytes. A sequence archive's table
// gives each sequence's start as an offset from the bytecode's start, and 0xFFFFFFFF for a record without a sequence.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/commands/sequence.h"
#include "ntdlib/ntr/snd/commands.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/sdat/cut.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/testing/sdat_files.h"

namespace
{

namespace files = ntdlib::testing::sdat_files;
using Bytes = std::vector<uint8_t>;
using ntdlib::sar::Padding;
using ntdlib::sar::Piece;
using ntdlib::sar::SequencePiece;
using ntdlib::sdat::Cut;
using ntdlib::sdat::Join;
using ntdlib::sdat::Layout;
using xsflib::testing::ErrorOf;

// A sequence of one track that plays a note and ends: 7 bytes, which the file pads with 1 zero.
const Bytes kNote = {
    0xFE, 0x01, 0x00, // allocate track 0
    0x3C, 0x7F, 0x30, // note
    0xFF,             // end
};

// A sequence of one track that loops: 12 bytes, which the file pads with none.
const Bytes kLoop = {
    0xFE, 0x01, 0x00,       // 0x00: allocate track 0
    0xC0, 0x40,             // 0x03: pan 64
    0x3C, 0x7F, 0x30,       // 0x05: note
    0x94, 0x05, 0x00, 0x00, // 0x08: jump to 0x05
};

// An archive of a sequence file, another file and a second sequence file.
files::Archive Spec(const Bytes& first, const Bytes& second)
{
    files::Archive spec;
    spec.items[0] = {files::SequenceItem(0, 0, "SEQ_A"), files::SequenceItem(2, 0, "SEQ_B")};
    spec.files = {files::Sequence(first), Bytes(40, 0xAB), files::Sequence(second)};

    return spec;
}

ntdlib::commands::Sequence Decode(const Bytes& bytecode)
{
    const uint32_t entry = 0;
    return ntdlib::ntr::snd::DecodeSequence(bytecode, {}, std::span(&entry, 1)).sequence;
}

// Two sequences in one block of bytecode: 17 bytes. A sequence archive pads them with 3 zeros. The first sequence plays
// a note and ends. The second starts at 0x07 and loops.
const Bytes kShared = {
    0xFE, 0x01, 0x00,       // 0x00, item 0: allocate track 0
    0x3C, 0x7F, 0x30,       // 0x03, item 1: note
    0xFF,                   // 0x06, item 2: end
    0xFE, 0x01, 0x00,       // 0x07, item 3: allocate track 0
    0x3C, 0x7F, 0x30,       // 0x0A, item 4: note
    0x94, 0x0A, 0x00, 0x00, // 0x0D, item 5: jump to 0x0A
};

constexpr uint32_t kNoSequence = 0xFFFFFFFF;

// The table of a sequence archive of kShared: the first sequence, a record without a sequence, and the second sequence
// twice.
const std::vector<files::ArchivedSequence> kSharedTable = {
    {.offset = 0}, {.offset = kNoSequence}, {.offset = 7}, {.offset = 7, .volume = 100}};

// An archive of a sequence archive and a sequence file.
files::Archive ArchiveSpec(const std::vector<files::ArchivedSequence>& table, const Bytes& bytecode)
{
    files::Archive spec;
    spec.items[0] = {files::SequenceItem(1, 0, "SEQ_A")};
    spec.items[1] = {files::SequenceArchiveItem(0, "SEQARC_A", {"SE_A", "", "SE_B", "SE_C"})};
    spec.files = {files::SequenceArchive(table, bytecode), files::Sequence(kNote)};

    return spec;
}

// The sequences of `bytecode`, a sequence archive's bytecode, from the starts in `entries`.
ntdlib::ntr::snd::Decoded DecodeShared(const Bytes& bytecode, const std::vector<uint32_t>& entries)
{
    return ntdlib::ntr::snd::DecodeSequence(bytecode, {}, entries);
}

void TestRoundTrip()
{
    const Bytes archive = files::SoundArchive(Spec(kNote, kLoop));

    const Layout layout = Cut(archive);

    // A run up to each sequence's bytecode, the sequence, and its padding to 4 bytes. The first sequence file ends
    // 0x24 bytes in, and zeros bring the next file to 0x40: padding to 32 bytes. The other file is a run of its own of
    // 40 bytes, and zeros bring the next file to 0x40 again. The second sequence file ends the archive without such
    // padding.
    XSFLIB_CHECK(layout.sequences.size() == 2 && layout.sequences[0] == Decode(kNote));
    XSFLIB_CHECK(layout.sequences[1] == Decode(kLoop));
    XSFLIB_CHECK(layout.pieces.size() == 9);
    XSFLIB_CHECK(std::holds_alternative<Bytes>(layout.pieces[0]));
    XSFLIB_CHECK(std::get<SequencePiece>(layout.pieces[1]) == (SequencePiece{0, 0x0100, 0}));
    XSFLIB_CHECK(std::get<Padding>(layout.pieces[2]).alignment == 4);
    XSFLIB_CHECK(std::get<Padding>(layout.pieces[3]).alignment == 32);
    XSFLIB_CHECK(std::get<Bytes>(layout.pieces[4]) == Bytes(40, 0xAB));
    XSFLIB_CHECK(std::get<Padding>(layout.pieces[5]).alignment == 32);
    XSFLIB_CHECK(std::get<Bytes>(layout.pieces[6]).size() == 0x1C);
    XSFLIB_CHECK(std::get<SequencePiece>(layout.pieces[7]).sequence == 1);
    XSFLIB_CHECK(std::get<Padding>(layout.pieces[8]).alignment == 4);

    // The archive's size, the FILE block's two sizes, each file's offset and size, and two sizes in each sequence
    // file's header.
    XSFLIB_CHECK(layout.fields.size() == 3 + 3 * 2 + 2 * 2);
    for (const ntdlib::sar::Field& field : layout.fields)
    {
        // Each place names a byte of a run, or the start of a piece or of the archive's end.
        for (const ntdlib::sar::Place& place : {field.at, field.from, field.to})
        {
            const Bytes* run =
                place.piece < layout.pieces.size() ? std::get_if<Bytes>(&layout.pieces[place.piece]) : nullptr;
            XSFLIB_CHECK(!place.item && !place.sequence && (run ? place.offset < run->size() : place.offset == 0));
        }
    }
    XSFLIB_CHECK(Join(layout.sequences, layout.pieces, layout.fields) == archive);
}

void TestGrowth()
{
    const Bytes archive = files::SoundArchive(Spec(kNote, kLoop));
    const ntdlib::sdat::Archive before = ntdlib::sdat::Archive::Parse(archive);
    Layout layout = Cut(archive);

    // 16 volumes before the note: 32 more bytes. The first file then ends at 0x44 bytes, and the files after it move
    // by 32 bytes.
    Bytes grown(kNote.begin(), kNote.begin() + 3);
    for (int i = 0; i < 16; i++)
    {
        grown.insert(grown.end(), {0xC1, 0x64});
    }
    grown.insert(grown.end(), kNote.begin() + 3, kNote.end());
    layout.sequences[0] = Decode(grown);

    const Bytes joined = Join(layout.sequences, layout.pieces, layout.fields);

    const ntdlib::sdat::Archive after = ntdlib::sdat::Archive::Parse(joined);
    XSFLIB_CHECK(joined.size() == archive.size() + 32 && after.header.size == joined.size());
    XSFLIB_CHECK(after.files[0].offset == before.files[0].offset && after.files[0].size == 0x44);
    XSFLIB_CHECK(after.files[1].offset == before.files[1].offset + 32 && after.files[1].size == 40);
    XSFLIB_CHECK(after.files[2].offset == before.files[2].offset + 32);
    XSFLIB_CHECK(std::ranges::equal(after.File(1), Bytes(40, 0xAB)));
    XSFLIB_CHECK(std::ranges::equal(after.File(2), files::Sequence(kLoop)));
    XSFLIB_CHECK(std::ranges::equal(after.File(0), files::Sequence(grown)));
    const uint32_t file_block = xsflib::LoadLe32(&joined[0x28]);
    XSFLIB_CHECK(xsflib::LoadLe32(&joined[0x2C]) == joined.size() - file_block);
    XSFLIB_CHECK(xsflib::LoadLe32(&joined[file_block + 4]) == joined.size() - file_block);

    // Shrunk back, the sequence gives the archive back.
    layout.sequences[0] = Decode(kNote);
    XSFLIB_CHECK(Join(layout.sequences, layout.pieces, layout.fields) == archive);
}

void TestRuns()
{
    // A byte other than zero before the next file stays in a run, and the file takes no padding to 32 bytes.
    Bytes archive = files::SoundArchive(Spec(kNote, kLoop));
    const uint32_t fat = xsflib::LoadLe32(&archive[0x20]);
    archive[xsflib::LoadLe32(&archive[fat + 12]) + 0x24] = 0x01;
    Layout layout = Cut(archive);
    XSFLIB_CHECK(layout.sequences.size() == 2 && layout.pieces.size() == 9);
    XSFLIB_CHECK(std::get<Bytes>(layout.pieces[3]).size() == 0x1C && std::get<Bytes>(layout.pieces[3]).front() == 0x01);
    XSFLIB_CHECK(Join(layout.sequences, layout.pieces, layout.fields) == archive);

    // A file that another file's FAT entry overlaps stays in a run.
    archive = files::SoundArchive(Spec(kNote, kLoop));
    xsflib::StoreLe32(&archive[fat + 12 + 16 + 0], xsflib::LoadLe32(&archive[fat + 12]) + 0x10);
    layout = Cut(archive);
    XSFLIB_CHECK(layout.sequences.size() == 1 && layout.sequences[0] == Decode(kLoop));
    XSFLIB_CHECK(Join(layout.sequences, layout.pieces, layout.fields) == archive);

    // An archive without a sequence file holds its other file in a run of its own. These archives end at their last
    // file, without padding.
    files::Archive spec = Spec(kNote, kLoop);
    spec.files = {Bytes(40, 0xAB)};
    spec.items[0] = {};
    archive = files::SoundArchive(spec);
    layout = Cut(archive);
    XSFLIB_CHECK(layout.sequences.empty() && layout.pieces.size() == 2);
    XSFLIB_CHECK(std::get<Bytes>(layout.pieces[1]) == Bytes(40, 0xAB));
    XSFLIB_CHECK(Join(layout.sequences, layout.pieces, layout.fields) == archive);

    // An archive without a file is one run.
    spec.files = {};
    archive = files::SoundArchive(spec);
    layout = Cut(archive);
    XSFLIB_CHECK(layout.sequences.empty() && layout.pieces.size() == 1 && layout.fields.empty());
    XSFLIB_CHECK(std::get<Bytes>(layout.pieces[0]) == archive);
}

void TestReplaceFile()
{
    const Bytes archive = files::SoundArchive(Spec(kNote, kLoop));
    const ntdlib::sdat::Archive before = ntdlib::sdat::Archive::Parse(archive);
    const Layout layout = Cut(archive);
    const auto replaced_by = [&](const Bytes& bytes)
    {
        const std::vector<Piece> pieces =
            ntdlib::sdat::ReplaceFile(layout.sequences, layout.pieces, layout.fields, 1, bytes);
        return Join(layout.sequences, pieces, layout.fields);
    };
    const auto check_sizes = [](const Bytes& joined)
    {
        const uint32_t file_block = xsflib::LoadLe32(&joined[0x28]);
        XSFLIB_CHECK(xsflib::LoadLe32(&joined[8]) == joined.size());
        XSFLIB_CHECK(xsflib::LoadLe32(&joined[0x2C]) == joined.size() - file_block);
        XSFLIB_CHECK(xsflib::LoadLe32(&joined[file_block + 4]) == joined.size() - file_block);
    };

    // 40 bytes take 0x40 bytes of the archive. 100 take 0x80, and the file after them moves by 0x40 bytes.
    const Bytes grown(100, 0xCD);
    Bytes joined = replaced_by(grown);
    ntdlib::sdat::Archive after = ntdlib::sdat::Archive::Parse(joined);
    XSFLIB_CHECK(joined.size() == archive.size() + 0x40);
    check_sizes(joined);
    XSFLIB_CHECK(after.files[0].offset == before.files[0].offset && after.files[0].size == before.files[0].size);
    XSFLIB_CHECK(after.files[1].offset == before.files[1].offset && after.files[1].size == 100);
    XSFLIB_CHECK(after.files[2].offset == before.files[2].offset + 0x40 && after.files[2].size == before.files[2].size);
    XSFLIB_CHECK(std::ranges::equal(after.File(0), before.File(0)));
    XSFLIB_CHECK(std::ranges::equal(after.File(1), grown));
    XSFLIB_CHECK(std::ranges::equal(after.File(2), before.File(2)));

    // The cut of the new archive gives the sequences again, and a further replacement.
    const Layout again = Cut(joined);
    XSFLIB_CHECK(again.sequences == layout.sequences && again.pieces.size() == layout.pieces.size());
    const std::vector<Piece> back =
        ntdlib::sdat::ReplaceFile(again.sequences, again.pieces, again.fields, 1, Bytes(40, 0xAB));
    XSFLIB_CHECK(Join(again.sequences, back, again.fields) == archive);

    // 32 bytes take 0x20 bytes of the archive, and the file after them moves back by 0x20 bytes.
    joined = replaced_by(Bytes(32, 0xEF));
    after = ntdlib::sdat::Archive::Parse(joined);
    XSFLIB_CHECK(joined.size() == archive.size() - 0x20 && after.files[1].size == 32);
    XSFLIB_CHECK(after.files[2].offset == before.files[2].offset - 0x20);
    check_sizes(joined);

    // The same bytes give the archive back.
    XSFLIB_CHECK(replaced_by(Bytes(40, 0xAB)) == archive);

    // The archive's last file: the archive ends at its new end.
    files::Archive last = Spec(kNote, kLoop);
    last.files = {files::Sequence(kNote), files::Sequence(kLoop), Bytes(40, 0xAB)};
    last.items[0] = {files::SequenceItem(0, 0, "SEQ_A"), files::SequenceItem(1, 0, "SEQ_B")};
    const Bytes last_archive = files::SoundArchive(last);
    const Layout last_layout = Cut(last_archive);
    const std::vector<Piece> last_pieces =
        ntdlib::sdat::ReplaceFile(last_layout.sequences, last_layout.pieces, last_layout.fields, 2, Bytes(70, 0x11));
    joined = Join(last_layout.sequences, last_pieces, last_layout.fields);
    XSFLIB_CHECK(joined.size() == last_archive.size() + 30);
    check_sizes(joined);
    XSFLIB_CHECK(std::ranges::equal(ntdlib::sdat::Archive::Parse(joined).File(2), Bytes(70, 0x11)));

    // Padding at the archive's end stays, and keeps the archive on a multiple of 32 bytes.
    Bytes padded = last_archive;
    padded.resize((padded.size() + 31) / 32 * 32);
    xsflib::StoreLe32(&padded[8], uint32_t(padded.size()));
    const uint32_t padded_block = xsflib::LoadLe32(&padded[0x28]);
    xsflib::StoreLe32(&padded[0x2C], uint32_t(padded.size() - padded_block));
    xsflib::StoreLe32(&padded[padded_block + 4], uint32_t(padded.size() - padded_block));
    const Layout padded_layout = Cut(padded);
    XSFLIB_CHECK(std::get<Padding>(padded_layout.pieces.back()).alignment == 32);
    joined = Join(padded_layout.sequences,
                  ntdlib::sdat::ReplaceFile(padded_layout.sequences, padded_layout.pieces, padded_layout.fields, 2,
                                            Bytes(70, 0x11)),
                  padded_layout.fields);
    XSFLIB_CHECK(joined.size() % 32 == 0);
    check_sizes(joined);

    // A file that ends on a multiple of 32 bytes keeps what follows on the next multiple when it takes a new size.
    files::Archive exact_spec = Spec(kNote, kLoop);
    exact_spec.files[1] = Bytes(0x40, 0xAB);
    const Bytes exact = files::SoundArchive(exact_spec);
    const Layout exact_layout = Cut(exact);
    XSFLIB_CHECK(Join(exact_layout.sequences, exact_layout.pieces, exact_layout.fields) == exact);
    joined = Join(exact_layout.sequences,
                  ntdlib::sdat::ReplaceFile(exact_layout.sequences, exact_layout.pieces, exact_layout.fields, 1,
                                            Bytes(0x46, 0x22)),
                  exact_layout.fields);
    const ntdlib::sdat::Archive exact_before = ntdlib::sdat::Archive::Parse(exact);
    const ntdlib::sdat::Archive exact_after = ntdlib::sdat::Archive::Parse(joined);
    XSFLIB_CHECK(exact_after.files[2].offset == exact_before.files[2].offset + 0x20);
    XSFLIB_CHECK(std::ranges::equal(exact_after.File(1), Bytes(0x46, 0x22)));
    XSFLIB_CHECK(std::ranges::equal(exact_after.File(2), exact_before.File(2)));

    // Refusals: a file that the archive lacks, a sequence file, and a file that another file's entry overlaps.
    const auto refusal = [&](const Layout& of, uint32_t file)
    {
        return ErrorOf<std::invalid_argument>(
            [&] { ntdlib::sdat::ReplaceFile(of.sequences, of.pieces, of.fields, file, Bytes(4, 0)); });
    };
    XSFLIB_CHECK(refusal(layout, 3) == "the archive holds no file 3");
    XSFLIB_CHECK(refusal(layout, 0) == "file 0 isn't a run of its own");
    Bytes overlapped = archive;
    const uint32_t fat = xsflib::LoadLe32(&overlapped[0x20]);
    xsflib::StoreLe32(&overlapped[fat + 12 + 16 + 4], 0x48);
    XSFLIB_CHECK(refusal(Cut(overlapped), 1) == "file 1 isn't a run of its own");

    // File 1's entry reaches the archive's end, over file 2: the run holds both. Fewer bytes leave file 2's place past
    // the run's end, and as many change file 2.
    xsflib::StoreLe32(&overlapped[fat + 12 + 16 + 4], uint32_t(archive.size() - before.files[1].offset));
    const Layout over = Cut(overlapped);
    XSFLIB_CHECK(refusal(over, 1) == "a field names a byte past its run");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&]
                     {
                         ntdlib::sdat::ReplaceFile(over.sequences, over.pieces, over.fields, 1,
                                                   Bytes(archive.size() - before.files[1].offset, 0x22));
                     }) == "file 2 shares bytes with file 1");
}

void TestReadSequenceFile()
{
    XSFLIB_CHECK(ntdlib::sdat::ReadSequenceFile(files::Sequence(kNote)) == Decode(kNote));
    XSFLIB_CHECK(ntdlib::sdat::ReadSequenceFile(files::Sequence(kLoop)) == Decode(kLoop));

    // Other bytes, a header that gives another size, and bytecode that the decoder keeps as data.
    XSFLIB_CHECK(!ntdlib::sdat::ReadSequenceFile(Bytes(40, 0xAB)));
    Bytes cut = files::Sequence(kLoop);
    cut.resize(cut.size() - 4);
    XSFLIB_CHECK(!ntdlib::sdat::ReadSequenceFile(cut));
}

void TestFileBlockHeader()
{
    // The FILE block moves up to 12 bytes before the first file, as in a game's archive: its magic word, its size and
    // the number of files. The first file then lies right after those bytes, and becomes a sequence piece.
    files::Archive spec = Spec(kNote, kLoop);
    Bytes archive = files::SoundArchive(spec);
    const uint32_t old_block = xsflib::LoadLe32(&archive[0x28]);
    const uint32_t fat = xsflib::LoadLe32(&archive[0x20]);
    const uint32_t first = xsflib::LoadLe32(&archive[fat + 12]);
    const uint32_t block = first - 12;
    XSFLIB_CHECK(block > old_block);
    const Bytes header(archive.begin() + old_block, archive.begin() + old_block + 12);
    std::fill(archive.begin() + old_block, archive.begin() + first, uint8_t(0));
    std::copy(header.begin(), header.end(), archive.begin() + block);
    xsflib::StoreLe32(&archive[0x28], block);
    xsflib::StoreLe32(&archive[0x2C], uint32_t(archive.size() - block));
    xsflib::StoreLe32(&archive[block + 4], uint32_t(archive.size() - block));

    const Layout layout = Cut(archive);
    XSFLIB_CHECK(layout.sequences.size() == 2 && layout.sequences[0] == Decode(kNote));
    XSFLIB_CHECK(std::get<Bytes>(layout.pieces[0]).size() == first + 0x1C);
    XSFLIB_CHECK(Join(layout.sequences, layout.pieces, layout.fields) == archive);
}

void TestSequenceArchive()
{
    const Bytes archive = files::SoundArchive(ArchiveSpec(kSharedTable, kShared));
    const uint32_t ssar = ntdlib::sdat::Archive::Parse(archive).files[0].offset;

    const Layout layout = Cut(archive);

    // A run up to the sequence archive's bytecode, past its headers and its table of 4 records (0x50 bytes), the
    // bytecode's sequence from the starts of its 3 sequences, and its padding to 4 bytes. The file ends 0x64 bytes in,
    // and zeros bring the sequence file to 0x80: padding to 32 bytes. Then the sequence file's pieces.
    XSFLIB_CHECK(layout.sequences.size() == 2 && layout.sequences[0] == DecodeShared(kShared, {0, 7, 7}).sequence);
    XSFLIB_CHECK(layout.sequences[1] == Decode(kNote));
    XSFLIB_CHECK(layout.pieces.size() == 7);
    XSFLIB_CHECK(std::get<Bytes>(layout.pieces[0]).size() == ssar + 0x50);
    XSFLIB_CHECK(std::get<SequencePiece>(layout.pieces[1]) == (SequencePiece{0, 0x0100, 0}));
    XSFLIB_CHECK(std::get<Padding>(layout.pieces[2]).alignment == 4);
    XSFLIB_CHECK(std::get<Padding>(layout.pieces[3]).alignment == 32);
    XSFLIB_CHECK(std::get<Bytes>(layout.pieces[4]).size() == 0x1C);
    XSFLIB_CHECK(std::get<SequencePiece>(layout.pieces[5]).sequence == 1);
    XSFLIB_CHECK(std::get<Padding>(layout.pieces[6]).alignment == 4);

    // The fields of TestRoundTrip's kinds for two files, and the start of each record with a sequence: the distance
    // from the bytecode's first item to item 0, 3 or 3. The record without a sequence keeps its 0xFFFFFFFF in the run.
    XSFLIB_CHECK(layout.fields.size() == 3 + 2 * 2 + 2 * 2 + 3);
    std::vector<ntdlib::sar::Field> starts;
    std::ranges::copy_if(layout.fields, std::back_inserter(starts),
                         [](const ntdlib::sar::Field& field) { return field.from.item; });
    const auto start = [&](uint32_t record, uint32_t item)
    {
        return ntdlib::sar::Field{{0, ssar + 0x20 + 12 * record}, {1, 0, true}, {1, item, true}};
    };
    XSFLIB_CHECK(starts == (std::vector{start(0, 0), start(2, 3), start(3, 3)}));
    XSFLIB_CHECK(Join(layout.sequences, layout.pieces, layout.fields) == archive);
}

void TestSequenceArchiveGrowth()
{
    const Bytes archive = files::SoundArchive(ArchiveSpec(kSharedTable, kShared));
    const ntdlib::sdat::Archive before = ntdlib::sdat::Archive::Parse(archive);
    Layout layout = Cut(archive);

    // An editor puts 16 volumes before the first sequence's end. They take 32 more bytes. The items from item 2 on move
    // by 16. Each target, and each field that names such an item, moves with them.
    const ntdlib::commands::Command volume = Decode({0xFE, 0x01, 0x00, 0xC1, 0x64, 0xFF}).items[1];
    std::vector<ntdlib::commands::Command>& items = layout.sequences[0].items;
    items.insert(items.begin() + 2, 16, volume);
    for (ntdlib::commands::Command& command : items)
    {
        if (ntdlib::commands::HasTarget(command.kind) && command.target >= 2)
        {
            command.target += 16;
        }
    }
    for (ntdlib::sar::Field& field : layout.fields)
    {
        if (field.to.item && field.to.offset >= 2)
        {
            field.to.offset += 16;
        }
    }

    const Bytes joined = Join(layout.sequences, layout.pieces, layout.fields);

    // The second sequence now starts at 0x27, and its jump goes to 0x2A. The sequence archive is the one that the
    // tools write from that bytecode and table: 0x84 bytes. The sequence file after it moves from 0x80 to 0xA0.
    Bytes grown(kShared.begin(), kShared.begin() + 6);
    for (int i = 0; i < 16; i++)
    {
        grown.insert(grown.end(), {0xC1, 0x64});
    }
    grown.insert(grown.end(), {0xFF, 0xFE, 0x01, 0x00, 0x3C, 0x7F, 0x30, 0x94, 0x2A, 0x00, 0x00});
    std::vector<files::ArchivedSequence> table = kSharedTable;
    table[2].offset = 0x27;
    table[3].offset = 0x27;
    const ntdlib::sdat::Archive after = ntdlib::sdat::Archive::Parse(joined);
    XSFLIB_CHECK(joined.size() == archive.size() + 32 && after.header.size == joined.size());
    XSFLIB_CHECK(after.files[0].offset == before.files[0].offset && after.files[0].size == 0x84);
    XSFLIB_CHECK(std::ranges::equal(after.File(0), files::SequenceArchive(table, grown)));
    XSFLIB_CHECK(after.files[1].offset == before.files[1].offset + 32);
    XSFLIB_CHECK(std::ranges::equal(after.File(1), files::Sequence(kNote)));

    // The cut of the new archive gives the same sequence, and the old one gives the archive back.
    XSFLIB_CHECK(Cut(joined).sequences[0] == layout.sequences[0]);
    const Layout old = Cut(archive);
    XSFLIB_CHECK(Join(old.sequences, old.pieces, old.fields) == archive);
}

void TestSequenceArchiveRuns()
{
    // A start past the bytecode, a table without a sequence, and a bytecode's offset that the tools don't write: the
    // sequence archive stays a run of its own, and the sequence file after it still becomes a sequence.
    std::vector<files::ArchivedSequence> past = kSharedTable;
    past[2].offset = 0x40;
    Bytes moved = files::SequenceArchive(kSharedTable, kShared);
    xsflib::StoreLe32(&moved[0x18], 0x54);
    files::Archive moved_spec = ArchiveSpec(kSharedTable, kShared);
    moved_spec.files[0] = moved;
    for (const Bytes& archive :
         {files::SoundArchive(ArchiveSpec(past, kShared)),
          files::SoundArchive(ArchiveSpec({{.offset = kNoSequence}}, kShared)), files::SoundArchive(moved_spec)})
    {
        const Layout layout = Cut(archive);
        XSFLIB_CHECK(layout.sequences.size() == 1 && layout.sequences[0] == Decode(kNote));
        XSFLIB_CHECK(std::get<Bytes>(layout.pieces[1]).size() == ntdlib::sdat::Archive::Parse(archive).files[0].size);
        XSFLIB_CHECK(Join(layout.sequences, layout.pieces, layout.fields) == archive);
    }

    // A sequence archive that becomes a sequence piece isn't a run of its own.
    const Layout layout = Cut(files::SoundArchive(ArchiveSpec(kSharedTable, kShared)));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&]
                     { ntdlib::sdat::ReplaceFile(layout.sequences, layout.pieces, layout.fields, 0, Bytes(4, 0)); }) ==
                 "file 0 isn't a run of its own");
}

void TestRefusals()
{
    const Bytes archive = files::SoundArchive(Spec(kNote, kLoop));
    const Layout layout = Cut(archive);

    std::vector<Piece> pieces = layout.pieces;
    pieces[1] = SequencePiece{7, 0x0100, 0};
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { Join(layout.sequences, pieces, layout.fields); }) ==
                 "a sequence piece names a sequence that the layout lacks");

    std::vector<ntdlib::sar::Field> fields = layout.fields;
    fields[0].at = {1, 0};
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { Join(layout.sequences, layout.pieces, fields); }) ==
                 "a field doesn't lie in a run of a DS sound archive");

    fields = layout.fields;
    fields[0].to = {1, 4};
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { Join(layout.sequences, layout.pieces, fields); }) ==
                 "a field names a byte inside a sequence or padding");

    fields = layout.fields;
    fields[0].from = {uint32_t(layout.pieces.size() + 1), 0};
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { Join(layout.sequences, layout.pieces, fields); }) ==
                 "a field names a place that the archive's pieces lack");

    fields = layout.fields;
    std::swap(fields[0].from, fields[0].to);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { Join(layout.sequences, layout.pieces, fields); }) ==
                 "a field's distance doesn't fit in 32 bits");

    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { Cut(Bytes(64, 0)); }).size() > 0);

    // Places that name items: of a run, past the sequence's items and the end after them, beside a byte, and of a
    // sequence without a piece.
    const Layout shared = Cut(files::SoundArchive(ArchiveSpec(kSharedTable, kShared)));
    const auto start =
        std::ranges::find_if(shared.fields, [](const ntdlib::sar::Field& field) { return field.to.item; });
    XSFLIB_CHECK(start != shared.fields.end());
    const auto refusal = [&](const ntdlib::sar::Place& from, const ntdlib::sar::Place& to)
    {
        std::vector<ntdlib::sar::Field> item_fields = shared.fields;
        item_fields[std::size_t(start - shared.fields.begin())].from = from;
        item_fields[std::size_t(start - shared.fields.begin())].to = to;
        return ErrorOf<std::invalid_argument>([&] { Join(shared.sequences, shared.pieces, item_fields); });
    };
    XSFLIB_CHECK(refusal({0, 0, true}, {0, 1, true}) == "a field names an item outside a sequence piece");
    XSFLIB_CHECK(refusal({1, 0, true}, {1, 7, true}) == "a field names an item past its sequence");
    XSFLIB_CHECK(refusal({1, 0, true}, {1, 6, true}).empty());
    XSFLIB_CHECK(refusal({1, 0}, {1, 3, true}) == "a field names an item of a sequence and another place");
    XSFLIB_CHECK(refusal({1, 0, true}, {5, 0, true}) == "a field names an item of a sequence and another place");
    XSFLIB_CHECK(refusal({0, 0, false, true}, {0, 3, false, true}) ==
                 "a field of a DS sound archive names a sequence without a piece");
}

} // namespace

int main()
{
    TestRoundTrip();
    TestGrowth();
    TestRuns();
    TestReplaceFile();
    TestReadSequenceFile();
    TestFileBlockHeader();
    TestSequenceArchive();
    TestSequenceArchiveGrowth();
    TestSequenceArchiveRuns();
    TestRefusals();

    return xsflib::testing::Summary("DS sound archive's cut");
}
