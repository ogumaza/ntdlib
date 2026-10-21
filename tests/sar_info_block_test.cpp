// SPDX-License-Identifier: MIT

// Tests of the reader and the writer of a sound archive's INFO block (ntdlib/sar/info_block.h), in each flavor's byte
// order. The bytes that the tests expect follow from the layout that the header describes, worked out by hand.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/info_block.h"

using xsflib::testing::ErrorOf;

namespace
{

using ntdlib::sar::Flavor;
using ntdlib::sar::InfoBlock;

constexpr Flavor kFlavors[] = {Flavor::kCtr, Flavor::kCafe, Flavor::kNx};

// Appends `value` to `bytes` in the byte order of `flavor`, `size` bytes of it.
void Put(std::vector<uint8_t>& bytes, Flavor flavor, uint32_t value, int size)
{
    for (int i = 0; i < size; ++i)
    {
        const int shift = flavor == Flavor::kCafe ? 8 * (size - 1 - i) : 8 * i;
        bytes.push_back(static_cast<uint8_t>(value >> shift));
    }
}

void Put32(std::vector<uint8_t>& bytes, Flavor flavor, uint32_t value)
{
    Put(bytes, flavor, value, 4);
}

// Appends a reference: its type, two bytes of padding and its offset.
void PutReference(std::vector<uint8_t>& bytes, Flavor flavor, uint16_t type, uint32_t offset)
{
    Put(bytes, flavor, type, 2);
    Put(bytes, flavor, 0, 2);
    Put32(bytes, flavor, offset);
}

template <class Bytes>
void Append(std::vector<uint8_t>& bytes, const Bytes& more)
{
    bytes.insert(bytes.end(), more.begin(), more.end());
}

// `size` bytes of `value`.
std::vector<uint8_t> Entry(std::size_t size, uint8_t value)
{
    return std::vector<uint8_t>(size, value);
}

// The settings in the tests: bytes 1 to 20.
std::array<uint8_t, 20> Settings()
{
    std::array<uint8_t, 20> settings{};
    for (std::size_t i = 0; i < settings.size(); ++i)
    {
        settings[i] = static_cast<uint8_t>(i + 1);
    }

    return settings;
}

// The start of a block of `size` bytes: its magic, its size, and the references to the tables and the settings, with
// offsets from byte 8.
std::vector<uint8_t> Start(Flavor flavor, uint32_t size, const std::array<uint32_t, 8>& offsets)
{
    constexpr uint16_t kTypes[] = {0x2100, 0x2104, 0x2101, 0x2103, 0x2105, 0x2102, 0x2106, 0x220B};
    std::vector<uint8_t> bytes = {'I', 'N', 'F', 'O'};
    Put32(bytes, flavor, size);
    for (std::size_t k = 0; k < offsets.size(); ++k)
    {
        PutReference(bytes, flavor, kTypes[k], offsets[k]);
    }

    return bytes;
}

template <class Error, class Call>
bool Throws(Call call)
{
    return !ErrorOf<Error>(call).empty();
}

// Checks that `block` writes `expected`, and that `expected` reads as `block`.
void CheckBoth(const InfoBlock& block, const std::vector<uint8_t>& expected)
{
    XSFLIB_CHECK(block.Write() == expected);
    XSFLIB_CHECK(InfoBlock::Read(expected, block.flavor) == block);
}

// A block without entries. Each table holds its count of 0 in 4 bytes, from byte 72. The settings start at 100 and end
// at 120. The block pads to 128.
void TestEmpty(Flavor flavor)
{
    InfoBlock block;
    block.flavor = flavor;
    block.player_settings = Settings();

    std::vector<uint8_t> expected = Start(flavor, 128, {64, 68, 72, 76, 80, 84, 88, 92});
    for (int k = 0; k < 7; ++k)
    {
        Put32(expected, flavor, 0);
    }
    Append(expected, block.player_settings);
    XSFLIB_CHECK(expected.size() == 120);
    expected.resize(128);

    CheckBoth(block, expected);
}

// The block of TestEntries: a sound of 8 bytes, two banks of 4 and 12 bytes, and a file of 8 bytes. The sound table
// starts at 72, with its count and one reference, and the sound at 84. The sound group table follows at 92 and the
// bank table at 96. The banks start at 116 and 120. The tables of wave archives, groups and players follow at 132, 136
// and 140. The file table starts at 144 and the file at 156. The settings run from 164 to 184, and the block pads to
// 192.
InfoBlock SomeEntries(Flavor flavor)
{
    InfoBlock block;
    block.flavor = flavor;
    block.sounds = {Entry(8, 0xA1)};
    block.banks = {Entry(4, 0xB1), Entry(12, 0xB2)};
    block.files = {Entry(8, 0xF1)};
    block.player_settings = Settings();

    return block;
}

std::vector<uint8_t> SomeEntriesBytes(Flavor flavor)
{
    const InfoBlock block = SomeEntries(flavor);
    std::vector<uint8_t> bytes = Start(flavor, 192, {64, 84, 88, 124, 128, 132, 136, 156});
    Put32(bytes, flavor, 1);
    PutReference(bytes, flavor, 0x2200, 12);
    Append(bytes, block.sounds[0]);
    Put32(bytes, flavor, 0);
    Put32(bytes, flavor, 2);
    PutReference(bytes, flavor, 0x2206, 20);
    PutReference(bytes, flavor, 0x2206, 24);
    Append(bytes, block.banks[0]);
    Append(bytes, block.banks[1]);
    Put32(bytes, flavor, 0);
    Put32(bytes, flavor, 0);
    Put32(bytes, flavor, 0);
    Put32(bytes, flavor, 1);
    PutReference(bytes, flavor, 0x220A, 12);
    Append(bytes, block.files[0]);
    Append(bytes, block.player_settings);
    XSFLIB_CHECK(bytes.size() == 184);
    bytes.resize(192);

    return bytes;
}

void TestEntries(Flavor flavor)
{
    CheckBoth(SomeEntries(flavor), SomeEntriesBytes(flavor));
}

// A block with an entry of 4 bytes in each table. Each table takes 16 bytes: its count, its reference and its entry.
// The tables start at 72, 88 and so on to 168. The settings run from 184 to 204, and the block pads to 224.
void TestEveryTable(Flavor flavor)
{
    InfoBlock block;
    block.flavor = flavor;
    block.sounds = {Entry(4, 1)};
    block.sound_groups = {Entry(4, 2)};
    block.banks = {Entry(4, 3)};
    block.wave_archives = {Entry(4, 4)};
    block.groups = {Entry(4, 5)};
    block.players = {Entry(4, 6)};
    block.files = {Entry(4, 7)};
    block.player_settings = Settings();

    std::vector<uint8_t> expected = Start(flavor, 224, {64, 80, 96, 112, 128, 144, 160, 176});
    constexpr uint16_t kEntryTypes[] = {0x2200, 0x2204, 0x2206, 0x2207, 0x2208, 0x2209, 0x220A};
    for (int k = 0; k < 7; ++k)
    {
        Put32(expected, flavor, 1);
        PutReference(expected, flavor, kEntryTypes[k], 12);
        Append(expected, Entry(4, static_cast<uint8_t>(k + 1)));
    }
    Append(expected, block.player_settings);
    XSFLIB_CHECK(expected.size() == 204);
    expected.resize(224);

    CheckBoth(block, expected);
}

// A block whose settings end at a multiple of 32 bytes needs no padding. The group of 32 bytes starts at 100, after
// its table at 88. The tables of players and files follow at 132 and 136. The settings run from 140 to 160.
void TestNoPadding(Flavor flavor)
{
    InfoBlock block;
    block.flavor = flavor;
    block.groups = {Entry(32, 0xC1)};
    block.player_settings = Settings();

    std::vector<uint8_t> expected = Start(flavor, 160, {64, 68, 72, 76, 80, 124, 128, 132});
    for (int k = 0; k < 4; ++k)
    {
        Put32(expected, flavor, 0);
    }
    Put32(expected, flavor, 1);
    PutReference(expected, flavor, 0x2208, 12);
    Append(expected, block.groups[0]);
    Put32(expected, flavor, 0);
    Put32(expected, flavor, 0);
    Append(expected, block.player_settings);

    CheckBoth(block, expected);
}

// An entry that grows moves the entries and the tables after it. The block reads back as written.
void TestGrowth(Flavor flavor)
{
    InfoBlock block = SomeEntries(flavor);
    block.sounds[0].resize(40, 0xA2);
    block.banks.push_back(Entry(16, 0xB3));
    block.files.insert(block.files.begin(), Entry(4, 0xF0));

    const std::vector<uint8_t> bytes = block.Write();
    XSFLIB_CHECK(bytes.size() % 32 == 0);
    XSFLIB_CHECK(InfoBlock::Read(bytes, flavor) == block);
}

void TestRefusals()
{
    const std::vector<uint8_t> good = SomeEntriesBytes(Flavor::kNx);
    XSFLIB_CHECK(InfoBlock::Read(good, Flavor::kNx) == SomeEntries(Flavor::kNx));
    auto refused = [](const std::vector<uint8_t>& bytes, Flavor flavor = Flavor::kNx)
    {
        return Throws<xsflib::FormatError>([&] { InfoBlock::Read(bytes, flavor); });
    };
    auto with = [&](std::size_t at, std::vector<uint8_t> bytes)
    {
        std::vector<uint8_t> changed = good;
        std::copy(bytes.begin(), bytes.end(), changed.begin() + static_cast<std::ptrdiff_t>(at));

        return changed;
    };

    // Bytes that aren't an INFO block, or not all of one.
    XSFLIB_CHECK(refused(with(0, {'I', 'N', 'F', 'X'})));
    XSFLIB_CHECK(refused(std::vector<uint8_t>(good.begin(), good.begin() + 71)));
    XSFLIB_CHECK(refused(with(4, {200, 0, 0, 0})));
    XSFLIB_CHECK(refused(good, Flavor::kCafe));

    // References of other types, or with bytes in their padding.
    XSFLIB_CHECK(refused(with(24, {0x02, 0x21})));
    XSFLIB_CHECK(refused(with(18, {1})));
    XSFLIB_CHECK(refused(with(76, {0x01, 0x22})));
    XSFLIB_CHECK(refused(with(64, {0x0C, 0x22})));

    // Tables and entries laid out another way. The sound table can't start after a gap, even with the other offsets
    // moved and the block 8 bytes longer, and sound 0 can't start after a gap either.
    XSFLIB_CHECK(refused(with(12, {68})));
    std::vector<uint8_t> gap = Start(Flavor::kNx, 192, {72, 92, 96, 132, 136, 140, 144, 164});
    gap.resize(80);
    Append(gap, std::span(good).subspan(72, 184 - 72));
    XSFLIB_CHECK(gap.size() == 192);
    XSFLIB_CHECK(refused(gap));
    XSFLIB_CHECK(refused(with(80, {16})));
    XSFLIB_CHECK(refused(with(112, {20})));
    XSFLIB_CHECK(refused(with(112, {26})));
    XSFLIB_CHECK(refused(with(112, {0, 0x10})));
    XSFLIB_CHECK(refused(with(36, {112})));
    XSFLIB_CHECK(refused(with(92, {0xE8, 0x03})));

    // The settings and the padding.
    XSFLIB_CHECK(refused(with(191, {1})));
    std::vector<uint8_t> longer = with(4, {224});
    longer.resize(224);
    XSFLIB_CHECK(refused(longer));

    // Entries that Write refuses.
    for (const std::size_t size : {std::size_t{0}, std::size_t{6}})
    {
        InfoBlock block = SomeEntries(Flavor::kNx);
        block.players.push_back(Entry(size, 0xD1));
        XSFLIB_CHECK(Throws<std::invalid_argument>([&] { block.Write(); }));
    }
}

} // namespace

int main()
{
    for (const Flavor flavor : kFlavors)
    {
        TestEmpty(flavor);
        TestEntries(flavor);
        TestEveryTable(flavor);
        TestNoPadding(flavor);
        TestGrowth(flavor);
    }
    TestRefusals();

    return xsflib::testing::Summary("INFO block");
}
