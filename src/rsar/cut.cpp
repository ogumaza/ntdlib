// SPDX-License-Identifier: MIT

// The copies of an RSAR that keep only the data that some sounds play, and the check of a sound's data (see cut.h).

#include "ntdlib/rsar/cut.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/reference.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"

namespace ntdlib::rsar
{

namespace
{

// An RSAR's FILE block keeps each file at a multiple of 32 bytes.
constexpr std::size_t kFileAlignment = 0x20;

// The largest archive whose offsets fit RSAR's 32-bit fields.
constexpr std::size_t kMaxArchiveSize = 0xFFFFFFFF;

// The bytes at the archive's start that DvdSoundArchive::LoadFileHeader reads: the header's 0x28 bytes, rounded up to a
// multiple of 32.
constexpr uint64_t kHeaderRead = 0x40;

// Pads `data` with zeros to a multiple of kFileAlignment.
void PadToFileAlignment(std::vector<uint8_t>& data)
{
    data.resize((data.size() + kFileAlignment - 1) / kFileAlignment * kFileAlignment, 0);
}

// The fields of an RSAR that KeepSequenceData and KeepGroups read, by their offsets. KeepSequenceData rewrites
// some of them.
struct RsarLayout
{
    // The header: the archive's size, the INFO block's offset and size, and the FILE block's offset and size.
    static constexpr std::size_t kArchiveSize = 0x08;
    static constexpr std::size_t kInfoBlock = 0x18;
    static constexpr std::size_t kInfoBlockSize = 0x1c;
    static constexpr std::size_t kFileBlock = 0x20;
    static constexpr std::size_t kFileBlockSize = 0x24;

    // A block's header: its magic, then its size. The INFO block's references count from the end of its header.
    static constexpr std::size_t kBlockSize = 0x04;
    static constexpr std::size_t kBlockHeader = 0x08;

    // A reference (Reference), and the reference to the groups' table in the INFO block. A table holds
    // a count, then a reference to each entry.
    static constexpr std::size_t kReference = 0x08;
    static constexpr std::size_t kGroupTable = 0x20;

    // A group's entry: the offset and size of its files in the archive, the same for their wave data, the reference
    // to the table of its items, and the size of the entry.
    static constexpr std::size_t kGroupOffset = 0x10;
    static constexpr std::size_t kGroupSize = 0x14;
    static constexpr std::size_t kGroupWaveDataOffset = 0x18;
    static constexpr std::size_t kGroupWaveDataSize = 0x1c;
    static constexpr std::size_t kGroupItems = 0x20;
    static constexpr std::size_t kGroupEntrySize = 0x28;

    // An item's entry: the offset of its file from its group's files, that of its wave data from the group's wave
    // data, and the size of the entry.
    static constexpr std::size_t kItemOffset = 0x04;
    static constexpr std::size_t kItemWaveDataOffset = 0x0c;
    static constexpr std::size_t kItemEntrySize = 0x14;
};

// Where an archive's INFO block holds a group's entry and its items' entries (SoundArchiveFileReader::ReadGroupInfo,
// main.dol 0x80042ac4, and ReadGroupItemInfo, 0x80042bd4).
struct GroupEntries
{
    std::size_t group = 0;
    std::vector<std::size_t> items;
};

// The groups' entries, in the order of SoundArchive::Groups. The INFO block's payload starts with references to its
// tables. The groups' table is the fifth. A table holds a count, then a reference to each entry. The references count
// from the payload.
std::vector<GroupEntries> FindGroupEntries(const xsflib::Reader& r)
{
    const std::size_t base = std::size_t{r.U32(RsarLayout::kInfoBlock)} + RsarLayout::kBlockHeader;
    const auto entries = [&r, base](std::size_t table_ref)
    {
        std::vector<std::size_t> found;
        const Reference table = ReadRef(r, table_ref);
        if (table.IsOffset())
        {
            const std::size_t at = base + table.offset;
            const uint32_t count = r.U32(at);
            for (uint32_t i = 0; i < count; i++)
            {
                found.push_back(base + ReadRef(r, at + 4 + i * RsarLayout::kReference).offset);
            }
        }

        return found;
    };

    std::vector<GroupEntries> groups;
    for (const std::size_t group : entries(base + RsarLayout::kGroupTable))
    {
        groups.push_back({group, entries(group + RsarLayout::kGroupItems)});
    }

    return groups;
}

// True if `entries` hold each of `groups` and its items, and lie before the FILE block at `file_block`.
bool EntriesBeforeFiles(const std::vector<GroupEntries>& entries,
                        const std::vector<SoundArchive::Group>& groups, std::size_t file_block)
{
    if (entries.size() != groups.size())
    {
        return false;
    }

    for (std::size_t g = 0; g < groups.size(); g++)
    {
        if (entries[g].items.size() != groups[g].items.size() ||
            entries[g].group + RsarLayout::kGroupEntrySize > file_block)
        {
            return false;
        }

        for (const std::size_t item : entries[g].items)
        {
            if (item + RsarLayout::kItemEntrySize > file_block)
            {
                return false;
            }
        }
    }

    return true;
}

// The files that a model reads for the sequences at `indices`: each sequence's file, and its bank's file and wave
// data (Engine::StartSequence and Engine::GetBank).
std::vector<uint32_t> SequenceFiles(const SoundArchive& archive, const std::vector<uint32_t>& indices)
{
    std::vector<uint32_t> files;
    for (const uint32_t index : indices)
    {
        if (index >= archive.Sounds().size())
        {
            continue;
        }

        const SoundInfo& sound = archive.Sounds()[index];
        files.push_back(sound.file_id);
        if (sound.bank < archive.Banks().size())
        {
            files.push_back(archive.Banks()[sound.bank].file_id);
        }
    }

    return files;
}

// For each group, which of its items SoundArchive::File reads for `files`. File returns spans into the archive's bytes.
// An item whose file and wave data have those offsets and sizes holds them. Like File, the test ignores the item's
// file ID.
std::vector<std::vector<bool>> ItemsRead(const SoundArchive& archive, const std::vector<uint32_t>& files)
{
    const std::vector<SoundArchive::Group>& groups = archive.Groups();
    std::vector<std::vector<bool>> read(groups.size());
    for (std::size_t g = 0; g < groups.size(); g++)
    {
        read[g].resize(groups[g].items.size());
    }

    const auto offset = [start = archive.Bytes().data()](std::span<const uint8_t> data)
    {
        return static_cast<uint64_t>(data.data() - start);
    };

    for (const uint32_t file_id : files)
    {
        const std::optional<FileData> file = archive.File(file_id);
        for (std::size_t g = 0; file && g < groups.size(); g++)
        {
            for (std::size_t i = 0; i < groups[g].items.size(); i++)
            {
                const SoundArchive::Group::Item& item = groups[g].items[i];
                read[g][i] = read[g][i] ||
                             (groups[g].internal && uint64_t{groups[g].offset} + item.offset == offset(file->file) &&
                              item.size == file->file.size() &&
                              uint64_t{groups[g].wave_data_offset} + item.wave_data_offset == offset(file->wave_data) &&
                              item.wave_data_size == file->wave_data.size());
            }
        }
    }

    return read;
}

// Where a group's kept items lie in the copy that KeepSequenceData makes: the group's files, then their wave data.
struct GroupPlace
{
    std::size_t offset = 0, size = 0, wave_data_offset = 0, wave_data_size = 0;
    std::vector<std::size_t> files, waves; // each kept item's offsets, from the group's two offsets
};

// Appends each group's kept items to `copy`: their files, then their wave data, each at a multiple of 32 bytes. Items
// of a group that hold the same bytes share one copy of them. Banks often share a wave archive. A group outside the
// archive keeps its place.
std::vector<GroupPlace> AppendKeptItems(const SoundArchive& archive,
                                        const std::vector<std::vector<bool>>& kept, std::vector<uint8_t>& copy)
{
    // The offsets and sizes of a group's bytes that the copy holds, and where each copy starts from the group's offset.
    using Copies = std::map<std::pair<uint64_t, uint32_t>, std::size_t>;

    // Appends the bytes at `offset` in the archive, unless `copies` holds them already. Returns where they start from
    // `base` in the copy.
    const std::span<const uint8_t> all = archive.Bytes();
    const auto append = [&all, &copy](Copies& copies, uint64_t offset, uint32_t size, std::size_t base)
    {
        const auto [at, added] = copies.try_emplace({offset, size}, 0);
        if (added)
        {
            PadToFileAlignment(copy);
            at->second = copy.size() - base;
            const std::span<const uint8_t> data = all.subspan(static_cast<std::size_t>(offset), size);
            copy.insert(copy.end(), data.begin(), data.end());
        }

        return at->second;
    };

    const std::vector<SoundArchive::Group>& groups = archive.Groups();
    std::vector<GroupPlace> places(groups.size());
    for (std::size_t g = 0; g < groups.size(); g++)
    {
        if (!groups[g].internal)
        {
            continue;
        }

        const SoundArchive::Group& group = groups[g];
        GroupPlace& place = places[g];
        place.files.resize(group.items.size());
        place.waves.resize(group.items.size());
        PadToFileAlignment(copy);
        place.offset = copy.size();
        Copies files;
        for (std::size_t i = 0; i < group.items.size(); i++)
        {
            if (kept[g][i])
            {
                place.files[i] =
                    append(files, uint64_t{group.offset} + group.items[i].offset, group.items[i].size, place.offset);
            }
        }

        place.size = copy.size() - place.offset;
        PadToFileAlignment(copy);
        place.wave_data_offset = copy.size();
        Copies waves;
        for (std::size_t i = 0; i < group.items.size(); i++)
        {
            if (kept[g][i])
            {
                place.waves[i] = append(waves, uint64_t{group.wave_data_offset} + group.items[i].wave_data_offset,
                                        group.items[i].wave_data_size, place.wave_data_offset);
            }
        }

        place.wave_data_size = copy.size() - place.wave_data_offset;
    }

    PadToFileAlignment(copy);

    return places;
}

// Writes the copy's size into its header and its FILE block's header, and each group's place and each item's into its
// INFO block. An item that isn't kept starts at the copy's end.
void WritePlaces(const SoundArchive& archive, const std::vector<std::vector<bool>>& kept,
                 const std::vector<GroupEntries>& entries, const std::vector<GroupPlace>& places,
                 std::size_t file_block, std::vector<uint8_t>& copy)
{
    const auto put = [&copy](std::size_t at, std::size_t value)
    {
        xsflib::StoreBe32(&copy[at], static_cast<uint32_t>(value));
    };

    const std::size_t end = copy.size();
    put(RsarLayout::kArchiveSize, end);
    put(RsarLayout::kFileBlockSize, end - file_block);
    put(file_block + RsarLayout::kBlockSize, end - file_block);
    for (std::size_t g = 0; g < places.size(); g++)
    {
        if (!archive.Groups()[g].internal)
        {
            continue;
        }

        const GroupPlace& place = places[g];
        put(entries[g].group + RsarLayout::kGroupOffset, place.offset);
        put(entries[g].group + RsarLayout::kGroupSize, place.size);
        put(entries[g].group + RsarLayout::kGroupWaveDataOffset, place.wave_data_offset);
        put(entries[g].group + RsarLayout::kGroupWaveDataSize, place.wave_data_size);
        for (std::size_t i = 0; i < entries[g].items.size(); i++)
        {
            put(entries[g].items[i] + RsarLayout::kItemOffset, kept[g][i] ? place.files[i] : end - place.offset);
            put(entries[g].items[i] + RsarLayout::kItemWaveDataOffset,
                kept[g][i] ? place.waves[i] : end - place.wave_data_offset);
        }
    }
}

// Whether two files hold the same bytes, wave data included, or neither is there.
bool SameFile(const std::optional<FileData>& a, const std::optional<FileData>& b)
{
    return a.has_value() == b.has_value() &&
           (!a || (std::ranges::equal(a->file, b->file) && std::ranges::equal(a->wave_data, b->wave_data)));
}

} // namespace

std::vector<uint8_t> KeepSequenceData(const SoundArchive& archive, const std::vector<uint32_t>& indices,
                                      const ReadOptions& options)
{
    // The copy keeps the archive up to its files: the header, the SYMB and INFO blocks and the FILE block's header. The
    // groups' entries must lie there.
    const std::span<const uint8_t> all = archive.Bytes();
    const xsflib::Reader r(all, true);
    const std::size_t file_block = r.U32(RsarLayout::kFileBlock);
    const std::size_t files_start = file_block + RsarLayout::kBlockHeader;
    const std::vector<GroupEntries> entries = FindGroupEntries(r);
    if (files_start > all.size() || !EntriesBeforeFiles(entries, archive.Groups(), file_block))
    {
        throw xsflib::FormatError("the archive's tables of groups don't come before its files");
    }

    // The FILE block then holds the items that a model reads.
    const std::vector<uint32_t> files = SequenceFiles(archive, indices);
    const std::vector<std::vector<bool>> kept = ItemsRead(archive, files);
    std::vector<uint8_t> copy(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(files_start));
    const std::vector<GroupPlace> places = AppendKeptItems(archive, kept, copy);
    if (copy.size() > kMaxArchiveSize)
    {
        throw xsflib::FormatError("the files that the sequences play are too large for a sound archive");
    }

    WritePlaces(archive, kept, entries, places, file_block, copy);

    // A model reads each sequence's files from the copy. They must hold the archive's bytes.
    const SoundArchive cut = SoundArchive::Load(copy, options);
    for (const uint32_t file_id : files)
    {
        if (!SameFile(archive.File(file_id), cut.File(file_id)))
        {
            throw xsflib::FormatError("the files that the sequences play can't be cut out of the archive");
        }
    }

    return copy;
}

std::vector<uint8_t> KeepGroups(const SoundArchive& archive, const std::vector<bool>& loaded)
{
    const std::vector<SoundArchive::Group>& groups = archive.Groups();

    // The ranges that the game reads: the header's first bytes, the INFO block, then each loaded group's files and
    // wave data. The loader skips wave data of no bytes.
    const std::span<const uint8_t> all = archive.Bytes();
    const xsflib::Reader r(all, true);
    const uint64_t info = r.U32(RsarLayout::kInfoBlock);
    std::vector<std::pair<uint64_t, uint64_t>> read = {{0, kHeaderRead},
                                                       {info, info + r.U32(RsarLayout::kInfoBlockSize)}};
    for (std::size_t g = 0; g < groups.size(); g++)
    {
        if (g >= loaded.size() || !loaded[g])
        {
            continue;
        }

        // A stream of a group of no bytes reads on to the archive's end (DvdSoundArchive's DvdFileStream). The copy
        // then keeps the whole archive.
        if (groups[g].size == 0)
        {
            return {all.begin(), all.end()};
        }

        read.emplace_back(groups[g].offset, uint64_t{groups[g].offset} + groups[g].size);
        if (groups[g].wave_data_size != 0)
        {
            read.emplace_back(groups[g].wave_data_offset,
                              uint64_t{groups[g].wave_data_offset} + groups[g].wave_data_size);
        }
    }

    // The copy keeps those ranges, rounded out to multiples of 32 bytes. The FILE block's other data becomes zeros.
    std::vector<uint8_t> copy(all.begin(), all.end());
    const uint64_t file_block = r.U32(RsarLayout::kFileBlock);
    const uint64_t end = std::min<uint64_t>(file_block + r.U32(RsarLayout::kFileBlockSize), copy.size());
    uint64_t at = file_block + RsarLayout::kBlockHeader;
    const auto zero_until = [&copy, &at, end](uint64_t stop)
    {
        if (at < std::min(stop, end))
        {
            std::fill(copy.begin() + static_cast<std::ptrdiff_t>(at),
                      copy.begin() + static_cast<std::ptrdiff_t>(std::min(stop, end)), uint8_t{0});
        }
    };

    std::sort(read.begin(), read.end());
    for (const auto& [start, stop] : read)
    {
        zero_until(start / kFileAlignment * kFileAlignment);
        at = std::max(at, (stop + kFileAlignment - 1) / kFileAlignment * kFileAlignment);
    }

    zero_until(end);

    return copy;
}

std::vector<std::string> MissingData(const SoundArchive& archive, uint32_t index)
{
    if (index >= archive.Sounds().size())
    {
        return {"the sound itself"};
    }

    const SoundInfo& sound = archive.Sounds()[index];
    std::vector<std::string> missing;
    if (!archive.File(sound.file_id))
    {
        missing.push_back("its sequence data");
    }

    if (sound.bank >= archive.Banks().size())
    {
        missing.push_back("its bank");
    }
    else if (!archive.File(archive.Banks()[sound.bank].file_id))
    {
        const std::string& name = archive.Banks()[sound.bank].name;
        missing.push_back("bank " + (name.empty() ? "#" + std::to_string(sound.bank) : name));
    }

    return missing;
}

} // namespace ntdlib::rsar
