// SPDX-License-Identifier: MIT

// The archive mode's parts of the rip tools (see archive.h).

#include "ntdlib/rip/archive.h"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/ascii.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/reference.h"

namespace ntdlib::rip
{

std::vector<std::string> ExternalGroups(const sar::SoundArchive& archive)
{
    std::vector<std::string> names;
    for (const sar::GroupInfo& group : archive.Groups())
    {
        if (group.file_id == sar::kExternalGroupFile && !group.name.empty())
        {
            names.push_back(group.name);
        }
    }

    return names;
}

std::vector<std::string> MissingData(const sar::SoundArchive& archive, uint32_t index, const sar::Options& options)
{
    if (index >= archive.Sounds().size())
    {
        return {"the sound itself"};
    }

    const sar::SoundInfo& sound = archive.Sounds()[index];
    std::vector<std::string> missing;
    if (archive.FileData(sound.file_id).empty())
    {
        missing.push_back("its sequence data");
    }

    // Each bank, and the wave archives its waves are in. An empty slot requires nothing.
    std::set<uint32_t> wave_archives;
    for (const uint32_t item : sound.banks)
    {
        if (item == sar::kNoItem)
        {
            continue;
        }

        const std::optional<std::size_t> bank = ItemIndex(item, kBankItem, archive.Banks().size());
        if (!bank)
        {
            missing.push_back("a bank the archive doesn't list");
            continue;
        }

        const sar::BankInfo& info = archive.Banks()[*bank];
        const std::span<const uint8_t> file = archive.FileData(info.file_id);
        const std::string name = info.name.empty() ? "#" + std::to_string(*bank) : info.name;
        if (file.empty())
        {
            missing.push_back("bank " + name);
            continue;
        }

        try
        {
            for (const sar::WaveId& wave : sar::Bank::Parse(file, options).waves)
            {
                wave_archives.insert(wave.wave_archive_item);
            }
        }
        catch (const std::exception&)
        {
            missing.push_back("bank " + name + " (can't be read)");
        }
    }

    for (const uint32_t item : wave_archives)
    {
        const std::optional<std::size_t> wave_archive =
            ItemIndex(item, kWaveArchiveItem, archive.WaveArchives().size());
        if (!wave_archive)
        {
            missing.push_back("a wave archive the archive doesn't list");
        }
        else if (archive.FileData(archive.WaveArchives()[*wave_archive].file_id).empty())
        {
            const std::string& name = archive.WaveArchives()[*wave_archive].name;
            missing.push_back("wave archive " + (name.empty() ? "#" + std::to_string(*wave_archive) : name));
        }
    }

    return missing;
}

bool IsMusic(const sar::SoundArchive& archive, uint32_t index)
{
    if (index >= archive.Sounds().size() || archive.Sounds()[index].type != sar::SoundType::kSequence)
    {
        return false;
    }

    const sar::SoundInfo& sound = archive.Sounds()[index];
    if (xsflib::AsciiLower(sound.name).find("bgm") != std::string::npos)
    {
        return true;
    }

    const std::optional<std::size_t> player = ItemIndex(sound.player_id, kPlayerItem, archive.Players().size());
    if (!player)
    {
        return false;
    }

    // The player's name, word by word, each without the digits it ends in (PLAYER_BGM1).
    bool music = false;
    bool effects = false;
    const std::string name = xsflib::AsciiLower(archive.Players()[*player].name);
    for (std::size_t start = 0; start <= name.size();)
    {
        const std::size_t end = std::min(name.find('_', start), name.size());
        std::string word = name.substr(start, end - start);
        word.erase(word.find_last_not_of("0123456789") + 1);
        music = music || word == "bgm" || word == "music" || word == "song" || word == "fanfare";
        effects = effects || word == "se";
        start = end + 1;
    }

    return music && !effects;
}

std::vector<Job> SelectSounds(const sar::SoundArchive& archive, const std::regex& only, bool bgm)
{
    std::vector<Job> jobs;
    const auto& sounds = archive.Sounds();
    for (std::size_t i = 0; i < sounds.size(); i++)
    {
        // Streams are out of scope. Wave sounds aren't ripped yet.
        const auto index = static_cast<uint32_t>(i);
        if (sounds[i].type == sar::SoundType::kSequence && std::regex_search(sounds[i].name, only) &&
            (!bgm || IsMusic(archive, index)))
        {
            jobs.push_back({index, sounds[i].name, std::nullopt});
        }
    }

    return jobs;
}

int NoSoundMatches(const std::string& only_text, bool bgm)
{
    if (only_text.empty())
    {
        std::fprintf(stderr, "error: no sequence is music (--bgm)\n");
    }
    else
    {
        std::fprintf(stderr, "error: no %s matches '%s'\n", bgm ? "music" : "sound", only_text.c_str());
    }

    return 1;
}

std::vector<Job> SkipIncomplete(const sar::SoundArchive& archive, std::vector<Job> jobs, const sar::Options& options,
                                Skipped& skipped)
{
    std::vector<Job> complete;
    for (Job& job : jobs)
    {
        const std::vector<std::string> missing = MissingData(archive, job.index, options);
        if (!missing.empty())
        {
            skipped.missing_data++;
            std::string what;
            for (std::size_t i = 0; i < missing.size(); i++)
            {
                what += (i ? ", " : "") + missing[i];
            }
            std::fprintf(stderr, "skipped %s: missing %s\n", job.label.c_str(), what.c_str());
        }
        else if (archive.Sounds()[job.index].volume == 0)
        {
            skipped.silent++;
            std::fprintf(stderr, "skipped %s: it makes no sound (volume 0)\n", job.label.c_str());
        }
        else
        {
            complete.push_back(std::move(job));
        }
    }

    return complete;
}

std::vector<Job> SkipSilent(std::vector<Job> jobs, Skipped& skipped)
{
    std::vector<Job> sounding;
    for (Job& job : jobs)
    {
        if (job.timing && job.timing->silent)
        {
            skipped.silent++;
            std::fprintf(stderr, "skipped %s: it makes no sound\n", job.label.c_str());
        }
        else
        {
            sounding.push_back(std::move(job));
        }
    }

    return sounding;
}

namespace
{

// The fields of a sound archive (CSAR or FSAR) that KeepSequenceData reads and writes. The header gives the byte order
// mark, the archive's size, the count of the blocks and a table of the blocks. Each entry of that table holds a type,
// two bytes of padding, an offset and a size. A block starts with its magic and its size. The INFO block's eight
// references to its tables follow its size. A file's entry in the file table starts with a reference to its place. A
// place in the FILE block holds a reference to the file's bytes, from the end of the FILE block's size, and the file's
// size. A file without bytes in the FILE block has a reference of type 0 there, with 0xFFFFFFFF for its offset and
// its size. A group's entry in the group table starts with the file ID of the group's file.
constexpr std::size_t kByteOrderMark = 0x04;
constexpr std::size_t kSizeField = 0x0C;
constexpr std::size_t kBlockCount = 0x10;
constexpr std::size_t kBlockTable = 0x14;
constexpr std::size_t kBlockEntrySize = 12;
constexpr std::size_t kEntryOffset = 4; // where a block's entry gives the block's offset
constexpr std::size_t kEntrySize = 8;   // where a block's entry gives the block's size
constexpr std::size_t kBlockSizeField = 4;
constexpr std::size_t kBlockHeaderSize = 8;
constexpr std::size_t kInfoTableCount = 8;
constexpr std::size_t kTableReferences = 4; // the references of a table follow its count
constexpr std::size_t kReferenceSize = 8;
constexpr std::size_t kPlaceOffset = 4; // in a place
constexpr std::size_t kPlaceSize = 8;   // in a place
constexpr uint16_t kInfoBlock = 0x2001;
constexpr uint16_t kFileBlock = 0x2002;
constexpr uint16_t kGroupTable = 0x2105;
constexpr uint16_t kFileTable = 0x2106;
constexpr uint16_t kInternalFile = 0x220C; // from a file's entry to its place in the FILE block
constexpr uint32_t kNoPlace = 0xFFFFFFFF;

// A span of an archive's bytes: its first byte and the byte after its last.
struct Span
{
    friend auto operator<=>(const Span&, const Span&) = default;

    std::size_t start = 0;
    std::size_t end = 0;
};

// A file's place in the FILE block: where the file table holds the place, whether the place has an offset, and the
// file's bytes. A place without an offset, or with bytes past the archive's end, gives no bytes.
struct Place
{
    std::size_t at = 0;
    bool has_offset = false;
    std::optional<Span> bytes;
};

// What KeepSequenceData reads of an archive's layout.
struct Layout
{
    bool big_endian = false;
    std::size_t file_block = 0;       // where the FILE block starts
    std::size_t file_block_entry = 0; // where the header's table of blocks lists it
    std::map<uint32_t, Place> places; // by file ID, for each file with a place in the FILE block
    std::set<uint32_t> group_files;   // the file IDs of the groups' files
};

// The position `offset` bytes from `base`. An offset that leads outside the archive gives a position that a read
// refuses.
std::size_t At(std::size_t base, int32_t offset)
{
    return base + static_cast<std::size_t>(static_cast<int64_t>(offset));
}

// Calls `use` with the index and the position of each entry of the INFO table at `table`.
template <typename Use>
void ForEachEntry(const xsflib::Reader& r, std::size_t table, const Use& use)
{
    const uint32_t count = r.U32(table);
    for (uint32_t i = 0; i < count; i++)
    {
        use(i, At(table, sar::ReadRef(r, table + kTableReferences + i * kReferenceSize).offset));
    }
}

// The layout of `bytes`, a sound archive. Nothing when the header's table of blocks, or a block other than the FILE
// block, ends after the FILE block's start. Throws xsflib::FormatError for tables that can't be read.
std::optional<Layout> ReadLayout(std::span<const uint8_t> bytes)
{
    Layout layout;
    layout.big_endian =
        bytes.size() > kByteOrderMark + 1 && bytes[kByteOrderMark] == 0xFE && bytes[kByteOrderMark + 1] == 0xFF;
    const xsflib::Reader r(bytes, layout.big_endian);

    // Everything before the FILE block's files keeps its place in the library's archive.
    const uint16_t count = r.U16(kBlockCount);
    uint64_t before_files = kBlockTable + uint64_t{count} * kBlockEntrySize;
    std::optional<std::size_t> info;
    std::optional<std::size_t> file_block;
    for (std::size_t entry = kBlockTable; entry < kBlockTable + count * kBlockEntrySize; entry += kBlockEntrySize)
    {
        const uint16_t type = r.U16(entry);
        const uint32_t offset = r.U32(entry + kEntryOffset);
        if (type == kFileBlock)
        {
            if (file_block)
            {
                return std::nullopt;
            }

            file_block = offset;
            layout.file_block_entry = entry;
            continue;
        }

        if (offset != kNoPlace)
        {
            before_files = std::max(before_files, uint64_t{offset} + r.U32(entry + kEntrySize));
        }
        if (type == kInfoBlock)
        {
            info = offset;
        }
    }

    if (!info || !file_block || before_files > *file_block || uint64_t{*file_block} + kBlockHeaderSize > bytes.size())
    {
        return std::nullopt;
    }
    layout.file_block = *file_block;

    // The tables of files and groups.
    std::map<uint16_t, std::size_t> tables;
    const std::size_t references = *info + kBlockHeaderSize;
    for (std::size_t i = 0; i < kInfoTableCount; i++)
    {
        const sar::Reference table = sar::ReadRef(r, references + i * kReferenceSize);
        tables[table.type] = At(references, table.offset);
    }
    const auto file_table = tables.find(kFileTable);
    const auto group_table = tables.find(kGroupTable);
    if (file_table == tables.end() || group_table == tables.end())
    {
        return std::nullopt;
    }

    // The places count from the end of the FILE block's size. The reader takes a file's bytes only when they lie in the
    // archive.
    const uint64_t data = layout.file_block + kBlockHeaderSize;
    const auto read_place = [&](uint32_t file, std::size_t entry)
    {
        const sar::Reference place = sar::ReadRef(r, entry);
        if (place.type != kInternalFile)
        {
            return;
        }

        Place& found = layout.places[file];
        found.at = At(entry, place.offset);
        const uint32_t offset = r.U32(found.at + kPlaceOffset);
        const uint32_t size = r.U32(found.at + kPlaceSize);
        found.has_offset = offset != kNoPlace;
        if (found.has_offset && data + offset + size <= bytes.size())
        {
            found.bytes = Span{static_cast<std::size_t>(data + offset), static_cast<std::size_t>(data + offset + size)};
        }
    };
    ForEachEntry(r, file_table->second, read_place);

    const auto read_group = [&](uint32_t, std::size_t entry)
    {
        layout.group_files.insert(r.U32(entry));
    };
    ForEachEntry(r, group_table->second, read_group);

    return layout;
}

// The files that the sequence sounds `sounds` of `archive` play: each sound's sequence file, the files of the banks in
// its slots, and the files of the wave archives that those banks name. Nothing for a sound that isn't one of the
// archive's sequence sounds. Throws xsflib::FormatError for a bank that can't be read.
std::optional<std::set<uint32_t>> FilesPlayed(const sar::SoundArchive& archive, std::span<const uint32_t> sounds)
{
    std::set<uint32_t> files;
    std::set<uint32_t> banks;
    std::set<uint32_t> wave_archives;
    for (const uint32_t index : sounds)
    {
        if (index >= archive.Sounds().size() || archive.Sounds()[index].type != sar::SoundType::kSequence)
        {
            return std::nullopt;
        }

        const sar::SoundInfo& sound = archive.Sounds()[index];
        files.insert(sound.file_id);
        for (const uint32_t item : sound.banks)
        {
            const std::optional<std::size_t> bank = ItemIndex(item, kBankItem, archive.Banks().size());
            if (!bank || !banks.insert(archive.Banks()[*bank].file_id).second)
            {
                continue; // an empty slot, or a bank read already
            }

            const std::span<const uint8_t> file = archive.FileData(archive.Banks()[*bank].file_id);
            if (file.empty())
            {
                continue; // a model plays nothing from it
            }

            const sar::Bank read = sar::Bank::Parse(file, archive.ReadOptions());
            for (const sar::WaveId& wave : read.waves)
            {
                wave_archives.insert(wave.wave_archive_item);
            }
        }
    }

    files.insert(banks.begin(), banks.end());
    for (const uint32_t item : wave_archives)
    {
        if (const std::optional<std::size_t> wave_archive =
                ItemIndex(item, kWaveArchiveItem, archive.WaveArchives().size()))
        {
            files.insert(archive.WaveArchives()[*wave_archive].file_id);
        }
    }

    return files;
}

// The place among the archive's group files of the one that holds `copy`, or nothing.
std::optional<std::size_t> GroupFileHolding(const sar::SoundArchive& archive, std::span<const uint8_t> copy)
{
    const std::less_equal<const uint8_t*> not_after;
    for (std::size_t i = 0; i < archive.GroupFiles().size(); i++)
    {
        const std::vector<uint8_t>& file = archive.GroupFiles()[i];
        if (not_after(file.data(), copy.data()) && not_after(copy.data() + copy.size(), file.data() + file.size()))
        {
            return i;
        }
    }

    return std::nullopt;
}

// The bytes that `archive` reads `files` from: the bytes of each file with a place, and those of each group's file
// in the archive that holds the copy of a file without one. Adds the group files beside the archive that hold the
// copies of the others to `beside`. Nothing when the bytes of a file can't be found.
std::optional<std::vector<Span>> SpansRead(const sar::SoundArchive& archive, const Layout& layout,
                                           const std::set<uint32_t>& files, std::set<std::size_t>& beside)
{
    std::vector<Span> spans;
    for (const uint32_t file : files)
    {
        const std::span<const uint8_t> data = archive.FileData(file);
        if (data.empty())
        {
            continue; // a model reads nothing from it
        }

        const std::optional<std::size_t> offset = archive.FileOffset(file);
        const auto place = layout.places.find(file);
        if (place != layout.places.end() && place->second.bytes)
        {
            if (offset != place->second.bytes->start)
            {
                return std::nullopt;
            }

            spans.push_back(*place->second.bytes);
            continue;
        }

        // The reader takes the copy from a group's file in the archive. Each group's file that holds the copy stays
        // whole.
        if (offset)
        {
            const Span copy{*offset, *offset + data.size()};
            const std::size_t before = spans.size();
            for (const uint32_t group_file : layout.group_files)
            {
                const auto group = layout.places.find(group_file);
                if (group != layout.places.end() && group->second.bytes && group->second.bytes->start <= copy.start &&
                    copy.end <= group->second.bytes->end)
                {
                    spans.push_back(*group->second.bytes);
                }
            }
            if (spans.size() == before)
            {
                return std::nullopt;
            }

            continue;
        }

        const std::optional<std::size_t> group = GroupFileHolding(archive, data);
        if (!group)
        {
            return std::nullopt;
        }
        beside.insert(*group);
    }

    return spans;
}

// A run of the archive's bytes that the library's archive keeps: its span in the archive, and where it starts in the
// library's archive.
struct Run
{
    Span bytes;
    std::size_t start = 0;
};

// The runs that hold `spans`. Spans that overlap share a run.
std::vector<Run> RunsOf(std::vector<Span> spans)
{
    std::ranges::sort(spans);
    std::vector<Run> runs;
    for (const Span& span : spans)
    {
        if (!runs.empty() && span.start < runs.back().bytes.end)
        {
            runs.back().bytes.end = std::max(runs.back().bytes.end, span.end);
        }
        else
        {
            runs.push_back({span, 0});
        }
    }

    return runs;
}

// The run that holds the whole of `span`, or null.
const Run* RunHolding(const std::vector<Run>& runs, const Span& span)
{
    const auto after = std::ranges::upper_bound(runs, span.start, {}, [](const Run& run) { return run.bytes.start; });
    if (after == runs.begin())
    {
        return nullptr;
    }

    const Run& run = *(after - 1);

    return span.end <= run.bytes.end ? &run : nullptr;
}

// The library's archive: `bytes` up to the FILE block's files, then `runs`. Each run starts on the first multiple of
// `alignment` bytes from the archive's start after the bytes before it. Each place whose bytes lie in a run moves with
// it. Each other place with an offset loses it. The place then matches that of a file that only groups hold. The header
// and the FILE block give the new sizes. Sets each run's start. Nothing for an archive past 4 GB, or for a place that
// doesn't lie between the archive's start and the FILE block's files.
std::optional<std::vector<uint8_t>> CopyRuns(std::span<const uint8_t> bytes, const Layout& layout,
                                             std::vector<Run>& runs, std::size_t alignment)
{
    const std::size_t data = layout.file_block + kBlockHeaderSize;
    std::vector<uint8_t> copy(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(data));
    const auto align = [&copy, alignment]
    {
        copy.resize((copy.size() + alignment - 1) / alignment * alignment);
    };
    for (Run& run : runs)
    {
        align();
        run.start = copy.size();
        copy.insert(copy.end(), bytes.begin() + static_cast<std::ptrdiff_t>(run.bytes.start),
                    bytes.begin() + static_cast<std::ptrdiff_t>(run.bytes.end));
    }
    if (runs.empty())
    {
        align(); // a FILE block without files ends on the first multiple after its size
    }
    if (copy.size() > std::numeric_limits<uint32_t>::max())
    {
        return std::nullopt;
    }

    const auto put = [&copy, &layout](std::size_t at, std::size_t value)
    {
        const auto word = static_cast<uint32_t>(value);
        layout.big_endian ? xsflib::StoreBe32(&copy[at], word) : xsflib::StoreLe32(&copy[at], word);
    };
    for (const auto& [file, place] : layout.places)
    {
        // The tools write each place in the INFO block, before the FILE block's files. A crafted file table can put a
        // place among the files, where the copy holds other bytes or none. It can also put one before the archive's
        // start. The place's position then wraps past the largest size_t.
        if (place.at >= data || data - place.at < kPlaceSize + sizeof(uint32_t))
        {
            return std::nullopt;
        }

        const Run* run = place.bytes ? RunHolding(runs, *place.bytes) : nullptr;
        if (run != nullptr)
        {
            put(place.at + kPlaceOffset, run->start + (place.bytes->start - run->bytes.start) - data);
        }
        else if (place.has_offset)
        {
            std::fill_n(copy.begin() + static_cast<std::ptrdiff_t>(place.at), kPlaceOffset, uint8_t{0});
            put(place.at + kPlaceOffset, kNoPlace);
            put(place.at + kPlaceSize, kNoPlace);
        }
    }

    put(kSizeField, copy.size());
    put(layout.file_block_entry + kEntrySize, copy.size() - layout.file_block);
    put(layout.file_block + kBlockSizeField, copy.size() - layout.file_block);

    return copy;
}

// The fields of a group file (CGRP or FGRP) that KeepGroupCopies reads and writes. The header holds the same fields as
// an archive's header. The INFO block's table of items follows its size: a count, then a reference to each item. An
// item holds a file ID, a reference to its copy's bytes from the end of the FILE block's size, and the copy's size. A
// link to the archive's file has a reference of type 0, with 0xFFFFFFFF for its offset and the size.
constexpr uint16_t kGroupInfoBlock = 0x7800;
constexpr uint16_t kGroupFileBlock = 0x7801;
constexpr uint16_t kGroupCopy = 0x1F00;     // from an item to its copy's bytes
constexpr std::size_t kItemReference = 4;   // where an item holds the reference to its copy
constexpr std::size_t kItemCopySize = 12;   // where an item gives its copy's size
constexpr std::size_t kItemLength = 16;     // the bytes of an item
constexpr std::size_t kReferenceOffset = 4; // where a reference gives its offset

// A copy of a file in a group file: the file's ID and the copy's bytes in the group file.
struct Copy
{
    friend auto operator<=>(const Copy&, const Copy&) = default;

    uint32_t file = 0;
    Span bytes;
};

// `file`, a group file, cut down to the copies `kept` (see KeepGroupCopies). The first item of each copy keeps it.
// Every other item with a copy becomes a link. Each run of kept bytes starts on the first multiple of `alignment` bytes
// from the file's start after the bytes before it. Nothing when one of `kept` isn't an item's copy, or when the blocks
// aren't the INFO block and the rest before the FILE block, and blocks after it.
std::optional<std::vector<uint8_t>> CutGroupFile(std::span<const uint8_t> file, const std::set<Copy>& kept,
                                                 std::size_t alignment)
{
    const bool big_endian =
        file.size() > kByteOrderMark + 1 && file[kByteOrderMark] == 0xFE && file[kByteOrderMark + 1] == 0xFF;
    const xsflib::Reader r(file, big_endian);

    // The FILE block, and the INFO block and the table of blocks before it. No other block overlaps it.
    const uint16_t count = r.U16(kBlockCount);
    const std::size_t table_end = kBlockTable + std::size_t{count} * kBlockEntrySize;
    std::optional<std::size_t> info;
    std::optional<std::size_t> file_entry;
    for (std::size_t entry = kBlockTable; entry < table_end; entry += kBlockEntrySize)
    {
        const uint16_t type = r.U16(entry);
        if (type == kGroupFileBlock)
        {
            if (file_entry)
            {
                return std::nullopt; // a second FILE block
            }
            file_entry = entry;
        }
        else if (type == kGroupInfoBlock)
        {
            info = r.U32(entry + kEntryOffset);
        }
    }
    if (!info || !file_entry)
    {
        return std::nullopt;
    }

    const uint64_t file_block = r.U32(*file_entry + kEntryOffset);
    const uint64_t file_end = file_block + r.U32(*file_entry + kEntrySize);
    if (table_end > file_block || file_block + kBlockHeaderSize > file_end || file_end > file.size())
    {
        return std::nullopt;
    }

    std::vector<std::size_t> later; // the entries of the blocks after the FILE block
    for (std::size_t entry = kBlockTable; entry < table_end; entry += kBlockEntrySize)
    {
        const uint64_t start = r.U32(entry + kEntryOffset);
        if (entry == *file_entry)
        {
            continue;
        }
        if (start >= file_end)
        {
            later.push_back(entry);
        }
        else if (start + r.U32(entry + kEntrySize) > file_block)
        {
            return std::nullopt;
        }
    }

    // The items, all in the INFO block before the FILE block. The first item of each kept copy keeps it.
    const std::size_t data = static_cast<std::size_t>(file_block) + kBlockHeaderSize;
    const std::size_t items = *info + kBlockHeaderSize;
    std::vector<std::pair<std::size_t, Span>> keep; // where each kept item lies, and its copy
    std::vector<std::size_t> links;                 // where each item that becomes a link lies
    std::set<Copy> found;
    const auto read_item = [&](uint32_t, std::size_t item)
    {
        if (item + kItemLength > file_block)
        {
            throw xsflib::FormatError("an item of the group file lies in its FILE block");
        }

        const sar::Reference copy = sar::ReadRef(r, item + kItemReference);
        if (copy.type != kGroupCopy)
        {
            return; // a link already
        }

        const std::size_t start = At(data, copy.offset);
        const Copy item_copy{r.U32(item), Span{start, start + r.U32(item + kItemCopySize)}};
        if (kept.contains(item_copy) && found.insert(item_copy).second)
        {
            keep.push_back({item, item_copy.bytes});
        }
        else
        {
            links.push_back(item);
        }
    };
    ForEachEntry(r, items, read_item);
    if (found.size() != kept.size())
    {
        return std::nullopt;
    }

    // The kept bytes, then the bytes after the FILE block, which hold the blocks after it.
    std::vector<Span> spans;
    for (const auto& [item, bytes] : keep)
    {
        spans.push_back(bytes);
    }
    std::vector<Run> runs = RunsOf(spans);
    std::vector<uint8_t> cut(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(data));
    const auto align = [&cut, alignment]
    {
        cut.resize((cut.size() + alignment - 1) / alignment * alignment);
    };
    for (Run& run : runs)
    {
        align();
        run.start = cut.size();
        cut.insert(cut.end(), file.begin() + static_cast<std::ptrdiff_t>(run.bytes.start),
                   file.begin() + static_cast<std::ptrdiff_t>(run.bytes.end));
    }
    align();
    const std::size_t cut_file_end = cut.size();
    cut.insert(cut.end(), file.begin() + static_cast<std::ptrdiff_t>(file_end), file.end());
    if (cut.size() > std::numeric_limits<uint32_t>::max())
    {
        return std::nullopt;
    }

    const auto put = [&cut, big_endian](std::size_t at, std::size_t value)
    {
        const auto word = static_cast<uint32_t>(value);
        big_endian ? xsflib::StoreBe32(&cut[at], word) : xsflib::StoreLe32(&cut[at], word);
    };
    for (const auto& [item, bytes] : keep)
    {
        const Run* run = RunHolding(runs, bytes);
        put(item + kItemReference + kReferenceOffset, run->start + (bytes.start - run->bytes.start) - data);
    }
    for (const std::size_t item : links)
    {
        std::fill_n(cut.begin() + static_cast<std::ptrdiff_t>(item + kItemReference), kReferenceOffset, uint8_t{0});
        put(item + kItemReference + kReferenceOffset, kNoPlace);
        put(item + kItemCopySize, kNoPlace);
    }
    for (const std::size_t entry : later)
    {
        put(entry + kEntryOffset, r.U32(entry + kEntryOffset) - file_end + cut_file_end);
    }

    put(kSizeField, cut.size());
    put(*file_entry + kEntrySize, cut_file_end - file_block);
    put(static_cast<std::size_t>(file_block) + kBlockSizeField, cut_file_end - file_block);

    return cut;
}

// Whether `cut`, read back by `rules` with the group files `beside` of `archive`, gives the bytes of `files` as
// `archive` does.
bool SameFiles(const sar::SoundArchive& archive, const std::vector<uint8_t>& cut, const std::set<std::size_t>& beside,
               const std::set<uint32_t>& files, sar::Rules rules)
{
    sar::Options options = archive.ReadOptions();
    options.rules = rules;
    sar::SoundArchive read = sar::SoundArchive::Load(cut, options);
    for (const std::size_t group : beside)
    {
        read.AddGroupFile(archive.GroupFiles()[group]);
    }

    const auto same = [&](uint32_t file)
    {
        return std::ranges::equal(read.FileData(file), archive.FileData(file));
    };

    return read.FileCount() == archive.FileCount() && std::ranges::all_of(files, same);
}

} // namespace

std::optional<LibraryArchive> KeepSequenceData(const sar::SoundArchive& archive, std::span<const uint32_t> sounds)
{
    try
    {
        const std::optional<Layout> layout = ReadLayout(archive.Bytes());
        if (!layout)
        {
            return std::nullopt;
        }

        // The bytes that the models read, and the group files beside the archive that they read too.
        const std::optional<std::set<uint32_t>> files = FilesPlayed(archive, sounds);
        std::set<std::size_t> beside;
        const std::optional<std::vector<Span>> spans =
            files ? SpansRead(archive, *layout, *files, beside) : std::nullopt;
        if (!spans)
        {
            return std::nullopt;
        }

        // The tools start each file on a multiple of 64 bytes on the Switch, and of 32 bytes on the 3DS and the Wii U.
        std::vector<Run> runs = RunsOf(*spans);
        const std::size_t alignment = archive.ReadOptions().flavor == sar::Flavor::kNx ? 64 : 32;
        std::optional<std::vector<uint8_t>> cut = CopyRuns(archive.Bytes(), *layout, runs, alignment);
        if (!cut || !SameFiles(archive, *cut, beside, *files, archive.ReadOptions().rules))
        {
            return std::nullopt;
        }

        return LibraryArchive{std::move(*cut), std::vector<std::size_t>(beside.begin(), beside.end())};
    }
    catch (const xsflib::FormatError&)
    {
        return std::nullopt;
    }
}

std::optional<std::vector<std::vector<uint8_t>>> KeepGroupCopies(const sar::SoundArchive& archive,
                                                                 const LibraryArchive& library,
                                                                 std::span<const uint32_t> sounds)
{
    try
    {
        const std::optional<std::set<uint32_t>> files = FilesPlayed(archive, sounds);
        if (!files)
        {
            return std::nullopt;
        }

        // The copies that the models read, in each group file that the library holds.
        std::map<std::size_t, std::set<Copy>> kept;
        for (const std::size_t group : library.group_files)
        {
            if (group >= archive.GroupFiles().size())
            {
                return std::nullopt;
            }
            kept.try_emplace(group);
        }
        for (const uint32_t file : *files)
        {
            const std::span<const uint8_t> data = archive.FileData(file);
            const std::optional<std::size_t> group = data.empty() ? std::nullopt : GroupFileHolding(archive, data);
            if (!group)
            {
                continue; // in the archive, or read from nowhere
            }

            const auto copies = kept.find(*group);
            if (copies == kept.end())
            {
                return std::nullopt;
            }
            const auto start = static_cast<std::size_t>(data.data() - archive.GroupFiles()[*group].data());
            copies->second.insert({file, Span{start, start + data.size()}});
        }

        // The tools start each file on a multiple of 64 bytes on the Switch, and of 32 bytes on the 3DS and the Wii U.
        const std::size_t alignment = archive.ReadOptions().flavor == sar::Flavor::kNx ? 64 : 32;
        std::vector<std::vector<uint8_t>> cuts;
        sar::SoundArchive read = sar::SoundArchive::Load(library.archive, archive.ReadOptions());
        for (const std::size_t group : library.group_files)
        {
            std::optional<std::vector<uint8_t>> cut = CutGroupFile(archive.GroupFiles()[group], kept[group], alignment);
            if (!cut)
            {
                return std::nullopt;
            }
            read.AddGroupFile(*cut);
            cuts.push_back(std::move(*cut));
        }

        // Each kept file reads back as before.
        const auto same = [&](uint32_t file)
        {
            return std::ranges::equal(read.FileData(file), archive.FileData(file));
        };
        if (read.FileCount() != archive.FileCount() || !std::ranges::all_of(*files, same))
        {
            return std::nullopt;
        }

        return cuts;
    }
    catch (const xsflib::FormatError&)
    {
        return std::nullopt;
    }
}

std::optional<std::vector<uint8_t>> KeepLoadedFiles(const sar::SoundArchive& archive, std::span<const uint32_t> sounds,
                                                    std::size_t read_alignment)
{
    try
    {
        const std::span<const uint8_t> bytes = archive.Bytes();
        const std::optional<Layout> layout = ReadLayout(bytes);
        const std::optional<std::set<uint32_t>> files = layout ? FilesPlayed(archive, sounds) : std::nullopt;
        if (!files)
        {
            return std::nullopt;
        }

        // The bytes that the game reads of each file, rounded out as its reads are.
        std::vector<Span> read;
        for (const uint32_t file : *files)
        {
            const auto place = layout->places.find(file);
            if (place == layout->places.end() || !place->second.bytes)
            {
                return std::nullopt;
            }

            const Span& span = *place->second.bytes;
            const std::size_t rounded_end = (span.end + read_alignment - 1) / read_alignment * read_alignment;
            read.push_back({span.start / read_alignment * read_alignment, std::min(rounded_end, bytes.size())});
        }

        // The FILE block's files become zeros, apart from those bytes.
        const xsflib::Reader r(bytes, layout->big_endian);
        const std::size_t data = layout->file_block + kBlockHeaderSize;
        const auto block_end = static_cast<std::size_t>(std::min<uint64_t>(
            uint64_t{layout->file_block} + r.U32(layout->file_block_entry + kEntrySize), bytes.size()));
        std::vector<uint8_t> copy(bytes.begin(), bytes.end());
        if (data < block_end)
        {
            std::fill(copy.begin() + static_cast<std::ptrdiff_t>(data),
                      copy.begin() + static_cast<std::ptrdiff_t>(block_end), uint8_t{0});
        }

        for (const Span& span : read)
        {
            std::copy(bytes.begin() + static_cast<std::ptrdiff_t>(span.start),
                      bytes.begin() + static_cast<std::ptrdiff_t>(span.end),
                      copy.begin() + static_cast<std::ptrdiff_t>(span.start));
        }

        // The zeros in place of the groups' files aren't group files. The copy therefore reads back by the rules that
        // read past damage.
        if (!SameFiles(archive, copy, {}, *files, sar::Rules::kReadPastDamage))
        {
            return std::nullopt;
        }

        return copy;
    }
    catch (const xsflib::FormatError&)
    {
        return std::nullopt;
    }
}

} // namespace ntdlib::rip
