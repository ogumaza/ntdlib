// SPDX-License-Identifier: MIT

// Tests of the reader and the writer of a sound archive's STRG block (ntdlib/sar/string_block.h), in each flavor's
// byte order. The bytes that the tests expect follow from the layout that the header describes, worked out by hand.
// FindItem walks the written tree as the sound library does. The tests also check the tree's rule at every inner node:
// the names below it agree on every bit before the node's bit, and the left names have 0 there and the right names 1.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/string_block.h"

using xsflib::testing::failures;

namespace
{

using ntdlib::sar::FindItem;
using ntdlib::sar::Flavor;
using ntdlib::sar::ItemId;
using ntdlib::sar::ItemName;
using ntdlib::sar::StringBlock;

constexpr Flavor kFlavors[] = {Flavor::kCtr, Flavor::kCafe, Flavor::kNx};
constexpr uint32_t kNone = 0xFFFFFFFF;

// Appends `value` to `bytes` in the byte order of `flavor`, `size` bytes of it.
void Put(std::vector<uint8_t>& bytes, Flavor flavor, uint32_t value, int size)
{
    for (int i = 0; i < size; ++i)
    {
        const int shift = flavor == Flavor::kCafe ? 8 * (size - 1 - i) : 8 * i;
        bytes.push_back(static_cast<uint8_t>(value >> shift));
    }
}

void Put16(std::vector<uint8_t>& bytes, Flavor flavor, uint32_t value)
{
    Put(bytes, flavor, value, 2);
}

void Put32(std::vector<uint8_t>& bytes, Flavor flavor, uint32_t value)
{
    Put(bytes, flavor, value, 4);
}

// Appends the bytes of `text` and, with `zero`, the zero that ends it.
void PutText(std::vector<uint8_t>& bytes, const std::string& text, bool zero)
{
    bytes.insert(bytes.end(), text.begin(), text.end());
    if (zero)
    {
        bytes.push_back(0);
    }
}

// Appends a node: its flags, its bit, its children, its string and its item.
void PutNode(std::vector<uint8_t>& bytes, Flavor flavor, uint16_t flags, uint16_t bit, uint32_t left, uint32_t right,
             uint32_t string, uint32_t item)
{
    Put16(bytes, flavor, flags);
    Put16(bytes, flavor, bit);
    Put32(bytes, flavor, left);
    Put32(bytes, flavor, right);
    Put32(bytes, flavor, string);
    Put32(bytes, flavor, item);
}

// The block of the names "B", sound 1, and "A", sound 0, in that order. The tree takes A first: A's leaf is node 0
// and B's is node 1. 'A' (0x41) and 'B' (0x42) first differ at bit 6, counted from the most significant bit. B has 1
// there and goes right. The table takes 4 + 2 * 12 + 4 bytes from byte 24, and the tree 8 + 3 * 20 from byte 56. The
// block ends at 124 and pads to 128.
std::vector<uint8_t> TwoNames(Flavor flavor)
{
    std::vector<uint8_t> bytes;
    PutText(bytes, "STRG", false);
    Put32(bytes, flavor, 128);
    Put16(bytes, flavor, 0x2400);
    Put16(bytes, flavor, 0);
    Put32(bytes, flavor, 16);
    Put16(bytes, flavor, 0x2401);
    Put16(bytes, flavor, 0);
    Put32(bytes, flavor, 48);

    Put32(bytes, flavor, 2);
    Put16(bytes, flavor, 0x1F01);
    Put16(bytes, flavor, 0);
    Put32(bytes, flavor, 28);
    Put32(bytes, flavor, 2);
    Put16(bytes, flavor, 0x1F01);
    Put16(bytes, flavor, 0);
    Put32(bytes, flavor, 30);
    Put32(bytes, flavor, 2);
    PutText(bytes, "B", true);
    PutText(bytes, "A", true);

    Put32(bytes, flavor, 2);
    Put32(bytes, flavor, 3);
    PutNode(bytes, flavor, 1, 0xFFFF, kNone, kNone, 1, ItemId(ntdlib::sar::kSoundItem, 0));
    PutNode(bytes, flavor, 1, 0xFFFF, kNone, kNone, 0, ItemId(ntdlib::sar::kSoundItem, 1));
    PutNode(bytes, flavor, 0, 6, 0, 1, kNone, kNone);
    bytes.resize(128);

    return bytes;
}

StringBlock Block(Flavor flavor, std::vector<ItemName> names)
{
    StringBlock block;
    block.flavor = flavor;
    block.names = std::move(names);

    return block;
}

template <class Error, class Call>
bool Throws(Call call)
{
    try
    {
        call();
    }
    catch (const Error&)
    {
        return true;
    }

    return false;
}

// Bit `index` of `name`, as the header describes it.
bool Bit(const std::string& name, std::size_t index)
{
    return index / 8 < name.size() && (static_cast<uint8_t>(name[index / 8]) >> (7 - index % 8) & 1) != 0;
}

// A reader of the tree of a written block, for the checks of its rule.
class Tree
{
public:
    Tree(const std::vector<uint8_t>& bytes, Flavor flavor) : r_(bytes, flavor == Flavor::kCafe)
    {
        table_ = 8 + static_cast<std::size_t>(r_.S32(12));
        tree_ = 8 + static_cast<std::size_t>(r_.S32(20));
    }

    uint32_t Root() const
    {
        return r_.U32(tree_);
    }

    uint32_t Count() const
    {
        return r_.U32(tree_ + 4);
    }

    uint16_t Flags(uint32_t node) const
    {
        return r_.U16(At(node));
    }

    uint16_t BitOf(uint32_t node) const
    {
        return r_.U16(At(node) + 2);
    }

    uint32_t Left(uint32_t node) const
    {
        return r_.U32(At(node) + 4);
    }

    uint32_t Right(uint32_t node) const
    {
        return r_.U32(At(node) + 8);
    }

    uint32_t String(uint32_t node) const
    {
        return r_.U32(At(node) + 12);
    }

    // The names of the leaves below `node`. Checks the tree's rule at each inner node on the way, and counts the leaves
    // in `leaves`.
    std::vector<std::string> NamesBelow(uint32_t node, std::vector<int>& leaves) const
    {
        if (Flags(node) == 1)
        {
            ++leaves[node];
            const std::size_t entry = table_ + 4 + std::size_t{String(node)} * 12;
            return {r_.CString(table_ + r_.U32(entry + 4), r_.U32(entry + 8))};
        }

        std::vector<std::string> left = NamesBelow(Left(node), leaves);
        const std::vector<std::string> right = NamesBelow(Right(node), leaves);
        const uint16_t bit = BitOf(node);
        for (const std::string& name : left)
        {
            XSFLIB_CHECK(!Bit(name, bit));
        }
        for (const std::string& name : right)
        {
            XSFLIB_CHECK(Bit(name, bit));
        }
        left.insert(left.end(), right.begin(), right.end());
        for (const std::string& name : left)
        {
            for (std::size_t k = 0; k < bit; ++k)
            {
                if (Bit(name, k) != Bit(left[0], k))
                {
                    XSFLIB_CHECK(false);
                    break;
                }
            }
        }

        return left;
    }

private:
    std::size_t At(uint32_t node) const
    {
        return tree_ + 8 + std::size_t{node} * 20;
    }

    xsflib::Reader r_;
    std::size_t table_ = 0;
    std::size_t tree_ = 0;
};

// Checks the rule at every inner node of the tree of `bytes`, and that each leaf hangs once from the root.
void CheckTree(const std::vector<uint8_t>& bytes, Flavor flavor, std::size_t names)
{
    const Tree tree(bytes, flavor);
    XSFLIB_CHECK(tree.Count() == (names == 0 ? 0 : 2 * names - 1));
    if (names == 0)
    {
        XSFLIB_CHECK(tree.Root() == kNone);
        return;
    }

    std::vector<int> leaves(tree.Count());
    const std::vector<std::string> below = tree.NamesBelow(tree.Root(), leaves);
    XSFLIB_CHECK(below.size() == names);
    for (uint32_t node = 0; node < tree.Count(); ++node)
    {
        XSFLIB_CHECK(leaves[node] == (tree.Flags(node) == 1 ? 1 : 0));
    }
}

// The block of two names, byte for byte, read back, and found.
void TestTwoNames(Flavor flavor)
{
    const StringBlock block =
        Block(flavor, {{"B", ItemId(ntdlib::sar::kSoundItem, 1)}, {"A", ItemId(ntdlib::sar::kSoundItem, 0)}});

    const std::vector<uint8_t> bytes = block.Write();

    XSFLIB_CHECK(bytes == TwoNames(flavor));
    XSFLIB_CHECK(StringBlock::Read(bytes, flavor) == block);
    XSFLIB_CHECK(FindItem(bytes, flavor, "A") == ItemId(ntdlib::sar::kSoundItem, 0));
    XSFLIB_CHECK(FindItem(bytes, flavor, "B") == ItemId(ntdlib::sar::kSoundItem, 1));
    XSFLIB_CHECK(!FindItem(bytes, flavor, "C") && !FindItem(bytes, flavor, "AB") && !FindItem(bytes, flavor, ""));
}

// A block of one name, "A". The table takes 4 + 12 + 2 bytes from byte 24 and ends at 42. Zeros pad it to 44, where the
// tree starts. The tree's root is the name's leaf. The tree ends at 72, and the block pads to 96.
void TestOneName(Flavor flavor)
{
    std::vector<uint8_t> expected;
    PutText(expected, "STRG", false);
    Put32(expected, flavor, 96);
    Put16(expected, flavor, 0x2400);
    Put16(expected, flavor, 0);
    Put32(expected, flavor, 16);
    Put16(expected, flavor, 0x2401);
    Put16(expected, flavor, 0);
    Put32(expected, flavor, 36);
    Put32(expected, flavor, 1);
    Put16(expected, flavor, 0x1F01);
    Put16(expected, flavor, 0);
    Put32(expected, flavor, 16);
    Put32(expected, flavor, 2);
    PutText(expected, "A", true);
    expected.resize(44);
    Put32(expected, flavor, 0);
    Put32(expected, flavor, 1);
    PutNode(expected, flavor, 1, 0xFFFF, kNone, kNone, 0, ItemId(ntdlib::sar::kBankItem, 3));
    expected.resize(96);

    const std::vector<uint8_t> bytes = Block(flavor, {{"A", ItemId(ntdlib::sar::kBankItem, 3)}}).Write();

    XSFLIB_CHECK(bytes == expected);
    XSFLIB_CHECK(FindItem(bytes, flavor, "A") == ItemId(ntdlib::sar::kBankItem, 3));
    XSFLIB_CHECK(!FindItem(bytes, flavor, "B") && !FindItem(bytes, flavor, ""));
}

// A block without names: the table's count, then the tree's root and count. The tree starts at byte 28 and ends at 36.
// The block pads to 64.
void TestNoNames(Flavor flavor)
{
    std::vector<uint8_t> expected;
    PutText(expected, "STRG", false);
    Put32(expected, flavor, 64);
    Put16(expected, flavor, 0x2400);
    Put16(expected, flavor, 0);
    Put32(expected, flavor, 16);
    Put16(expected, flavor, 0x2401);
    Put16(expected, flavor, 0);
    Put32(expected, flavor, 20);
    Put32(expected, flavor, 0);
    Put32(expected, flavor, kNone);
    Put32(expected, flavor, 0);
    expected.resize(64);

    const std::vector<uint8_t> bytes = Block(flavor, {}).Write();

    XSFLIB_CHECK(bytes == expected);
    XSFLIB_CHECK(StringBlock::Read(bytes, flavor).names.empty());
    XSFLIB_CHECK(!FindItem(bytes, flavor, "A") && !FindItem(bytes, flavor, ""));
}

// The tree takes sounds, sound groups, banks, players, groups and wave archives in that order, each type by its
// items' indices, whatever the order of the strings.
void TestOrderOfTypes()
{
    using namespace ntdlib::sar;
    const StringBlock block = Block(Flavor::kNx, {{"WARC", ItemId(kWaveArchiveItem, 0)},
                                                  {"GROUP", ItemId(kGroupItem, 0)},
                                                  {"PLAYER", ItemId(kPlayerItem, 0)},
                                                  {"BANK", ItemId(kBankItem, 0)},
                                                  {"SOUND_GROUP", ItemId(kSoundGroupItem, 0)},
                                                  {"SE_B", ItemId(kSoundItem, 1)},
                                                  {"SE_A", ItemId(kSoundItem, 0)}});

    const std::vector<uint8_t> bytes = block.Write();
    const Tree tree(bytes, Flavor::kNx);

    // Leaves at 0, 1, 3, 5 and on: each name after the first adds its leaf and then an inner node.
    const uint32_t expected[] = {6, 5, 4, 3, 2, 1, 0};
    const uint32_t places[] = {0, 1, 3, 5, 7, 9, 11};
    for (std::size_t k = 0; k < 7; ++k)
    {
        XSFLIB_CHECK(tree.Flags(places[k]) == 1 && tree.String(places[k]) == expected[k]);
    }
    CheckTree(bytes, Flavor::kNx, 7);
}

// Names that hold others at their start, and an empty name. The bits past a name's end are 0.
void TestPrefixes(Flavor flavor)
{
    const std::vector<std::string> names = {"SE_AB", "SE", "S", "", "SE_A", "SE_B", "SE_A0"};
    std::vector<ItemName> items;
    for (std::size_t k = 0; k < names.size(); ++k)
    {
        items.push_back({names[k], ItemId(ntdlib::sar::kSoundItem, static_cast<uint32_t>(k))});
    }

    const std::vector<uint8_t> bytes = Block(flavor, items).Write();

    for (const ItemName& item : items)
    {
        XSFLIB_CHECK(FindItem(bytes, flavor, item.name) == item.item_id);
    }
    for (const char* absent : {"SE_", "SE_ABC", "SE_A1", "SF", "T", "SE_AA"})
    {
        XSFLIB_CHECK(!FindItem(bytes, flavor, absent));
    }
    CheckTree(bytes, flavor, items.size());
    XSFLIB_CHECK(StringBlock::Read(bytes, flavor).names == items);
}

// Thousands of names of every type, found and read back, with the tree's rule at every node.
void TestManyNames(Flavor flavor)
{
    const char kLetters[] = "ABCDEGHILMNOPRSTUW_0123456789";
    const uint32_t kTypes[] = {ntdlib::sar::kSoundItem,  ntdlib::sar::kSoundGroupItem,  ntdlib::sar::kBankItem,
                               ntdlib::sar::kPlayerItem, ntdlib::sar::kWaveArchiveItem, ntdlib::sar::kGroupItem};
    uint32_t state = 12345;
    auto next = [&](uint32_t bound)
    {
        state = state * 1664525 + 1013904223;
        return (state >> 8) % bound;
    };

    std::set<std::string> seen;
    uint32_t counts[6] = {};
    std::vector<ItemName> items;
    while (items.size() < 3000)
    {
        std::string name = next(2) ? "SE_" : "BGM_";
        const uint32_t length = 1 + next(12);
        for (uint32_t k = 0; k < length; ++k)
        {
            name += kLetters[next(sizeof(kLetters) - 1)];
        }
        if (!seen.insert(name).second)
        {
            continue;
        }
        const uint32_t type = next(6);
        items.push_back({name, ItemId(kTypes[type], counts[type]++)});
    }

    const StringBlock block = Block(flavor, items);
    const std::vector<uint8_t> bytes = block.Write();

    XSFLIB_CHECK(StringBlock::Read(bytes, flavor) == block);
    XSFLIB_CHECK(bytes.size() % 32 == 0);
    for (const ItemName& item : items)
    {
        XSFLIB_CHECK(FindItem(bytes, flavor, item.name) == item.item_id);
        XSFLIB_CHECK(!seen.contains(item.name + "X") || FindItem(bytes, flavor, item.name + "X"));
        if (!seen.contains(item.name + "X"))
        {
            XSFLIB_CHECK(!FindItem(bytes, flavor, item.name + "X"));
        }
    }
    CheckTree(bytes, flavor, items.size());
}

// A block laid out another way reads: here, the tree comes before the string table. Write() lays it out again.
void TestOtherLayout(Flavor flavor)
{
    const std::vector<uint8_t> two = TwoNames(flavor);
    std::vector<uint8_t> moved(two.begin(), two.begin() + 8);
    Put16(moved, flavor, 0x2401);
    Put16(moved, flavor, 0);
    Put32(moved, flavor, 16);
    Put16(moved, flavor, 0x2400);
    Put16(moved, flavor, 0);
    Put32(moved, flavor, 16 + 68);
    moved.insert(moved.end(), two.begin() + 56, two.begin() + 124);
    moved.insert(moved.end(), two.begin() + 24, two.begin() + 56);

    const StringBlock read = StringBlock::Read(moved, flavor);

    XSFLIB_CHECK(read.names.size() == 2 && read.names[0].name == "B" && read.names[1].name == "A");
    XSFLIB_CHECK(FindItem(moved, flavor, "A") == ItemId(ntdlib::sar::kSoundItem, 0));
    XSFLIB_CHECK(read.Write() == two);
}

// Whether StringBlock::Read refuses `bytes` with a message that holds `part`.
bool Refuses(const std::vector<uint8_t>& bytes, const std::string& part)
{
    try
    {
        StringBlock::Read(bytes, Flavor::kNx);
    }
    catch (const xsflib::FormatError& e)
    {
        return std::string(e.what()).find(part) != std::string::npos;
    }

    return false;
}

// The writer refuses a name with a zero byte, two names alike, an item of a type without names and two names that
// first differ past bit 65,534. The reader refuses another block, a table or a tree past the block's end, a leaf of a
// string past the table, two leaves of one string and a string without a leaf. FindItem refuses a tree whose walk
// leaves it or goes round in a loop.
void TestRefusals()
{
    using namespace ntdlib::sar;
    const std::string kLong(8200, 'a');
    auto refused = [](std::vector<ItemName> names)
    {
        return Throws<std::invalid_argument>([&] { Block(Flavor::kNx, names).Write(); });
    };
    XSFLIB_CHECK(refused({{std::string("A\0B", 3), ItemId(kSoundItem, 0)}}));
    XSFLIB_CHECK(refused({{"A", ItemId(kSoundItem, 0)}, {"A", ItemId(kBankItem, 0)}}));
    XSFLIB_CHECK(refused({{"A", ItemId(7, 0)}}) && refused({{"A", 0}}));
    XSFLIB_CHECK(refused({{kLong, ItemId(kSoundItem, 0)}, {kLong + "b", ItemId(kSoundItem, 1)}}));

    // Byte 8,191 holds bits 65,528 to 65,535. 'a' and 'c' first differ at its bit 6, and 'b' and 'c' at its bit 7.
    const std::string kStart(8191, 'a');
    XSFLIB_CHECK(refused({{kStart + "b", ItemId(kSoundItem, 0)}, {kStart + "c", ItemId(kSoundItem, 1)}}));
    XSFLIB_CHECK(!refused({{kStart + "a", ItemId(kSoundItem, 0)}, {kStart + "c", ItemId(kSoundItem, 1)}}));

    const std::vector<uint8_t> two = TwoNames(Flavor::kNx);
    std::vector<uint8_t> other = two;
    other[0] = 'X';
    std::vector<uint8_t> short_table = two;
    xsflib::StoreLe32(&short_table[24], 100);
    std::vector<uint8_t> short_tree = two;
    xsflib::StoreLe32(&short_tree[60], 100);
    std::vector<uint8_t> far_string = two;
    xsflib::StoreLe32(&far_string[32], 200);
    std::vector<uint8_t> far_leaf = two;
    xsflib::StoreLe32(&far_leaf[64 + 12], 2);
    std::vector<uint8_t> twice = two;
    xsflib::StoreLe32(&twice[84 + 12], 1);
    std::vector<uint8_t> no_leaf = two;
    xsflib::StoreLe16(&no_leaf[84], 0);
    XSFLIB_CHECK(Refuses(other, "not a STRG block") && Refuses(short_table, "string table runs past"));
    XSFLIB_CHECK(Refuses(short_tree, "tree runs past") && Refuses(far_string, "string 0 runs past"));
    XSFLIB_CHECK(Refuses(far_leaf, "past its table") && Refuses(twice, "two leaves for string 1"));
    XSFLIB_CHECK(Refuses(no_leaf, "string 0 has no leaf"));

    std::vector<uint8_t> loop = two;
    xsflib::StoreLe32(&loop[104 + 4], 2);
    std::vector<uint8_t> outside = two;
    xsflib::StoreLe32(&outside[104 + 8], 7);
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { FindItem(loop, Flavor::kNx, "A"); }));
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { FindItem(outside, Flavor::kNx, "B"); }));
}

} // namespace

int main()
{
    for (const Flavor flavor : kFlavors)
    {
        TestTwoNames(flavor);
        TestOneName(flavor);
        TestNoNames(flavor);
        TestPrefixes(flavor);
        TestManyNames(flavor);
        TestOtherLayout(flavor);
    }
    TestOrderOfTypes();
    TestRefusals();

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::printf("STRG block tests passed\n");

    return 0;
}
