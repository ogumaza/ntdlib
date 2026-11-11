// SPDX-License-Identifier: MIT

// Tests the copies of an RSAR that keep only the data that some sounds play (rsar/cut.h), on archives whose files lie
// in several groups: the copy that a model reads, the copy for the game's code, and the list of a sound's missing data.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/rsar/cut.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/testing/rsar_files.h"

namespace
{

namespace rsar = ntdlib::rsar;

using namespace ntdlib::testing::rsar_files;
using rsar::SoundArchive;
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;

// The rules by which the model reads sound archives.
constexpr rsar::ReadOptions kOptions = ntdlib::rvl::snd::kRsarOptions;

// Whether two files hold the same bytes, wave data included, or neither is there.
bool SameFile(const std::optional<rsar::FileData>& a, const std::optional<rsar::FileData>& b)
{
    return a.has_value() == b.has_value() &&
           (!a || (std::ranges::equal(a->file, b->file) && std::ranges::equal(a->wave_data, b->wave_data)));
}

// Tests the copy of an archive that contains only the data that some sequences play (KeepSequenceData).
void TestKeepSequenceData()
{
    // Sound 0 plays file 0 with bank 0 (file 2). Sound 1 plays file 1 with bank 1 (file 3). Both internal groups hold
    // files 0 and 2. File 0's first place is in group 1, and file 2's in group 0. File 3's first place is in group 2,
    // which lies outside the archive.
    const std::vector<std::pair<Bytes, Bytes>> files = {
        {MakeSequence({0xFF}), {}},
        {MakeSequence({60, 127, 24, 0xFF}), {}},
        {MakeNewBank(), MakeRwar()},
        {MakeOldBank(), MakeOldWaveData()},
    };
    const GroupedArchive grouped =
        MakeGroupedArchive(files, {{true, {1, 0, 2}}, {true, {0, 2, 3}}, {false, {3}}},
                           {{{1, 0}, {0, 1}}, {{0, 0}}, {{0, 2}, {1, 1}}, {{2, 0}, {1, 2}}}, {0, 1}, {2, 3});
    const SoundArchive archive = SoundArchive::Load(grouped.bytes, kOptions);

    // A size rounded up to a multiple of 32 bytes.
    const auto padded = [](std::size_t size)
    {
        return (size + 0x1F) & ~std::size_t{0x1F};
    };

    // The size of a copy that holds `kept` once each, at multiples of 32 bytes after the FILE block's header.
    const auto size_holding = [&](const std::vector<uint32_t>& kept)
    {
        std::size_t size = grouped.file_block + 0x20;
        for (const uint32_t f : kept)
        {
            size += padded(files[f].first.size()) + padded(files[f].second.size());
        }

        return size;
    };

    const Bytes one = rsar::KeepSequenceData(archive, {0}, kOptions);
    const SoundArchive cut = SoundArchive::Load(one, kOptions);
    const xsflib::Reader r(one, true);
    Check(one.size() == size_holding({0, 2}), "the copy holds the sequence's file and its bank's once each");
    Check(r.U32(8) == one.size() && r.U32(0x24) == one.size() - grouped.file_block &&
              r.U32(grouped.file_block + 4) == r.U32(0x24) && !cut.Truncated(),
          "the header and the FILE block give the copy's size");
    Check(SameFile(archive.File(0), cut.File(0)) && SameFile(archive.File(2), cut.File(2)),
          "the sequence and its bank read the same from the copy");
    Check(!cut.File(1) && !cut.File(3), "the copy doesn't hold the other sequence and bank");
    Check(uint64_t{cut.Groups()[0].offset} + cut.Groups()[0].items[1].offset == one.size() &&
              cut.Groups()[0].items[1].size == files[0].first.size(),
          "a copy of a file that File doesn't read starts at the copy's end");

    // Before its files, the copy differs only in the sizes and in the places of the internal groups and their items.
    std::vector<bool> may_differ(grouped.file_block + 8);
    const auto field = [&may_differ](std::size_t at, std::size_t size)
    {
        std::fill_n(may_differ.begin() + static_cast<std::ptrdiff_t>(at), size, true);
    };

    field(8, 4);
    field(0x24, 4);
    field(grouped.file_block + 4, 4);
    for (std::size_t g = 0; g < 2; g++)
    {
        field(grouped.groups[g] + 0x10, 0x10);
        for (const std::size_t item : grouped.items[g])
        {
            field(item + 4, 4);
            field(item + 0xC, 4);
        }
    }

    bool same = true;
    for (std::size_t i = 0; i < may_differ.size(); i++)
    {
        same = same && (may_differ[i] || one[i] == grouped.bytes[i]);
    }

    Check(same, "the copy keeps the archive's tables and the external group's places");

    const Bytes both = rsar::KeepSequenceData(archive, {0, 1}, kOptions);
    const SoundArchive cut_both = SoundArchive::Load(both, kOptions);
    bool all_same = true;
    for (uint32_t f = 0; f < files.size(); f++)
    {
        all_same = all_same && cut_both.File(f) && SameFile(archive.File(f), cut_both.File(f));
    }

    Check(both.size() == size_holding({0, 1, 2, 3}) && all_same,
          "a copy for both sequences holds each file once and reads the same");

    // Banks 0 and 1 (files 1 and 2) share a wave archive in the group. Both sounds play file 0.
    GroupedArchive shared = MakeGroupedArchive({files[0], files[2], {files[3].first, files[2].second}},
                                               {{true, {0, 1, 2}}}, {{{0, 0}}, {{0, 1}}, {{0, 2}}}, {0, 0}, {1, 2});
    xsflib::StoreBe32(&shared.bytes[shared.items[0][2] + 0xC],
                      xsflib::LoadBe32(&shared.bytes[shared.items[0][1] + 0xC]));
    const SoundArchive shared_archive = SoundArchive::Load(shared.bytes, kOptions);
    const Bytes shared_cut = rsar::KeepSequenceData(shared_archive, {0, 1}, kOptions);
    Check(shared_cut.size() == shared.file_block + 0x20 + padded(files[0].first.size()) +
                                   padded(files[2].first.size()) + padded(files[3].first.size()) +
                                   padded(files[2].second.size()),
          "banks that share a wave archive share one copy of it");

    // A FILE block that starts before the groups' entries.
    Bytes early = grouped.bytes;
    xsflib::StoreBe32(&early[0x20], 0x40);
    const SoundArchive early_files = SoundArchive::Load(early, kOptions);
    Check(ErrorOf<xsflib::FormatError>([&] { rsar::KeepSequenceData(early_files, {0}, kOptions); }) ==
              "the archive's tables of groups don't come before its files",
          "an archive whose files start before its groups' entries is refused");

    // File 0's first place is an empty item past the archive's end. File skips it in the archive, but not at the end
    // of the copy. The copy would then read differently.
    GroupedArchive crafted =
        MakeGroupedArchive({files[0], files[2]}, {{true, {0, 0, 1}}}, {{{0, 0}, {0, 1}}, {{0, 2}}}, {0}, {1});
    const std::size_t empty = crafted.items[0][0];
    xsflib::StoreBe32(&crafted.bytes[empty + 4], 0x7FFFFFFF);
    xsflib::StoreBe32(&crafted.bytes[empty + 8], 0);
    xsflib::StoreBe32(&crafted.bytes[empty + 0x10], 0);
    const SoundArchive crafted_archive = SoundArchive::Load(crafted.bytes, kOptions);
    Check(SameFile(crafted_archive.File(0), archive.File(0)), "File reads the second copy of the sequence");
    Check(ErrorOf<xsflib::FormatError>([&] { rsar::KeepSequenceData(crafted_archive, {0}, kOptions); }) ==
              "the files that the sequences play can't be cut out of the archive",
          "a copy that would read differently from the archive is refused");
}

// Tests the copy of an archive for the game's code, which loads some of its groups (KeepGroups).
void TestKeepGroups()
{
    // Group 0 holds files 0 and 2, group 1 files 1 and 3, and group 2 all four. The game loads groups 0 and 1.
    const std::vector<std::pair<Bytes, Bytes>> files = {
        {MakeSequence({0xFF}), {}},
        {MakeSequence({60, 127, 24, 0xFF}), {}},
        {MakeNewBank(), MakeRwar()},
        {MakeOldBank(), MakeOldWaveData()},
    };
    const GroupedArchive grouped = MakeGroupedArchive(
        files, {{true, {0, 2}}, {true, {1, 3}}, {true, {0, 1, 2, 3}}},
        {{{0, 0}, {2, 0}}, {{1, 0}, {2, 1}}, {{0, 1}, {2, 2}}, {{1, 1}, {2, 3}}}, {0, 1, 0}, {2, 3, 3});
    const SoundArchive archive = SoundArchive::Load(grouped.bytes, kOptions);

    // Whether `copy` holds the archive's bytes up to its files and in the files and wave data of `groups`, and zeros in
    // the FILE block's other data.
    const auto keeps = [&grouped, &archive](const Bytes& copy, const std::vector<uint32_t>& groups)
    {
        std::vector<bool> kept(grouped.bytes.size());
        const auto keep = [&kept](std::size_t at, std::size_t size)
        {
            std::fill_n(kept.begin() + static_cast<std::ptrdiff_t>(at), size, true);
        };

        keep(0, grouped.file_block + 8);
        for (const uint32_t g : groups)
        {
            keep(archive.Groups()[g].offset, archive.Groups()[g].size);
            keep(archive.Groups()[g].wave_data_offset, archive.Groups()[g].wave_data_size);
        }

        bool same = copy.size() == grouped.bytes.size();
        for (std::size_t i = 0; same && i < copy.size(); i++)
        {
            same = copy[i] == (kept[i] ? grouped.bytes[i] : 0);
        }

        return same;
    };

    const Bytes one = rsar::KeepGroups(archive, {true});
    const Bytes two = rsar::KeepGroups(archive, {true, true, false});
    const Bytes none = rsar::KeepGroups(archive, {});
    const SoundArchive cut = SoundArchive::Load(one, kOptions);

    Check(keeps(one, {0}), "a copy that loads one group keeps that group's files and wave data, and nothing else");
    Check(SameFile(archive.File(0), cut.File(0)) && SameFile(archive.File(2), cut.File(2)),
          "the sequence and its bank read the same from the copy");
    Check(keeps(two, {0, 1}), "a copy that loads two groups keeps both");
    Check(keeps(none, {}), "a copy that loads no group keeps only the archive's tables");

    // Group 0's files start 4 bytes after a multiple of 32 and end 4 bytes before one. The stream reads on to the
    // multiple, and the disc reads from a multiple of 4. A group of no bytes would read on to the archive's end. Bytes
    // after the FILE block aren't the groups' data.
    const std::size_t entry = grouped.groups[0];
    const uint32_t offset = xsflib::LoadBe32(&grouped.bytes[entry + 0x10]);
    const uint32_t size = xsflib::LoadBe32(&grouped.bytes[entry + 0x14]);
    const auto start = static_cast<std::ptrdiff_t>(offset);
    const auto end = static_cast<std::ptrdiff_t>(offset + size);
    Bytes inner = grouped.bytes;
    xsflib::StoreBe32(&inner[entry + 0x10], offset + 4);
    xsflib::StoreBe32(&inner[entry + 0x14], size - 8);
    std::fill_n(inner.begin() + start, 4, uint8_t{0xAA});
    std::fill_n(inner.begin() + end - 4, 4, uint8_t{0xAA});
    Bytes empty_group = grouped.bytes;
    xsflib::StoreBe32(&empty_group[entry + 0x14], 0);
    Bytes trailing = grouped.bytes;
    trailing.insert(trailing.end(), 32, uint8_t{0xAA});

    const Bytes rounded = rsar::KeepGroups(SoundArchive::Load(inner, kOptions), {true});
    const Bytes whole = rsar::KeepGroups(SoundArchive::Load(empty_group, kOptions), {true});
    const Bytes after = rsar::KeepGroups(SoundArchive::Load(trailing, kOptions), {true});

    const auto marked = [](Bytes::const_iterator from, std::size_t count)
    {
        return std::all_of(from, from + static_cast<std::ptrdiff_t>(count), [](uint8_t b) { return b == 0xAA; });
    };
    Check(marked(rounded.begin() + start, 4) && marked(rounded.begin() + end - 4, 4),
          "a group's bytes out to multiples of 32 are kept");
    Check(whole == empty_group, "a copy that loads a group of no bytes keeps the whole archive");
    Check(after.size() == trailing.size() && marked(after.end() - 32, 32), "the bytes after the FILE block are kept");
}

// Tests the list of the data that a sound requires and the archive doesn't contain (MissingData).
void TestMissingData()
{
    // Sound 0 plays file 0 with bank 0 (file 2), both in the archive. Sound 1 plays file 1 with bank 1 (file 1), which
    // only the external group holds. Sound 2 names a bank that the archive doesn't list.
    const std::vector<std::pair<Bytes, Bytes>> files = {
        {MakeSequence({0xFF}), {}},
        {MakeSequence({60, 127, 24, 0xFF}), {}},
        {MakeNewBank(), MakeRwar()},
    };
    const GroupedArchive grouped = MakeGroupedArchive(files, {{true, {0, 2}}, {false, {1}}},
                                                      {{{0, 0}}, {{1, 0}}, {{0, 1}}}, {0, 1, 0}, {2, 1});
    const SoundArchive archive = SoundArchive::Load(grouped.bytes, kOptions);

    Check(rsar::MissingData(archive, 0).empty(), "a sound with its data misses nothing");
    Check(rsar::MissingData(archive, 1) == std::vector<std::string>{"its sequence data", "bank BANK_1"},
          "a sound whose files lie outside the archive misses its sequence and its bank");
    Check(rsar::MissingData(archive, 2) == std::vector<std::string>{"its bank"}, "a sound with no bank misses it");
    Check(rsar::MissingData(archive, 3) == std::vector<std::string>{"the sound itself"},
          "a sound past the table is missing");
}

} // namespace

int main()
{
    const auto tests = []
    {
        TestKeepSequenceData();
        TestKeepGroups();
        TestMissingData();
    };

    return xsflib::testing::RunTest("rsar_cut", tests);
}
