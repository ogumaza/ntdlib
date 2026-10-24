// SPDX-License-Identifier: MIT

// Tests of the groups of sound archives and their files (ntdlib/sar/groups.h). The tests lay their groups and archives
// out as NintendoWare's tools do, with ntdlib/testing/sar_tools.h: a group that holds copies of its files, one that
// links to them, one that ends without padding, and one without files. GroupWith gives each back, and adds files and
// items to it. WithGroupLists names a group in the files' lists. WithGroupMembers adds files to a group whose file the
// archive holds. GroupOfFile finds the group of a group's file beside an archive. The 3DS's groups' files come in two
// versions: 1.0.0.0 without items, and 1.1.0.0 with them. The 3DS's file table holds no lists.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/groups.h"
#include "ntdlib/sar/info_block.h"
#include "ntdlib/sar/string_block.h"
#include "ntdlib/sar/tables.h"
#include "ntdlib/testing/sar_files.h"
#include "ntdlib/testing/sar_tools.h"

using xsflib::testing::ErrorOf;

namespace
{

namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;
namespace sar_tools = ntdlib::testing::sar_tools;
using sar::Flavor;
using sar::GroupItem;
using sar::GroupMember;
using sar::ItemId;

constexpr uint32_t kAll = 0xFFFFFFFF;
constexpr uint32_t kCtrFirst = 0x01000000; // the 3DS's version of a group's file without INFX
constexpr uint32_t kCtrItems = 0x01010000; // and with INFX

const std::vector<uint8_t> kBank = {1, 2, 3, 4, 5};
const std::vector<uint8_t> kWaves(40, 0xA5);
const std::vector<uint8_t> kOther = {9, 8, 7};

// Whether a group's file of `flavor` in the 3DS's version `version` lists items.
bool ListsItems(Flavor flavor, uint32_t version)
{
    return flavor != Flavor::kCtr || version >= kCtrItems;
}

// A group that holds copies of files 3 and 5, and lists a bank and a sound. `version` is the version on the 3DS.
sar_tools::GroupSpec Copies(uint32_t version = kCtrFirst)
{
    return {.files = {{3, kBank}, {5, kWaves}},
            .items = {{ItemId(sar::kBankItem, 0), kAll}, {ItemId(sar::kSoundItem, 1), 1}},
            .ctr_version = version};
}

// A group that links to files 3 and 5.
sar_tools::GroupSpec Links(uint32_t version = kCtrFirst)
{
    return {.files = {{3, std::nullopt}, {5, std::nullopt}},
            .items = {{ItemId(sar::kBankItem, 0), kAll}},
            .ctr_version = version};
}

sar_tools::GroupSpec Unpadded(uint32_t version = kCtrFirst)
{
    sar_tools::GroupSpec spec = Copies(version);
    spec.items.pop_back();
    spec.padded = false;

    return spec;
}

// A group without files.
sar_tools::GroupSpec Empty(uint32_t version = kCtrFirst)
{
    return {.ctr_version = version};
}

sar::SoundArchive Read(const std::vector<uint8_t>& archive, Flavor flavor)
{
    sar::Options options;
    options.flavor = flavor;
    options.rules = sar::Rules::kStopAtFirstProblem;

    return sar::SoundArchive::Load(archive, options);
}

// The groups that file `file`'s entry lists, by their item IDs.
std::vector<uint32_t> ListOf(const std::vector<uint8_t>& archive, Flavor flavor, uint32_t file)
{
    const sar::InfoBlock info = sar::InfoBlock::Read(sar::TablesOf(archive, flavor).info, flavor);
    const xsflib::Reader r(info.files.at(file), flavor == Flavor::kCafe);
    std::vector<uint32_t> groups;
    for (uint32_t k = 0; k < r.U32(32); ++k)
    {
        groups.push_back(r.U32(36 + 4 * std::size_t{k}));
    }

    return groups;
}

std::vector<uint8_t> Bytes(std::span<const uint8_t> bytes)
{
    return {bytes.begin(), bytes.end()};
}

// ReadGroupFile gives each file with its copy, or as a link, and each item with the parts that the group loads.
void TestRead(Flavor flavor, uint32_t version = kCtrFirst)
{
    const sar::GroupContents copies = sar::ReadGroupFile(sar_tools::Group(flavor, Copies(version)), flavor);
    const sar::GroupContents links = sar::ReadGroupFile(sar_tools::Group(flavor, Links(version)), flavor);
    const sar::GroupContents none = sar::ReadGroupFile(sar_tools::Group(flavor, Empty(version)), flavor);

    XSFLIB_CHECK((copies.files == std::vector<GroupMember>{{3, kBank}, {5, kWaves}}));
    const std::vector<GroupItem> items = {{ItemId(sar::kBankItem, 0), kAll}, {ItemId(sar::kSoundItem, 1), 1}};
    XSFLIB_CHECK(copies.items == (ListsItems(flavor, version) ? items : std::vector<GroupItem>{}));
    XSFLIB_CHECK((links.files == std::vector<GroupMember>{{3, std::nullopt}, {5, std::nullopt}}));
    XSFLIB_CHECK(none.files.empty() && none.items.empty());
    XSFLIB_CHECK(!ErrorOf<xsflib::FormatError>([&] { sar::ReadGroupFile(kWaves, flavor); }).empty());

    // A file of another kind, in the console's byte order.
    std::vector<uint8_t> other = sar_tools::Group(flavor, Copies(version));
    std::ranges::copy(std::string_view(flavor == Flavor::kCtr ? "CWAV" : "FWAV"), other.begin());
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { sar::ReadGroupFile(other, flavor); }) ==
                 "bytes that aren't a group's file of the console");
}

// Each group comes back byte for byte. New files and items go in after those that the group holds, in the tools'
// layout.
void TestGroupWith(Flavor flavor, uint32_t version = kCtrFirst)
{
    const auto with =
        [flavor](const sar_tools::GroupSpec& spec, std::vector<GroupMember> files, std::vector<GroupItem> items)
    {
        return sar::GroupWith(sar_tools::Group(flavor, spec), flavor, files, items);
    };
    const auto grown =
        [flavor](sar_tools::GroupSpec spec, const std::vector<GroupMember>& files, const std::vector<GroupItem>& items)
    {
        for (const GroupMember& member : files)
        {
            spec.files.push_back({member.file, member.copy});
        }
        for (const GroupItem& item : items)
        {
            spec.items.push_back({item.item, item.load});
        }

        return sar_tools::Group(flavor, spec);
    };
    const std::vector<GroupItem> item = {{ItemId(sar::kWaveArchiveItem, 2), 8}};

    for (const sar_tools::GroupSpec& spec : {Copies(version), Links(version), Unpadded(version), Empty(version)})
    {
        XSFLIB_CHECK(with(spec, {}, {}) == sar_tools::Group(flavor, spec));
    }
    XSFLIB_CHECK(with(Copies(version), {{7, kOther}}, item) == grown(Copies(version), {{7, kOther}}, item));
    XSFLIB_CHECK(with(Links(version), {{7, std::nullopt}}, {}) == grown(Links(version), {{7, std::nullopt}}, {}));
    XSFLIB_CHECK(with(Unpadded(version), {{7, kOther}, {8, kBank}}, item) ==
                 grown(Unpadded(version), {{7, kOther}, {8, kBank}}, item));
    XSFLIB_CHECK(with(Empty(version), {{7, kOther}}, item) == grown(Empty(version), {{7, kOther}}, item));
    XSFLIB_CHECK(with(Empty(version), {{7, std::nullopt}}, {}) == grown(Empty(version), {{7, std::nullopt}}, {}));
}

// GroupWith refuses a file that the group holds, an item that it lists, files of the other kind, and a group's file
// laid out another way.
void TestGroupWithRefusals(Flavor flavor, uint32_t version = kCtrFirst)
{
    const std::vector<uint8_t> copies = sar_tools::Group(flavor, Copies(version));
    const std::vector<uint8_t> links = sar_tools::Group(flavor, Links(version));
    const std::vector<uint8_t> none = sar_tools::Group(flavor, Empty(version));
    std::vector<uint8_t> longer = copies;
    longer.push_back(0);
    std::vector<uint8_t> moved = copies;
    moved.insert(moved.begin() + 0x40 + 0x40, 32, 0); // zeros between INFO and FILE
    const auto refusal =
        [flavor](const std::vector<uint8_t>& group, std::vector<GroupMember> files, std::vector<GroupItem> items)
    {
        return ErrorOf<std::invalid_argument>([&] { sar::GroupWith(group, flavor, files, items); });
    };
    const auto other_layout = [flavor](const std::vector<uint8_t>& group)
    {
        return ErrorOf<xsflib::FormatError>([&] { sar::GroupWith(group, flavor, {}, {}); });
    };

    XSFLIB_CHECK(refusal(copies, {{3, kOther}}, {}) == "the group holds file 3 already");
    XSFLIB_CHECK(refusal(copies, {{7, kOther}, {7, kBank}}, {}) == "the group holds file 7 already");
    XSFLIB_CHECK(refusal(copies, {}, {{ItemId(sar::kSoundItem, 1), kAll}}) ==
                 (ListsItems(flavor, version) ? "the group lists item 16777217 already" : ""));
    XSFLIB_CHECK(refusal(copies, {{7, std::nullopt}}, {}) == "a link to file 7 for a group that holds copies");
    XSFLIB_CHECK(refusal(links, {{7, kOther}}, {}) == "a copy of file 7 for a group that links to its files");
    XSFLIB_CHECK(refusal(none, {{7, kOther}, {8, std::nullopt}}, {}) ==
                 "a link to file 8 for a group that holds copies");
    XSFLIB_CHECK(refusal(none, {{7, kOther}}, {}).empty());
    XSFLIB_CHECK(other_layout(longer) == "a group's file laid out another way than the tools lay it out");
    XSFLIB_CHECK(other_layout(moved) == "a group's file laid out another way than the tools lay it out");
    XSFLIB_CHECK(other_layout(copies).empty());
}

// An archive with a sequence file, a bank's file, a wave archive's file, a group's file that holds a copy of the bank
// file, a group's file that links to it, a file that only a group holds, and a stream outside the archive. Group 2
// lies beside the archive. The files' lists name the groups that hold them. `version` is the version of the groups'
// files on the 3DS.
sar_files::ArchiveSpec MembersSpec(Flavor flavor, uint32_t version = kCtrFirst)
{
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "SEQ_A", .file = 0, .banks = {0}}};
    spec.banks = {{"BANK_A", 1}};
    spec.wave_archives = {{"", 2}};
    spec.groups = {{"GROUP_COPIES", 3}, {"GROUP_LINKS", 4}, {"GROUP_BESIDE", sar::kExternalGroupFile}};
    spec.players = {{"PLAYER_A", 4}};
    spec.files = {{.bytes = sar_files::Sequence(flavor, {0xFF}, {{"SEQ_A", 0}}), .groups = {1}},
                  {.bytes = kBank, .groups = {0, 1}},
                  {.bytes = kWaves},
                  {.bytes = sar_tools::GroupInArchive(flavor, {.files = {{1, kBank}}, .ctr_version = version})},
                  {.bytes = sar_tools::GroupInArchive(
                       flavor, {.files = {{0, std::nullopt}, {1, std::nullopt}}, .ctr_version = version})},
                  {.group_only = true, .groups = {2}},
                  {.external = "stream.bfstm"}};

    return spec;
}

// The lists of the files of `members` name the group in its place, once. A link to a wave archive's file leaves its
// list as it was. The archive's other bytes stay.
void TestLists(Flavor flavor)
{
    const sar_files::ArchiveSpec spec = MembersSpec(flavor);
    const std::vector<uint8_t> archive = sar_tools::Archive(flavor, spec);
    const std::vector<GroupMember> kMembers = {{0, kOther}, {1, std::nullopt}, {2, std::nullopt}, {5, kOther}};
    const std::vector<uint8_t> listed = sar::WithGroupLists(archive, flavor, 0, kMembers);
    const std::vector<uint8_t> copied = sar::WithGroupLists(archive, flavor, 2, {{{2, kOther}}});
    const auto group = [](uint32_t index)
    {
        return ItemId(sar::kGroupItem, index);
    };

    XSFLIB_CHECK((ListOf(listed, flavor, 0) == std::vector<uint32_t>{group(0), group(1)}));
    XSFLIB_CHECK((ListOf(listed, flavor, 1) == std::vector<uint32_t>{group(0), group(1)}));
    XSFLIB_CHECK(ListOf(listed, flavor, 2).empty());
    XSFLIB_CHECK((ListOf(listed, flavor, 5) == std::vector<uint32_t>{group(0), group(2)}));
    XSFLIB_CHECK((ListOf(copied, flavor, 2) == std::vector<uint32_t>{group(2)}));
    XSFLIB_CHECK(sar::WithGroupLists(listed, flavor, 0, kMembers) == listed);

    // The archive is the one that the tools lay out with the new lists.
    sar_files::ArchiveSpec expected = spec;
    expected.files[0].groups = {0, 1};
    expected.files[1].groups = {0, 1};
    expected.files[5].groups = {0, 2};
    XSFLIB_CHECK(listed == sar_tools::Archive(flavor, expected));
    const auto refusal = [&](uint32_t index, std::vector<GroupMember> files)
    {
        return ErrorOf<std::invalid_argument>([&] { sar::WithGroupLists(archive, flavor, index, files); });
    };
    XSFLIB_CHECK(refusal(3, {}) == "the archive holds no group 3");
    XSFLIB_CHECK(refusal(0, {{9, kOther}}) == "the archive holds no file 9");
    XSFLIB_CHECK(refusal(0, {{6, kOther}}) == "file 6 lies outside the archive");

    // An entry with bytes after its list.
    const sar::Tables tables = sar::TablesOf(archive, flavor);
    sar::InfoBlock info = sar::InfoBlock::Read(tables.info, flavor);
    info.files[0].resize(info.files[0].size() + 4);
    const std::vector<uint8_t> longer =
        sar::WithTables(archive, flavor, sar::StringBlock::Read(*tables.strings, flavor), std::move(info));
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { sar::WithGroupLists(longer, flavor, 0, {{{0, kOther}}}); }) ==
                 "an archive laid out another way: the entry of file 0 holds no list of groups, or bytes after it");
}

// The offset of the copy of INFO's entry `index` from the start of `group`, a group's file.
std::size_t CopyOffset(std::span<const uint8_t> group, Flavor flavor, std::size_t index)
{
    const xsflib::Reader r(group, flavor == Flavor::kCafe);
    const std::size_t info = r.U32(0x18) + 8;
    const std::size_t file = r.U32(0x24) + 8;
    const std::size_t entry = info + r.U32(info + 4 + 8 * index + 4);

    return file + r.U32(entry + 8);
}

// A group whose file the archive holds takes copies or links, and lists items. Its file keeps its place, and the files
// after it move. The files' lists name the group.
void TestMembers(Flavor flavor, uint32_t version = kCtrFirst)
{
    const sar_files::ArchiveSpec spec = MembersSpec(flavor, version);
    const std::vector<uint8_t> archive = sar_tools::Archive(flavor, spec);
    const sar::SoundArchive before = Read(archive, flavor);
    const std::vector<GroupItem> items = {{ItemId(sar::kSoundItem, 0), kAll}};
    const std::vector<uint8_t> copies =
        sar::WithGroupMembers(archive, flavor, 0, {{{0, Bytes(before.FileData(0))}, {2, kWaves}}}, items);
    const std::vector<uint8_t> links = sar::WithGroupMembers(archive, flavor, 1, {{{2, std::nullopt}}}, items);

    // The archive that the tools would lay out with the groups' new files and lists.
    sar_files::ArchiveSpec expected = spec;
    expected.files[3].bytes =
        sar_tools::GroupInArchive(flavor, {.files = {{1, kBank}, {0, Bytes(before.FileData(0))}, {2, kWaves}},
                                           .items = {{ItemId(sar::kSoundItem, 0), kAll}},
                                           .ctr_version = version});
    expected.files[0].groups = {0, 1};
    expected.files[2].groups = {0};
    XSFLIB_CHECK(copies == sar_tools::Archive(flavor, expected));
    expected = spec;
    expected.files[4].bytes =
        sar_tools::GroupInArchive(flavor, {.files = {{0, std::nullopt}, {1, std::nullopt}, {2, std::nullopt}},
                                           .items = {{ItemId(sar::kSoundItem, 0), kAll}},
                                           .ctr_version = version});
    XSFLIB_CHECK(links == sar_tools::Archive(flavor, expected));

    // The reader gives the groups' files, and every file's bytes as before.
    const sar::SoundArchive with_copies = Read(copies, flavor);
    const sar::SoundArchive with_links = Read(links, flavor);
    XSFLIB_CHECK((with_copies.GroupFileIds(0) == std::vector<uint32_t>{1, 0, 2}));
    XSFLIB_CHECK((with_links.GroupFileIds(1) == std::vector<uint32_t>{0, 1, 2}));
    for (const uint32_t file : {0u, 1u, 2u})
    {
        XSFLIB_CHECK(std::ranges::equal(with_copies.FileData(file), before.FileData(file)));
        XSFLIB_CHECK(std::ranges::equal(with_links.FileData(file), before.FileData(file)));
    }

    const auto refusal = [&](uint32_t group, std::vector<GroupMember> files)
    {
        return ErrorOf<std::invalid_argument>([&] { sar::WithGroupMembers(archive, flavor, group, files, {}); });
    };
    XSFLIB_CHECK(refusal(3, {}) == "the archive holds no group 3");
    XSFLIB_CHECK(refusal(2, {}) == "group 2 lies beside the archive, in a file of its own");

    // The file table gives the group's file its size with the zeros that pad it.
    sar_files::ArchiveSpec padded = spec;
    padded.files[3].bytes = sar_tools::Group(flavor, {.files = {{1, kBank}}, .ctr_version = version});
    const std::vector<uint8_t> padded_archive = sar_tools::Archive(flavor, padded);
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { sar::WithGroupMembers(padded_archive, flavor, 0, {}, {}); }) ==
                 "an archive whose file table gives the file of group 0 another size than the tools give it");
    XSFLIB_CHECK(refusal(1, {{5, std::nullopt}}) == "a link to file 5: the archive's FILE block doesn't hold it");
    XSFLIB_CHECK(refusal(0, {{1, kBank}}) == "the group holds file 1 already");
}

// Some archives give a file no bytes of its own: the file table names the copy in a group's file. When the group
// takes files, its copies move, and such a file moves with its copy. A file whose bytes lie elsewhere in the group's
// file keeps the group as it is.
void TestMembersNamedInGroups(Flavor flavor, uint32_t version = kCtrFirst)
{
    // File 1 names the copy in GROUP_COPIES' file, file 3.
    const std::vector<uint8_t> archive = sar_tools::Archive(flavor, MembersSpec(flavor, version));
    const sar::SoundArchive before = Read(archive, flavor);
    const std::vector<uint8_t> group = Bytes(before.FileData(3));
    const bool big_endian = flavor == Flavor::kCafe;
    const auto naming = [&](std::size_t offset)
    {
        const sar::Tables tables = sar::TablesOf(archive, flavor);
        sar::InfoBlock info = sar::InfoBlock::Read(tables.info, flavor);
        const uint32_t value = xsflib::Reader(info.files[3], big_endian).U32(16) + static_cast<uint32_t>(offset);
        for (std::size_t k = 0; k < 4; ++k)
        {
            info.files[1][16 + k] = static_cast<uint8_t>(value >> (big_endian ? 24 - 8 * k : 8 * k));
        }

        return sar::WithTables(archive, flavor, sar::StringBlock::Read(*tables.strings, flavor), std::move(info));
    };
    const std::vector<uint8_t> inside = naming(CopyOffset(group, flavor, 0));
    const sar::SoundArchive read = Read(inside, flavor);
    XSFLIB_CHECK(std::ranges::equal(read.FileData(1), kBank));

    // Four new entries move the copies on both consoles.
    const std::vector<GroupMember> members = {{0, Bytes(before.FileData(0))}, {2, kWaves}, {4, kOther}, {5, kOther}};
    const sar::SoundArchive after = Read(sar::WithGroupMembers(inside, flavor, 0, members, {}), flavor);
    const std::vector<uint8_t> grown = Bytes(after.FileData(3));
    XSFLIB_CHECK(CopyOffset(grown, flavor, 0) > CopyOffset(group, flavor, 0));
    XSFLIB_CHECK(after.FileOffset(1) == after.FileOffset(3).value_or(0) + CopyOffset(grown, flavor, 0));
    for (const uint32_t file : {0u, 1u, 2u, 4u})
    {
        XSFLIB_CHECK(std::ranges::equal(after.FileData(file), read.FileData(file)));
    }

    const std::vector<uint8_t> header = naming(4);
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { sar::WithGroupMembers(header, flavor, 0, members, {}); }) ==
                 "an archive laid out another way: file 1 overlaps file 3");
}

// GroupOfFile gives the group beside the archive that the lists of the group file's files all name. A group whose file
// the archive holds doesn't count.
void TestGroupOfFile(Flavor flavor)
{
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "SEQ_A", .file = 0}};
    spec.groups = {{"GROUP_A", sar::kExternalGroupFile}, {"GROUP_B", sar::kExternalGroupFile}, {"GROUP_C", 4}};
    spec.players = {{"PLAYER_A", 4}};
    spec.files = {{.bytes = sar_files::Sequence(flavor, {0xFF}, {{"SEQ_A", 0}}), .groups = {2}},
                  {.group_only = true, .groups = {0}},
                  {.group_only = true, .groups = {0, 1}},
                  {.group_only = true, .groups = {1}},
                  {.bytes = sar_tools::GroupInArchive(flavor, {.files = {{0, std::nullopt}}})}};
    const std::vector<uint8_t> archive = sar_tools::Archive(flavor, spec);
    const auto group_of = [&](const sar_tools::GroupSpec& group)
    {
        return sar::GroupOfFile(archive, flavor, sar_tools::Group(flavor, group));
    };

    XSFLIB_CHECK(group_of({.files = {{1, kBank}, {2, kOther}}}) == 0u);
    XSFLIB_CHECK(group_of({.files = {{2, kOther}, {3, kWaves}}}) == 1u);
    XSFLIB_CHECK(!group_of({.files = {{2, kOther}}}));
    XSFLIB_CHECK(!group_of({.files = {{1, kBank}, {3, kWaves}}}));
    XSFLIB_CHECK(!group_of({.files = {{0, kOther}}}));
    XSFLIB_CHECK(!group_of({.files = {{9, kOther}}}));
    XSFLIB_CHECK(!group_of({}));
    XSFLIB_CHECK(!sar::GroupOfFile(archive, flavor, kWaves));

    // With one group beside the archive, a group's file without files still has none.
    spec.groups.pop_back();
    spec.groups.pop_back();
    spec.files.pop_back();
    spec.files[0].groups.clear();
    XSFLIB_CHECK(!sar::GroupOfFile(sar_tools::Archive(flavor, spec), flavor, sar_tools::Group(flavor, {})));
    XSFLIB_CHECK(sar::GroupOfFile(sar_tools::Archive(flavor, spec), flavor,
                                  sar_tools::Group(flavor, {.files = {{1, kBank}}})) == 0u);
}

// On the 3DS, a new group's file takes version 1.0.0.0 unless the caller gives another. Without INFX, the file table
// gives a group's file the size up to the end of its last copy. The file table holds no lists: WithGroupLists gives
// the archive back, and GroupOfFile finds no group. A group's file of the Wii U or the Switch isn't one of the 3DS.
void TestCtr()
{
    XSFLIB_CHECK(sar::EmptyGroupFile(Flavor::kCtr) == sar_tools::Group(Flavor::kCtr, Empty()));
    XSFLIB_CHECK(sar::EmptyGroupFile(Flavor::kCtr, kCtrItems) == sar_tools::Group(Flavor::kCtr, Empty(kCtrItems)));
    XSFLIB_CHECK(sar::EmptyGroupFile(Flavor::kCtr).size() == 128);

    // The copy of kBank takes 5 bytes, and 27 zeros pad it to 32. A group of links ends where its file does.
    const std::vector<uint8_t> copy = sar_tools::Group(Flavor::kCtr, {.files = {{1, kBank}}});
    const std::vector<uint8_t> links = sar_tools::Group(Flavor::kCtr, Links());
    XSFLIB_CHECK(sar::GroupSizeInArchive(copy, Flavor::kCtr) + 27 == copy.size());
    XSFLIB_CHECK(sar::GroupSizeInArchive(links, Flavor::kCtr) == links.size());

    const std::vector<uint8_t> archive = sar_tools::Archive(Flavor::kCtr, MembersSpec(Flavor::kCtr));
    XSFLIB_CHECK(sar::WithGroupLists(archive, Flavor::kCtr, 0, {{{0, kOther}, {5, kOther}}}) == archive);
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { sar::WithGroupLists(archive, Flavor::kCtr, 0, {{{9, kOther}}}); }) ==
        "the archive holds no file 9");
    XSFLIB_CHECK(!sar::GroupOfFile(archive, Flavor::kCtr, copy));
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>(
                     [] { sar::ReadGroupFile(sar_tools::Group(Flavor::kNx, Copies()), Flavor::kCtr); }) ==
                 "bytes that aren't a group's file of the console");
}

} // namespace

int main()
{
    for (const Flavor flavor : {Flavor::kCafe, Flavor::kNx})
    {
        TestRead(flavor);
        TestGroupWith(flavor);
        TestGroupWithRefusals(flavor);
        TestLists(flavor);
        TestMembers(flavor);
        TestMembersNamedInGroups(flavor);
        TestGroupOfFile(flavor);
    }
    for (const uint32_t version : {kCtrFirst, kCtrItems})
    {
        TestRead(Flavor::kCtr, version);
        TestGroupWith(Flavor::kCtr, version);
        TestGroupWithRefusals(Flavor::kCtr, version);
        TestMembers(Flavor::kCtr, version);
        TestMembersNamedInGroups(Flavor::kCtr, version);
    }
    TestCtr();

    return xsflib::testing::Summary("Groups");
}
