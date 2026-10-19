// SPDX-License-Identifier: MIT

#include "ntdlib/sar/string_block.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

#include "ntdlib/sar/formats.h"
#include "sar/file_bytes.h"

namespace ntdlib::sar
{
namespace
{

constexpr uint16_t kStringTableReference = 0x2400;
constexpr uint16_t kTreeReference = 0x2401;
constexpr uint16_t kStringReference = 0x1F01; // from the string table to a string
constexpr uint16_t kLeaf = 1;                 // a node's flags
constexpr uint16_t kNoBit = 0xFFFF;           // a leaf's bit
constexpr uint32_t kNone = 0xFFFFFFFF;        // a leaf's children, an inner node's string and item, no root
constexpr std::size_t kBody = 8;              // after the block's magic and size
constexpr std::size_t kStringEntrySize = 12;
constexpr std::size_t kNodeSize = 20;
constexpr std::size_t kTableAlignment = 4;
constexpr std::size_t kBlockAlignment = 32;

using detail::Out;

struct Node
{
    uint16_t flags = 0;
    uint16_t bit = kNoBit;
    uint32_t left = kNone;
    uint32_t right = kNone;
    uint32_t string = kNone;
    uint32_t item_id = kNone;
};

// Where the string table and the tree start in a block that `r` reads. Throws xsflib::FormatError for bytes that
// aren't a STRG block, and for a block without either.
std::pair<std::size_t, std::size_t> PartsOf(const xsflib::Reader& r)
{
    if (r.Size() < kBody + 16 || !r.Magic(0, "STRG"))
    {
        throw xsflib::FormatError("not a STRG block");
    }

    std::optional<std::size_t> table;
    std::optional<std::size_t> tree;
    for (std::size_t reference = kBody; reference < kBody + 16; reference += 8)
    {
        const int64_t at = int64_t{kBody} + r.S32(reference + 4);
        if (at < 0 || at > static_cast<int64_t>(r.Size()))
        {
            throw xsflib::FormatError("a STRG block whose references point outside it");
        }
        if (r.U16(reference) == kStringTableReference)
        {
            table = static_cast<std::size_t>(at);
        }
        else if (r.U16(reference) == kTreeReference)
        {
            tree = static_cast<std::size_t>(at);
        }
    }
    if (!table || !tree)
    {
        throw xsflib::FormatError("a STRG block without its string table or its tree");
    }

    return {*table, *tree};
}

// The count of entries of `size` bytes from `at`, after a 32-bit count at `count_at` and up to `at`. Throws
// xsflib::FormatError when they run past the end of what `r` reads. `what` names them in the message.
uint32_t CountOf(const xsflib::Reader& r, std::size_t count_at, std::size_t at, std::size_t size, const char* what)
{
    const uint32_t count = r.U32(count_at);
    if (at > r.Size() || count > (r.Size() - at) / size)
    {
        throw xsflib::FormatError(std::string("a STRG block whose ") + what + " runs past its end");
    }

    return count;
}

// String `index` of the table at `table`. Throws xsflib::FormatError for a string past the block's end.
std::string StringOf(const xsflib::Reader& r, std::size_t table, uint32_t index)
{
    const std::size_t entry = table + 4 + std::size_t{index} * kStringEntrySize;
    const std::size_t start = table + r.U32(entry + 4);
    const uint32_t size = r.U32(entry + 8);
    if (start > r.Size() || size > r.Size() - start)
    {
        throw xsflib::FormatError("a STRG block whose string " + std::to_string(index) + " runs past its end");
    }

    return r.CString(start, size);
}

// Bit `index` of `name`, counted from the most significant bit of its first byte. The bits past its end are 0.
bool Bit(std::string_view name, std::size_t index)
{
    const std::size_t byte = index / 8;

    return byte < name.size() && (static_cast<uint8_t>(name[byte]) >> (7 - index % 8) & 1) != 0;
}

// Where the tree takes the names of items of type `type` among the others. Throws std::invalid_argument for a type
// without names.
int TreeRank(uint32_t type)
{
    switch (type)
    {
    case kSoundItem:
        return 0;
    case kSoundGroupItem:
        return 1;
    case kBankItem:
        return 2;
    case kPlayerItem:
        return 3;
    case kGroupItem:
        return 4;
    case kWaveArchiveItem:
        return 5;
    default:
        throw std::invalid_argument("a name for an item of type " + std::to_string(type) +
                                    ". Items of that type have no names.");
    }
}

// Throws std::invalid_argument for a name with a zero byte, an item ID of a type without names and two names alike.
void CheckNames(std::span<const ItemName> names)
{
    std::vector<std::string_view> sorted;
    sorted.reserve(names.size());
    for (const ItemName& name : names)
    {
        if (name.name.find('\0') != std::string::npos)
        {
            throw std::invalid_argument("a name with a zero byte");
        }
        TreeRank(ItemType(name.item_id));
        sorted.push_back(name.name);
    }

    std::ranges::sort(sorted);
    const auto alike = std::ranges::adjacent_find(sorted);
    if (alike != sorted.end())
    {
        throw std::invalid_argument("two names alike: " + std::string(*alike));
    }
}

// The tree of `names`: the root's index and the nodes. CheckNames has checked the names.
std::pair<uint32_t, std::vector<Node>> BuildTree(std::span<const ItemName> names)
{
    // The order in which the names go in.
    std::vector<uint32_t> order(names.size());
    std::iota(order.begin(), order.end(), 0U);
    auto key = [&](uint32_t string)
    {
        return std::tuple(TreeRank(ItemType(names[string].item_id)), names[string].item_id, string);
    };
    std::ranges::sort(order, {}, key);

    std::vector<Node> nodes;
    uint32_t root = kNone;
    for (const uint32_t string : order)
    {
        const std::string_view name = names[string].name;
        const auto leaf = static_cast<uint32_t>(nodes.size());
        nodes.push_back({kLeaf, kNoBit, kNone, kNone, string, names[string].item_id});
        if (root == kNone)
        {
            root = leaf;
            continue;
        }

        // The first bit where the name differs from the name that its bits lead to. Two names without zero bytes
        // that differ also differ in a bit of the longer one.
        uint32_t closest = root;
        while ((nodes[closest].flags & kLeaf) == 0)
        {
            closest = Bit(name, nodes[closest].bit) ? nodes[closest].right : nodes[closest].left;
        }
        const std::string_view other = names[nodes[closest].string].name;
        std::size_t bit = 0;
        while (Bit(name, bit) == Bit(other, bit))
        {
            ++bit;
        }
        if (bit >= kNoBit)
        {
            throw std::invalid_argument("two names that first differ past bit 65,534: " + std::string(name));
        }

        // The inner node goes on the name's path, above the first node that tests a later bit.
        const auto inner = static_cast<uint32_t>(nodes.size());
        nodes.push_back({0, static_cast<uint16_t>(bit), kNone, kNone, kNone, kNone});
        uint32_t parent = kNone;
        bool parent_right = false;
        uint32_t child = root;
        while ((nodes[child].flags & kLeaf) == 0 && nodes[child].bit < bit)
        {
            parent = child;
            parent_right = Bit(name, nodes[child].bit);
            child = parent_right ? nodes[child].right : nodes[child].left;
        }
        nodes[inner].left = Bit(name, bit) ? child : leaf;
        nodes[inner].right = Bit(name, bit) ? leaf : child;
        if (parent == kNone)
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

} // namespace

StringBlock StringBlock::Read(std::span<const uint8_t> block, Flavor flavor)
{
    const xsflib::Reader r(block, flavor == Flavor::kCafe);
    const auto [table, tree] = PartsOf(r);
    const uint32_t strings = CountOf(r, table, table + 4, kStringEntrySize, "string table");
    const uint32_t nodes = CountOf(r, tree + 4, tree + 8, kNodeSize, "tree");

    StringBlock result;
    result.flavor = flavor;
    result.names.resize(strings);
    for (uint32_t i = 0; i < strings; ++i)
    {
        result.names[i].name = StringOf(r, table, i);
    }

    // Each string's item, from its leaf.
    std::vector<bool> found(strings);
    for (uint32_t i = 0; i < nodes; ++i)
    {
        const std::size_t node = tree + 8 + std::size_t{i} * kNodeSize;
        if ((r.U16(node) & kLeaf) == 0)
        {
            continue;
        }

        const uint32_t string = r.U32(node + 12);
        if (string >= strings)
        {
            throw xsflib::FormatError("a STRG block whose leaf " + std::to_string(i) + " names string " +
                                      std::to_string(string) + " past its table");
        }
        if (found[string])
        {
            throw xsflib::FormatError("a STRG block with two leaves for string " + std::to_string(string));
        }
        found[string] = true;
        result.names[string].item_id = r.U32(node + 16);
    }
    const auto missing = std::ranges::find(found, false);
    if (missing != found.end())
    {
        throw xsflib::FormatError("a STRG block whose string " + std::to_string(missing - found.begin()) +
                                  " has no leaf");
    }

    return result;
}

std::vector<uint8_t> StringBlock::Write() const
{
    CheckNames(names);
    const auto [root, nodes] = BuildTree(names);

    Out out(flavor == Flavor::kCafe);
    out.Append(std::span(reinterpret_cast<const uint8_t*>("STRG"), 4));
    out.Zeros(4 + 16);

    // The string table: the count, the entries, then the strings.
    const std::size_t table = out.Zeros(4 + kStringEntrySize * names.size());
    out.U32(table, static_cast<uint32_t>(names.size()));
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        const std::string& name = names[i].name;
        const std::size_t at = out.Append(std::span(reinterpret_cast<const uint8_t*>(name.data()), name.size()));
        out.Zeros(1);
        const std::size_t entry = table + 4 + i * kStringEntrySize;
        out.Reference(entry, kStringReference, static_cast<uint32_t>(at - table));
        out.U32(entry + 8, static_cast<uint32_t>(name.size() + 1));
    }
    out.Align(kTableAlignment);

    // The tree: the root, the count, then the nodes.
    const std::size_t tree = out.Zeros(8 + kNodeSize * nodes.size());
    out.U32(tree, root);
    out.U32(tree + 4, static_cast<uint32_t>(nodes.size()));
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        const std::size_t at = tree + 8 + i * kNodeSize;
        out.U16(at, nodes[i].flags);
        out.U16(at + 2, nodes[i].bit);
        out.U32(at + 4, nodes[i].left);
        out.U32(at + 8, nodes[i].right);
        out.U32(at + 12, nodes[i].string);
        out.U32(at + 16, nodes[i].item_id);
    }

    out.Reference(kBody, kStringTableReference, static_cast<uint32_t>(table - kBody));
    out.Reference(kBody + 8, kTreeReference, static_cast<uint32_t>(tree - kBody));
    out.Align(kBlockAlignment);
    out.U32(4, static_cast<uint32_t>(out.Size()));

    return out.Take();
}

std::optional<uint32_t> FindItem(std::span<const uint8_t> block, Flavor flavor, std::string_view name)
{
    const xsflib::Reader r(block, flavor == Flavor::kCafe);
    const auto [table, tree] = PartsOf(r);
    const uint32_t strings = CountOf(r, table, table + 4, kStringEntrySize, "string table");
    const uint32_t nodes = CountOf(r, tree + 4, tree + 8, kNodeSize, "tree");
    const uint32_t root = r.U32(tree);
    if (root == kNone)
    {
        return std::nullopt;
    }

    // Down from the root to a leaf. A walk of more steps than the tree has nodes goes round in a loop.
    uint32_t index = root;
    for (uint32_t steps = 0;; ++steps)
    {
        if (index >= nodes || steps >= nodes)
        {
            throw xsflib::FormatError("a STRG block whose tree can't be walked");
        }

        const std::size_t node = tree + 8 + std::size_t{index} * kNodeSize;
        if (r.U16(node) & kLeaf)
        {
            const uint32_t string = r.U32(node + 12);
            if (string >= strings || StringOf(r, table, string) != name)
            {
                return std::nullopt;
            }

            return r.U32(node + 16);
        }
        index = Bit(name, r.U16(node + 2)) ? r.U32(node + 8) : r.U32(node + 4);
    }
}

} // namespace ntdlib::sar
