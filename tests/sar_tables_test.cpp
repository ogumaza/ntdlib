// SPDX-License-Identifier: MIT

// Tests of the search for a sound archive's tables and of their replacement (ntdlib/sar/tables.h), in each flavor's
// byte order. The tests build archives as the header describes. The places that they expect are worked out by hand.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/info_block.h"
#include "ntdlib/sar/string_block.h"
#include "ntdlib/sar/tables.h"

using xsflib::testing::failures;

namespace
{

using ntdlib::sar::Flavor;
using ntdlib::sar::InfoBlock;
using ntdlib::sar::StringBlock;
using ntdlib::sar::TablesOf;
using ntdlib::sar::WithTables;

constexpr Flavor kFlavors[] = {Flavor::kCtr, Flavor::kCafe, Flavor::kNx};
constexpr uint32_t kNone = 0xFFFFFFFF;

// Stores `value` at `at` in the byte order of `flavor`, `size` bytes of it.
void Store(std::vector<uint8_t>& bytes, std::size_t at, Flavor flavor, uint32_t value, int size)
{
    for (int i = 0; i < size; ++i)
    {
        const int shift = flavor == Flavor::kCafe ? 8 * (size - 1 - i) : 8 * i;
        bytes[at + static_cast<std::size_t>(i)] = static_cast<uint8_t>(value >> shift);
    }
}

uint32_t Load32(std::span<const uint8_t> bytes, std::size_t at, Flavor flavor)
{
    return xsflib::Reader(bytes, flavor == Flavor::kCafe).U32(at);
}

// A file's entry: a reference to its location 12 bytes on, no options, then the location. An internal file's location
// is a reference with the file's offset in the FILE block, and the file's size. A file outside the archive has a name
// there instead.
std::vector<uint8_t> FileEntry(Flavor flavor, uint32_t offset, uint32_t size)
{
    std::vector<uint8_t> entry(24);
    Store(entry, 0, flavor, 0x220C, 2);
    Store(entry, 4, flavor, 12, 4);
    Store(entry, 12, flavor, 0x1F00, 2);
    Store(entry, 16, flavor, offset, 4);
    Store(entry, 20, flavor, size, 4);

    return entry;
}

std::vector<uint8_t> ExternalFileEntry(Flavor flavor)
{
    std::vector<uint8_t> entry(20);
    Store(entry, 0, flavor, 0x220D, 2);
    Store(entry, 4, flavor, 12, 4);
    const std::string name = "a.bin";
    std::ranges::copy(name, entry.begin() + 12);

    return entry;
}

// The files of the tests: file 0 of 40 bytes, file 1 in a group, file 2 outside the archive, and file 3 of 8 bytes.
const std::vector<uint8_t> kFile0(40, 0xF0);
const std::vector<uint8_t> kFile3(8, 0xF3);

// The tables of the tests: the name "A" of sound 0, a sound of 8 bytes, and the four files at `offsets` in the FILE
// block. The STRG block takes 96 bytes, and the INFO block 288. The file table starts at 112 in the INFO block. The
// settings run from 240 to 260.
std::optional<StringBlock> Strings(Flavor flavor)
{
    StringBlock strings;
    strings.flavor = flavor;
    strings.names = {{"A", ntdlib::sar::ItemId(ntdlib::sar::kSoundItem, 0)}};

    return strings;
}

InfoBlock Info(Flavor flavor, uint32_t offset0, uint32_t offset3)
{
    InfoBlock info;
    info.flavor = flavor;
    info.sounds = {std::vector<uint8_t>(8, 0xA0)};
    info.files = {FileEntry(flavor, offset0, 40), FileEntry(flavor, kNone, kNone), ExternalFileEntry(flavor),
                  FileEntry(flavor, offset3, 8)};
    for (std::size_t i = 0; i < info.player_settings.size(); ++i)
    {
        info.player_settings[i] = static_cast<uint8_t>(i + 1);
    }

    return info;
}

// The files' alignment from the archive's start.
std::size_t AlignmentOf(Flavor flavor)
{
    return flavor == Flavor::kNx ? 64 : 32;
}

// An archive of `flavor` laid out as NintendoWare's tools lay it out, with `strings` and `info`. The header takes 64
// bytes. The FILE block holds files 0 and 3 at the offsets that `info` gives.
std::vector<uint8_t> Archive(Flavor flavor, const std::optional<StringBlock>& strings, const InfoBlock& info)
{
    const std::vector<uint8_t> strg = strings ? strings->Write() : std::vector<uint8_t>();
    const std::vector<uint8_t> blocks = info.Write();
    std::vector<uint8_t> archive(64);
    std::ranges::copy(std::string(flavor == Flavor::kCtr ? "CSAR" : "FSAR"), archive.begin());
    Store(archive, 4, flavor, 0xFEFF, 2);
    Store(archive, 6, flavor, 64, 2);
    Store(archive, 8, flavor, 0x02000000, 4);
    Store(archive, 0x10, flavor, 3, 2);
    Store(archive, 0x14, flavor, 0x2000, 2);
    Store(archive, 0x18, flavor, strings ? 64 : kNone, 4);
    Store(archive, 0x1C, flavor, static_cast<uint32_t>(strg.size()), 4);
    Store(archive, 0x20, flavor, 0x2001, 2);
    Store(archive, 0x24, flavor, static_cast<uint32_t>(64 + strg.size()), 4);
    Store(archive, 0x28, flavor, static_cast<uint32_t>(blocks.size()), 4);
    archive.insert(archive.end(), strg.begin(), strg.end());
    archive.insert(archive.end(), blocks.begin(), blocks.end());

    // The FILE block, with the files where the file table puts them.
    const std::size_t file_block = archive.size();
    const InfoBlock read = InfoBlock::Read(blocks, flavor);
    const uint32_t offset0 = Load32(read.files[0], 16, flavor);
    const uint32_t offset3 = Load32(read.files[3], 16, flavor);
    archive.resize(file_block + 8 + offset3 + kFile3.size());
    std::ranges::copy(std::string("FILE"), archive.begin() + static_cast<std::ptrdiff_t>(file_block));
    std::ranges::copy(kFile0, archive.begin() + static_cast<std::ptrdiff_t>(file_block + 8 + offset0));
    std::ranges::copy(kFile3, archive.begin() + static_cast<std::ptrdiff_t>(file_block + 8 + offset3));
    const auto file_size = static_cast<uint32_t>(archive.size() - file_block);
    Store(archive, file_block + 4, flavor, file_size, 4);
    Store(archive, 0x2C, flavor, 0x2002, 2);
    Store(archive, 0x30, flavor, static_cast<uint32_t>(file_block), 4);
    Store(archive, 0x34, flavor, file_size, 4);
    Store(archive, 0x0C, flavor, static_cast<uint32_t>(archive.size()), 4);

    return archive;
}

// The archive of the tests. Its FILE block starts at 64 + 96 + 288 = 448, a multiple of 64. On the Switch, the first
// file starts at 512, and file 3 at 576. Their offsets from the end of the block's size, at 456, are 56 and 120. On the
// other consoles, the first file starts at 480 and file 3 at 544: offsets 24 and 88.
std::vector<uint8_t> TestArchive(Flavor flavor)
{
    const bool nx = flavor == Flavor::kNx;

    return Archive(flavor, Strings(flavor), Info(flavor, nx ? 56 : 24, nx ? 120 : 88));
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
    catch (...)
    {
        return false;
    }

    return false;
}

// Checks that `archive` holds files 0 and 3 at `start0` and `start3`, as its tables say, and that its header gives its
// blocks' places and its size.
void CheckFiles(const std::vector<uint8_t>& archive, Flavor flavor, std::size_t file_block, std::size_t start0,
                std::size_t start3)
{
    const ntdlib::sar::Tables tables = TablesOf(archive, flavor);
    const InfoBlock info = InfoBlock::Read(tables.info, flavor);
    XSFLIB_CHECK(Load32(archive, 0x30, flavor) == file_block);
    XSFLIB_CHECK(Load32(archive, 0x34, flavor) == archive.size() - file_block);
    XSFLIB_CHECK(Load32(archive, file_block + 4, flavor) == archive.size() - file_block);
    XSFLIB_CHECK(Load32(archive, 0x0C, flavor) == archive.size());
    XSFLIB_CHECK(file_block + 8 + Load32(info.files[0], 16, flavor) == start0);
    XSFLIB_CHECK(file_block + 8 + Load32(info.files[3], 16, flavor) == start3);
    XSFLIB_CHECK(start0 % AlignmentOf(flavor) == 0 && start3 % AlignmentOf(flavor) == 0);
    XSFLIB_CHECK(std::ranges::equal(std::span(archive).subspan(start0, kFile0.size()), kFile0));
    XSFLIB_CHECK(std::ranges::equal(std::span(archive).subspan(start3, kFile3.size()), kFile3));
    XSFLIB_CHECK(archive.size() == start3 + kFile3.size());
    XSFLIB_CHECK(info.files[1] == FileEntry(flavor, kNone, kNone) && info.files[2] == ExternalFileEntry(flavor));
}

void TestFind(Flavor flavor)
{
    const std::vector<uint8_t> archive = TestArchive(flavor);
    const ntdlib::sar::Tables tables = TablesOf(archive, flavor);
    XSFLIB_CHECK(tables.strings && std::ranges::equal(*tables.strings, Strings(flavor)->Write()));
    XSFLIB_CHECK(tables.strings && tables.strings->data() == archive.data() + 64);
    XSFLIB_CHECK(tables.info.data() == archive.data() + 160 && tables.info.size() == 288);
    CheckFiles(archive, flavor, 448, flavor == Flavor::kNx ? 512 : 480, flavor == Flavor::kNx ? 576 : 544);
}

// The same tables give the archive back.
void TestSame(Flavor flavor)
{
    const std::vector<uint8_t> archive = TestArchive(flavor);
    const ntdlib::sar::Tables tables = TablesOf(archive, flavor);
    XSFLIB_CHECK(WithTables(archive, flavor, StringBlock::Read(*tables.strings, flavor),
                            InfoBlock::Read(tables.info, flavor)) == archive);
}

// The INFO block grows by 32 bytes twice: a sound of 24 bytes and its reference each time. The FILE block starts at
// 480, then at 512. On the Switch, the first file stays at 512, then moves to 576. The zeros before it shrink from 56
// bytes to 24, then grow to 56 again. The files' offsets change with them. On the other consoles, the files move with
// the block: the first one to 512, then 544.
void TestGrowth(Flavor flavor)
{
    const bool nx = flavor == Flavor::kNx;
    std::vector<uint8_t> archive = TestArchive(flavor);
    for (int step = 1; step <= 2; ++step)
    {
        const ntdlib::sar::Tables tables = TablesOf(archive, flavor);
        InfoBlock info = InfoBlock::Read(tables.info, flavor);
        info.sounds.push_back(std::vector<uint8_t>(24, static_cast<uint8_t>(0xA0 + step)));
        archive = WithTables(archive, flavor, StringBlock::Read(*tables.strings, flavor), info);

        const std::size_t file_block = step == 1 ? 480 : 512;
        const std::size_t first = nx ? (step == 1 ? 512 : 576) : file_block + 32;
        XSFLIB_CHECK(Load32(archive, 0x28, flavor) == 288 + 32 * static_cast<uint32_t>(step));
        CheckFiles(archive, flavor, file_block, first, first + 64);

        // The new archive's tables give it back.
        const ntdlib::sar::Tables grown = TablesOf(archive, flavor);
        XSFLIB_CHECK(InfoBlock::Read(grown.info, flavor).sounds.size() == static_cast<std::size_t>(step) + 1);
        XSFLIB_CHECK(WithTables(archive, flavor, StringBlock::Read(*grown.strings, flavor),
                                InfoBlock::Read(grown.info, flavor)) == archive);
    }
}

// A STRG block that grows by 32 bytes, with the name "B" of sound 1. The INFO block follows at 192, and the FILE block
// at 192 + 288 = 480. The first file starts at 512 on every console, and file 3 at 576.
void TestNewName(Flavor flavor)
{
    const std::vector<uint8_t> archive = TestArchive(flavor);
    const ntdlib::sar::Tables tables = TablesOf(archive, flavor);
    StringBlock strings = StringBlock::Read(*tables.strings, flavor);
    strings.names.push_back({"B", ntdlib::sar::ItemId(ntdlib::sar::kSoundItem, 1)});
    const std::vector<uint8_t> named = WithTables(archive, flavor, strings, InfoBlock::Read(tables.info, flavor));

    XSFLIB_CHECK(Load32(named, 0x18, flavor) == 64 && Load32(named, 0x1C, flavor) == 128);
    XSFLIB_CHECK(Load32(named, 0x24, flavor) == 192 && Load32(named, 0x28, flavor) == 288);
    XSFLIB_CHECK(StringBlock::Read(*TablesOf(named, flavor).strings, flavor) == strings);
    CheckFiles(named, flavor, 480, 512, 576);
}

// An archive without names lists its STRG block at 0xFFFFFFFF. Its INFO block follows the header. The FILE block starts
// at 64 + 288 = 352. The first file starts at 384 on every console, and file 3 at 448.
void TestNoNames(Flavor flavor)
{
    const std::vector<uint8_t> archive = Archive(flavor, std::nullopt, Info(flavor, 24, 88));
    const ntdlib::sar::Tables tables = TablesOf(archive, flavor);
    XSFLIB_CHECK(!tables.strings && tables.info.data() == archive.data() + 64);
    const InfoBlock info = InfoBlock::Read(tables.info, flavor);
    XSFLIB_CHECK(WithTables(archive, flavor, std::nullopt, info) == archive);
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { WithTables(archive, flavor, Strings(flavor), info); }));

    const std::vector<uint8_t> named = TestArchive(flavor);
    const InfoBlock named_info = InfoBlock::Read(TablesOf(named, flavor).info, flavor);
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { WithTables(named, flavor, std::nullopt, named_info); }));
}

void TestRefusals()
{
    const Flavor flavor = Flavor::kNx;
    const std::vector<uint8_t> good = TestArchive(flavor);
    const ntdlib::sar::Tables tables = TablesOf(good, flavor);
    const std::optional<StringBlock> strings = StringBlock::Read(*tables.strings, flavor);
    InfoBlock grown = InfoBlock::Read(tables.info, flavor);
    grown.sounds.push_back(std::vector<uint8_t>(24, 0xA1));
    auto refused = [&](const std::vector<uint8_t>& archive)
    {
        return Throws<xsflib::FormatError>([&] { WithTables(archive, flavor, strings, grown); });
    };
    auto with = [&](std::size_t at, uint32_t value, int size)
    {
        std::vector<uint8_t> changed = good;
        Store(changed, at, flavor, value, size);
        return changed;
    };

    // TablesOf refuses an archive without an INFO block, and one cut short in a table.
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { TablesOf(with(0x20, 0x2003, 2), flavor); }));
    const std::vector<uint8_t> cut(good.begin(), good.begin() + 300);
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { TablesOf(cut, flavor); }));

    // Tables of another flavor.
    InfoBlock cafe = grown;
    cafe.flavor = Flavor::kCafe;
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { WithTables(good, flavor, strings, cafe); }));

    // Headers that list other blocks, or the blocks in another order.
    XSFLIB_CHECK(refused(with(0x10, 2, 2)));
    XSFLIB_CHECK(refused(with(0x10, 4, 2)));
    XSFLIB_CHECK(refused(with(0x20, 0x2002, 2)));
    std::vector<uint8_t> swapped = good;
    std::swap_ranges(swapped.begin() + 0x20, swapped.begin() + 0x2C, swapped.begin() + 0x2C);
    XSFLIB_CHECK(refused(swapped));

    // Blocks that don't follow each other, a gap before the FILE block, an archive cut short, an archive with another
    // size, and one that the FILE block doesn't end.
    XSFLIB_CHECK(refused(with(0x18, 96, 4)));
    XSFLIB_CHECK(refused(with(0x24, 192, 4)));
    XSFLIB_CHECK(refused(with(0x30, 480, 4)));
    std::vector<uint8_t> gap = with(0x30, 480, 4);
    gap.insert(gap.begin() + 448, 32, 0);
    Store(gap, 0x0C, flavor, static_cast<uint32_t>(gap.size()), 4);
    XSFLIB_CHECK(refused(gap));
    XSFLIB_CHECK(refused(std::vector<uint8_t>(good.begin(), good.end() - 1)));
    XSFLIB_CHECK(refused(with(0x0C, 600, 4)));
    std::vector<uint8_t> longer = good;
    longer.push_back(0);
    Store(longer, 0x0C, flavor, static_cast<uint32_t>(longer.size()), 4);
    XSFLIB_CHECK(refused(longer));

    // A FILE block with another magic or size, with bytes before its first file, or that ends before its first file's
    // place.
    XSFLIB_CHECK(refused(with(448, 'X', 1)));
    XSFLIB_CHECK(refused(with(448 + 4, 100, 4)));
    XSFLIB_CHECK(refused(with(448 + 8 + 55, 1, 1)));
    std::vector<uint8_t> short_block(good.begin(), good.begin() + 480);
    Store(short_block, 0x0C, flavor, 480, 4);
    Store(short_block, 0x34, flavor, 32, 4);
    Store(short_block, 448 + 4, flavor, 32, 4);
    XSFLIB_CHECK(refused(short_block));

    // Files whose locations lie outside their entries, or before the first file.
    InfoBlock outside = grown;
    Store(outside.files[0], 4, flavor, 100, 4);
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { WithTables(good, flavor, strings, outside); }));
    InfoBlock before = grown;
    Store(before.files[3], 16, flavor, 8, 4);
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { WithTables(good, flavor, strings, before); }));
}

} // namespace

int main()
{
    for (const Flavor flavor : kFlavors)
    {
        TestFind(flavor);
        TestSame(flavor);
        TestGrowth(flavor);
        TestNewName(flavor);
        TestNoNames(flavor);
    }
    TestRefusals();

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::printf("Tables tests passed\n");

    return 0;
}
