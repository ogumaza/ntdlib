// SPDX-License-Identifier: MIT

// Tests of the cut and the join of a DS sound archive (ntdlib/sdat/cut.h), on small synthetic archives. The expected
// places come from the rules in cut.h and from the layout of the SDK's tools: a file on a multiple of 32 bytes from the
// archive's start, and a sequence file on a multiple of 4 bytes.

#include <algorithm>
#include <cstddef>
#include <cstdint>
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
    TestRefusals();

    return xsflib::testing::Summary("DS sound archive's cut");
}
