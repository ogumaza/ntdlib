// SPDX-License-Identifier: MIT

#include "ntdlib/rsar/layout.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "xsflib/binary.h"

#include "ntdlib/commands/sequence.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/reference.h"
#include "ntdlib/rsar/sequence_file.h"
#include "ntdlib/sar/layout.h"
#include "sar/layout_core.h"

namespace ntdlib::rsar
{
namespace
{

using sar::core::Distance;
using sar::core::Location;
using sar::core::Start;
using sar::core::Structure;

// The blocks whose offsets and sizes the header holds: SYMB, INFO and FILE.
constexpr std::size_t kBlocks = 3;

// Marks the wave data of a file among the cutter's locations. A file's ID and its wave data's differ only in this bit.
constexpr uint32_t kWaveData = 0x80000000;

// Where the writer puts a sequence file's bytecode: DATA follows the header of 0x20 bytes, and its bytecode starts 0xC
// bytes into it.
constexpr std::size_t kBytecode = 0x2C;

// NW4R's tools align the files of a group to 32 bytes. An archive whose files all start at multiples of 64 still pads
// a file that grows to 32 bytes only.
constexpr std::size_t kAlignment = 32;

// A sequence sound's type in the sound table.
constexpr uint8_t kSequenceSound = 1;

// The entries of the table that the reference at `ref` points to: where each entry starts, and its reference. The
// references count from `base`, the start of INFO's payload. Nothing for a reference that isn't an offset.
std::vector<std::pair<std::size_t, Reference>> EntriesOf(const xsflib::Reader& r, std::size_t base, std::size_t ref)
{
    std::vector<std::pair<std::size_t, Reference>> entries;
    const Reference table = ReadRef(r, ref);
    if (!table.IsOffset())
    {
        return entries;
    }

    const std::size_t at = base + table.offset;
    const uint32_t count = r.U32(at);
    for (uint32_t i = 0; i < count; ++i)
    {
        const Reference entry = ReadRef(r, at + 4 + std::size_t{i} * 8);
        entries.emplace_back(base + entry.offset, entry);
    }

    return entries;
}

// The start of each sound of the sound table that is a sequence sound: its file, its offset in the bytecode, and the
// field that holds the offset. Before version 1.1, the entry's reference gives a sound's type. From 1.1 on, a field
// does (SoundArchiveFileReader::ReadSoundInfo, main.dol 0x800425a8).
std::vector<std::optional<Start>> SoundStarts(const xsflib::Reader& r)
{
    const uint16_t version = r.U16(6);
    const std::size_t base = r.U32(0x18) + std::size_t{8};
    std::vector<std::optional<Start>> starts;
    for (const auto& [sound, entry] : EntriesOf(r, base, base))
    {
        const Reference details = ReadRef(r, sound + 0x18);
        const uint8_t type = version >= 0x0101 ? r.U8(sound + 0x16) : entry.data_type;
        if (type != kSequenceSound || !details.IsOffset())
        {
            starts.emplace_back();
            continue;
        }

        const std::size_t at = base + details.offset;
        starts.push_back(Start{r.U32(sound + 4), r.U32(at), at});
    }

    return starts;
}

// A copy of a file in a group: the file, and its wave data.
struct Item
{
    Location file;
    Location wave_data;
};

// What the cutter reads of the tables of `archive`: where each group holds a copy of a file and of its wave data, the
// fields that hold their offsets and sizes, and the sequence sounds' starts. Throws xsflib::FormatError for tables that
// can't be read.
Structure ReadStructure(std::span<const uint8_t> archive)
{
    const xsflib::Reader r(archive, true);
    if (!r.Magic(0, "RSAR"))
    {
        throw xsflib::FormatError("not a sound archive of the Wii (RSAR)");
    }

    // The header holds the archive's size, and each block's offset and size. A block's header holds its size again.
    // An archive without names has no SYMB block.
    Structure structure;
    structure.distances.push_back({0x08, 0, r.U32(0x08), true});
    for (std::size_t i = 0; i < kBlocks; ++i)
    {
        const std::size_t block = r.U32(0x10 + i * 8);
        if (block != 0)
        {
            structure.distances.push_back({0x10 + i * 8, 0, block, false});
            structure.distances.push_back({0x14 + i * 8, block, block + r.U32(0x14 + i * 8), true});
            structure.distances.push_back({block + 4, block, block + r.U32(block + 4), true});
        }
    }

    // A group holds its files at offsets from its start, and their wave data at offsets from the start of its wave
    // data. A group with the path of an external file keeps them in that file (SoundArchiveFileReader::ReadGroupInfo,
    // main.dol 0x80042ac4). An item without wave data holds 0 for its wave data's offset.
    const std::size_t base = r.U32(0x18) + std::size_t{8};
    std::vector<std::vector<std::optional<Item>>> groups;
    for (const auto& [group, entry] : EntriesOf(r, base, base + 0x20))
    {
        std::vector<std::optional<Item>>& items = groups.emplace_back();
        const bool external = ReadRef(r, group + 8).IsOffset();
        const std::size_t files = r.U32(group + 0x10);
        const std::size_t waves = r.U32(group + 0x18);
        for (const auto& [item, item_entry] : EntriesOf(r, base, group + 0x20))
        {
            if (external)
            {
                items.emplace_back();
                continue;
            }

            const uint32_t id = r.U32(item);
            const Location file{id, files + r.U32(item + 4), r.U32(item + 8)};
            const Location wave_data{id | kWaveData, waves + r.U32(item + 0xC), r.U32(item + 0x10)};
            structure.files.push_back(file);
            structure.distances.push_back({item + 4, files, file.start, false});
            structure.distances.push_back({item + 8, file.start, file.start + file.size, true});
            if (wave_data.size > 0)
            {
                structure.files.push_back(wave_data);
                structure.distances.push_back({item + 0xC, waves, wave_data.start, false});
                structure.distances.push_back({item + 0x10, wave_data.start, wave_data.start + wave_data.size, true});
            }
            items.push_back(Item{file, wave_data});
        }
        if (!external)
        {
            structure.distances.push_back({group + 0x10, 0, files, false});
            structure.distances.push_back({group + 0x14, files, files + r.U32(group + 0x14), true});
            structure.distances.push_back({group + 0x18, 0, waves, false});
            structure.distances.push_back({group + 0x1C, waves, waves + r.U32(group + 0x1C), true});
        }
    }

    // The file table holds each file's size and its wave data's, those of each copy. The first copy in the archive
    // gives the fields' places.
    for (const auto& [file, entry] : EntriesOf(r, base, base + 0x18))
    {
        for (const auto& [position, position_entry] : EntriesOf(r, base, file + 0x14))
        {
            const uint32_t group = r.U32(position);
            const uint32_t item = r.U32(position + 4);
            if (group >= groups.size() || item >= groups[group].size() || !groups[group][item])
            {
                continue;
            }

            const Item& copy = *groups[group][item];
            structure.distances.push_back({file, copy.file.start, copy.file.start + r.U32(file), true});
            if (copy.wave_data.size > 0)
            {
                structure.distances.push_back(
                    {file + 4, copy.wave_data.start, copy.wave_data.start + r.U32(file + 4), true});
            }
            break;
        }
    }

    for (const std::optional<Start>& start : SoundStarts(r))
    {
        if (start)
        {
            structure.starts.push_back(*start);
        }
    }

    return structure;
}

// The rules of the Wii's archives: RSEQ files, big-endian, with the bytecode of NW4R SND's build `build`.
sar::core::Container ContainerOf(const SndBuild& build)
{
    sar::core::Container container;
    std::memcpy(container.sequence_magic.data(), "RSEQ", 4);
    container.big_endian = true;
    container.grammar = nwsnd::GrammarOf<nwsnd::RevolutionPolicy>(build);
    container.size_field = 0x08;
    container.most_alignment = kAlignment;

    container.read = [](std::span<const uint8_t> bytes)
    {
        const SequenceFile file = SequenceFile::Read(bytes);
        sar::core::SequenceParts parts;
        parts.version = file.version;
        parts.data = file.data;
        for (const Sequence::Label& label : file.labels)
        {
            parts.labels.push_back({label.name, label.offset});
        }

        return parts;
    };

    container.write = [](const sar::core::SequenceParts& parts)
    {
        SequenceFile file;
        file.version = static_cast<uint16_t>(parts.version);
        file.data = parts.data;
        for (const nwsnd::LabelOffset& label : parts.labels)
        {
            file.labels.push_back({label.name, label.offset});
        }

        return sar::core::WrittenFile{file.Write(), kBytecode};
    };

    return container;
}

// The bytes of `location` that `archive` holds: all of them, or those before its end.
std::span<const uint8_t> BytesOf(std::span<const uint8_t> archive, const Location& location)
{
    const std::size_t start = std::min(location.start, archive.size());

    return archive.subspan(start, std::min(location.size, archive.size() - start));
}

// `bytes` with zeros up to a multiple of kAlignment bytes, as NW4R's tools pad each file and each wave data.
std::vector<uint8_t> Padded(std::span<const uint8_t> bytes)
{
    std::vector<uint8_t> padded(bytes.begin(), bytes.end());
    padded.resize((padded.size() + kAlignment - 1) / kAlignment * kAlignment);

    return padded;
}

// Whether `joined`, whose tables `after` reads, holds the bytes of `news` in each copy of each of its locations, by
// their IDs, and the bytes of `archive`, whose tables `before` reads, in every other location.
bool Follows(std::span<const uint8_t> archive, const Structure& before, std::span<const uint8_t> joined,
             const Structure& after, const std::map<uint32_t, std::vector<uint8_t>>& news)
{
    if (after.files.size() != before.files.size())
    {
        return false;
    }

    for (std::size_t i = 0; i < before.files.size(); ++i)
    {
        const auto found = news.find(before.files[i].file);
        const std::span<const uint8_t> now = BytesOf(joined, after.files[i]);
        if (after.files[i].file != before.files[i].file ||
            !std::ranges::equal(now, found != news.end() ? std::span<const uint8_t>(found->second)
                                                          : BytesOf(archive, before.files[i])))
        {
            return false;
        }
    }

    return true;
}

// The name of location `id` for a message: a file, or a file's wave data.
std::string NameOf(uint32_t id)
{
    const std::string file = "file " + std::to_string(id & ~kWaveData);

    return (id & kWaveData) != 0 ? "the wave data of " + file : file;
}

} // namespace

Layout Cut(std::span<const uint8_t> archive, const SndBuild& build, const sar::KnownSequences& known)
{
    // The reader checks the tables, and reports what it can't read.
    static_cast<void>(SoundArchive::Load(std::vector<uint8_t>(archive.begin(), archive.end())));

    try
    {
        const sar::core::Source source{archive, ReadStructure(archive)};
        sar::core::Cutting cut = sar::core::CutBy(std::span(&source, 1), ContainerOf(build), known);
        Layout layout;
        layout.sequences = std::move(cut.sequences);
        layout.pieces = std::move(cut.pieces);
        layout.fields = std::move(cut.fields);

        return layout;
    }
    catch (const xsflib::FormatError&)
    {
    }

    Layout layout;
    layout.pieces.emplace_back(std::vector<uint8_t>(archive.begin(), archive.end()));

    return layout;
}

std::vector<uint8_t> Join(const SndBuild& build, std::span<const commands::Sequence> sequences,
                          std::span<const sar::Piece> pieces, std::span<const sar::Field> fields)
{
    return sar::core::JoinBy(ContainerOf(build), sequences, pieces, fields);
}

std::vector<std::size_t> Starts(std::span<const uint8_t> archive, std::span<const sar::Piece> pieces)
{
    return sar::core::StartsBy(ContainerOf(SndBuild{}), archive, pieces);
}

std::vector<sar::Piece> ReplaceFile(const SndBuild& build, std::span<const commands::Sequence> sequences,
                                    std::span<const sar::Piece> pieces, std::span<const sar::Field> fields,
                                    uint32_t file, std::span<const uint8_t> bytes,
                                    std::optional<std::span<const uint8_t>> wave_data)
{
    const sar::core::Container container = ContainerOf(build);
    const std::vector<uint8_t> joined = sar::core::JoinBy(container, sequences, pieces, fields);
    const Structure before = ReadStructure(joined);
    const std::vector<std::size_t> starts = sar::core::StartsBy(container, joined, pieces);
    std::map<uint32_t, std::vector<uint8_t>> news{{file, Padded(bytes)}};
    if (wave_data)
    {
        news[file | kWaveData] = Padded(*wave_data);
    }

    // Each copy in the groups is a run of its own. The run takes the new bytes. Several pieces can start where the copy
    // starts: padding of no bytes, then the run.
    std::vector<sar::Piece> replaced(pieces.begin(), pieces.end());
    std::vector<Location> copies;
    for (const Location& location : before.files)
    {
        const auto found = news.find(location.file);
        if (found == news.end())
        {
            continue;
        }

        auto piece = static_cast<std::size_t>(std::ranges::lower_bound(starts, location.start) - starts.begin());
        const auto holds_copy = [&](std::size_t i)
        {
            const auto* run = std::get_if<std::vector<uint8_t>>(&pieces[i]);
            return run != nullptr && run->size() == location.size;
        };
        while (piece < pieces.size() && starts[piece] == location.start && !holds_copy(piece))
        {
            ++piece;
        }
        if (piece == pieces.size() || starts[piece] != location.start)
        {
            throw std::invalid_argument("a copy of " + NameOf(location.file) + " isn't a piece of its own");
        }

        replaced[piece] = found->second;
        copies.push_back(location);
    }
    for (const auto& [id, new_bytes] : news)
    {
        if (std::ranges::none_of(copies, [id](const Location& copy) { return copy.file == id; }))
        {
            throw std::invalid_argument("no group of the archive holds " + NameOf(id));
        }
    }

    // Another location that names the same bytes would take them too.
    for (const Location& location : before.files)
    {
        const auto same = [&](const Location& copy)
        {
            return copy.start == location.start && copy.size == location.size;
        };
        if (!news.contains(location.file) && std::ranges::any_of(copies, same))
        {
            throw std::invalid_argument(NameOf(file) + " shares its bytes with " + NameOf(location.file));
        }
    }

    // The tables follow when each copy holds the new bytes, and every other location keeps its bytes.
    const std::vector<uint8_t> after = sar::core::JoinBy(container, sequences, replaced, fields);
    if (!Follows(joined, before, after, ReadStructure(after), news))
    {
        throw std::invalid_argument("the archive's tables don't follow the new bytes of " + NameOf(file));
    }

    return replaced;
}

std::vector<sar::FileCopy> FileCopies(const SndBuild& build, std::span<const commands::Sequence> sequences,
                                      std::span<const sar::Piece> pieces, std::span<const sar::Field> fields,
                                      uint32_t file, bool wave_data)
{
    const std::vector<uint8_t> joined = Join(build, sequences, pieces, fields);
    const uint32_t id = wave_data ? file | kWaveData : file;
    std::vector<sar::FileCopy> copies;
    for (const Location& location : ReadStructure(joined).files)
    {
        if (location.file == id)
        {
            const std::span<const uint8_t> bytes = BytesOf(joined, location);
            copies.push_back(sar::FileCopy{.start = location.start, .bytes = {bytes.begin(), bytes.end()}});
        }
    }
    std::ranges::sort(copies, {}, &sar::FileCopy::start);

    return copies;
}

std::vector<std::optional<std::size_t>> StartFields(std::span<const uint8_t> archive)
{
    const xsflib::Reader r(archive, true);
    if (!r.Magic(0, "RSAR"))
    {
        throw xsflib::FormatError("not a sound archive of the Wii (RSAR)");
    }

    std::vector<std::optional<std::size_t>> fields;
    for (const std::optional<Start>& start : SoundStarts(r))
    {
        fields.push_back(start ? start->at : std::nullopt);
    }

    return fields;
}

} // namespace ntdlib::rsar
