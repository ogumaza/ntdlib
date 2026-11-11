// SPDX-License-Identifier: MIT

#include "ntdlib/sar/layout.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "ntdlib/commands/sequence.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/reference.h"
#include "ntdlib/sar/sequence_file.h"
#include "ntdlib/sar/tables.h"
#include "sar/file_parts.h"
#include "sar/format_ids.h"
#include "sar/layout_core.h"

namespace ntdlib::sar
{
namespace
{

using core::BytesOf;
using core::Container;
using core::Distance;
using core::Location;
using core::Source;
using core::Start;
using core::Structure;
using detail::kDataReference;
using detail::kFileBlock;
using detail::kFileTable;
using detail::kGroupFileBlock;
using detail::kGroupInfoBlock;
using detail::kGroupTable;
using detail::kInfoBlock;
using detail::kInternalFile;
using detail::kNone;
using detail::kPlayerTable;
using detail::kSequenceDetails;
using detail::kSoundTable;

// The rules by which the console of `flavor` reads its sequences.
nwsnd::Grammar GrammarFor(Flavor flavor)
{
    switch (flavor)
    {
    case Flavor::kCtr:
        return nwsnd::GrammarOf<nwsnd::CtrPolicy>();
    case Flavor::kCafe:
        return nwsnd::GrammarOf<nwsnd::CafePolicy>();
    case Flavor::kNx:
        return nwsnd::GrammarOf<nwsnd::NxPolicy>();
    }

    throw std::invalid_argument("an unknown flavor of sound archive");
}

// The rules of the archives of `flavor`: CSEQ or FSEQ files, in the console's byte order.
Container ContainerOf(Flavor flavor)
{
    Container container;
    std::memcpy(container.sequence_magic.data(), flavor == Flavor::kCtr ? "CSEQ" : "FSEQ", 4);
    // The largest alignment that padding keeps in the archives and in their group files is that of their files. On the
    // 3DS and the Wii U, every file can start on a multiple of 64 by chance. Padding to 64 bytes would then keep a file
    // that ends 32 bytes past such a multiple in the bytes.
    container.most_alignment = detail::FileAlignment(flavor);
    container.big_endian = flavor == Flavor::kCafe;
    container.grammar = GrammarFor(flavor);

    container.read = [flavor](std::span<const uint8_t> bytes)
    {
        const SequenceFile file = SequenceFile::Read(bytes);
        if (file.flavor != flavor)
        {
            throw xsflib::FormatError("a sequence file of another console");
        }

        core::SequenceParts parts;
        parts.version = file.version;
        parts.data = file.data;
        for (const Sequence::Label& label : file.labels)
        {
            parts.labels.push_back({label.name, label.offset});
        }

        return parts;
    };

    // The bytecode follows the header of the DATA block. The first entry of the file's table of blocks gives it.
    container.write = [flavor](const core::SequenceParts& parts)
    {
        SequenceFile file;
        file.flavor = flavor;
        file.version = parts.version;
        file.data = parts.data;
        for (const nwsnd::LabelOffset& label : parts.labels)
        {
            file.labels.push_back({label.name, label.offset});
        }

        core::WrittenFile written;
        written.bytes = file.Write();
        written.bytecode = xsflib::Reader(written.bytes, flavor == Flavor::kCafe).U32(0x18) + 8;

        return written;
    };

    return container;
}

// Reads the header and the table of blocks of the file at `base`: a NintendoWare file of the archive's kind or a group
// in it. Adds the fields of the file's size, of each block's offset and size, and of the size in each block's header.
// Returns each block's start.
std::map<uint16_t, std::size_t> ReadBlocks(const xsflib::Reader& r, std::size_t base, std::vector<Distance>& distances)
{
    distances.push_back({base + 0x0C, base, base + r.U32(base + 0x0C), true});
    std::map<uint16_t, std::size_t> blocks;
    for (uint16_t i = 0; i < r.U16(base + 0x10); ++i)
    {
        const std::size_t entry = base + 0x14 + std::size_t{i} * 12;
        const uint32_t offset = r.U32(entry + 4);
        if (offset == kNone)
        {
            continue;
        }

        const std::size_t start = base + offset;
        distances.push_back({entry + 4, base, start, false});
        distances.push_back({entry + 8, start, start + r.U32(entry + 8), true});
        distances.push_back({start + 4, start, start + r.U32(start + 4), true});
        blocks[r.U16(entry)] = start;
    }

    return blocks;
}

// The block of type `type` among `blocks`.
std::size_t BlockOf(const std::map<uint16_t, std::size_t>& blocks, uint16_t type)
{
    const auto found = blocks.find(type);
    if (found == blocks.end())
    {
        throw xsflib::FormatError("a block is missing");
    }

    return found->second;
}

// The tables of the INFO block whose body starts at `info`, by type. The body starts with a reference to each.
std::map<uint16_t, std::size_t> TablesOf(const xsflib::Reader& r, std::size_t info)
{
    std::map<uint16_t, std::size_t> tables;
    for (std::size_t i = 0; i < 8; ++i)
    {
        const Reference table = ReadRef(r, info + i * 8);
        tables[table.type] = info + static_cast<uint32_t>(table.offset);
    }

    return tables;
}

// Where each entry of the table of type `type` starts. A table holds a count and a reference to each entry.
std::vector<std::size_t> EntriesOf(const xsflib::Reader& r, const std::map<uint16_t, std::size_t>& tables,
                                   uint16_t type)
{
    const std::size_t table = BlockOf(tables, type);
    std::vector<std::size_t> entries;
    for (uint32_t i = 0; i < r.U32(table); ++i)
    {
        entries.push_back(table + static_cast<uint32_t>(ReadRef(r, table + 4 + std::size_t{i} * 8).offset));
    }

    return entries;
}

// Where the value of option `bit` lies among the options at `at`: a word of flags, then a 32-bit value for each flag
// that is set, in the order of the bits. Nothing when the flag isn't set.
std::optional<std::size_t> OptionAt(const xsflib::Reader& r, std::size_t at, int bit)
{
    const uint32_t flags = r.U32(at);
    if ((flags >> bit & 1) == 0)
    {
        return std::nullopt;
    }

    return at + 4 + 4 * static_cast<std::size_t>(std::popcount(flags & ((1u << bit) - 1)));
}

// Reads the group at `group`, and adds the files it holds and their fields.
void ReadGroup(const xsflib::Reader& r, std::size_t group, const char* magic, Structure& structure)
{
    if (!r.Magic(group, magic))
    {
        throw xsflib::FormatError("a group that isn't a group file");
    }

    // INFO lists each file's ID, and where it lies in the group's FILE block.
    const std::map<uint16_t, std::size_t> blocks = ReadBlocks(r, group, structure.distances);
    const std::size_t items = BlockOf(blocks, kGroupInfoBlock) + 8;
    const std::size_t body = BlockOf(blocks, kGroupFileBlock) + 8;
    const uint32_t count = r.U32(items);
    for (uint32_t i = 0; i < count; ++i)
    {
        const std::size_t item = items + static_cast<uint32_t>(ReadRef(r, items + 4 + std::size_t{i} * 8).offset);
        const Reference where = ReadRef(r, item + 4);
        if (where.type != kDataReference || static_cast<uint32_t>(where.offset) == kNone)
        {
            continue;
        }

        const std::size_t start = body + static_cast<uint32_t>(where.offset);
        const uint32_t size = r.U32(item + 12);
        structure.files.push_back({r.U32(item), start, size});
        structure.distances.push_back({item + 8, body, start, false});
        structure.distances.push_back({item + 12, start, start + size, true});
    }
}

// Reads the files of `archive`, the files in its groups, the fields that hold their offsets and sizes, and the sequence
// sounds' starts. Throws xsflib::FormatError for tables that can't be read.
Structure ReadStructure(std::span<const uint8_t> archive, Flavor flavor)
{
    const bool big_endian = flavor == Flavor::kCafe;
    const xsflib::Reader r(archive, big_endian);
    if (r.U16(4) != 0xFEFF)
    {
        throw xsflib::FormatError("an archive in another byte order");
    }

    Structure structure;
    const std::map<uint16_t, std::size_t> blocks = ReadBlocks(r, 0, structure.distances);
    const std::size_t info = BlockOf(blocks, kInfoBlock) + 8;
    const std::size_t body = BlockOf(blocks, kFileBlock) + 8;

    // A file in the FILE block has an offset from the block's body and a size.
    const std::map<uint16_t, std::size_t> tables = TablesOf(r, info);
    const std::vector<std::size_t> files = EntriesOf(r, tables, kFileTable);
    std::map<uint32_t, Location> located;
    for (std::size_t id = 0; id < files.size(); ++id)
    {
        const Reference location = ReadRef(r, files[id]);
        if (location.type != kInternalFile)
        {
            continue;
        }

        const std::size_t where = files[id] + static_cast<uint32_t>(location.offset);
        const uint32_t offset = r.U32(where + 4);
        if (offset == kNone)
        {
            continue;
        }

        const std::size_t start = body + offset;
        const uint32_t size = r.U32(where + 8);
        located[static_cast<uint32_t>(id)] = {static_cast<uint32_t>(id), start, size};
        structure.files.push_back({static_cast<uint32_t>(id), start, size});
        structure.distances.push_back({where + 4, body, start, false});
        structure.distances.push_back({where + 8, start, start + size, true});
    }

    // A sequence sound's start is its first option.
    for (const std::size_t sound : EntriesOf(r, tables, kSoundTable))
    {
        const Reference detail = ReadRef(r, sound + 12);
        if (detail.type != kSequenceDetails)
        {
            continue;
        }

        const std::size_t options = sound + static_cast<uint32_t>(detail.offset) + 12;
        Start& start = structure.starts.emplace_back(Start{.file = r.U32(sound)});
        if ((r.U32(options) & 1) != 0)
        {
            start.offset = r.U32(options + 4);
            start.at = options + 4;
        }
    }

    // Each group whose file the archive holds whole. A group that an archive cut short lacks in part moves whole.
    const char* magic = flavor == Flavor::kCtr ? "CGRP" : "FGRP";
    std::set<std::size_t> groups;
    for (const std::size_t group : EntriesOf(r, tables, kGroupTable))
    {
        const auto found = located.find(r.U32(group));
        const Location* file = found == located.end() ? nullptr : &found->second;
        if (file && file->start + file->size <= archive.size() && groups.insert(file->start).second)
        {
            ReadGroup(r, file->start, magic, structure);
        }
    }

    return structure;
}

// Reads the files of `group`, a group file that a game keeps beside an archive of `flavor`, and the fields of the group
// file that hold their offsets and sizes. Nothing for a file that isn't a group file of the console, or whose tables
// can't be read.
std::optional<Structure> ReadGroupFileStructure(std::span<const uint8_t> group, Flavor flavor)
{
    try
    {
        const xsflib::Reader r(group, flavor == Flavor::kCafe);
        if (r.U16(4) != 0xFEFF)
        {
            return std::nullopt;
        }

        Structure structure;
        ReadGroup(r, 0, flavor == Flavor::kCtr ? "CGRP" : "FGRP", structure);

        return structure;
    }
    catch (const xsflib::FormatError&)
    {
        return std::nullopt;
    }
}

// Whether `joined`, whose tables `after` reads, holds `copy_bytes[k]` in copy k of file `file` that `copies` lists, and
// the bytes of `archive`, whose tables `before` reads, in every other file that holds no copy. `copies` lists the
// file's locations in the order of `before`.
bool Follows(std::span<const uint8_t> archive, const Structure& before, std::span<const uint8_t> joined,
             const Structure& after, uint32_t file, const std::vector<Location>& copies,
             std::span<const std::vector<uint8_t>> copy_bytes)
{
    if (after.files.size() != before.files.size())
    {
        return false;
    }

    std::size_t copy = 0;
    for (std::size_t i = 0; i < before.files.size(); ++i)
    {
        const Location& old = before.files[i];
        const auto holds = [&](const Location& location)
        {
            return old.start <= location.start && location.start + location.size <= old.start + old.size;
        };
        const std::span<const uint8_t> now = BytesOf(joined, after.files[i]);
        if (old.file == file ? !std::ranges::equal(now, copy_bytes[copy++])
                             : !std::ranges::any_of(copies, holds) && !std::ranges::equal(now, BytesOf(archive, old)))
        {
            return false;
        }
    }

    return true;
}

// What the cutter reads of `bytes`: an archive of `flavor`, or a group file beside one when `group` gives the group
// file's place among them. Nothing for a group file whose tables can't be read. Throws xsflib::FormatError for an
// archive whose tables can't be read.
std::optional<Structure> StructureOf(std::span<const uint8_t> bytes, Flavor flavor, std::optional<std::size_t> group)
{
    return group ? ReadGroupFileStructure(bytes, flavor) : ReadStructure(bytes, flavor);
}

// The copy of a file at `location` in `joined`: an archive, or a group file beside one when `group` gives its place
// among them. The copy gives the larger file that holds it whole, if one does: a group's file in an archive.
FileCopy FileCopyAt(std::span<const uint8_t> joined, const Structure& structure, const Location& location,
                    std::optional<std::size_t> group)
{
    FileCopy copy;
    copy.start = location.start;
    copy.group_file = group;
    const std::span<const uint8_t> bytes = BytesOf(joined, location);
    copy.bytes.assign(bytes.begin(), bytes.end());
    for (const Location& other : structure.files)
    {
        const bool holds = other.start <= location.start && location.start + location.size <= other.start + other.size;
        if (other.file != location.file && holds && other.size > location.size)
        {
            copy.group = other.file;
        }
    }

    return copy;
}

// The pieces in which each copy of file `file` holds the bytes that `bytes_of` gives it: the pieces of the archive, or
// of group file `group` beside it when `group` gives the group file's place. Nothing when they hold no copy, and for a
// group file whose tables can't be read. Throws the exceptions of ReplaceFile.
std::optional<std::vector<Piece>> ReplaceCopies(Flavor flavor, std::span<const commands::Sequence> sequences,
                                                std::span<const Piece> pieces, std::span<const Field> fields,
                                                std::optional<std::size_t> group, uint32_t file,
                                                const CopyBytes& bytes_of)
{
    const std::vector<uint8_t> joined = Join(flavor, sequences, pieces, fields);
    const std::optional<Structure> before = StructureOf(joined, flavor, group);
    if (!before)
    {
        return std::nullopt;
    }
    const std::vector<std::size_t> starts = Starts(joined, flavor, pieces);

    // Each copy of the file, in the FILE block and in the groups, is a run of its own. The run takes the copy's bytes.
    // Several pieces can start where the copy starts: padding of no bytes, then the run.
    const std::string name = "file " + std::to_string(file);
    const std::string where = group ? " in group file " + std::to_string(*group) : "";
    std::vector<Piece> replaced(pieces.begin(), pieces.end());
    std::vector<Location> copies;
    std::vector<std::vector<uint8_t>> copy_bytes;
    for (const Location& location : before->files)
    {
        if (location.file != file)
        {
            continue;
        }

        auto piece = static_cast<std::size_t>(std::ranges::lower_bound(starts, location.start) - starts.begin());
        const auto holds_copy = [&](std::size_t i)
        {
            const auto* run = std::get_if<std::vector<uint8_t>>(&pieces[i]);
            return run && run->size() == location.size;
        };
        while (piece < pieces.size() && starts[piece] == location.start && !holds_copy(piece))
        {
            ++piece;
        }
        if (piece == pieces.size() || starts[piece] != location.start)
        {
            throw std::invalid_argument("a copy of " + name + where + " isn't a piece of its own");
        }

        copy_bytes.push_back(bytes_of(FileCopyAt(joined, *before, location, group)));
        replaced[piece] = copy_bytes.back();
        copies.push_back(location);
    }
    if (copies.empty())
    {
        return std::nullopt;
    }

    // Another file ID that names the same bytes would take them too.
    for (const Location& location : before->files)
    {
        const auto same = [&](const Location& copy)
        {
            return copy.start == location.start && copy.size == location.size;
        };
        if (location.file != file && std::ranges::any_of(copies, same))
        {
            throw std::invalid_argument(name + where + " shares its bytes with file " + std::to_string(location.file));
        }
    }

    // The tables follow when each copy holds the bytes, and every other file keeps its bytes.
    const std::vector<uint8_t> after = Join(flavor, sequences, replaced, fields);
    const std::optional<Structure> read = StructureOf(after, flavor, group);
    if (!read || !Follows(joined, *before, after, *read, file, copies, copy_bytes))
    {
        const std::string tables =
            group ? "the tables of group file " + std::to_string(*group) : "the archive's tables";
        throw std::invalid_argument(tables + " don't follow the new bytes of " + name);
    }

    return replaced;
}

// The copies of file `file` in `joined`: an archive, or a group file beside one when `group` gives its place among
// them. Each copy gives the larger file that holds it whole, if one does: a group's file in an archive.
std::vector<FileCopy> CopiesIn(std::span<const uint8_t> joined, const Structure& structure, uint32_t file,
                               std::optional<std::size_t> group)
{
    std::vector<FileCopy> copies;
    for (const Location& location : structure.files)
    {
        if (location.file == file)
        {
            copies.push_back(FileCopyAt(joined, structure, location, group));
        }
    }
    std::ranges::sort(copies, {}, &FileCopy::start);

    return copies;
}

} // namespace

Layout Cut(std::span<const uint8_t> archive, const Options& options)
{
    return Cut(archive, {}, options);
}

Layout Cut(std::span<const uint8_t> archive, std::span<const std::vector<uint8_t>> group_files, const Options& options,
           const KnownSequences& known)
{
    // The reader checks the tables, and reports what it can't read.
    static_cast<void>(SoundArchive::Load(std::vector<uint8_t>(archive.begin(), archive.end()), options));

    try
    {
        // A group file that isn't one of the console's has no structure, and stays one run.
        std::vector<Source> sources;
        sources.push_back({archive, ReadStructure(archive, options.flavor)});
        for (const std::vector<uint8_t>& group_file : group_files)
        {
            sources.push_back({group_file, ReadGroupFileStructure(group_file, options.flavor).value_or(Structure{})});
        }

        core::Cutting cut = core::CutBy(sources, ContainerOf(options.flavor), known);
        Layout layout;
        layout.flavor = options.flavor;
        layout.sequences = std::move(cut.sequences);
        layout.pieces = std::move(cut.pieces);
        layout.fields = std::move(cut.fields);
        layout.group_files = std::move(cut.group_files);

        return layout;
    }
    catch (const xsflib::FormatError&)
    {
        // Tables that the cutter can't read, or a field that the pieces can't place. The archive and each group file
        // then become one run each.
    }

    Layout layout;
    layout.flavor = options.flavor;
    layout.pieces.emplace_back(std::vector<uint8_t>(archive.begin(), archive.end()));
    for (const std::vector<uint8_t>& group_file : group_files)
    {
        GroupFileLayout& cut = layout.group_files.emplace_back();
        if (!group_file.empty())
        {
            cut.pieces.emplace_back(group_file);
        }
    }

    return layout;
}

std::vector<uint8_t> Join(Flavor flavor, std::span<const commands::Sequence> sequences, std::span<const Piece> pieces,
                          std::span<const Field> fields)
{
    return core::JoinBy(ContainerOf(flavor), sequences, pieces, fields);
}

std::vector<std::size_t> Starts(std::span<const uint8_t> archive, Flavor flavor, std::span<const Piece> pieces)
{
    return core::StartsBy(ContainerOf(flavor), archive, pieces);
}

std::vector<Piece> ReplaceFile(Flavor flavor, std::span<const commands::Sequence> sequences,
                               std::span<const Piece> pieces, std::span<const Field> fields, uint32_t file,
                               std::span<const uint8_t> bytes)
{
    return ReplaceFile(flavor, sequences, pieces, fields, {}, file, bytes).pieces;
}

ReplacedPieces ReplaceFile(Flavor flavor, std::span<const commands::Sequence> sequences, std::span<const Piece> pieces,
                           std::span<const Field> fields, std::span<const GroupFileLayout> group_files, uint32_t file,
                           std::span<const uint8_t> bytes)
{
    const std::vector<uint8_t> every(bytes.begin(), bytes.end());

    return ReplaceFile(flavor, sequences, pieces, fields, group_files, file,
                       [&every](const FileCopy&) { return every; });
}

ReplacedPieces ReplaceFile(Flavor flavor, std::span<const commands::Sequence> sequences, std::span<const Piece> pieces,
                           std::span<const Field> fields, std::span<const GroupFileLayout> group_files, uint32_t file,
                           const CopyBytes& bytes_of)
{
    ReplacedPieces replaced;
    std::optional<std::vector<Piece>> archive =
        ReplaceCopies(flavor, sequences, pieces, fields, std::nullopt, file, bytes_of);
    bool found = archive.has_value();
    replaced.pieces = archive ? std::move(*archive) : std::vector<Piece>(pieces.begin(), pieces.end());
    for (std::size_t g = 0; g < group_files.size(); ++g)
    {
        const GroupFileLayout& group_file = group_files[g];
        std::optional<std::vector<Piece>> group =
            ReplaceCopies(flavor, sequences, group_file.pieces, group_file.fields, g, file, bytes_of);
        found = found || group.has_value();
        if (group)
        {
            replaced.group_files.push_back(std::move(*group));
        }
        else
        {
            replaced.group_files.push_back(group_file.pieces);
        }
    }
    if (!found)
    {
        throw std::invalid_argument("the archive holds no file " + std::to_string(file));
    }

    return replaced;
}

std::vector<FileCopy> FileCopies(Flavor flavor, std::span<const commands::Sequence> sequences,
                                 std::span<const Piece> pieces, std::span<const Field> fields, uint32_t file)
{
    const std::vector<uint8_t> archive = Join(flavor, sequences, pieces, fields);

    return CopiesIn(archive, ReadStructure(archive, flavor), file, std::nullopt);
}

std::vector<FileCopy> FileCopies(Flavor flavor, std::span<const commands::Sequence> sequences,
                                 std::span<const Piece> pieces, std::span<const Field> fields,
                                 std::span<const GroupFileLayout> group_files, uint32_t file)
{
    std::vector<FileCopy> copies = FileCopies(flavor, sequences, pieces, fields, file);
    for (std::size_t g = 0; g < group_files.size(); ++g)
    {
        const std::vector<uint8_t> joined = Join(flavor, sequences, group_files[g].pieces, group_files[g].fields);
        if (const std::optional<Structure> structure = ReadGroupFileStructure(joined, flavor))
        {
            std::ranges::move(CopiesIn(joined, *structure, file, g), std::back_inserter(copies));
        }
    }

    return copies;
}

void WriteTableValues(std::span<uint8_t> archive, Flavor flavor, std::span<const SoundInfo> sounds,
                      std::span<const PlayerInfo> players)
{
    const bool big_endian = flavor == Flavor::kCafe;
    const xsflib::Reader r(archive, big_endian);
    std::vector<Distance> distances;
    const std::map<uint16_t, std::size_t> tables = TablesOf(r, BlockOf(ReadBlocks(r, 0, distances), kInfoBlock) + 8);

    // The bits of `value` under `mask` replace those of the 32-bit value at `at`. The read checks that it lies within
    // the archive.
    const auto put = [&](std::size_t at, uint32_t value, uint32_t mask)
    {
        const uint32_t merged = (r.U32(at) & ~mask) | (value & mask);
        big_endian ? xsflib::StoreBe32(&archive[at], merged) : xsflib::StoreLe32(&archive[at], merged);
    };

    // Each sound's values. Two bytes of an option hold two of them.
    const std::vector<std::size_t> entries = EntriesOf(r, tables, kSoundTable);
    for (std::size_t i = 0; i < entries.size() && i < sounds.size(); ++i)
    {
        const SoundInfo& sound = sounds[i];
        const std::size_t p = entries[i];
        put(p + 4, sound.player_id, 0xFFFFFFFF);
        put(p + 8, big_endian ? uint32_t{sound.volume} << 24 : sound.volume, big_endian ? 0xFF000000 : 0xFF);
        if (const std::optional<std::size_t> pan = OptionAt(r, p + 20, 1))
        {
            put(*pan, sound.pan_mode | uint32_t{sound.pan_curve} << 8, 0xFFFF);
        }
        if (const std::optional<std::size_t> priority = OptionAt(r, p + 20, 2))
        {
            put(*priority, sound.player_priority | uint32_t{sound.actor_player_id} << 8, 0xFFFF);
        }

        const Reference detail = ReadRef(r, p + 12);
        if (detail.type != kSequenceDetails)
        {
            continue;
        }

        // A sequence sound's banks, as many as its list holds, its tracks and its channels' priority.
        const std::size_t q = p + static_cast<uint32_t>(detail.offset);
        const std::size_t list = q + static_cast<uint32_t>(ReadRef(r, q).offset);
        const uint32_t slots = std::min<uint32_t>(r.U32(list), kBankSlots);
        for (uint32_t k = 0; k < slots && k < sound.banks.size(); ++k)
        {
            put(list + 4 + std::size_t{k} * 4, sound.banks[k], 0xFFFFFFFF);
        }
        put(q + 8, sound.allocate_track_flags, 0xFFFFFFFF);
        // A release priority fix that stays on keeps its byte.
        if (const std::optional<std::size_t> channel = OptionAt(r, q + 12, 1))
        {
            const uint32_t old = r.U32(*channel) & 0xFF00;
            uint32_t fix = sound.release_priority_fix ? 0x100 : 0;
            if ((old != 0) == sound.release_priority_fix)
            {
                fix = old;
            }

            put(*channel, sound.channel_priority | fix, 0xFFFF);
        }
    }

    // Each player's sound limit.
    const std::vector<std::size_t> rows = EntriesOf(r, tables, kPlayerTable);
    for (std::size_t i = 0; i < rows.size() && i < players.size(); ++i)
    {
        put(rows[i], players[i].playable_sound_max, 0xFFFFFFFF);
    }
}

std::vector<std::optional<std::size_t>> StartFields(std::span<const uint8_t> archive, Flavor flavor)
{
    const xsflib::Reader r(archive, flavor == Flavor::kCafe);
    if (r.U16(4) != 0xFEFF)
    {
        throw xsflib::FormatError("an archive in another byte order");
    }

    std::vector<Distance> distances;
    const std::map<uint16_t, std::size_t> tables = TablesOf(r, BlockOf(ReadBlocks(r, 0, distances), kInfoBlock) + 8);

    // A sequence sound's start is its first option, as ReadStructure reads it.
    std::vector<std::optional<std::size_t>> fields;
    for (const std::size_t sound : EntriesOf(r, tables, kSoundTable))
    {
        const Reference detail = ReadRef(r, sound + 12);
        std::optional<std::size_t>& field = fields.emplace_back();
        if (detail.type == kSequenceDetails)
        {
            field = OptionAt(r, sound + static_cast<uint32_t>(detail.offset) + 12, 0);
        }
    }

    return fields;
}

std::vector<ValuePlaces> TableValuePlaces(std::span<const uint8_t> archive, Flavor flavor)
{
    // The header's table of blocks gives the INFO block. The reads stop at the block's end.
    const std::span<const uint8_t> block = TablesOf(archive, flavor).info;
    const auto info = static_cast<std::size_t>(block.data() - archive.data());
    const xsflib::Reader r(archive.first(info + block.size()), flavor == Flavor::kCafe);
    const std::map<uint16_t, std::size_t> tables = TablesOf(r, info + 8);

    // The options where WriteTableValues writes the values.
    std::vector<ValuePlaces> places;
    for (const std::size_t sound : EntriesOf(r, tables, kSoundTable))
    {
        ValuePlaces& place = places.emplace_back();
        place.pan = OptionAt(r, sound + 20, 1).has_value();
        place.priority = OptionAt(r, sound + 20, 2).has_value();
        const Reference detail = ReadRef(r, sound + 12);
        if (detail.type == kSequenceDetails)
        {
            place.channel = OptionAt(r, sound + static_cast<uint32_t>(detail.offset) + 12, 1).has_value();
        }
    }

    return places;
}

} // namespace ntdlib::sar
