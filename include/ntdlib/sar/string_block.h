// SPDX-License-Identifier: MIT

// The STRG block of a sound archive of the 3DS (CSAR), the Wii U or the Switch (FSAR): the names of the archive's
// items. A string table holds the names. A Patricia tree gives an item's ID for its name. The sound library walks the
// tree from its root. Each inner node tests one bit of the name. The bits count from the most significant bit of the
// name's first byte, and the bits past the name's end are 0. A 0 leads left and a 1 leads right. The walk ends at a
// leaf. A leaf holds the index of a string in the table and an item ID. The ID is the name's when that string is the
// name.
//
// The writer lays the block out as NintendoWare's tools do. The block's magic and size come first. Two references
// follow, to the string table and to the tree. Their offsets count from the end of the block's size. The string table
// starts right after the references. It holds its count of strings and an entry for each string: the string's offset
// from the table's start and its size with the zero that ends it. The strings follow in order, each ended by a zero.
// Zeros pad the table to 4 bytes. The tree holds its root's index and its count of nodes, then the nodes, 20 bytes
// each: flags, the bit that the node tests, the left and the right child, a string's index and an item ID. A leaf's
// flags are 1 and an inner node's 0. A leaf's bit is 0xFFFF. Its children, and an inner node's string and item, are
// 0xFFFFFFFF. The names go into the tree in the order of their items' types: sounds, sound groups, banks, players,
// groups and wave archives. Within a type, they go in by the items' indices. The first name adds its leaf. Each later
// name adds its leaf and then an inner node. A tree without names has its root at 0xFFFFFFFF. Zeros pad the block to
// 32 bytes.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/sar/formats.h"

namespace ntdlib::sar
{

// The name of an item, and the item's ID.
struct ItemName
{
    friend bool operator==(const ItemName&, const ItemName&) = default;

    std::string name;
    uint32_t item_id = 0;
};

struct StringBlock
{
    friend bool operator==(const StringBlock&, const StringBlock&) = default;

    // Reads `block`, a STRG block from its magic, in the byte order of `flavor`. Each name takes the item ID of its
    // string's leaf. Throws xsflib::FormatError for bytes that aren't a STRG block, a table or a tree that runs past
    // the block's end, and a string without a leaf or with two. A block laid out another way reads too, and Write()
    // then gives other bytes.
    static StringBlock Read(std::span<const uint8_t> block, Flavor flavor);

    // The block's bytes. Throws std::invalid_argument for a name with a zero byte, two names alike, two names that
    // first differ past bit 65,534, and an item ID of a type without names.
    std::vector<uint8_t> Write() const;

    Flavor flavor = Flavor::kNx;
    std::vector<ItemName> names; // in the string table's order
};

// The ID of the item named `name` in `block`, a STRG block from its magic in the byte order of `flavor`. The walk of
// the tree follows the sound library's. Nothing for a name that the block lacks. Throws xsflib::FormatError for bytes
// that aren't a STRG block, and for a tree whose walk leaves it or goes round in a loop.
std::optional<uint32_t> FindItem(std::span<const uint8_t> block, Flavor flavor, std::string_view name);

} // namespace ntdlib::sar
