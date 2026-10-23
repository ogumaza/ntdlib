// SPDX-License-Identifier: MIT

// The groups of a sound archive of the Wii U or the Switch (FSAR), and their files (FGRP). A game loads a group into
// the sound heap: the files that the group's file lists in its INFO block. The FILE block of the group's file holds a
// copy of each of them, or the group's file links to the archive's copy. The game then loads a linked file from the
// archive. The INFX block lists the items that the group loads: sounds, sound groups, banks and wave archives, each
// with the parts that the group loads. The archive's group table gives each group's file, and the file table gives
// each file a list of the groups that hold it. A game keeps some groups in files beside the archive. The group table
// gives such a group no file. ReadGroupFile reads a group's file, and GroupWith adds files and items to it.
// WithGroupMembers adds them to a group whose file the archive holds. WithGroupLists writes the groups into the files'
// lists. GroupOfFile finds the group of a group's file that a game keeps beside an archive. EmptyGroupFile gives the
// file of a new group, and WithNewGroup (ntdlib/sar/tables.h) adds the group to an archive. GroupSizeInArchive gives
// the size of a group's file in the archive's file table.
//
// NintendoWare's tools lay a group's file out as a header of 64 bytes, then the INFO, FILE and INFX blocks, in that
// order and without gaps. INFO holds the count of files and a reference to each file's entry, then the entries. An
// entry holds a file ID, a reference to the copy from the end of the FILE block's size, and the copy's size. A link
// holds a reference to no data and a size of 0xFFFFFFFF. Zeros pad INFO to a multiple of 32 bytes. FILE holds the
// copies in the order of INFO. Each copy starts on a multiple of 64 bytes from the group's start on the Switch, and of
// 32 bytes on the Wii U. Zeros follow the last copy up to such a multiple, and fill a FILE block without copies up to
// the first one. INFX holds the count of items and a reference to each item's entry, then the entries: an item ID and
// the bits of the parts that the group loads. Zeros pad INFX until the group ends on a multiple of 64 or 32 bytes, but
// a few groups end without them. The header gives the group's size. The archive's file table gives a group's file the
// size up to the end of INFX's entries. A file's list of groups names each group that holds a copy of the file, and
// each group that links to it, but for a wave archive's file. A list can name a group more than once.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/sar/formats.h"

namespace ntdlib::sar
{

// An item that a group loads: its ID, and the bits of the parts of it that the group loads. Bit 0 stands for sequence
// files, bit 1 for wave sound files, bit 2 for banks and bit 3 for wave archives. The tools set every bit for most
// items.
struct GroupItem
{
    friend bool operator==(const GroupItem&, const GroupItem&) = default;

    uint32_t item = 0;
    uint32_t load = 0xFFFFFFFF;
};

// A file that a group holds: its ID, and the bytes of the group's copy. A link has no copy.
struct GroupMember
{
    friend bool operator==(const GroupMember&, const GroupMember&) = default;

    uint32_t file = 0;
    std::optional<std::vector<uint8_t>> copy;
};

// What a group's file holds.
struct GroupContents
{
    friend bool operator==(const GroupContents&, const GroupContents&) = default;

    std::vector<GroupMember> files; // in the order of INFO
    std::vector<GroupItem> items;   // in the order of INFX
};

// A group's file of `flavor` without files or items, in version 1.0.0, laid out as the tools lay a group's file out.
// Every game of the Wii U and the Switch reads that version. Throws std::invalid_argument for the 3DS.
std::vector<uint8_t> EmptyGroupFile(Flavor flavor);

// The size that an archive's file table gives `group`, a group's file of `flavor`: the end of its INFX block's
// entries. The archive's FILE block holds the group's file up to there. Throws std::invalid_argument for the 3DS, and
// xsflib::FormatError for bytes without an INFX block.
std::size_t GroupSizeInArchive(std::span<const uint8_t> group, Flavor flavor);

// The contents of `group`, a group's file of `flavor`. A file without an INFX block lists no items. Throws
// std::invalid_argument for the 3DS. Throws xsflib::FormatError for bytes that aren't a group's file of the console,
// and for a copy that runs past the group's end.
GroupContents ReadGroupFile(std::span<const uint8_t> group, Flavor flavor);

// `group`, a group's file of `flavor`, with `files` after its files and `items` after its items, laid out as the tools
// lay a group's file out. A group that holds copies takes copies, and a group that links takes links. A group without
// files takes either, but not both. INFX keeps its padding, or its lack of padding. Throws std::invalid_argument for
// the 3DS, a file that the group holds already or that `files` holds twice, an item that the group lists already or
// that `items` lists twice, and files of the other kind. Throws xsflib::FormatError for a group's file laid out another
// way than the tools lay it out, and the exceptions of ReadGroupFile. GroupWith(group, flavor, {}, {}) gives every
// other group's file back byte for byte.
std::vector<uint8_t> GroupWith(std::span<const uint8_t> group, Flavor flavor, std::span<const GroupMember> files,
                               std::span<const GroupItem> items);

// `archive`, an archive of `flavor`, in which the list of groups of each file of `files` names group `group`: the file
// of each copy, and of each link but a wave archive's file. A list that names the group already stays as it is. The
// group goes in before the first group with a higher index. The tools keep most lists in that order. Throws
// std::invalid_argument for the 3DS, a group past the group table, and a file past the file table or outside the
// archive. Throws xsflib::FormatError for a file's entry that holds no list or holds bytes after it, and the exceptions
// of WithTables.
std::vector<uint8_t> WithGroupLists(std::span<const uint8_t> archive, Flavor flavor, uint32_t group,
                                    std::span<const GroupMember> files);

// `archive`, an archive of `flavor`, in which group `group`, whose file the archive holds, holds `files` and lists
// `items` after what it holds and lists. The group's file takes them through GroupWith, and keeps its place in the FILE
// block. The files after it move by whole multiples of the files' alignment (WithFileBytes). Some archives give a file
// no bytes of its own: the file table names the copy in a group's file. Such a file moves with the copy. The files'
// lists of groups name the group through WithGroupLists. Throws std::invalid_argument for the 3DS, a group past the
// group table, a group that the game keeps beside the archive, and a link to a file that the FILE block doesn't hold:
// the game loads a linked file from the archive. Throws xsflib::FormatError for a group's file whose size in the file
// table isn't the end of its INFX block's entries, and the exceptions of GroupWith, WithFileBytes and WithGroupLists.
std::vector<uint8_t> WithGroupMembers(std::span<const uint8_t> archive, Flavor flavor, uint32_t group,
                                      std::span<const GroupMember> files, std::span<const GroupItem> items);

// The group of `group_file`, a group's file that a game keeps beside `archive`, an archive of `flavor`: the group that
// the group table gives no file, and that the list of groups of each file of `group_file` names. Nothing when no group
// or more than one fits, for a group's file without files, and for bytes that aren't a group's file of the console.
// Throws std::invalid_argument for the 3DS, and xsflib::FormatError if the archive's tables can't be read.
std::optional<uint32_t> GroupOfFile(std::span<const uint8_t> archive, Flavor flavor,
                                    std::span<const uint8_t> group_file);

} // namespace ntdlib::sar
