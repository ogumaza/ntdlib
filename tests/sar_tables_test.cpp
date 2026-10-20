// SPDX-License-Identifier: MIT

// Tests of the search for a sound archive's tables, of their replacement and of new items (ntdlib/sar/tables.h), in
// each flavor's byte order. The tests build archives as the header describes. The places that they expect are worked
// out by hand. The tests of new items build their archives with ntdlib/testing/sar_tools.h.

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
#include "ntdlib/testing/sar_files.h"
#include "ntdlib/testing/sar_tools.h"

using xsflib::testing::failures;

namespace
{

using ntdlib::sar::Flavor;
using ntdlib::sar::InfoBlock;
using ntdlib::sar::ItemId;
using ntdlib::sar::NewBank;
using ntdlib::sar::NewSequenceSound;
using ntdlib::sar::NewWaveArchive;
using ntdlib::sar::NewWaveSound;
using ntdlib::sar::SoundArchive;
using ntdlib::sar::SoundInfo;
using ntdlib::sar::StringBlock;
using ntdlib::sar::TablesOf;
using ntdlib::sar::WithBankLists;
using ntdlib::sar::WithNewBank;
using ntdlib::sar::WithNewFile;
using ntdlib::sar::WithNewSequenceSound;
using ntdlib::sar::WithNewWaveArchive;
using ntdlib::sar::WithNewWaveSounds;
using ntdlib::sar::WithTables;

namespace sar_files = ntdlib::testing::sar_files;
namespace sar_tools = ntdlib::testing::sar_tools;

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

// The archive of the tests of new items: sequence sounds A and B, which play file 0, wave sound C in file 1, a bank, a
// wave archive without a name, a group that the game keeps outside the archive, a player, and four files. ntdlib reads
// no players on the 3DS.
sar_files::ArchiveSpec ItemsSpec(bool names)
{
    sar_files::SoundSpec a;
    a.name = "SEQ_A";
    a.volume = 90;
    a.pan = 0x0101;
    a.priority = 0x0230;
    a.banks = {0};
    a.allocate_track_flags = 3;
    a.start_offset = 5;
    a.channel_priority = 0x0150;
    sar_files::SoundSpec b;
    b.name = "SEQ_B";
    b.banks = {0, kNone};
    b.start_offset = 9;
    sar_files::SoundSpec c;
    c.name = "WSD_C";
    c.kind = sar_files::SoundSpec::kWaveSound;
    c.file = 1;
    c.wave_sound_index = 2;

    sar_files::ArchiveSpec spec;
    spec.sounds = {a, b, c};
    spec.banks = {{"BANK_A", 2}};
    spec.wave_archives = {{"", 3}};
    spec.groups = {{"GROUP_A", ntdlib::sar::kExternalGroupFile}};
    spec.players = {{"PLAYER_A", 4}};
    for (uint8_t i = 0; i < 4; ++i)
    {
        sar_files::FileSpec file;
        file.bytes.assign(40u - 8u * i, static_cast<uint8_t>(0xF0 + i));
        spec.files.push_back(file);
    }
    spec.names = names;

    return spec;
}

SoundArchive Load(const std::vector<uint8_t>& archive, Flavor flavor)
{
    ntdlib::sar::Options options;
    options.flavor = flavor;
    options.rules = ntdlib::sar::Rules::kStopAtFirstProblem;

    return SoundArchive::Load(archive, options);
}

// Whether two sounds hold the same values, the name aside.
bool SameValues(const SoundInfo& a, const SoundInfo& b)
{
    return a.file_id == b.file_id && a.volume == b.volume && a.pan_mode == b.pan_mode && a.pan_curve == b.pan_curve &&
           a.type == b.type && a.player_id == b.player_id && a.player_priority == b.player_priority &&
           a.actor_player_id == b.actor_player_id && a.wave_sound_index == b.wave_sound_index && a.banks == b.banks &&
           a.allocate_track_flags == b.allocate_track_flags && a.start_offset == b.start_offset &&
           a.channel_priority == b.channel_priority && a.release_priority_fix == b.release_priority_fix;
}

// The string index of each entry of `archive`'s tables that holds one, by its item ID, in the order of the tables.
std::vector<std::pair<uint32_t, uint32_t>> StringIndices(const std::vector<uint8_t>& archive, Flavor flavor)
{
    const InfoBlock info = InfoBlock::Read(TablesOf(archive, flavor).info, flavor);
    const std::vector<std::vector<uint8_t>>* tables[] = {&info.sounds, &info.sound_groups, &info.banks,
                                                         &info.wave_archives, &info.groups, &info.players};
    const uint32_t types[] = {ntdlib::sar::kSoundItem, ntdlib::sar::kSoundGroupItem, ntdlib::sar::kBankItem,
                              ntdlib::sar::kWaveArchiveItem, ntdlib::sar::kGroupItem, ntdlib::sar::kPlayerItem};
    const std::size_t options[] = {20, 24, 12, 8, 4, 4};
    std::vector<std::pair<uint32_t, uint32_t>> indices;
    for (std::size_t k = 0; k < 6; ++k)
    {
        for (std::size_t i = 0; i < tables[k]->size(); ++i)
        {
            const std::vector<uint8_t>& entry = (*tables[k])[i];
            if ((Load32(entry, options[k], flavor) & 1) != 0)
            {
                indices.push_back({ItemId(types[k], static_cast<uint32_t>(i)), Load32(entry, options[k] + 4, flavor)});
            }
        }
    }

    return indices;
}

// Checks that `archive`'s string table holds `names` in that order, and that each entry's string index names its item.
void CheckNames(const std::vector<uint8_t>& archive, Flavor flavor, const std::vector<std::string>& names)
{
    const StringBlock strings = StringBlock::Read(*TablesOf(archive, flavor).strings, flavor);
    XSFLIB_CHECK(strings.names.size() == names.size());
    for (std::size_t i = 0; i < names.size() && i < strings.names.size(); ++i)
    {
        XSFLIB_CHECK(strings.names[i].name == names[i]);
    }

    const std::vector<std::pair<uint32_t, uint32_t>> indices = StringIndices(archive, flavor);
    XSFLIB_CHECK(indices.size() == names.size());
    for (std::size_t i = 0; i < indices.size(); ++i)
    {
        XSFLIB_CHECK(indices[i].second == i && i < strings.names.size() &&
                     strings.names[i].item_id == indices[i].first);
    }
}

// An archive of the tools' layout reads as its spec says, and the same tables give it back.
void TestToolsArchive(Flavor flavor)
{
    const std::vector<uint8_t> archive = sar_tools::Archive(flavor, ItemsSpec(true));
    const ntdlib::sar::Tables tables = TablesOf(archive, flavor);
    XSFLIB_CHECK(WithTables(archive, flavor, StringBlock::Read(*tables.strings, flavor),
                            InfoBlock::Read(tables.info, flavor)) == archive);

    const SoundArchive read = Load(archive, flavor);
    XSFLIB_CHECK(read.Sounds().size() == 3 && read.Sounds()[0].name == "SEQ_A" && read.Sounds()[2].name == "WSD_C");
    XSFLIB_CHECK(read.Sounds()[0].volume == 90 && read.Sounds()[0].start_offset == 5 &&
                 read.Sounds()[1].start_offset == 9);
    XSFLIB_CHECK(read.Sounds()[1].banks == std::vector<uint32_t>({ItemId(ntdlib::sar::kBankItem, 0), kNone}));
    XSFLIB_CHECK(read.Sounds()[2].wave_sound_index == 2 && read.Sounds()[2].file_id == 1);
    XSFLIB_CHECK(read.Banks().size() == 1 && read.Banks()[0].name == "BANK_A" && read.Banks()[0].file_id == 2);
    XSFLIB_CHECK(read.FileCount() == 4 && read.FileData(3).size() == 16 && read.FileData(3)[0] == 0xF3);
    CheckNames(archive, flavor, {"SEQ_A", "SEQ_B", "WSD_C", "BANK_A", "GROUP_A", "PLAYER_A"});
}

// A new file goes after the last one, on the next multiple of the alignment. The archive ends with it. A second new
// file follows the first.
void TestNewFile(Flavor flavor)
{
    const std::vector<uint8_t> archive = sar_tools::Archive(flavor, ItemsSpec(true));
    const std::vector<uint8_t> bytes(100, 0x5A);
    const ntdlib::sar::ArchiveWithFile first = WithNewFile(archive, flavor, bytes);
    const ntdlib::sar::ArchiveWithFile second = WithNewFile(first.archive, flavor, std::vector<uint8_t>(3, 0x5B));

    XSFLIB_CHECK(first.file == 4 && second.file == 5);
    for (const ntdlib::sar::ArchiveWithFile* with : {&first, &second})
    {
        const SoundArchive read = Load(with->archive, flavor);
        XSFLIB_CHECK(read.FileCount() == with->file + 1);
        XSFLIB_CHECK(std::ranges::equal(read.FileData(4), bytes));
        const std::size_t start = read.FileOffset(with->file).value_or(1);
        XSFLIB_CHECK(start % AlignmentOf(flavor) == 0);
        XSFLIB_CHECK(with->archive.size() == start + read.FileData(with->file).size());
        for (uint32_t i = 0; i < 4; ++i)
        {
            XSFLIB_CHECK(std::ranges::equal(read.FileData(i), Load(archive, flavor).FileData(i)));
        }

        // The header and the FILE block give the archive's size and the block's.
        const std::size_t block = Load32(with->archive, 0x30, flavor);
        XSFLIB_CHECK(Load32(with->archive, 0x0C, flavor) == with->archive.size());
        XSFLIB_CHECK(Load32(with->archive, 0x34, flavor) == with->archive.size() - block);
        XSFLIB_CHECK(Load32(with->archive, block + 4, flavor) == with->archive.size() - block);
        const ntdlib::sar::Tables tables = TablesOf(with->archive, flavor);
        XSFLIB_CHECK(WithTables(with->archive, flavor, StringBlock::Read(*tables.strings, flavor),
                                InfoBlock::Read(tables.info, flavor)) == with->archive);
    }

    // The entry as the tools write it: a reference to the location 12 bytes on, no options, the location, and on the
    // Wii U and the Switch, a reference to an empty list of groups 20 bytes after the location.
    const InfoBlock info = InfoBlock::Read(TablesOf(first.archive, flavor).info, flavor);
    const std::vector<uint8_t>& entry = info.files[4];
    const bool ctr = flavor == Flavor::kCtr;
    XSFLIB_CHECK(entry.size() == (ctr ? 24u : 36u));
    XSFLIB_CHECK(Load32(entry, 4, flavor) == 12 && Load32(entry, 8, flavor) == 0 && Load32(entry, 20, flavor) == 100);
    XSFLIB_CHECK(xsflib::Reader(entry, flavor == Flavor::kCafe).U16(0) == 0x220C);
    XSFLIB_CHECK(xsflib::Reader(entry, flavor == Flavor::kCafe).U16(12) == 0x1F00);
    XSFLIB_CHECK(ctr || (Load32(entry, 28, flavor) == 20 && Load32(entry, 32, flavor) == 0));
}

// A new sound copies its model's values, and takes its name, its file, its start and its banks. Its name goes in after
// the sounds' names. A second new sound follows the first.
void TestNewSequenceSound(Flavor flavor)
{
    const std::vector<uint8_t> archive = sar_tools::Archive(flavor, ItemsSpec(true));
    const SoundArchive before = Load(archive, flavor);
    const ntdlib::sar::ArchiveWithFile with = WithNewFile(archive, flavor, before.FileData(0));
    const uint32_t bank = ItemId(ntdlib::sar::kBankItem, 0);
    NewSequenceSound sound{.model = 0, .name = "SEQ_NEW", .file = with.file, .start = 2, .banks = {kNone, bank}};
    const std::vector<uint8_t> first = WithNewSequenceSound(with.archive, flavor, sound);
    sound = {.model = 1, .name = "SEQ_NEW2", .file = 0, .start = 7, .banks = {}};
    const std::vector<uint8_t> second = WithNewSequenceSound(first, flavor, sound);

    const SoundArchive read = Load(second, flavor);
    XSFLIB_CHECK(read.Sounds().size() == 5);
    for (std::size_t i = 0; i < 3 && read.Sounds().size() == 5; ++i)
    {
        XSFLIB_CHECK(read.Sounds()[i].name == before.Sounds()[i].name);
        XSFLIB_CHECK(SameValues(read.Sounds()[i], before.Sounds()[i]));
    }
    if (read.Sounds().size() == 5)
    {
        SoundInfo expected = before.Sounds()[0];
        expected.file_id = with.file;
        expected.start_offset = 2;
        expected.banks = {kNone, bank};
        XSFLIB_CHECK(read.Sounds()[3].name == "SEQ_NEW" && SameValues(read.Sounds()[3], expected));
        expected = before.Sounds()[1];
        expected.start_offset = 7;
        expected.banks = {};
        XSFLIB_CHECK(read.Sounds()[4].name == "SEQ_NEW2" && SameValues(read.Sounds()[4], expected));
    }
    XSFLIB_CHECK(read.Banks().size() == 1 && read.Banks()[0].name == "BANK_A");
    XSFLIB_CHECK(flavor == Flavor::kCtr || (read.Players().size() == 1 && read.Players()[0].name == "PLAYER_A"));
    XSFLIB_CHECK(std::ranges::equal(read.FileData(with.file), before.FileData(0)));
    CheckNames(second, flavor, {"SEQ_A", "SEQ_B", "WSD_C", "SEQ_NEW", "SEQ_NEW2", "BANK_A", "GROUP_A", "PLAYER_A"});
    const std::span<const uint8_t> strings = *TablesOf(second, flavor).strings;
    XSFLIB_CHECK(ntdlib::sar::FindItem(strings, flavor, "SEQ_NEW") == ItemId(ntdlib::sar::kSoundItem, 3));
    XSFLIB_CHECK(ntdlib::sar::FindItem(strings, flavor, "SEQ_NEW2") == ItemId(ntdlib::sar::kSoundItem, 4));
    XSFLIB_CHECK(ntdlib::sar::FindItem(strings, flavor, "PLAYER_A") == ItemId(ntdlib::sar::kPlayerItem, 0));
}

// In an archive without names, the entries keep their string indices. The new sound takes the one after the sounds',
// and the later ones move up by one.
void TestNewSoundWithoutNames(Flavor flavor)
{
    const std::vector<uint8_t> archive = sar_tools::Archive(flavor, ItemsSpec(false));
    const NewSequenceSound sound{.model = 1, .name = "", .file = 0, .start = 0, .banks = {}};
    const std::vector<uint8_t> added = WithNewSequenceSound(archive, flavor, sound);

    const SoundArchive read = Load(added, flavor);
    XSFLIB_CHECK(read.Sounds().size() == 4 && read.Sounds()[3].name == "SEQ_3" && read.Sounds()[3].file_id == 0);
    using Index = std::pair<uint32_t, uint32_t>;
    const std::vector<Index> expected = {
        {ItemId(ntdlib::sar::kSoundItem, 0), 0}, {ItemId(ntdlib::sar::kSoundItem, 1), 1},
        {ItemId(ntdlib::sar::kSoundItem, 2), 2}, {ItemId(ntdlib::sar::kSoundItem, 3), 3},
        {ItemId(ntdlib::sar::kBankItem, 0), 4},  {ItemId(ntdlib::sar::kGroupItem, 0), 5},
        {ItemId(ntdlib::sar::kPlayerItem, 0), 6}};
    XSFLIB_CHECK(StringIndices(added, flavor) == expected);
}

void TestNewSoundRefusals()
{
    const Flavor flavor = Flavor::kNx;
    const std::vector<uint8_t> archive = sar_tools::Archive(flavor, ItemsSpec(true));
    const uint32_t bank = ItemId(ntdlib::sar::kBankItem, 0);
    const auto refused = [&](const NewSequenceSound& sound, const std::vector<uint8_t>& in)
    {
        return Throws<std::invalid_argument>([&] { WithNewSequenceSound(in, flavor, sound); });
    };
    const NewSequenceSound good{.model = 0, .name = "NEW", .file = 0, .start = 0, .banks = {bank}};
    XSFLIB_CHECK(!refused(good, archive));

    // Models past the table or of another kind, a file past the table, too many banks, banks past the table or of
    // another type, and names that are missing, taken or out of place.
    NewSequenceSound sound = good;
    sound.model = 3;
    XSFLIB_CHECK(refused(sound, archive));
    sound.model = 2;
    XSFLIB_CHECK(refused(sound, archive));
    sound = good;
    sound.file = 4;
    XSFLIB_CHECK(refused(sound, archive));
    sound = good;
    sound.banks = {bank, bank, bank, bank, bank};
    XSFLIB_CHECK(refused(sound, archive));
    sound.banks = {ItemId(ntdlib::sar::kBankItem, 1)};
    XSFLIB_CHECK(refused(sound, archive));
    sound.banks = {ItemId(ntdlib::sar::kPlayerItem, 0)};
    XSFLIB_CHECK(refused(sound, archive));
    sound = good;
    sound.name = "";
    XSFLIB_CHECK(refused(sound, archive));
    sound.name = "SEQ_B";
    XSFLIB_CHECK(refused(sound, archive));
    sound.name = "BANK_A";
    XSFLIB_CHECK(refused(sound, archive));
    XSFLIB_CHECK(refused(good, sar_tools::Archive(flavor, ItemsSpec(false))));

    // A model whose details leave the start out takes only a start of 0.
    const ntdlib::sar::Tables tables = TablesOf(archive, flavor);
    const StringBlock strings = StringBlock::Read(*tables.strings, flavor);
    InfoBlock info = InfoBlock::Read(tables.info, flavor);
    const std::size_t details = Load32(info.sounds[0], 16, flavor);
    Store(info.sounds[0], details + 12, flavor, 2, 4);
    const std::vector<uint8_t> no_start = WithTables(archive, flavor, strings, info);
    sound = good;
    sound.start = 1;
    XSFLIB_CHECK(refused(sound, no_start));
    const SoundArchive read = Load(WithNewSequenceSound(no_start, flavor, good), flavor);
    XSFLIB_CHECK(read.Sounds().size() == 4 && read.Sounds()[3].start_offset == 0);

    // A model whose list of banks doesn't end its entry.
    info = InfoBlock::Read(tables.info, flavor);
    info.sounds[0].resize(info.sounds[0].size() + 4);
    const std::vector<uint8_t> longer = WithTables(archive, flavor, strings, info);
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { WithNewSequenceSound(longer, flavor, good); }));
}

// New wave sounds copy their models' values, and take their names, their file and their indices. A copy of a wave sound
// keeps the model's details. A copy of a sequence sound takes the details that the tools write for a wave sound. They
// hold the model's channel priority. The names go in after the sounds' names, in order.
void TestNewWaveSounds(Flavor flavor)
{
    const std::vector<uint8_t> archive = sar_tools::Archive(flavor, ItemsSpec(true));
    const SoundArchive before = Load(archive, flavor);
    const ntdlib::sar::ArchiveWithFile with = WithNewFile(archive, flavor, before.FileData(1));
    const std::vector<NewWaveSound> sounds = {{.model = 2, .name = "WSD_NEW", .file = with.file, .index = 0},
                                              {.model = 0, .name = "WSD_NEW2", .file = with.file, .index = 3}};

    const std::vector<uint8_t> added = WithNewWaveSounds(with.archive, flavor, sounds);

    // The values that ntdlib reads.
    const SoundArchive read = Load(added, flavor);
    XSFLIB_CHECK(read.Sounds().size() == 5);
    for (std::size_t i = 0; i < 3 && read.Sounds().size() == 5; ++i)
    {
        XSFLIB_CHECK(read.Sounds()[i].name == before.Sounds()[i].name);
        XSFLIB_CHECK(SameValues(read.Sounds()[i], before.Sounds()[i]));
    }
    if (read.Sounds().size() == 5)
    {
        SoundInfo expected = before.Sounds()[2];
        expected.file_id = with.file;
        expected.wave_sound_index = 0;
        XSFLIB_CHECK(read.Sounds()[3].name == "WSD_NEW" && SameValues(read.Sounds()[3], expected));
        const SoundInfo plain;
        expected = before.Sounds()[0];
        expected.type = ntdlib::sar::SoundType::kWaveSound;
        expected.file_id = with.file;
        expected.wave_sound_index = 3;
        expected.banks = {};
        expected.allocate_track_flags = plain.allocate_track_flags;
        expected.start_offset = plain.start_offset;
        expected.channel_priority = plain.channel_priority;
        expected.release_priority_fix = plain.release_priority_fix;
        XSFLIB_CHECK(read.Sounds()[4].name == "WSD_NEW2" && SameValues(read.Sounds()[4], expected));
    }
    CheckNames(added, flavor, {"SEQ_A", "SEQ_B", "WSD_C", "WSD_NEW", "WSD_NEW2", "BANK_A", "GROUP_A", "PLAYER_A"});
    const std::span<const uint8_t> strings = *TablesOf(added, flavor).strings;
    XSFLIB_CHECK(ntdlib::sar::FindItem(strings, flavor, "WSD_NEW") == ItemId(ntdlib::sar::kSoundItem, 3));
    XSFLIB_CHECK(ntdlib::sar::FindItem(strings, flavor, "WSD_NEW2") == ItemId(ntdlib::sar::kSoundItem, 4));

    // The entries. The copy of the wave sound differs from its model in the file, the string index and the index. The
    // copy of the sequence sound ends with the details of a wave sound: the index, 1 track, and the channel priority.
    const InfoBlock old_info = InfoBlock::Read(TablesOf(archive, flavor).info, flavor);
    const InfoBlock info = InfoBlock::Read(TablesOf(added, flavor).info, flavor);
    std::vector<uint8_t> copy = old_info.sounds[2];
    const std::size_t details = Load32(copy, 16, flavor);
    Store(copy, 0, flavor, with.file, 4);
    Store(copy, 24, flavor, 3, 4);
    Store(copy, details, flavor, 0, 4);
    XSFLIB_CHECK(info.sounds.size() == 5 && info.sounds[3] == copy);
    std::vector<uint8_t> converted = old_info.sounds[0];
    const std::size_t sequence_details = Load32(converted, 16, flavor);
    converted.resize(sequence_details + 16);
    Store(converted, 0, flavor, with.file, 4);
    Store(converted, 12, flavor, 0x2202, 2);
    Store(converted, 24, flavor, 4, 4);
    Store(converted, sequence_details, flavor, 3, 4);
    Store(converted, sequence_details + 4, flavor, 1, 4);
    Store(converted, sequence_details + 8, flavor, 1, 4);
    Store(converted, sequence_details + 12, flavor, 0x0150, 4);
    XSFLIB_CHECK(info.sounds.size() == 5 && info.sounds[4] == converted);
}

// In an archive without names, the entries keep their string indices. The new sounds take the ones after the sounds',
// and the later ones move up.
void TestNewWaveSoundsWithoutNames(Flavor flavor)
{
    const std::vector<uint8_t> archive = sar_tools::Archive(flavor, ItemsSpec(false));
    const std::vector<NewWaveSound> sounds = {{.model = 2, .name = "", .file = 1, .index = 1},
                                              {.model = 1, .name = "", .file = 1, .index = 0}};

    const std::vector<uint8_t> added = WithNewWaveSounds(archive, flavor, sounds);

    const SoundArchive read = Load(added, flavor);
    XSFLIB_CHECK(read.Sounds().size() == 5 && read.Sounds()[3].name == "WSD_3" && read.Sounds()[4].name == "WSD_4");
    XSFLIB_CHECK(read.Sounds().size() == 5 && read.Sounds()[3].wave_sound_index == 1 &&
                 read.Sounds()[4].wave_sound_index == 0);
    using Index = std::pair<uint32_t, uint32_t>;
    const std::vector<Index> expected = {
        {ItemId(ntdlib::sar::kSoundItem, 0), 0}, {ItemId(ntdlib::sar::kSoundItem, 1), 1},
        {ItemId(ntdlib::sar::kSoundItem, 2), 2}, {ItemId(ntdlib::sar::kSoundItem, 3), 3},
        {ItemId(ntdlib::sar::kSoundItem, 4), 4}, {ItemId(ntdlib::sar::kBankItem, 0), 5},
        {ItemId(ntdlib::sar::kGroupItem, 0), 6}, {ItemId(ntdlib::sar::kPlayerItem, 0), 7}};
    XSFLIB_CHECK(StringIndices(added, flavor) == expected);
}

void TestNewWaveSoundRefusals()
{
    const Flavor flavor = Flavor::kNx;
    sar_files::ArchiveSpec spec = ItemsSpec(true);
    sar_files::SoundSpec stream;
    stream.name = "STRM_D";
    stream.kind = sar_files::SoundSpec::kStream;
    spec.sounds.push_back(stream);
    const std::vector<uint8_t> archive = sar_tools::Archive(flavor, spec);
    const auto refused = [&](const std::vector<NewWaveSound>& sounds, const std::vector<uint8_t>& in)
    {
        return Throws<std::invalid_argument>([&] { WithNewWaveSounds(in, flavor, sounds); });
    };
    const NewWaveSound good{.model = 2, .name = "NEW", .file = 1, .index = 0};
    XSFLIB_CHECK(!refused({good}, archive));
    XSFLIB_CHECK(WithNewWaveSounds(archive, flavor, {}) == archive);

    // Models past the table or of another kind, a file past the table, and names that are missing, taken or out of
    // place.
    NewWaveSound sound = good;
    sound.model = 4;
    XSFLIB_CHECK(refused({sound}, archive));
    sound.model = 3;
    XSFLIB_CHECK(refused({sound}, archive));
    sound = good;
    sound.file = 4;
    XSFLIB_CHECK(refused({sound}, archive));
    sound = good;
    sound.name = "";
    XSFLIB_CHECK(refused({sound}, archive));
    sound.name = "WSD_C";
    XSFLIB_CHECK(refused({sound}, archive));
    sound.name = "PLAYER_A";
    XSFLIB_CHECK(refused({sound}, archive));
    XSFLIB_CHECK(refused({good, good}, archive));
    XSFLIB_CHECK(refused({good}, sar_tools::Archive(flavor, ItemsSpec(false))));

    // A sequence sound whose details leave its channel priority out, and one whose list of banks doesn't end its entry.
    const ntdlib::sar::Tables tables = TablesOf(archive, flavor);
    const StringBlock strings = StringBlock::Read(*tables.strings, flavor);
    InfoBlock info = InfoBlock::Read(tables.info, flavor);
    const std::size_t details = Load32(info.sounds[0], 16, flavor);
    Store(info.sounds[0], details + 12, flavor, 1, 4);
    const std::vector<uint8_t> no_priority = WithTables(archive, flavor, strings, info);
    sound = good;
    sound.model = 0;
    XSFLIB_CHECK(
        Throws<xsflib::FormatError>([&] { WithNewWaveSounds(no_priority, flavor, std::vector<NewWaveSound>{sound}); }));
    info = InfoBlock::Read(tables.info, flavor);
    info.sounds[0].resize(info.sounds[0].size() + 4);
    const std::vector<uint8_t> longer = WithTables(archive, flavor, strings, info);
    XSFLIB_CHECK(
        Throws<xsflib::FormatError>([&] { WithNewWaveSounds(longer, flavor, std::vector<NewWaveSound>{sound}); }));

    // A sequence sound whose details hold no start gives the channel priority in the start's place.
    info = InfoBlock::Read(tables.info, flavor);
    Store(info.sounds[0], details + 12, flavor, 2, 4);
    const std::vector<uint8_t> no_start = WithTables(archive, flavor, strings, info);
    const InfoBlock added = InfoBlock::Read(
        TablesOf(WithNewWaveSounds(no_start, flavor, std::vector<NewWaveSound>{sound}), flavor).info, flavor);
    XSFLIB_CHECK(added.sounds.size() == 5 && Load32(added.sounds[4], details + 12, flavor) == 5);
}

// The wave archive and the index of each wave of a bank or a wave sound file.
std::vector<std::pair<uint32_t, uint32_t>> PairsOf(const std::vector<ntdlib::sar::WaveId>& waves)
{
    std::vector<std::pair<uint32_t, uint32_t>> pairs;
    for (const ntdlib::sar::WaveId& wave : waves)
    {
        pairs.emplace_back(wave.wave_archive_item, wave.index);
    }

    return pairs;
}

// A new bank takes its name, its file and its list of wave archives, as the tools lay a bank's entry out. Its name goes
// in after the banks' names. A second new bank follows the first.
void TestNewBank(Flavor flavor)
{
    const std::vector<uint8_t> archive = sar_tools::Archive(flavor, ItemsSpec(true));
    const SoundArchive before = Load(archive, flavor);
    const uint32_t wave_archive = ItemId(ntdlib::sar::kWaveArchiveItem, 0);
    sar_files::BankSpec spec;
    spec.waves = {{wave_archive, 0}};
    const ntdlib::sar::ArchiveWithFile with = WithNewFile(archive, flavor, sar_files::Bank(flavor, spec));
    const std::vector<uint8_t> first =
        WithNewBank(with.archive, flavor, {.name = "BANK_NEW", .file = with.file, .wave_archives = {wave_archive}});
    const std::vector<uint8_t> second = WithNewBank(first, flavor, {.name = "BANK_NEW2", .file = 2});

    const SoundArchive read = Load(second, flavor);
    XSFLIB_CHECK(read.Banks().size() == 3);
    if (read.Banks().size() == 3)
    {
        XSFLIB_CHECK(read.Banks()[0].name == "BANK_A" && read.Banks()[0].file_id == 2);
        XSFLIB_CHECK(read.Banks()[1].name == "BANK_NEW" && read.Banks()[1].file_id == with.file);
        XSFLIB_CHECK(read.Banks()[2].name == "BANK_NEW2" && read.Banks()[2].file_id == 2);
    }
    XSFLIB_CHECK(read.Sounds().size() == 3);
    for (std::size_t i = 0; i < 3 && read.Sounds().size() == 3; ++i)
    {
        XSFLIB_CHECK(read.Sounds()[i].name == before.Sounds()[i].name);
        XSFLIB_CHECK(SameValues(read.Sounds()[i], before.Sounds()[i]));
    }
    XSFLIB_CHECK(PairsOf(ntdlib::sar::Bank::Parse(read.FileData(with.file), read.ReadOptions()).waves) == spec.waves);
    CheckNames(second, flavor, {"SEQ_A", "SEQ_B", "WSD_C", "BANK_A", "BANK_NEW", "BANK_NEW2", "GROUP_A", "PLAYER_A"});
    const std::span<const uint8_t> strings = *TablesOf(second, flavor).strings;
    XSFLIB_CHECK(ntdlib::sar::FindItem(strings, flavor, "BANK_NEW") == ItemId(ntdlib::sar::kBankItem, 1));
    XSFLIB_CHECK(ntdlib::sar::FindItem(strings, flavor, "BANK_NEW2") == ItemId(ntdlib::sar::kBankItem, 2));

    // The entry: the file, a reference to the list 20 bytes in, the string index, and the list.
    const InfoBlock info = InfoBlock::Read(TablesOf(second, flavor).info, flavor);
    const std::vector<uint8_t>& entry = info.banks[1];
    XSFLIB_CHECK(entry.size() == 28 && Load32(entry, 0, flavor) == with.file);
    XSFLIB_CHECK(xsflib::Reader(entry, flavor == Flavor::kCafe).U16(4) == 0x0100 && Load32(entry, 8, flavor) == 20);
    XSFLIB_CHECK(Load32(entry, 12, flavor) == 1 && Load32(entry, 16, flavor) == 4);
    XSFLIB_CHECK(Load32(entry, 20, flavor) == 1 && Load32(entry, 24, flavor) == wave_archive);
    XSFLIB_CHECK(info.banks[2].size() == 24 && Load32(info.banks[2], 20, flavor) == 0);
}

// A new wave archive takes its file, and a name only when it has one. A named one's name goes in after the wave
// archives' names, and a second named one after the first.
void TestNewWaveArchive(Flavor flavor)
{
    const std::vector<uint8_t> archive = sar_tools::Archive(flavor, ItemsSpec(true));
    const std::vector<uint8_t> bytes = sar_files::WaveArchive(flavor, {});
    const ntdlib::sar::ArchiveWithFile with = WithNewFile(archive, flavor, bytes);
    const std::vector<uint8_t> first = WithNewWaveArchive(with.archive, flavor, {.name = "", .file = with.file});
    const std::vector<uint8_t> second = WithNewWaveArchive(first, flavor, {.name = "WARC_NEW", .file = 3});
    const std::vector<uint8_t> third = WithNewWaveArchive(second, flavor, {.name = "WARC_NEW2", .file = 3});

    const SoundArchive read = Load(third, flavor);
    XSFLIB_CHECK(read.WaveArchives().size() == 4);
    if (read.WaveArchives().size() == 4)
    {
        XSFLIB_CHECK(read.WaveArchives()[0].file_id == 3);
        XSFLIB_CHECK(read.WaveArchives()[1].name.empty() && read.WaveArchives()[1].file_id == with.file);
        XSFLIB_CHECK(read.WaveArchives()[2].name == "WARC_NEW" && read.WaveArchives()[2].file_id == 3);
        XSFLIB_CHECK(read.WaveArchives()[3].name == "WARC_NEW2" && read.WaveArchives()[3].file_id == 3);
    }
    XSFLIB_CHECK(std::ranges::equal(read.FileData(with.file), bytes));
    CheckNames(third, flavor, {"SEQ_A", "SEQ_B", "WSD_C", "BANK_A", "WARC_NEW", "WARC_NEW2", "GROUP_A", "PLAYER_A"});
    const std::span<const uint8_t> strings = *TablesOf(third, flavor).strings;
    XSFLIB_CHECK(ntdlib::sar::FindItem(strings, flavor, "WARC_NEW") == ItemId(ntdlib::sar::kWaveArchiveItem, 2));
    XSFLIB_CHECK(ntdlib::sar::FindItem(strings, flavor, "WARC_NEW2") == ItemId(ntdlib::sar::kWaveArchiveItem, 3));

    // The entries: the file, a word of zeros, then the options.
    const InfoBlock info = InfoBlock::Read(TablesOf(third, flavor).info, flavor);
    XSFLIB_CHECK(info.wave_archives[1].size() == 12 && Load32(info.wave_archives[1], 0, flavor) == with.file);
    XSFLIB_CHECK(Load32(info.wave_archives[1], 4, flavor) == 0 && Load32(info.wave_archives[1], 8, flavor) == 0);
    XSFLIB_CHECK(info.wave_archives[2].size() == 16 && Load32(info.wave_archives[2], 8, flavor) == 1);
    XSFLIB_CHECK(Load32(info.wave_archives[2], 12, flavor) == 4);
}

// In an archive without names, a new bank takes the string index after the banks', and the later ones move up by one.
// A new wave archive takes none.
void TestNewItemsWithoutNames(Flavor flavor)
{
    const std::vector<uint8_t> archive = sar_tools::Archive(flavor, ItemsSpec(false));
    const std::vector<uint8_t> with_bank = WithNewBank(archive, flavor, {.name = "", .file = 2});
    const std::vector<uint8_t> added = WithNewWaveArchive(with_bank, flavor, {.name = "", .file = 3});

    const SoundArchive read = Load(added, flavor);
    XSFLIB_CHECK(read.Banks().size() == 2 && read.Banks()[1].file_id == 2);
    XSFLIB_CHECK(read.WaveArchives().size() == 2 && read.WaveArchives()[1].file_id == 3);
    using Index = std::pair<uint32_t, uint32_t>;
    const std::vector<Index> expected = {
        {ItemId(ntdlib::sar::kSoundItem, 0), 0}, {ItemId(ntdlib::sar::kSoundItem, 1), 1},
        {ItemId(ntdlib::sar::kSoundItem, 2), 2}, {ItemId(ntdlib::sar::kBankItem, 0), 3},
        {ItemId(ntdlib::sar::kBankItem, 1), 4},  {ItemId(ntdlib::sar::kGroupItem, 0), 5},
        {ItemId(ntdlib::sar::kPlayerItem, 0), 6}};
    XSFLIB_CHECK(StringIndices(added, flavor) == expected);
}

// The new banks and wave archives refuse files past the file table, wave archives past their table or of another type,
// and names that are missing, taken or out of place.
void TestNewItemRefusals()
{
    const Flavor flavor = Flavor::kNx;
    const std::vector<uint8_t> archive = sar_tools::Archive(flavor, ItemsSpec(true));
    const std::vector<uint8_t> unnamed = sar_tools::Archive(flavor, ItemsSpec(false));
    const uint32_t wave_archive = ItemId(ntdlib::sar::kWaveArchiveItem, 0);
    const auto refused = [&](const NewBank& bank, const std::vector<uint8_t>& in)
    {
        return Throws<std::invalid_argument>([&] { WithNewBank(in, flavor, bank); });
    };
    const auto refused_wave_archive = [&](const NewWaveArchive& added, const std::vector<uint8_t>& in)
    {
        return Throws<std::invalid_argument>([&] { WithNewWaveArchive(in, flavor, added); });
    };
    const NewBank good{.name = "NEW", .file = 0, .wave_archives = {wave_archive}};
    XSFLIB_CHECK(!refused(good, archive));

    NewBank bank = good;
    bank.file = 4;
    XSFLIB_CHECK(refused(bank, archive));
    bank = good;
    bank.wave_archives = {ItemId(ntdlib::sar::kWaveArchiveItem, 1)};
    XSFLIB_CHECK(refused(bank, archive));
    bank.wave_archives = {ItemId(ntdlib::sar::kBankItem, 0)};
    XSFLIB_CHECK(refused(bank, archive));
    bank = good;
    bank.name = "";
    XSFLIB_CHECK(refused(bank, archive));
    bank.name = "SEQ_A";
    XSFLIB_CHECK(refused(bank, archive));
    XSFLIB_CHECK(refused(good, unnamed));

    XSFLIB_CHECK(!refused_wave_archive({.name = "", .file = 0}, archive));
    XSFLIB_CHECK(refused_wave_archive({.name = "", .file = 4}, archive));
    XSFLIB_CHECK(refused_wave_archive({.name = "BANK_A", .file = 0}, archive));
    XSFLIB_CHECK(refused_wave_archive({.name = "NEW", .file = 0}, unnamed));
}

// A sequence sound's list of banks takes another length. The list ends the sound's entry, and the entry grows or
// shrinks with it. The other values, the other sounds and the names stay. The old lists give the archive back.
void TestBankLists(Flavor flavor)
{
    const std::vector<uint8_t> archive = sar_tools::Archive(flavor, ItemsSpec(true));
    const uint32_t bank = ItemId(ntdlib::sar::kBankItem, 0);
    const std::vector<uint32_t> longer = {kNone, bank, kNone, bank};

    const std::vector<uint8_t> changed = WithBankLists(archive, flavor, {{0, longer}, {1, {}}});
    const std::vector<uint8_t> back = WithBankLists(changed, flavor, {{0, {bank}}, {1, {bank, kNone}}});

    const SoundArchive read = Load(changed, flavor);
    XSFLIB_CHECK(read.Sounds()[0].banks == longer && read.Sounds()[1].banks.empty());
    XSFLIB_CHECK(read.Sounds()[0].volume == 90 && read.Sounds()[0].allocate_track_flags == 3);
    XSFLIB_CHECK(read.Sounds()[0].start_offset == 5 && read.Sounds()[1].start_offset == 9);
    XSFLIB_CHECK(read.Sounds()[2].wave_sound_index == 2 && read.Sounds()[2].file_id == 1);
    XSFLIB_CHECK(read.FileCount() == 4 && read.FileData(3).size() == 16 && read.FileData(3)[0] == 0xF3);
    CheckNames(changed, flavor, {"SEQ_A", "SEQ_B", "WSD_C", "BANK_A", "GROUP_A", "PLAYER_A"});
    XSFLIB_CHECK(back == archive);
}

// The lists of banks refuse sounds past the table or of another kind, more than four banks, banks past the table or of
// another kind, and an entry that its list doesn't end.
void TestBankListRefusals()
{
    const Flavor flavor = Flavor::kNx;
    const std::vector<uint8_t> archive = sar_tools::Archive(flavor, ItemsSpec(true));
    const uint32_t bank = ItemId(ntdlib::sar::kBankItem, 0);
    const auto refused = [&](uint32_t sound, const std::vector<uint32_t>& list)
    {
        return Throws<std::invalid_argument>([&] { WithBankLists(archive, flavor, {{sound, list}}); });
    };
    XSFLIB_CHECK(!refused(0, {bank}));

    XSFLIB_CHECK(refused(3, {bank}));
    XSFLIB_CHECK(refused(2, {bank}));
    XSFLIB_CHECK(refused(0, {bank, bank, bank, bank, bank}));
    XSFLIB_CHECK(refused(0, {ItemId(ntdlib::sar::kBankItem, 1)}));
    XSFLIB_CHECK(refused(0, {ItemId(ntdlib::sar::kPlayerItem, 0)}));

    const ntdlib::sar::Tables tables = TablesOf(archive, flavor);
    InfoBlock info = InfoBlock::Read(tables.info, flavor);
    info.sounds[0].resize(info.sounds[0].size() + 4);
    const std::vector<uint8_t> longer = WithTables(archive, flavor, StringBlock::Read(*tables.strings, flavor), info);
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { WithBankLists(longer, flavor, {{0, {bank}}}); }));
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
        TestToolsArchive(flavor);
        TestNewFile(flavor);
        TestNewSequenceSound(flavor);
        TestNewSoundWithoutNames(flavor);
        TestNewWaveSounds(flavor);
        TestNewWaveSoundsWithoutNames(flavor);
        TestNewBank(flavor);
        TestNewWaveArchive(flavor);
        TestNewItemsWithoutNames(flavor);
        TestBankLists(flavor);
    }
    TestRefusals();
    TestNewSoundRefusals();
    TestNewWaveSoundRefusals();
    TestNewItemRefusals();
    TestBankListRefusals();

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::printf("Tables tests passed\n");

    return 0;
}
