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
#include "sar/file_parts.h"
#include "sar/name_tree.h"

namespace ntdlib::sar
{
namespace
{

constexpr uint16_t kStringTableReference = 0x2400;
constexpr uint16_t kTreeReference = 0x2401;
constexpr uint16_t kStringReference = 0x1F01; // from the string table to a string
constexpr std::size_t kStringEntrySize = 12;
constexpr std::size_t kTableAlignment = 4;

using detail::kBlockAlignment;
using detail::kBlockHeaderSize;
using detail::kTreeLeaf;
using detail::kTreeNodeSize;
using detail::kTreeNone;
using detail::Out;

// Where the string table and the tree start in a block that `r` reads. Throws xsflib::FormatError for bytes that
// aren't a STRG block, and for a block without either.
std::pair<std::size_t, std::size_t> PartsOf(const xsflib::Reader& r)
{
    if (r.Size() < kBlockHeaderSize + 16 || !r.Magic(0, "STRG"))
    {
        throw xsflib::FormatError("not a STRG block");
    }

    std::optional<std::size_t> table;
    std::optional<std::size_t> tree;
    for (std::size_t reference = kBlockHeaderSize; reference < kBlockHeaderSize + 16; reference += 8)
    {
        const int64_t at = int64_t{kBlockHeaderSize} + r.S32(reference + 4);
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

// The tree of `names`: the root's index and the nodes. The names go in sorted by the rank of the item type (TreeRank),
// then by item ID, then by position in `names`. CheckNames has checked the names.
std::pair<uint32_t, std::vector<detail::TreeNode>> BuildTree(std::span<const ItemName> names)
{
    std::vector<uint32_t> order(names.size());
    std::iota(order.begin(), order.end(), 0U);
    auto key = [&](uint32_t string)
    {
        return std::tuple(TreeRank(ItemType(names[string].item_id)), names[string].item_id, string);
    };
    std::ranges::sort(order, {}, key);

    std::vector<detail::TreeName> sorted;
    for (const uint32_t string : order)
    {
        sorted.push_back({names[string].name, string, names[string].item_id});
    }

    return detail::BuildTree(sorted, "two names alike: ");
}

} // namespace

StringBlock StringBlock::Read(std::span<const uint8_t> block, Flavor flavor)
{
    const xsflib::Reader r(block, flavor == Flavor::kCafe);
    const auto [table, tree] = PartsOf(r);
    const uint32_t strings = CountOf(r, table, table + 4, kStringEntrySize, "string table");
    const uint32_t nodes = CountOf(r, tree + 4, tree + 8, kTreeNodeSize, "tree");

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
        const std::size_t node = tree + 8 + std::size_t{i} * kTreeNodeSize;
        if ((r.U16(node) & kTreeLeaf) == 0)
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
    const std::size_t tree = detail::AppendTree(out, root, nodes);

    out.Reference(kBlockHeaderSize, kStringTableReference, static_cast<uint32_t>(table - kBlockHeaderSize));
    out.Reference(kBlockHeaderSize + 8, kTreeReference, static_cast<uint32_t>(tree - kBlockHeaderSize));
    out.Align(kBlockAlignment);
    out.U32(4, static_cast<uint32_t>(out.Size()));

    return out.Take();
}

std::optional<uint32_t> FindItem(std::span<const uint8_t> block, Flavor flavor, std::string_view name)
{
    const xsflib::Reader r(block, flavor == Flavor::kCafe);
    const auto [table, tree] = PartsOf(r);
    const uint32_t strings = CountOf(r, table, table + 4, kStringEntrySize, "string table");
    const uint32_t nodes = CountOf(r, tree + 4, tree + 8, kTreeNodeSize, "tree");
    const uint32_t root = r.U32(tree);
    if (root == kTreeNone)
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

        const std::size_t node = tree + 8 + std::size_t{index} * kTreeNodeSize;
        if (r.U16(node) & kTreeLeaf)
        {
            const uint32_t string = r.U32(node + 12);
            if (string >= strings || StringOf(r, table, string) != name)
            {
                return std::nullopt;
            }

            return r.U32(node + 16);
        }
        index = detail::NameBit(name, r.U16(node + 2)) ? r.U32(node + 8) : r.U32(node + 4);
    }
}

} // namespace ntdlib::sar
