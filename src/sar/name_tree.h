// SPDX-License-Identifier: MIT

// The Patricia tree of names that NintendoWare's tools write: in the STRG block of an archive of the 3DS, the Wii U and
// the Switch (ntdlib/sar/string_block.h), and in the SYMB block of an archive of the Wii. A node takes 20 bytes: its
// flags, the bit that an inner node tests, its two children, and a leaf's string index and item.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sar/file_bytes.h"

namespace ntdlib::sar::detail
{

inline constexpr uint16_t kTreeLeaf = 1;          // a node's flags
inline constexpr uint16_t kTreeNoBit = 0xFFFF;    // a leaf's bit
inline constexpr uint32_t kTreeNone = 0xFFFFFFFF; // a leaf's children, an inner node's string and item, no root
inline constexpr std::size_t kTreeNodeSize = 20;

// A node of the tree.
struct TreeNode
{
    uint16_t flags = 0;
    uint16_t bit = kTreeNoBit;
    uint32_t left = kTreeNone;
    uint32_t right = kTreeNone;
    uint32_t string = kTreeNone;
    uint32_t item = kTreeNone;
};

// A name that goes into the tree: the name, its string index and its item.
struct TreeName
{
    std::string_view name;
    uint32_t string = 0;
    uint32_t item = 0;
};

// Bit `index` of `name`, from the most significant bit of its first byte. The bits past its end are 0.
inline bool NameBit(std::string_view name, std::size_t index)
{
    const std::size_t byte = index / 8;

    return byte < name.size() && (static_cast<uint8_t>(name[byte]) >> (7 - index % 8) & 1) != 0;
}

// The tree of `names`: the root's index and the nodes. The names go in in their order. The first name adds its leaf.
// Each later name adds its leaf and an inner node on its path, above the first node that tests a later bit. The callers
// reject names with a zero byte. Two names without zero bytes that differ also differ in a bit of the longer one.
// Throws std::invalid_argument for two names alike, with `alike` before the name in the message, and for two names
// that first differ past bit 65,534.
inline std::pair<uint32_t, std::vector<TreeNode>> BuildTree(std::span<const TreeName> names, const std::string& alike)
{
    std::vector<TreeNode> nodes;
    std::vector<std::string_view> leaf_names; // by node, for the leaves
    uint32_t root = kTreeNone;
    for (const TreeName& name : names)
    {
        const auto leaf = static_cast<uint32_t>(nodes.size());
        nodes.push_back({kTreeLeaf, kTreeNoBit, kTreeNone, kTreeNone, name.string, name.item});
        leaf_names.resize(nodes.size());
        leaf_names[leaf] = name.name;
        if (root == kTreeNone)
        {
            root = leaf;
            continue;
        }

        // The first bit where the name differs from the name that its bits lead to.
        uint32_t closest = root;
        while ((nodes[closest].flags & kTreeLeaf) == 0)
        {
            closest = NameBit(name.name, nodes[closest].bit) ? nodes[closest].right : nodes[closest].left;
        }
        const std::string_view closest_name = leaf_names[closest];
        if (closest_name == name.name)
        {
            throw std::invalid_argument(alike + std::string(name.name));
        }
        std::size_t bit = 0;
        while (NameBit(name.name, bit) == NameBit(closest_name, bit))
        {
            ++bit;
        }
        if (bit >= kTreeNoBit)
        {
            throw std::invalid_argument("two names that first differ past bit 65,534: " + std::string(name.name));
        }

        // The inner node goes on the name's path, above the first node that tests a later bit.
        const auto inner = static_cast<uint32_t>(nodes.size());
        nodes.push_back({0, static_cast<uint16_t>(bit), kTreeNone, kTreeNone, kTreeNone, kTreeNone});
        uint32_t parent = kTreeNone;
        bool parent_right = false;
        uint32_t child = root;
        while ((nodes[child].flags & kTreeLeaf) == 0 && nodes[child].bit < bit)
        {
            parent = child;
            parent_right = NameBit(name.name, nodes[child].bit);
            child = parent_right ? nodes[child].right : nodes[child].left;
        }
        nodes[inner].left = NameBit(name.name, bit) ? child : leaf;
        nodes[inner].right = NameBit(name.name, bit) ? leaf : child;
        if (parent == kTreeNone)
        {
            root = inner;
        }
        else
        {
            (parent_right ? nodes[parent].right : nodes[parent].left) = inner;
        }
    }

    return {root, std::move(nodes)};
}

// Appends the tree with `root` and `nodes` to `out`: the root's index, the count of nodes, then the nodes. Returns
// where the tree starts.
inline std::size_t AppendTree(Out& out, uint32_t root, std::span<const TreeNode> nodes)
{
    const std::size_t tree = out.Zeros(8 + kTreeNodeSize * nodes.size());
    out.U32(tree, root);
    out.U32(tree + 4, static_cast<uint32_t>(nodes.size()));
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        const std::size_t at = tree + 8 + i * kTreeNodeSize;
        out.U16(at, nodes[i].flags);
        out.U16(at + 2, nodes[i].bit);
        out.U32(at + 4, nodes[i].left);
        out.U32(at + 8, nodes[i].right);
        out.U32(at + 12, nodes[i].string);
        out.U32(at + 16, nodes[i].item);
    }

    return tree;
}

} // namespace ntdlib::sar::detail
