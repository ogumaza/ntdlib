// SPDX-License-Identifier: MIT

// Tests of the cutting of sound archives into pieces and their joining (ntdlib/sar/layout.h), on archives built in each
// flavor's format: the 3DS's, the Wii U's and the Switch's. Each archive holds two sequence files, a file of other
// bytes, a sequence file laid out otherwise than the writer lays it out, and a group. The group holds a copy of a file
// that only it holds, and a copy of one of the archive's sequence files. Another archive holds a sequence file with
// bytes that no command reaches. A third archive's tables take new values. A fourth holds a wave sound beside a
// sequence sound, for the fields of the sounds' starts. A fifth holds sounds with some of the options and without
// them, for the places of the values. The files that stay in the bytes become runs of their own, and take new bytes:
// in the FILE block, in a group, and in both.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/testing/sar_files.h"

using xsflib::testing::failures;

namespace
{

namespace sar_files = ntdlib::testing::sar_files;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::sar::Field;
using ntdlib::sar::FileCopy;
using ntdlib::sar::Flavor;
using ntdlib::sar::Layout;
using ntdlib::sar::Padding;
using ntdlib::sar::Piece;
using ntdlib::sar::SequencePiece;

// A volume, a wait and an end, then a note and an end that only a sound's start reaches, at offset 5.
const std::vector<uint8_t> kSong = {0xC1, 0x64, 0x80, 0x30, 0xFF, 0x3C, 0x64, 0x30, 0xFF};
const std::vector<uint8_t> kOther = {0xC1, 0x50, 0xFF};

std::vector<uint8_t> Odd(Flavor flavor)
{
    std::vector<uint8_t> odd = sar_files::Sequence(flavor, kOther, {{"Odd", 0}});
    odd.resize(odd.size() + 32);

    return odd;
}

std::vector<uint8_t> Archive(Flavor flavor)
{
    const std::vector<uint8_t> other = sar_files::Sequence(flavor, kOther, {{"Other", 0}});
    const std::vector<uint8_t> copy = sar_files::Sequence(flavor, kOther, {{"Copy", 0}});

    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "Song", .file = 0, .start_offset = 0},
                   {.name = "Entry", .file = 0, .start_offset = 5},
                   {.name = "Other", .file = 2},
                   {.name = "Odd", .file = 3},
                   {.name = "Copy", .file = 5}};
    spec.players = {{"PLAYER", 1}};
    spec.groups = {{"GROUP", 4}};
    spec.files = {{.bytes = sar_files::Sequence(flavor, kSong, {{"Song", 0}})},
                  {.bytes = {1, 2, 3, 4, 5}},
                  {.bytes = other},
                  {.bytes = Odd(flavor)},
                  {.bytes = sar_files::Group(flavor, {{5, copy}, {2, other}})},
                  {.group_only = true}};

    return sar_files::Archive(flavor, spec);
}

ntdlib::sar::Options OptionsFor(Flavor flavor)
{
    ntdlib::sar::Options options;
    options.flavor = flavor;

    return options;
}

Layout Cut(const std::vector<uint8_t>& archive, Flavor flavor)
{
    return ntdlib::sar::Cut(archive, OptionsFor(flavor));
}

ntdlib::sar::SoundArchive Read(const std::vector<uint8_t>& archive, Flavor flavor)
{
    return ntdlib::sar::SoundArchive::Load(archive, OptionsFor(flavor));
}

// The sequence of each sequence piece, in order.
std::vector<uint32_t> SequencesOfPieces(const Layout& layout)
{
    std::vector<uint32_t> sequences;
    for (const ntdlib::sar::Piece& piece : layout.pieces)
    {
        if (const auto* file = std::get_if<SequencePiece>(&piece))
        {
            sequences.push_back(file->sequence);
        }
    }

    return sequences;
}

std::vector<uint8_t> Join(const Layout& layout)
{
    return ntdlib::sar::Join(layout.flavor, layout.sequences, layout.pieces, layout.fields);
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
        // A place at the archive's end has no piece. Only an item's place names a sequence piece.
        for (ntdlib::sar::Place* place : {&field.from, &field.to})
        {
            if (!place->item)
            {
                continue;
            }

            const auto* piece = std::get_if<SequencePiece>(&layout.pieces[place->piece]);
            if (piece->sequence == sequence && place->offset >= at)
            {
                place->offset += count;
            }
        }
    }
}

// Whether `cut` holds the commands, the copies and the fields of `layout`. The runs differ in the fields' values.
bool SameCommands(const Layout& cut, const Layout& layout)
{
    return cut.sequences == layout.sequences && SequencesOfPieces(cut) == SequencesOfPieces(layout) &&
           cut.fields == layout.fields;
}

bool Refuses(const Layout& layout)
{
    try
    {
        Join(layout);
    }
    catch (const std::invalid_argument&)
    {
        return true;
    }

    return false;
}

// The bytes of file `id`.
std::vector<uint8_t> FileOf(const ntdlib::sar::SoundArchive& archive, uint32_t id)
{
    const std::span<const uint8_t> data = archive.FileData(id);

    return {data.begin(), data.end()};
}

// The message of the std::invalid_argument that `call` throws, or nothing when it throws none.
template <class Call>
std::string RefusalOf(const Call& call)
{
    try
    {
        call();
    }
    catch (const std::invalid_argument& error)
    {
        return error.what();
    }

    return "";
}

// The piece of `layout` that holds file `id` of `archive` as a run of its own, with padding after it. Nothing when no
// piece does.
std::optional<std::size_t> RunOf(const std::vector<uint8_t>& archive, Flavor flavor, const Layout& layout, uint32_t id)
{
    const ntdlib::sar::SoundArchive tables = Read(archive, flavor);
    const std::vector<std::size_t> starts = ntdlib::sar::Starts(archive, flavor, layout.pieces);
    for (std::size_t i = 0; i + 1 < layout.pieces.size(); ++i)
    {
        const auto* run = std::get_if<std::vector<uint8_t>>(&layout.pieces[i]);
        if (run && *run == FileOf(tables, id) && starts[i] == tables.FileOffset(id) &&
            std::holds_alternative<Padding>(layout.pieces[i + 1]))
        {
            return i;
        }
    }

    return std::nullopt;
}

// The pieces of `layout` in which file `id` holds `bytes`.
std::vector<Piece> Replace(const Layout& layout, uint32_t id, const std::vector<uint8_t>& bytes)
{
    return ntdlib::sar::ReplaceFile(layout.flavor, layout.sequences, layout.pieces, layout.fields, id, bytes);
}

// A position rounded up to a multiple of 8 bytes. Archive() starts each file 8 bytes past a multiple of 32. The cut
// then pads each file to a multiple of 8 bytes.
std::size_t Padded(std::size_t position)
{
    return (position + 7) / 8 * 8;
}

// Where the archive's file table gives the offset of file `id`.
std::size_t OffsetFieldOf(const std::vector<uint8_t>& archive, Flavor flavor, uint32_t id)
{
    const xsflib::Reader r(archive, flavor == Flavor::kCafe);
    std::size_t info = 0;
    for (uint16_t i = 0; i < r.U16(0x10); ++i)
    {
        info = r.U16(0x14 + i * 12) == 0x2001 ? r.U32(0x18 + i * 12) + 8 : info;
    }

    std::size_t table = 0;
    for (std::size_t i = 0; i < 8; ++i)
    {
        table = r.U16(info + i * 8) == 0x2106 ? info + r.U32(info + i * 8 + 4) : table;
    }

    const std::size_t entry = table + r.U32(table + 4 + id * 8 + 4);

    return entry + r.U32(entry + 4) + 4;
}

// `archive` without the zeros after file `last`. The file then ends the archive. The header's size, and the FILE
// block's size in the table of blocks and in the block's header, end where the file ends.
std::vector<uint8_t> Unpadded(const std::vector<uint8_t>& archive, Flavor flavor, uint32_t last)
{
    const ntdlib::sar::SoundArchive tables = Read(archive, flavor);
    const std::size_t end = tables.FileOffset(last).value_or(0) + tables.FileData(last).size();
    std::vector<uint8_t> unpadded(archive.begin(), archive.begin() + static_cast<std::ptrdiff_t>(end));

    const xsflib::Reader r(archive, flavor == Flavor::kCafe);
    const auto store = flavor == Flavor::kCafe ? xsflib::StoreBe32 : xsflib::StoreLe32;
    store(&unpadded[0x0C], static_cast<uint32_t>(end));
    for (uint16_t i = 0; i < r.U16(0x10); ++i)
    {
        if (r.U16(0x14 + i * 12) == 0x2002)
        {
            const uint32_t block = r.U32(0x18 + i * 12);
            store(&unpadded[0x1C + i * 12], static_cast<uint32_t>(end - block));
            store(&unpadded[block + 4], static_cast<uint32_t>(end - block));
        }
    }

    return unpadded;
}

void TestCut(Flavor flavor)
{
    const std::vector<uint8_t> archive = Archive(flavor);

    const Layout layout = Cut(archive, flavor);

    // The two sequence files and the group's copies are pieces. The copy of the second file is a piece of the same
    // sequence. The odd file stays in the bytes. Padding follows each piece: the archive's files start 8 bytes past a
    // multiple of 32.
    XSFLIB_CHECK(layout.flavor == flavor && layout.sequences.size() == 3);
    XSFLIB_CHECK((SequencesOfPieces(layout) == std::vector<uint32_t>{0, 1, 2, 1}));
    XSFLIB_CHECK(std::holds_alternative<std::vector<uint8_t>>(layout.pieces.front()));
    const auto* song = std::get_if<SequencePiece>(&layout.pieces[1]);
    XSFLIB_CHECK(song && song->version == (flavor == Flavor::kCtr ? 0x01000000u : 0x00020000u));
    XSFLIB_CHECK(std::get_if<Padding>(&layout.pieces[2]) && std::get<Padding>(layout.pieces[2]).alignment == 8);

    // The commands without the padding, in units, with the label.
    const ntdlib::commands::Sequence& commands = layout.sequences[0];
    XSFLIB_CHECK(commands.items.size() == 5 && commands.items[0].kind == Kind::kVolume);
    XSFLIB_CHECK(commands.items[3].kind == Kind::kNote && commands.items[3].velocity == 100 * kOne);
    XSFLIB_CHECK(commands.labels.size() == 1 && commands.labels[0].name == "Song" && commands.labels[0].item == 0);
    XSFLIB_CHECK(layout.sequences[1].items.size() == 2 && layout.sequences[2].labels[0].name == "Copy");

    // The two sounds that give a start count it from the first item.
    std::vector<uint32_t> starts;
    for (const Field& field : layout.fields)
    {
        if (field.to.item)
        {
            XSFLIB_CHECK(field.to.piece == 1 && field.from == (ntdlib::sar::Place{1, 0, true}));
            starts.push_back(field.to.offset);
        }
    }
    XSFLIB_CHECK((starts == std::vector<uint32_t>{0, 3}));

    XSFLIB_CHECK(Join(layout) == archive);
}

void TestEdits(Flavor flavor)
{
    const std::vector<uint8_t> archive = Archive(flavor);
    const ntdlib::sar::SoundArchive tables = Read(archive, flavor);
    const Layout layout = Cut(archive, flavor);

    // A new volume keeps the file's size.
    Layout louder = layout;
    louder.sequences[0].items[0].value.value = 120 * kOne;
    const std::vector<uint8_t> loud = Join(louder);
    XSFLIB_CHECK(loud.size() == archive.size() && loud != archive);
    XSFLIB_CHECK(Cut(loud, flavor).sequences[0].items[0].value.value == 120 * kOne);

    // A longer wait moves the second sound's start.
    Layout longer = layout;
    longer.sequences[0].items[1].value.value = 200 * kOne;
    const std::vector<uint8_t> long_wait = Join(longer);
    XSFLIB_CHECK(long_wait.size() == archive.size());
    XSFLIB_CHECK(Read(long_wait, flavor).Sounds()[1].start_offset == 6);
    XSFLIB_CHECK(Read(long_wait, flavor).Sounds()[0].start_offset == 0);
    XSFLIB_CHECK(SameCommands(Cut(long_wait, flavor), longer));

    // Twenty more waits make the file 32 bytes larger, and the files after it move.
    Layout more = layout;
    Insert(more, 0, 2, 20, 1);
    const std::vector<uint8_t> grown = Join(more);
    XSFLIB_CHECK(grown.size() == archive.size() + 32);
    const ntdlib::sar::SoundArchive grown_tables = Read(grown, flavor);
    XSFLIB_CHECK(grown_tables.GroupErrors().empty() && !grown_tables.Truncated());
    for (uint32_t id = 1; id < tables.FileCount(); ++id)
    {
        XSFLIB_CHECK(FileOf(grown_tables, id) == FileOf(tables, id));
    }
    XSFLIB_CHECK(grown_tables.Sounds()[1].start_offset == 45);
    XSFLIB_CHECK(SameCommands(Cut(grown, flavor), more));

    // Both copies of the second file grow by 64 bytes, and so does the group that holds one.
    Layout both = layout;
    Insert(both, 1, 1, 40, 0);
    const std::vector<uint8_t> copies = Join(both);
    const ntdlib::sar::SoundArchive copies_tables = Read(copies, flavor);
    XSFLIB_CHECK(copies.size() == archive.size() + 2 * 64);
    XSFLIB_CHECK(FileOf(copies_tables, 4).size() == FileOf(tables, 4).size() + 64);
    XSFLIB_CHECK(FileOf(copies_tables, 5) == FileOf(tables, 5));
    XSFLIB_CHECK(SameCommands(Cut(copies, flavor), both));
}

void TestStarts(Flavor flavor)
{
    // Twenty more waits in the first sequence move the pieces after it.
    Layout layout = Cut(Archive(flavor), flavor);
    Insert(layout, 0, 2, 20, 1);
    const std::vector<uint8_t> archive = Join(layout);
    const ntdlib::sar::SoundArchive tables = Read(archive, flavor);

    const std::vector<std::size_t> starts = ntdlib::sar::Starts(archive, flavor, layout.pieces);

    // The files of the songs, of the other sound and of the copy that only the group holds start at their sequences'
    // pieces. The odd file starts at none. Each run holds its bytes, but for the fields that Join writes.
    XSFLIB_CHECK(starts.size() == layout.pieces.size() + 1 && starts.back() == archive.size());
    const auto sequence_of = [&](uint32_t sound) -> std::optional<uint32_t>
    {
        const auto start = std::ranges::find(starts, tables.FileOffset(tables.Sounds()[sound].file_id));
        const std::size_t piece = static_cast<std::size_t>(start - starts.begin());
        const auto* file = piece < layout.pieces.size() ? std::get_if<SequencePiece>(&layout.pieces[piece]) : nullptr;

        return file ? std::optional(file->sequence) : std::nullopt;
    };
    XSFLIB_CHECK(sequence_of(0) == 0u && sequence_of(1) == 0u && sequence_of(2) == 1u && sequence_of(4) == 2u);
    XSFLIB_CHECK(!sequence_of(3));
    for (std::size_t i = 0; i < layout.pieces.size(); ++i)
    {
        const auto* run = std::get_if<std::vector<uint8_t>>(&layout.pieces[i]);
        if (!run || starts[i] + run->size() > archive.size())
        {
            XSFLIB_CHECK(!run);
            continue;
        }

        std::vector<uint8_t> laid(archive.begin() + starts[i], archive.begin() + starts[i] + run->size());
        for (const Field& field : layout.fields)
        {
            if (field.at.piece == i)
            {
                std::copy_n(run->begin() + field.at.offset, 4, laid.begin() + field.at.offset);
            }
        }
        XSFLIB_CHECK(laid == *run);
    }

    // Pieces that run past the archive's end.
    bool refused = false;
    try
    {
        ntdlib::sar::Starts(std::span(archive).first(archive.size() - 1), flavor, layout.pieces);
    }
    catch (const std::invalid_argument&)
    {
        refused = true;
    }
    XSFLIB_CHECK(refused);
}

// The fields of the sounds' starts. Song starts at 0 and Entry at 5, each with the option. The other sounds' entries
// leave the start out. Cut makes both fields fields of the layout. A wave sound has no start. StartFields refuses an
// archive read in the other byte order.
void TestStartFields(Flavor flavor)
{
    const std::vector<uint8_t> archive = Archive(flavor);
    const Layout layout = Cut(archive, flavor);
    const std::vector<std::size_t> starts = ntdlib::sar::Starts(archive, flavor, layout.pieces);
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "Wave", .kind = sar_files::SoundSpec::kWaveSound, .file = 1},
                   {.name = "Song", .file = 0, .start_offset = 5}};
    spec.players = {{"PLAYER", 1}};
    spec.files = {{.bytes = sar_files::Sequence(flavor, kSong, {{"Song", 0}})}, {.bytes = {1, 2, 3, 4}}};
    const std::vector<uint8_t> waves = sar_files::Archive(flavor, spec);
    const Flavor other = flavor == Flavor::kCafe ? Flavor::kNx : Flavor::kCafe;

    const std::vector<std::optional<std::size_t>> fields = ntdlib::sar::StartFields(archive, flavor);
    const std::vector<std::optional<std::size_t>> wave_fields = ntdlib::sar::StartFields(waves, flavor);
    bool refused = false;
    try
    {
        ntdlib::sar::StartFields(archive, other);
    }
    catch (const xsflib::FormatError&)
    {
        refused = true;
    }

    // A missing position reads the archive's first bytes, and its check fails.
    const bool big_endian = flavor == Flavor::kCafe;
    const xsflib::Reader r(archive, big_endian);
    const auto in_layout = [&](const std::optional<std::size_t>& at)
    {
        const auto lies_at = [&](const Field& field)
        {
            return field.to.item && at && starts[field.at.piece] + field.at.offset == *at;
        };

        return std::ranges::any_of(layout.fields, lies_at);
    };
    XSFLIB_CHECK(fields.size() == 5 && fields[0] && fields[1] && !fields[2] && !fields[3] && !fields[4]);
    XSFLIB_CHECK(r.U32(fields[0].value_or(0)) == 0 && r.U32(fields[1].value_or(0)) == 5);
    XSFLIB_CHECK(in_layout(fields[0]) && in_layout(fields[1]));
    XSFLIB_CHECK(wave_fields.size() == 2 && !wave_fields[0] && wave_fields[1]);
    XSFLIB_CHECK(xsflib::Reader(waves, big_endian).U32(wave_fields[1].value_or(0)) == 5);
    XSFLIB_CHECK(refused);
}

void TestRefusals(Flavor flavor)
{
    const Layout layout = Cut(Archive(flavor), flavor);

    // A piece that names a sequence the layout lacks.
    Layout missing = layout;
    missing.sequences.pop_back();
    XSFLIB_CHECK(Refuses(missing));

    // A sound's start past its sequence.
    Layout past = layout;
    for (Field& field : past.fields)
    {
        field.to.offset = field.to.item ? 100 : field.to.offset;
    }
    XSFLIB_CHECK(Refuses(past));

    // A field in a sequence piece, and a distance below zero.
    Layout in_piece = layout;
    in_piece.fields[0].at = {1, 0, false};
    XSFLIB_CHECK(Refuses(in_piece));
    Layout backwards = layout;
    std::swap(backwards.fields[0].from, backwards.fields[0].to);
    XSFLIB_CHECK(Refuses(backwards));
}

void TestBytes(Flavor flavor)
{
    // A file whose location points inside the first sequence file keeps that file in the bytes.
    std::vector<uint8_t> inside = Archive(flavor);
    const xsflib::Reader r(inside, flavor == Flavor::kCafe);
    const uint32_t offset = r.U32(OffsetFieldOf(inside, flavor, 0)) + 0x10;
    uint8_t* field = &inside[OffsetFieldOf(inside, flavor, 1)];
    flavor == Flavor::kCafe ? xsflib::StoreBe32(field, offset) : xsflib::StoreLe32(field, offset);

    const Layout kept = Cut(inside, flavor);

    XSFLIB_CHECK(kept.sequences.size() == 2 && kept.sequences[0].labels[0].name == "Other");
    XSFLIB_CHECK(Join(kept) == inside);

    // In an archive cut short, the fields that point past the end count from there. The archive stays as short of its
    // size after a sequence grows.
    std::vector<uint8_t> short_archive = Archive(flavor);
    short_archive.resize(short_archive.size() - 8);

    Layout cut = Cut(short_archive, flavor);

    XSFLIB_CHECK(cut.sequences.size() == 3 && Join(cut) == short_archive);
    Insert(cut, 0, 2, 20, 1);
    const std::vector<uint8_t> grown = Join(cut);
    const xsflib::Reader header(grown, flavor == Flavor::kCafe);
    XSFLIB_CHECK(grown.size() == short_archive.size() + 32 && header.U32(0x0C) == grown.size() + 8);

    // A group that lies partly past the end stays whole in the bytes, with the file that only it holds.
    std::vector<uint8_t> shorter = Archive(flavor);
    shorter.resize(shorter.size() - 0x30);

    const Layout without_group = Cut(shorter, flavor);

    XSFLIB_CHECK(without_group.sequences.size() == 2 && Join(without_group) == shorter);
    XSFLIB_CHECK((SequencesOfPieces(without_group) == std::vector<uint32_t>{0, 1}));

    // A file with bytes that no command reaches stays in the bytes. After its end, a jump leads into the middle of its
    // volume. Only the Switch's commands take their arguments little-endian.
    std::vector<uint8_t> stray = {0xC1, 0x64, 0xFF, 0x89, 0x01, 0x00, 0x00, 0xFF};
    if (flavor != Flavor::kNx)
    {
        std::swap(stray[4], stray[6]);
    }
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "Stray", .file = 0}, {.name = "Other", .file = 1}};
    spec.players = {{"PLAYER", 1}};
    spec.files = {{.bytes = sar_files::Sequence(flavor, stray, {{"Stray", 0}})},
                  {.bytes = sar_files::Sequence(flavor, kOther, {{"Other", 0}})}};
    const std::vector<uint8_t> strays = sar_files::Archive(flavor, spec);

    const Layout stays = Cut(strays, flavor);

    XSFLIB_CHECK(stays.sequences.size() == 1 && stays.sequences[0].labels[0].name == "Other");
    XSFLIB_CHECK(Join(stays) == strays);
}

// The tables' values change where the reader reads them. Sound "Song" holds every option, with bytes beside the values
// and a release priority fix of 2. Sound "Plain" holds none and lists no banks: its options and banks stay as the
// reader gives them. The 3DS's reader reads no player, player priority, actor player or players. Writing the first
// values back gives the archive back.
void TestTableValues(Flavor flavor)
{
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "Song",
                    .file = 0,
                    .volume = 100,
                    .pan = 0x5A5A0001,
                    .priority = 0x3C3C0040,
                    .banks = {0, 0xFFFFFFFF},
                    .start_offset = 0,
                    .channel_priority = 0xAB000240},
                   {.name = "Plain", .file = 0, .player = 1}};
    spec.banks = {{"BANK_A", 1}, {"BANK_B", 1}};
    spec.players = {{"PLAYER", 1}, {"OTHER", 2}};
    spec.files = {{.bytes = sar_files::Sequence(flavor, kSong, {{"Song", 0}})}, {.bytes = {1, 2, 3, 4}}};
    const std::vector<uint8_t> archive = sar_files::Archive(flavor, spec);
    ntdlib::sar::Options strict = OptionsFor(flavor);
    strict.rules = ntdlib::sar::Rules::kStopAtFirstProblem;
    const ntdlib::sar::SoundArchive before = ntdlib::sar::SoundArchive::Load(archive, strict);
    std::vector<ntdlib::sar::SoundInfo> sounds = before.Sounds();
    std::vector<ntdlib::sar::PlayerInfo> players = before.Players();
    ntdlib::sar::SoundInfo& song = sounds[0];
    song.player_id = 0x04000001;
    song.volume = 90;
    song.pan_mode = 1;
    song.pan_curve = 2;
    song.player_priority = 70;
    song.actor_player_id = 1;
    song.banks[0] = 0x03000001;
    song.allocate_track_flags = 3;
    song.channel_priority = 80;
    ntdlib::sar::SoundInfo& plain = sounds[1];
    plain.volume = 50;
    plain.player_priority = 99;
    plain.channel_priority = 1;
    plain.banks = {0x03000000};
    const bool ctr = flavor == Flavor::kCtr;
    if (!ctr)
    {
        players[1].playable_sound_max = 7;
    }

    std::vector<uint8_t> written = archive;
    ntdlib::sar::WriteTableValues(written, flavor, before.Sounds(), before.Players());
    const bool unchanged = written == archive;
    ntdlib::sar::WriteTableValues(written, flavor, sounds, players);
    const ntdlib::sar::SoundArchive after = ntdlib::sar::SoundArchive::Load(written, strict);
    std::vector<uint8_t> restored = written;
    ntdlib::sar::WriteTableValues(restored, flavor, before.Sounds(), before.Players());

    const ntdlib::sar::SoundInfo& s = after.Sounds()[0];
    const ntdlib::sar::SoundInfo& p = after.Sounds()[1];
    XSFLIB_CHECK(unchanged && written != archive && restored == archive);
    XSFLIB_CHECK(s.volume == 90 && s.pan_mode == 1 && s.pan_curve == 2 && s.allocate_track_flags == 3);
    XSFLIB_CHECK((s.banks == std::vector<uint32_t>{0x03000001, 0xFFFFFFFF}));
    XSFLIB_CHECK(s.channel_priority == 80 && s.release_priority_fix && s.start_offset == 0);
    XSFLIB_CHECK(s.player_id == (ctr ? 0 : 0x04000001u) && s.player_priority == (ctr ? 64 : 70));
    XSFLIB_CHECK(s.actor_player_id == (ctr ? 0 : 1));
    XSFLIB_CHECK(p.volume == 50 && p.player_priority == 64 && p.channel_priority == 64 && p.banks.empty());
    XSFLIB_CHECK(after.Players().size() == (ctr ? 0u : 2u) && (ctr || after.Players()[1].playable_sound_max == 7));

    // A release priority fix turned off writes 0 into its byte. The byte above it keeps its value.
    song.release_priority_fix = false;
    std::vector<uint8_t> off = archive;
    ntdlib::sar::WriteTableValues(off, flavor, sounds, players);
    const std::vector<uint8_t> channel = flavor == Flavor::kCafe ? std::vector<uint8_t>{0xAB, 0x00, 0x00, 0x50}
                                                                 : std::vector<uint8_t>{0x50, 0x00, 0x00, 0xAB};

    XSFLIB_CHECK(!ntdlib::sar::SoundArchive::Load(off, strict).Sounds()[0].release_priority_fix);
    XSFLIB_CHECK(std::search(off.begin(), off.end(), channel.begin(), channel.end()) != off.end());
}

// The places of the optional values. Song holds every option, Plain none, Wave a pan and Ranked a priority. A new
// value reads back where TableValuePlaces finds a place for it, and nowhere else. Ranked plays file 2. Its entry
// follows Wave's details, and its first word has bit 1 set: a reader that took Wave for a sequence sound would find a
// channel priority there. The header and the INFO block give the places by themselves. An archive read in the other
// byte order, or cut short inside its INFO block, is refused.
void TestValuePlaces(Flavor flavor)
{
    using ntdlib::sar::ValuePlaces;
    sar_files::ArchiveSpec spec;
    spec.sounds = {
        {.name = "Song", .file = 0, .pan = 0x0001, .priority = 0x0140, .banks = {0}, .channel_priority = 0x40},
        {.name = "Plain", .file = 0},
        {.name = "Wave", .kind = sar_files::SoundSpec::kWaveSound, .file = 1, .pan = 0x0100},
        {.name = "Ranked", .file = 2, .priority = 0x0040}};
    spec.banks = {{"BANK", 1}};
    spec.players = {{"PLAYER", 1}};
    spec.files = {{.bytes = sar_files::Sequence(flavor, kSong, {{"Song", 0}})},
                  {.bytes = {1, 2, 3, 4}},
                  {.bytes = sar_files::Sequence(flavor, kOther, {{"Ranked", 0}})}};
    const std::vector<uint8_t> archive = sar_files::Archive(flavor, spec);
    ntdlib::sar::Options strict = OptionsFor(flavor);
    strict.rules = ntdlib::sar::Rules::kStopAtFirstProblem;
    const ntdlib::sar::SoundArchive before = ntdlib::sar::SoundArchive::Load(archive, strict);
    std::vector<ntdlib::sar::SoundInfo> sounds = before.Sounds();
    for (ntdlib::sar::SoundInfo& sound : sounds)
    {
        sound.pan_curve = 5;
        sound.player_priority = 33;
        sound.channel_priority = 99;
    }
    std::vector<uint8_t> written = archive;
    ntdlib::sar::WriteTableValues(written, flavor, sounds, before.Players());
    const std::vector<ntdlib::sar::SoundInfo> after = ntdlib::sar::SoundArchive::Load(written, strict).Sounds();

    // The INFO block ends the bytes that the reader reads.
    const std::string_view magic = "INFO";
    const auto info = static_cast<std::size_t>(std::search(archive.begin(), archive.end(), magic.begin(), magic.end()) -
                                               archive.begin());
    const std::size_t end = info + xsflib::Reader(archive, flavor == Flavor::kCafe).U32(info + 4);
    const Flavor other = flavor == Flavor::kCafe ? Flavor::kNx : Flavor::kCafe;
    const auto refused = [](std::span<const uint8_t> bytes, Flavor read_as)
    {
        try
        {
            ntdlib::sar::TableValuePlaces(bytes, read_as);
        }
        catch (const xsflib::FormatError&)
        {
            return true;
        }

        return false;
    };

    const std::vector<ValuePlaces> places = ntdlib::sar::TableValuePlaces(archive, flavor);
    const std::vector<ValuePlaces> head = ntdlib::sar::TableValuePlaces(std::span(archive).first(end), flavor);

    const bool ctr = flavor == Flavor::kCtr;
    const std::vector<ValuePlaces> expected = {
        {true, !ctr, true}, {false, false, false}, {true, false, false}, {false, !ctr, false}};
    XSFLIB_CHECK(places == expected && head == places && after.size() == places.size());
    for (std::size_t i = 0; i < places.size() && i < after.size(); ++i)
    {
        const ntdlib::sar::SoundInfo& sound = after[i];
        const ValuePlaces changed{sound.pan_curve == 5, sound.player_priority == 33, sound.channel_priority == 99};
        XSFLIB_CHECK(changed == places[i]);
    }
    XSFLIB_CHECK(refused(archive, other) && refused(std::span(archive).first(info + 16), flavor));
}

// The file of other bytes and the odd sequence file become runs of their own. The group holds fields, and stays in the
// bytes. A longer file of other bytes moves the files after it, and the tables follow. A shorter odd file moves them
// back. A new cut of the new archive gives the same fields. In an archive cut short, the group lies partly past the
// end, and a longer file still goes in. Without the fields, a shorter odd file leaves the group past the end.
void TestFiles(Flavor flavor)
{
    const std::vector<uint8_t> archive = Archive(flavor);
    const ntdlib::sar::SoundArchive tables = Read(archive, flavor);
    const Layout layout = Cut(archive, flavor);
    const std::vector<uint8_t> longer(40, 0x5A);
    const std::vector<uint8_t> shorter = {7, 7, 7};
    std::vector<uint8_t> short_archive = archive;
    short_archive.resize(short_archive.size() - 0x30);
    const Layout short_layout = Cut(short_archive, flavor);

    const std::vector<uint8_t> grown =
        ntdlib::sar::Join(flavor, layout.sequences, Replace(layout, 1, longer), layout.fields);
    const std::vector<uint8_t> shrunk =
        ntdlib::sar::Join(flavor, layout.sequences, Replace(layout, 3, shorter), layout.fields);
    const std::vector<uint8_t> short_grown =
        ntdlib::sar::Join(flavor, short_layout.sequences, Replace(short_layout, 1, longer), short_layout.fields);
    Layout unfollowed = layout;
    unfollowed.fields.clear();
    const std::string lost = RefusalOf([&] { Replace(unfollowed, 3, shorter); });

    XSFLIB_CHECK(RunOf(archive, flavor, layout, 1) && RunOf(archive, flavor, layout, 3));
    XSFLIB_CHECK(!RunOf(archive, flavor, layout, 4));
    const ntdlib::sar::SoundArchive grown_tables = Read(grown, flavor);
    XSFLIB_CHECK(grown.size() == archive.size() + Padded(40) - Padded(5) && FileOf(grown_tables, 1) == longer);
    XSFLIB_CHECK(grown_tables.GroupErrors().empty() && !grown_tables.Truncated());
    const ntdlib::sar::SoundArchive shrunk_tables = Read(shrunk, flavor);
    const std::size_t odd = FileOf(tables, 3).size();
    XSFLIB_CHECK(shrunk.size() == archive.size() - Padded(odd) + Padded(3) && FileOf(shrunk_tables, 3) == shorter);
    for (const uint32_t id : {0u, 2u, 4u, 5u})
    {
        XSFLIB_CHECK(FileOf(grown_tables, id) == FileOf(tables, id) && FileOf(shrunk_tables, id) == FileOf(tables, id));
    }
    XSFLIB_CHECK(FileOf(grown_tables, 3) == FileOf(tables, 3) && FileOf(shrunk_tables, 1) == FileOf(tables, 1));
    const Layout recut = Cut(grown, flavor);
    XSFLIB_CHECK(recut.fields == layout.fields && RunOf(grown, flavor, recut, 1));
    XSFLIB_CHECK(short_grown.size() == short_archive.size() + Padded(40) - Padded(5));
    XSFLIB_CHECK(FileOf(Read(short_grown, flavor), 1) == longer);

    // Without the fields, the group's offset points where the shorter archive holds only part of it. The tables no
    // longer list the files in the group.
    XSFLIB_CHECK(lost == "the archive's tables don't follow the new bytes of file 3");
}

// A group holds a copy of a file of other bytes, and a file that only it holds. A longer file goes into both copies of
// the first, and the group grows. A longer file that only the group holds makes it grow too. The group itself isn't a
// run of its own.
void TestFilesInGroups(Flavor flavor)
{
    const std::vector<uint8_t> first = {1, 2, 3, 4, 5};
    const std::vector<uint8_t> only = {6, 7, 8};
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "Song", .file = 0}};
    spec.players = {{"PLAYER", 1}};
    spec.groups = {{"GROUP", 2}};
    spec.files = {{.bytes = sar_files::Sequence(flavor, kSong, {{"Song", 0}})},
                  {.bytes = first},
                  {.bytes = sar_files::Group(flavor, {{1, first}, {3, only}})},
                  {.group_only = true}};
    const std::vector<uint8_t> archive = sar_files::Archive(flavor, spec);
    const ntdlib::sar::SoundArchive tables = Read(archive, flavor);
    const Layout layout = Cut(archive, flavor);
    const std::vector<uint8_t> longer(20, 0x5A);
    const std::vector<uint8_t> longest(50, 0xA5);

    const ntdlib::sar::SoundArchive both =
        Read(ntdlib::sar::Join(flavor, layout.sequences, Replace(layout, 1, longer), layout.fields), flavor);
    const ntdlib::sar::SoundArchive grouped =
        Read(ntdlib::sar::Join(flavor, layout.sequences, Replace(layout, 3, longest), layout.fields), flavor);
    const std::string refusal = RefusalOf([&] { Replace(layout, 2, longer); });

    const std::vector<uint8_t> group = FileOf(both, 2);
    XSFLIB_CHECK(FileOf(both, 1) == longer && FileOf(both, 3) == only && FileOf(both, 0) == FileOf(tables, 0));
    XSFLIB_CHECK(group.size() == FileOf(tables, 2).size() + Padded(20) - Padded(5));
    XSFLIB_CHECK(std::ranges::search(group, longer).begin() != group.end());
    XSFLIB_CHECK(both.GroupErrors().empty() && grouped.GroupErrors().empty());
    XSFLIB_CHECK(FileOf(grouped, 3) == longest && FileOf(grouped, 1) == first);
    XSFLIB_CHECK(FileOf(grouped, 2).size() == FileOf(tables, 2).size() + Padded(50) - Padded(3));
    XSFLIB_CHECK(refusal == "a copy of file 2 isn't a piece of its own");
}

// A group holds a copy of a file with other bytes, and a file that only it holds. FileCopies gives both copies of the
// first, in the order of their starts, and the one copy of the second. A file that the archive lacks has none.
void TestFileCopies(Flavor flavor)
{
    const std::vector<uint8_t> first = {1, 2, 3, 4, 5};
    const std::vector<uint8_t> other = {9, 8, 7, 6, 5};
    const std::vector<uint8_t> only = {6, 7, 8};
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "Song", .file = 0}};
    spec.players = {{"PLAYER", 1}};
    spec.groups = {{"GROUP", 2}};
    spec.files = {{.bytes = sar_files::Sequence(flavor, kSong, {{"Song", 0}})},
                  {.bytes = first},
                  {.bytes = sar_files::Group(flavor, {{1, other}, {3, only}})},
                  {.group_only = true}};
    const std::vector<uint8_t> archive = sar_files::Archive(flavor, spec);
    const ntdlib::sar::SoundArchive tables = Read(archive, flavor);
    const Layout layout = Cut(archive, flavor);
    const auto copies_of = [&](uint32_t file)
    {
        return ntdlib::sar::FileCopies(flavor, layout.sequences, layout.pieces, layout.fields, file);
    };

    const std::vector<FileCopy> copies = copies_of(1);
    const std::vector<FileCopy> only_copies = copies_of(3);

    XSFLIB_CHECK(copies.size() == 2 && only_copies.size() == 1 && copies_of(7).empty());
    if (copies.size() == 2 && only_copies.size() == 1)
    {
        XSFLIB_CHECK(copies[0].start == tables.FileOffset(1) && !copies[0].group && copies[0].bytes == first);
        XSFLIB_CHECK(copies[1].start > copies[0].start && copies[1].group == 2u && copies[1].bytes == other);
        XSFLIB_CHECK(only_copies[0].group == 2u && only_copies[0].bytes == only);
    }
}

// ReplaceFile refuses a file that the archive lacks, an empty file, a file in a layout that keeps it inside a run, a
// file whose bytes another file ID names too, and fields that don't follow the new size.
void TestFileRefusals(Flavor flavor)
{
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "Song", .file = 0}};
    spec.players = {{"PLAYER", 1}};
    spec.files = {{.bytes = sar_files::Sequence(flavor, kSong, {{"Song", 0}})},
                  {.bytes = {1, 2, 3, 4, 5}},
                  {.bytes = {6, 7}},
                  {.bytes = {}}};
    const std::vector<uint8_t> archive = sar_files::Archive(flavor, spec);
    const Layout layout = Cut(archive, flavor);
    Layout one_run;
    one_run.flavor = flavor;
    one_run.pieces = {archive};
    Layout unfollowed = layout;
    unfollowed.fields.clear();

    // A run that holds file 1 and the bytes after it. A cut keeps a file that way when the file can't become a run of
    // its own.
    Layout inside = layout;
    const std::size_t first = RunOf(archive, flavor, layout, 1).value_or(0);
    auto& run = std::get<std::vector<uint8_t>>(inside.pieces[first]);
    run.resize(Padded(run.size()));
    const auto& next = std::get<std::vector<uint8_t>>(inside.pieces[first + 2]);
    run.insert(run.end(), next.begin(), next.end());
    inside.pieces.erase(inside.pieces.begin() + static_cast<std::ptrdiff_t>(first) + 1,
                        inside.pieces.begin() + static_cast<std::ptrdiff_t>(first) + 3);
    inside.fields.clear();

    // Only the fields that count from the start of file 1: its size follows, and the offset of file 2 doesn't.
    Layout sized = layout;
    std::erase_if(sized.fields, [&](const Field& field) { return field.from.piece != first; });

    // File 2 names the bytes of file 1 too: the offset and the size that follow it in the file table.
    std::vector<uint8_t> shared = archive;
    std::copy_n(archive.begin() + OffsetFieldOf(archive, flavor, 1), 8,
                shared.begin() + OffsetFieldOf(archive, flavor, 2));
    const Layout shared_layout = Cut(shared, flavor);
    const std::vector<uint8_t> longer(40, 0x5A);

    const auto refusal = [&](const Layout& cut, uint32_t id)
    {
        return RefusalOf([&] { Replace(cut, id, longer); });
    };

    XSFLIB_CHECK(refusal(layout, 4) == "the archive holds no file 4");
    XSFLIB_CHECK(refusal(layout, 3) == "a copy of file 3 isn't a piece of its own");
    XSFLIB_CHECK(refusal(one_run, 1) == "a copy of file 1 isn't a piece of its own");
    XSFLIB_CHECK(refusal(shared_layout, 1) == "file 1 shares its bytes with file 2");
    XSFLIB_CHECK(refusal(unfollowed, 1) == "the archive's tables don't follow the new bytes of file 1");
    XSFLIB_CHECK(refusal(unfollowed, 2) == "the archive's tables don't follow the new bytes of file 2");
    XSFLIB_CHECK(refusal(inside, 1) == "a copy of file 1 isn't a piece of its own");
    XSFLIB_CHECK(refusal(sized, 1) == "the archive's tables don't follow the new bytes of file 1");
    XSFLIB_CHECK(refusal(layout, 1).empty());
}

// A file that ends the archive, with no zeros after it, becomes a piece with padding to 1 byte after it: a file of
// other bytes, or a sequence file. The pieces join to give the archive back. A longer or shorter file, or a sequence
// with more commands, gives the archive that the builder makes with it, without the zeros after it.
void TestLastFiles(Flavor flavor)
{
    // A sequence file and a file of other bytes, in either order.
    const auto archive = [&](const std::vector<uint8_t>& other, const std::vector<uint8_t>& song, bool song_last)
    {
        sar_files::ArchiveSpec spec;
        spec.sounds = {{.name = "Song", .file = song_last ? 1u : 0u}};
        spec.players = {{"PLAYER", 1}};
        spec.files = {{.bytes = sar_files::Sequence(flavor, song, {{"Song", 0}})}, {.bytes = other}};
        if (song_last)
        {
            std::swap(spec.files[0], spec.files[1]);
        }

        return Unpadded(sar_files::Archive(flavor, spec), flavor, 1);
    };
    const std::vector<uint8_t> other = {1, 2, 3, 4, 5};
    const std::vector<uint8_t> longer(40, 0x5A);
    const std::vector<uint8_t> shorter = {7, 7, 7};
    std::vector<uint8_t> longer_song;
    for (int i = 0; i < 21; ++i)
    {
        longer_song.insert(longer_song.end(), {0xC1, 0x50});
    }
    longer_song.push_back(0xFF);
    const std::vector<uint8_t> other_last = archive(other, kOther, false);
    const std::vector<uint8_t> song_last = archive(other, kOther, true);
    const Layout layout = Cut(other_last, flavor);
    Layout song_layout = Cut(song_last, flavor);

    const std::vector<uint8_t> grown =
        ntdlib::sar::Join(flavor, layout.sequences, Replace(layout, 1, longer), layout.fields);
    const std::vector<uint8_t> shrunk =
        ntdlib::sar::Join(flavor, layout.sequences, Replace(layout, 1, shorter), layout.fields);
    const std::vector<uint8_t> song_joined = Join(song_layout);
    Insert(song_layout, 0, 1, 20, 0);
    const std::vector<uint8_t> song_grown = Join(song_layout);

    XSFLIB_CHECK(Join(layout) == other_last && RunOf(other_last, flavor, layout, 1));
    XSFLIB_CHECK(layout.pieces.back() == Piece{Padding{1}});
    XSFLIB_CHECK(grown == archive(longer, kOther, false) && shrunk == archive(shorter, kOther, false));
    XSFLIB_CHECK(song_joined == song_last && (SequencesOfPieces(song_layout) == std::vector<uint32_t>{0}));
    XSFLIB_CHECK(song_layout.pieces.back() == Piece{Padding{1}});
    XSFLIB_CHECK(song_grown == archive(other, longer_song, true));
}

} // namespace

int main()
{
    for (const Flavor flavor : {Flavor::kCtr, Flavor::kCafe, Flavor::kNx})
    {
        TestCut(flavor);
        TestEdits(flavor);
        TestStarts(flavor);
        TestStartFields(flavor);
        TestRefusals(flavor);
        TestBytes(flavor);
        TestTableValues(flavor);
        TestValuePlaces(flavor);
        TestFiles(flavor);
        TestFilesInGroups(flavor);
        TestFileRefusals(flavor);
        TestFileCopies(flavor);
        TestLastFiles(flavor);
    }

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::printf("Layout tests passed\n");

    return 0;
}
