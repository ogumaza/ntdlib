// SPDX-License-Identifier: MIT

#include "ntdlib/sar/groups.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/info_block.h"
#include "ntdlib/sar/reference.h"
#include "ntdlib/sar/string_block.h"
#include "ntdlib/sar/tables.h"
#include "sar/file_bytes.h"

namespace ntdlib::sar
{
namespace
{

constexpr uint16_t kFileBlock = 0x2002;
constexpr uint16_t kGroupInfoBlock = 0x7800;
constexpr uint16_t kGroupFileBlock = 0x7801;
constexpr uint16_t kGroupItemBlock = 0x7802; // INFX
constexpr uint16_t kFileEntry = 0x7900;      // a reference to an entry of INFO
constexpr uint16_t kItemEntry = 0x7901;      // a reference to an entry of INFX
constexpr uint16_t kDataReference = 0x1F00;  // a copy's place in the FILE block
constexpr uint16_t kInternalFile = 0x220C;   // from a file's entry to its place in the FILE block
constexpr uint16_t kListReference = 0x0100;  // a list of 32-bit values
constexpr uint32_t kNone = 0xFFFFFFFF;
constexpr std::size_t kHeaderSize = 0x40;
constexpr std::size_t kSizeField = 0x0C;
constexpr std::size_t kBlockCount = 0x10;
constexpr std::size_t kBlockTable = 0x14;
constexpr std::size_t kBlockEntrySize = 12; // a type, two bytes of padding, an offset and a size
constexpr std::size_t kBlockHeaderSize = 8; // a block's magic and size
constexpr std::size_t kInfoAlignment = 32;
constexpr std::size_t kFileEntrySize = 16; // an entry of INFO
constexpr std::size_t kItemEntrySize = 8;  // an entry of INFX
constexpr std::size_t kReferenceSize = 8;

// The versions of a group's file: 1.0.0 on the Wii U and the Switch, with INFX. On the 3DS, 1.0.0.0 without INFX, and
// 1.1.0.0 with it.
constexpr uint32_t kGroupVersion = 0x00010000;
constexpr uint32_t kCtrGroupVersion = 0x01000000;
constexpr uint32_t kCtrItemsVersion = 0x01010000;

using detail::Out;

// The magic of a group's file of `flavor`.
const char* MagicOf(Flavor flavor)
{
    return flavor == Flavor::kCtr ? "CGRP" : "FGRP";
}

// Whether a group's file of `flavor` and `version` holds an INFX block.
bool HasItems(Flavor flavor, uint32_t version)
{
    return flavor != Flavor::kCtr || version >= kCtrItemsVersion;
}

// The alignment of the copies in a group's file and of the group's end: that of an archive's files.
std::size_t AlignmentOf(Flavor flavor)
{
    return flavor == Flavor::kNx ? 64 : 32;
}

// The blocks of a group's file: where each starts and its size.
struct GroupBlocks
{
    std::size_t info = 0;
    std::size_t file = 0;
    std::optional<std::pair<std::size_t, std::size_t>> items; // INFX
};

// The blocks of the group's file of `flavor` that `r` reads, from the header's table. Throws xsflib::FormatError for
// bytes that aren't a group's file of the console, and for one without INFO or FILE.
GroupBlocks BlocksOf(const xsflib::Reader& r, Flavor flavor)
{
    if (!r.Magic(0, MagicOf(flavor)) || r.U16(4) != 0xFEFF)
    {
        throw xsflib::FormatError("bytes that aren't a group's file of the console");
    }

    std::optional<std::size_t> info;
    std::optional<std::size_t> file;
    GroupBlocks blocks;
    for (std::size_t i = 0; i < r.U16(kBlockCount); ++i)
    {
        const std::size_t entry = kBlockTable + i * kBlockEntrySize;
        const uint16_t type = r.U16(entry);
        const std::size_t offset = r.U32(entry + 4);
        if (type == kGroupInfoBlock)
        {
            info = offset;
        }
        else if (type == kGroupFileBlock)
        {
            file = offset;
        }
        else if (type == kGroupItemBlock)
        {
            blocks.items = {offset, r.U32(entry + 8)};
        }
    }
    if (!info || !file)
    {
        throw xsflib::FormatError("a group's file without an INFO or a FILE block");
    }
    blocks.info = *info;
    blocks.file = *file;

    return blocks;
}

// An entry of a group's INFO: the file's ID, and where its copy starts in the group's file, with its size. A link has
// no place.
struct CopyEntry
{
    uint32_t file = 0;
    std::optional<std::size_t> place;
    uint32_t size = 0;
};

// The entries of the INFO block of the group's file that `r` reads, in order.
std::vector<CopyEntry> EntriesOf(const xsflib::Reader& r, const GroupBlocks& blocks)
{
    const std::size_t info = blocks.info + kBlockHeaderSize;
    const std::size_t body = blocks.file + kBlockHeaderSize;
    std::vector<CopyEntry> entries;
    for (uint32_t i = 0; i < r.U32(info); ++i)
    {
        const std::size_t entry = info + static_cast<uint32_t>(ReadRef(r, info + 4 + kReferenceSize * i).offset);
        CopyEntry& copy = entries.emplace_back();
        copy.file = r.U32(entry);
        const Reference where = ReadRef(r, entry + 4);
        if (where.type == kDataReference && static_cast<uint32_t>(where.offset) != kNone)
        {
            copy.place = body + static_cast<uint32_t>(where.offset);
            copy.size = r.U32(entry + 12);
        }
    }

    return entries;
}

// The group's file that holds `contents`, laid out as the tools lay it out after `header`, the header's 64 bytes. Zeros
// pad INFX when `padded` says so. A file of a version without INFX lists no items.
std::vector<uint8_t> Build(std::span<const uint8_t> header, const GroupContents& contents, Flavor flavor, bool padded)
{
    const bool has_items = HasItems(flavor, xsflib::Reader(header, flavor == Flavor::kCafe).U32(8));
    const std::size_t alignment = AlignmentOf(flavor);
    Out out(flavor == Flavor::kCafe);
    out.Append(header);

    // INFO: the count, a reference to each entry, then the entries. Each copy's offset waits for FILE.
    const std::size_t files = contents.files.size();
    const std::size_t info = out.Append(std::span(reinterpret_cast<const uint8_t*>("INFO"), 4));
    out.Zeros(4 + 4 + (kReferenceSize + kFileEntrySize) * files);
    out.Align(kInfoAlignment);
    out.U32(info + 4, static_cast<uint32_t>(out.Size() - info));
    const std::size_t info_body = info + kBlockHeaderSize;
    out.U32(info_body, static_cast<uint32_t>(files));
    const std::size_t entries = 4 + kReferenceSize * files;
    for (std::size_t i = 0; i < files; ++i)
    {
        const std::size_t entry = entries + kFileEntrySize * i;
        out.Reference(info_body + 4 + kReferenceSize * i, kFileEntry, static_cast<uint32_t>(entry));
        out.U32(info_body + entry, contents.files[i].file);
        out.Reference(info_body + entry + 4, 0, kNone);
        out.U32(info_body + entry + 12, kNone);
    }

    // FILE: the copies, each on a multiple of the alignment, and zeros up to the next one.
    const std::size_t file = out.Append(std::span(reinterpret_cast<const uint8_t*>("FILE"), 4));
    out.Zeros(4);
    for (std::size_t i = 0; i < files; ++i)
    {
        const std::optional<std::vector<uint8_t>>& copy = contents.files[i].copy;
        if (!copy)
        {
            continue;
        }

        out.Align(alignment);
        const std::size_t entry = info_body + entries + kFileEntrySize * i;
        out.Reference(entry + 4, kDataReference, static_cast<uint32_t>(out.Append(*copy) - file - kBlockHeaderSize));
        out.U32(entry + 12, static_cast<uint32_t>(copy->size()));
    }
    out.Align(alignment);
    out.U32(file + 4, static_cast<uint32_t>(out.Size() - file));
    if (!has_items)
    {
        out.U32(kSizeField, static_cast<uint32_t>(out.Size()));
        out.U16(kBlockCount, 2);
        const std::size_t blocks[][2] = {{kGroupInfoBlock, info}, {kGroupFileBlock, file}};
        const std::size_t sizes[] = {file - info, out.Size() - file};
        for (std::size_t i = 0; i < 2; ++i)
        {
            const std::size_t entry = kBlockTable + kBlockEntrySize * i;
            out.U16(entry, static_cast<uint16_t>(blocks[i][0]));
            out.U16(entry + 2, 0);
            out.U32(entry + 4, static_cast<uint32_t>(blocks[i][1]));
            out.U32(entry + 8, static_cast<uint32_t>(sizes[i]));
        }

        return out.Take();
    }

    // INFX: the count, a reference to each entry, then the entries.
    const std::size_t items = contents.items.size();
    const std::size_t infx = out.Append(std::span(reinterpret_cast<const uint8_t*>("INFX"), 4));
    out.Zeros(4 + 4 + (kReferenceSize + kItemEntrySize) * items);
    if (padded)
    {
        out.Align(alignment);
    }
    out.U32(infx + 4, static_cast<uint32_t>(out.Size() - infx));
    const std::size_t infx_body = infx + kBlockHeaderSize;
    out.U32(infx_body, static_cast<uint32_t>(items));
    for (std::size_t i = 0; i < items; ++i)
    {
        const std::size_t entry = 4 + kReferenceSize * items + kItemEntrySize * i;
        out.Reference(infx_body + 4 + kReferenceSize * i, kItemEntry, static_cast<uint32_t>(entry));
        out.U32(infx_body + entry, contents.items[i].item);
        out.U32(infx_body + entry + 4, contents.items[i].load);
    }

    // The header: the group's size and its three blocks.
    out.U32(kSizeField, static_cast<uint32_t>(out.Size()));
    out.U16(kBlockCount, 3);
    const std::size_t blocks[][3] = {
        {kGroupInfoBlock, info, file - info}, {kGroupFileBlock, file, infx - file}, {kGroupItemBlock, infx, 0}};
    for (std::size_t i = 0; i < 3; ++i)
    {
        const std::size_t entry = kBlockTable + kBlockEntrySize * i;
        const std::size_t size = i == 2 ? out.Size() - infx : blocks[i][2];
        out.U16(entry, static_cast<uint16_t>(blocks[i][0]));
        out.U16(entry + 2, 0);
        out.U32(entry + 4, static_cast<uint32_t>(blocks[i][1]));
        out.U32(entry + 8, static_cast<uint32_t>(size));
    }

    return out.Take();
}

// Where INFX's entries end in `group`, a group's file laid out as the tools lay it out. In a version without INFX, the
// end of the last copy, or the file's end without copies: the zeros after the last copy pad the file.
std::size_t ItemsEnd(std::span<const uint8_t> group, Flavor flavor)
{
    const xsflib::Reader r(group, flavor == Flavor::kCafe);
    const GroupBlocks blocks = BlocksOf(r, flavor);
    if (!HasItems(flavor, r.U32(8)) && !blocks.items)
    {
        std::optional<std::size_t> end;
        for (const CopyEntry& entry : EntriesOf(r, blocks))
        {
            if (entry.place)
            {
                end = std::max(end.value_or(0), *entry.place + entry.size);
            }
        }

        return end.value_or(r.U32(kSizeField));
    }
    if (!blocks.items)
    {
        throw xsflib::FormatError("a group's file without an INFX block");
    }
    const std::size_t body = blocks.items->first + kBlockHeaderSize;

    return body + 4 + (kReferenceSize + kItemEntrySize) * std::size_t{r.U32(body)};
}

// The groups that the list of `entry`, a file's entry of the file table, names. Nothing for an entry that holds no
// list: an external file's. Throws xsflib::FormatError for an entry that ends before its list does.
std::optional<std::vector<uint32_t>> ListOf(std::span<const uint8_t> entry, bool big_endian)
{
    const xsflib::Reader r(entry, big_endian);
    if (r.U16(0) != kInternalFile)
    {
        return std::nullopt;
    }

    const std::size_t location = r.U32(4);
    const Reference list = ReadRef(r, location + 12);
    if (list.type != kListReference)
    {
        return std::nullopt;
    }

    const std::size_t at = location + static_cast<uint32_t>(list.offset);
    std::vector<uint32_t> groups;
    for (uint32_t k = 0; k < r.U32(at); ++k)
    {
        groups.push_back(r.U32(at + 4 + 4 * std::size_t{k}));
    }

    return groups;
}

} // namespace

std::vector<uint8_t> EmptyGroupFile(Flavor flavor, std::optional<uint32_t> version)
{
    // The header: the magic, the byte order mark, the header's size and the version. Build gives the rest.
    Out header(flavor == Flavor::kCafe);
    header.Append(std::span(reinterpret_cast<const uint8_t*>(MagicOf(flavor)), 4));
    header.Zeros(kHeaderSize - 4);
    header.U16(4, 0xFEFF);
    header.U16(6, static_cast<uint16_t>(kHeaderSize));
    header.U32(8, version.value_or(flavor == Flavor::kCtr ? kCtrGroupVersion : kGroupVersion));

    return Build(header.Take(), {}, flavor, true);
}

std::size_t GroupSizeInArchive(std::span<const uint8_t> group, Flavor flavor)
{
    return ItemsEnd(group, flavor);
}

GroupContents ReadGroupFile(std::span<const uint8_t> group, Flavor flavor)
{
    const xsflib::Reader r(group, flavor == Flavor::kCafe);
    const GroupBlocks blocks = BlocksOf(r, flavor);

    // INFO's entries give each file's ID, and the copy's place and size. A link has no place.
    GroupContents contents;
    for (const CopyEntry& entry : EntriesOf(r, blocks))
    {
        GroupMember& member = contents.files.emplace_back();
        member.file = entry.file;
        if (entry.place)
        {
            const std::span<const uint8_t> copy = r.Sub(*entry.place, entry.size);
            member.copy.emplace(copy.begin(), copy.end());
        }
    }

    // INFX's entries give each item's ID and the parts of it that the group loads.
    if (blocks.items)
    {
        const std::size_t items = blocks.items->first + kBlockHeaderSize;
        for (uint32_t i = 0; i < r.U32(items); ++i)
        {
            const std::size_t entry = items + static_cast<uint32_t>(ReadRef(r, items + 4 + kReferenceSize * i).offset);
            contents.items.push_back({r.U32(entry), r.U32(entry + 4)});
        }
    }

    return contents;
}

std::vector<uint8_t> GroupWith(std::span<const uint8_t> group, Flavor flavor, std::span<const GroupMember> files,
                               std::span<const GroupItem> items)
{
    // The group's file has to come back as the tools lay it out. Zeros pad INFX unless it ends where its entries end.
    // INFX starts on a multiple of 32 bytes, and its entries never end on one.
    GroupContents contents = ReadGroupFile(group, flavor);
    const xsflib::Reader r(group, flavor == Flavor::kCafe);
    const GroupBlocks blocks = BlocksOf(r, flavor);
    const bool has_items = HasItems(flavor, r.U32(8));
    if ((has_items && !blocks.items) || group.size() < kHeaderSize)
    {
        throw xsflib::FormatError("a group's file laid out another way than the tools lay it out: it lacks INFX");
    }
    const bool padded = has_items && blocks.items->first + blocks.items->second != ItemsEnd(group, flavor);
    const std::span<const uint8_t> header = group.first(kHeaderSize);
    if (!std::ranges::equal(Build(header, contents, flavor, padded), group))
    {
        throw xsflib::FormatError("a group's file laid out another way than the tools lay it out");
    }

    // The new files have to be all copies or all links, of the kind that the group holds.
    const auto has_copy = [](const GroupMember& member)
    {
        return member.copy.has_value();
    };
    std::optional<bool> copies;
    if (!contents.files.empty())
    {
        copies = has_copy(contents.files.front());
    }
    for (const GroupMember& member : files)
    {
        const std::string name = "file " + std::to_string(member.file);
        const auto same = [&](const GroupMember& held)
        {
            return held.file == member.file;
        };
        if (std::ranges::any_of(contents.files, same))
        {
            throw std::invalid_argument("the group holds " + name + " already");
        }
        if (copies && *copies != has_copy(member))
        {
            throw std::invalid_argument(*copies ? "a link to " + name + " for a group that holds copies"
                                                : "a copy of " + name + " for a group that links to its files");
        }

        copies = has_copy(member);
        contents.files.push_back(member);
    }
    for (const GroupItem& item : items)
    {
        const auto same = [&](const GroupItem& held)
        {
            return held.item == item.item;
        };
        if (std::ranges::any_of(contents.items, same))
        {
            throw std::invalid_argument("the group lists item " + std::to_string(item.item) + " already");
        }

        if (has_items)
        {
            contents.items.push_back(item);
        }
    }

    return Build(header, contents, flavor, padded);
}

std::vector<uint8_t> WithGroupLists(std::span<const uint8_t> archive, Flavor flavor, uint32_t group,
                                    std::span<const GroupMember> files)
{
    const bool big_endian = flavor == Flavor::kCafe;
    const Tables tables = TablesOf(archive, flavor);
    std::optional<StringBlock> strings;
    if (tables.strings)
    {
        strings = StringBlock::Read(*tables.strings, flavor);
    }
    InfoBlock info = InfoBlock::Read(tables.info, flavor);
    if (group >= info.groups.size())
    {
        throw std::invalid_argument("the archive holds no group " + std::to_string(group));
    }

    // The 3DS's file table holds no lists.
    if (flavor == Flavor::kCtr)
    {
        for (const GroupMember& member : files)
        {
            if (member.file >= info.files.size())
            {
                throw std::invalid_argument("the archive holds no file " + std::to_string(member.file));
            }
        }

        return std::vector<uint8_t>(archive.begin(), archive.end());
    }

    // A link to a wave archive's file leaves the file's list as it is.
    std::set<uint32_t> wave_archives;
    for (const std::vector<uint8_t>& entry : info.wave_archives)
    {
        wave_archives.insert(xsflib::Reader(entry, big_endian).U32(0));
    }

    const uint32_t id = ItemId(kGroupItem, group);
    for (const GroupMember& member : files)
    {
        const std::string name = "file " + std::to_string(member.file);
        if (member.file >= info.files.size())
        {
            throw std::invalid_argument("the archive holds no " + name);
        }
        if (!member.copy && wave_archives.contains(member.file))
        {
            continue;
        }

        // The list ends the entry, after the file's location.
        std::vector<uint8_t>& entry = info.files[member.file];
        const xsflib::Reader r(entry, big_endian);
        if (r.U16(0) != kInternalFile)
        {
            throw std::invalid_argument(name + " lies outside the archive");
        }
        std::optional<std::vector<uint32_t>> named = ListOf(entry, big_endian);
        const std::size_t location = r.U32(4);
        const std::size_t at = location + static_cast<uint32_t>(ReadRef(r, location + 12).offset);
        if (!named || at + 4 + 4 * named->size() != entry.size())
        {
            throw xsflib::FormatError("an archive laid out another way: the entry of " + name +
                                      " holds no list of groups, or bytes after it");
        }
        if (std::ranges::find(*named, id) != named->end())
        {
            continue;
        }

        named->insert(std::ranges::find_if(*named, [id](uint32_t other) { return other > id; }), id);
        Out out(big_endian);
        out.Append(std::span(entry).first(at));
        out.Zeros(4 + 4 * named->size());
        out.U32(at, static_cast<uint32_t>(named->size()));
        for (std::size_t k = 0; k < named->size(); ++k)
        {
            out.U32(at + 4 + 4 * k, (*named)[k]);
        }
        entry = out.Take();
    }

    return WithTables(archive, flavor, strings, std::move(info));
}

std::vector<uint8_t> WithGroupMembers(std::span<const uint8_t> archive, Flavor flavor, uint32_t group,
                                      std::span<const GroupMember> files, std::span<const GroupItem> items)
{
    const bool big_endian = flavor == Flavor::kCafe;
    const InfoBlock info = InfoBlock::Read(TablesOf(archive, flavor).info, flavor);
    const std::string name = "group " + std::to_string(group);
    if (group >= info.groups.size())
    {
        throw std::invalid_argument("the archive holds no " + name);
    }
    const uint32_t file = xsflib::Reader(info.groups[group], big_endian).U32(0);
    if (file == kExternalGroupFile)
    {
        throw std::invalid_argument(name + " lies beside the archive, in a file of its own");
    }
    if (file >= info.files.size())
    {
        throw xsflib::FormatError("an archive whose " + name + " names a file past the file table");
    }

    // Where the FILE block holds a file: its offset from the block's body, and its size. Nothing for a file that the
    // block doesn't hold.
    const xsflib::Reader r(archive, big_endian);
    std::optional<std::size_t> body;
    for (std::size_t i = 0; i < r.U16(kBlockCount); ++i)
    {
        const std::size_t entry = kBlockTable + i * kBlockEntrySize;
        if (r.U16(entry) == kFileBlock)
        {
            body = std::size_t{r.U32(entry + 4)} + kBlockHeaderSize;
        }
    }
    if (!body)
    {
        throw xsflib::FormatError("an archive without a FILE block");
    }
    const auto location_of = [&](uint32_t id) -> std::optional<std::pair<std::size_t, std::size_t>>
    {
        const xsflib::Reader entry(info.files[id], big_endian);
        if (entry.U16(0) != kInternalFile)
        {
            return std::nullopt;
        }

        const std::size_t location = entry.U32(4);
        const uint32_t offset = entry.U32(location + 4);
        if (offset == kNone)
        {
            return std::nullopt;
        }

        return std::pair<std::size_t, std::size_t>{*body + offset, entry.U32(location + 8)};
    };

    // The group's file: the header gives its size, and the file table the end of INFX's entries, or the end of the last
    // copy without INFX. A group's file that ends the archive ends without the zeros that pad it.
    const std::optional<std::pair<std::size_t, std::size_t>> place = location_of(file);
    if (!place)
    {
        throw xsflib::FormatError("an archive whose FILE block doesn't hold the file of " + name);
    }
    const std::size_t size = r.U32(place->first + kSizeField);
    const bool last = place->first + place->second == archive.size();
    std::vector<uint8_t> old(size);
    const std::span<const uint8_t> held = r.Sub(place->first, last ? std::min(size, place->second) : size);
    std::ranges::copy(held, old.begin());
    if (place->second != ItemsEnd(old, flavor))
    {
        throw xsflib::FormatError("an archive whose file table gives the file of " + name +
                                  " another size than the tools give it");
    }

    // The game loads a linked file from the archive.
    for (const GroupMember& member : files)
    {
        if (!member.copy && (member.file >= info.files.size() || !location_of(member.file)))
        {
            throw std::invalid_argument("a link to file " + std::to_string(member.file) +
                                        ": the archive's FILE block doesn't hold it");
        }
    }

    // A file whose entry names its copy in the group's file moves with the copy. GroupWith keeps the copies in their
    // order, and the new ones follow them.
    const std::vector<uint8_t> grown = GroupWith(old, flavor, files, items);
    const xsflib::Reader old_reader(old, big_endian);
    const xsflib::Reader grown_reader(grown, big_endian);
    const std::vector<CopyEntry> from = EntriesOf(old_reader, BlocksOf(old_reader, flavor));
    const std::vector<CopyEntry> to = EntriesOf(grown_reader, BlocksOf(grown_reader, flavor));
    std::vector<InnerFile> inner;
    for (std::size_t k = 0; k < from.size() && k < to.size(); ++k)
    {
        if (from[k].place && to[k].place)
        {
            inner.push_back({static_cast<uint32_t>(*from[k].place), static_cast<uint32_t>(*to[k].place)});
        }
    }
    const std::vector<uint8_t> moved =
        WithFileBytes(archive, flavor, file, std::span(grown).first(ItemsEnd(grown, flavor)), inner);

    return WithGroupLists(moved, flavor, group, files);
}

std::optional<uint32_t> GroupOfFile(std::span<const uint8_t> archive, Flavor flavor,
                                    std::span<const uint8_t> group_file)
{
    // The 3DS's file table holds no lists to find the group by.
    if (flavor == Flavor::kCtr)
    {
        return std::nullopt;
    }

    GroupContents contents;
    try
    {
        contents = ReadGroupFile(group_file, flavor);
    }
    catch (const xsflib::FormatError&)
    {
        return std::nullopt;
    }
    if (contents.files.empty())
    {
        return std::nullopt;
    }

    // The groups that the game keeps beside the archive, and the lists of the group file's files.
    const bool big_endian = flavor == Flavor::kCafe;
    const InfoBlock info = InfoBlock::Read(TablesOf(archive, flavor).info, flavor);
    std::vector<std::vector<uint32_t>> lists;
    for (const GroupMember& member : contents.files)
    {
        if (member.file >= info.files.size())
        {
            return std::nullopt;
        }
        lists.push_back(ListOf(info.files[member.file], big_endian).value_or(std::vector<uint32_t>()));
    }

    std::optional<uint32_t> found;
    for (uint32_t group = 0; group < info.groups.size(); ++group)
    {
        const uint32_t id = ItemId(kGroupItem, group);
        const auto names = [id](const std::vector<uint32_t>& list)
        {
            return std::ranges::find(list, id) != list.end();
        };
        if (xsflib::Reader(info.groups[group], big_endian).U32(0) != kExternalGroupFile ||
            !std::ranges::all_of(lists, names))
        {
            continue;
        }
        if (found)
        {
            return std::nullopt;
        }

        found = group;
    }

    return found;
}

} // namespace ntdlib::sar
