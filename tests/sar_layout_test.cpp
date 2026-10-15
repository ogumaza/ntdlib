// SPDX-License-Identifier: MIT

// Tests of the cutting of sound archives into pieces and their joining (ntdlib/sar/layout.h), on archives built in each
// flavor's format: the 3DS's, the Wii U's and the Switch's. Each archive holds two sequence files, a file of other
// bytes, a sequence file laid out otherwise than the writer lays it out, and a group. The group holds a copy of a file
// that only it holds, and a copy of one of the archive's sequence files. Another archive holds a sequence file with
// bytes that no command reaches. A third archive's tables take new values.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <string>
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
using ntdlib::sar::Flavor;
using ntdlib::sar::Layout;
using ntdlib::sar::Padding;
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

} // namespace

int main()
{
    for (const Flavor flavor : {Flavor::kCtr, Flavor::kCafe, Flavor::kNx})
    {
        TestCut(flavor);
        TestEdits(flavor);
        TestRefusals(flavor);
        TestBytes(flavor);
        TestTableValues(flavor);
    }

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::printf("Layout tests passed\n");

    return 0;
}
