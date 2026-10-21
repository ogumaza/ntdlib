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

namespace ntdlib::sar
{
namespace
{

constexpr uint16_t kInfoBlock = 0x2001;
constexpr uint16_t kFileBlock = 0x2002;
constexpr uint16_t kSoundTable = 0x2100;
constexpr uint16_t kPlayerTable = 0x2102;
constexpr uint16_t kGroupTable = 0x2105;
constexpr uint16_t kFileTable = 0x2106;
constexpr uint16_t kSequenceSound = 0x2203;
constexpr uint16_t kInternalFile = 0x220C;
constexpr uint16_t kGroupInfoBlock = 0x7800;
constexpr uint16_t kGroupFileBlock = 0x7801;
constexpr uint16_t kDataReference = 0x1F00;
constexpr uint32_t kNone = 0xFFFFFFFF;

// The largest alignment that padding keeps. The Switch's archives align their files to 64 bytes, and those of the 3DS
// and the Wii U to 32.
constexpr std::size_t kMostAlignment = 64;

// A file of the archive, in its FILE block or in a group there: its file ID, where it starts and its size.
struct Location
{
    uint32_t file = 0;
    std::size_t start = 0;
    std::size_t size = 0;
};

// A field of the archive's tables, with positions from the archive's start: the field, and the distance it holds. A
// size's distance ends where a range that starts at `from` ends. Any other distance ends where something starts.
struct Distance
{
    std::size_t at = 0;
    std::size_t from = 0;
    std::size_t to = 0;
    bool size = false;
};

// A sequence sound's start in its file's bytecode, and where the archive's tables hold it. A sound without the option
// starts at 0.
struct Start
{
    uint32_t file = 0;
    uint32_t offset = 0;
    std::optional<std::size_t> at = std::nullopt;
};

// What the cutter reads of an archive's tables.
struct Structure
{
    std::vector<Location> files;
    std::vector<Distance> distances;
    std::vector<Start> starts;
};

// A sequence file's commands and version, and the item at each of its sounds' starts.
struct Decoded
{
    commands::Sequence sequence;
    uint32_t version = 0;
    std::map<uint32_t, uint32_t> entries; // from a start's offset in the bytecode to its item
};

// A sequence file as the writer lays it out.
struct Written
{
    std::vector<uint8_t> bytes;
    std::size_t bytecode = 0;      // where the bytecode starts in the file
    std::vector<uint32_t> offsets; // each item's offset in the bytecode, then the end
};

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

// The file of sequence `index`.
Written WriteFile(const commands::Sequence& sequence, uint32_t version, Flavor flavor, const nwsnd::Grammar& grammar,
                  std::size_t index)
{
    nwsnd::Encoded encoded = nwsnd::EncodeSequence(sequence, grammar);
    SequenceFile file;
    file.flavor = flavor;
    file.version = version;
    file.data = encoded.bytes;
    for (const commands::Label& label : sequence.labels)
    {
        if (label.item >= encoded.offsets.size())
        {
            throw std::invalid_argument("sequence " + std::to_string(index) + ": label " + label.name +
                                        " is past the end");
        }

        file.labels.push_back({label.name, encoded.offsets[label.item]});
    }

    // The bytecode follows the header of the DATA block. The first entry of the file's table of blocks gives it.
    Written written;
    written.bytes = file.Write();
    written.bytecode = xsflib::Reader(written.bytes, flavor == Flavor::kCafe).U32(0x18) + 8;
    written.offsets = std::move(encoded.offsets);

    return written;
}

// Whether `decoded` gives `bytes` back, with each start at its offset.
bool GivesBack(const Decoded& decoded, Flavor flavor, const nwsnd::Grammar& grammar, std::span<const uint8_t> bytes)
{
    try
    {
        const Written written = WriteFile(decoded.sequence, decoded.version, flavor, grammar, 0);
        for (const auto& [offset, item] : decoded.entries)
        {
            if (item >= written.offsets.size() || written.offsets[item] != offset)
            {
                return false;
            }
        }

        return std::equal(written.bytes.begin(), written.bytes.end(), bytes.begin(), bytes.end());
    }
    catch (const std::invalid_argument&)
    {
        return false;
    }
}

// Whether the last item of `decoded` holds only the zeros that pad the bytecode, with no label or start on it.
bool EndsInPadding(const Decoded& decoded)
{
    const std::vector<commands::Command>& items = decoded.sequence.items;
    if (items.empty() || items.back().kind != commands::Kind::kData)
    {
        return false;
    }

    const auto last = static_cast<uint32_t>(items.size() - 1);
    for (const uint8_t byte : items.back().bytes)
    {
        if (byte != 0)
        {
            return false;
        }
    }
    for (const commands::Label& label : decoded.sequence.labels)
    {
        if (label.item >= last)
        {
            return false;
        }
    }
    for (const auto& [offset, item] : decoded.entries)
    {
        if (item >= last)
        {
            return false;
        }
    }

    return true;
}

// Whether `decoded` holds bytes that no command reaches, other than zeros at its end.
bool HoldsData(const Decoded& decoded)
{
    const std::vector<commands::Command>& items = decoded.sequence.items;
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        const std::vector<uint8_t>& run = items[i].bytes;
        const bool zeros_at_end =
            i + 1 == items.size() && std::all_of(run.begin(), run.end(), [](uint8_t byte) { return byte == 0; });
        if (items[i].kind == commands::Kind::kData && !zeros_at_end)
        {
            return true;
        }
    }

    return false;
}

// The commands of the sequence file `bytes`, when they give its bytes back. Any file that the codec can't take back
// stays in the bytes: a file laid out otherwise than the writer lays it out, or one that the decoder can't read. So
// does a file with bytes that no command reaches, but for zeros at its end. The player can still run into such bytes,
// such as after a return with no call on the stack. Join can't move the offsets that they hold.
std::optional<Decoded> Decode(std::span<const uint8_t> bytes, const std::set<uint32_t>& starts, Flavor flavor,
                              const nwsnd::Grammar& grammar)
{
    try
    {
        const SequenceFile file = SequenceFile::Read(bytes);
        if (file.flavor != flavor)
        {
            return std::nullopt;
        }

        std::vector<nwsnd::LabelOffset> labels;
        for (const Sequence::Label& label : file.labels)
        {
            labels.push_back({label.name, label.offset});
        }

        const std::vector<uint32_t> entries(starts.begin(), starts.end());
        nwsnd::Decoded read = nwsnd::DecodeSequence(file.data, labels, grammar, entries);
        Decoded decoded;
        decoded.sequence = std::move(read.sequence);
        decoded.version = file.version;
        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            decoded.entries[entries[i]] = read.entries[i];
        }

        // Commands that overlap, and bytes that no command reaches, keep the file in the bytes.
        if (read.opaque || HoldsData(decoded))
        {
            return std::nullopt;
        }

        // Without the padding, the commands give the file back when the writer pads the bytecode the same way.
        if (EndsInPadding(decoded))
        {
            Decoded trimmed = decoded;
            trimmed.sequence.items.pop_back();
            if (GivesBack(trimmed, flavor, grammar, bytes))
            {
                return trimmed;
            }
        }
        if (GivesBack(decoded, flavor, grammar, bytes))
        {
            return decoded;
        }
    }
    catch (const xsflib::FormatError&)
    {
    }
    catch (const std::invalid_argument&)
    {
    }
    catch (const std::out_of_range&)
    {
    }

    return std::nullopt;
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
        if (detail.type != kSequenceSound)
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

// A file that a cut takes: the archive, or a group file beside it. Its bytes, and what the cutter reads of its tables.
struct Source
{
    std::span<const uint8_t> bytes;
    Structure structure;
};

// Where a copy of a file lies: in which source of a cut, the archive first, and where it starts there.
struct CopyAt
{
    friend auto operator<=>(const CopyAt&, const CopyAt&) = default;

    std::size_t source = 0;
    std::size_t start = 0;
};

// The copies of a sequence file: where each lies, its size, and its sounds' starts.
struct Family
{
    std::vector<CopyAt> copies; // in order, the archive's first
    std::size_t size = 0;
    std::set<uint32_t> starts;
    std::optional<Decoded> decoded;
    bool kept = false;
    bool placed = false;   // whether the pieces hold the first copy
    uint32_t sequence = 0; // in the layout
    uint32_t piece = 0;    // the first copy's piece, in the archive
};

// The sequence files of `sources`. Copies that share a file ID belong to one family, in the archive and in the group
// files. Returns each file ID's family.
std::map<uint32_t, std::size_t> FamiliesOf(std::span<const Source> sources, Flavor flavor,
                                           std::vector<Family>& families)
{
    // The sequence files, by where they lie, and the file IDs that name each.
    const char* magic = flavor == Flavor::kCtr ? "CSEQ" : "FSEQ";
    std::map<std::pair<CopyAt, std::size_t>, std::set<uint32_t>> copies;
    for (std::size_t s = 0; s < sources.size(); ++s)
    {
        const std::span<const uint8_t> bytes = sources[s].bytes;
        for (const Location& file : sources[s].structure.files)
        {
            if (file.size >= 4 && file.start <= bytes.size() && file.size <= bytes.size() - file.start &&
                std::memcmp(bytes.data() + file.start, magic, 4) == 0)
            {
                copies[{CopyAt{s, file.start}, file.size}].insert(file.file);
            }
        }
    }

    // File IDs that name the same copy name the same sequence.
    std::map<uint32_t, uint32_t> parent;
    auto root = [&](uint32_t file)
    {
        while (parent.at(file) != file)
        {
            file = parent.at(file);
        }

        return file;
    };

    for (const auto& [where, files] : copies)
    {
        for (const uint32_t file : files)
        {
            parent.emplace(file, file);
        }
        for (const uint32_t file : files)
        {
            parent[root(file)] = root(*files.begin());
        }
    }

    // A family for each root, in the order of its first copy.
    std::map<uint32_t, std::size_t> family_of_root;
    for (const auto& [where, files] : copies)
    {
        const auto [found, added] = family_of_root.emplace(root(*files.begin()), families.size());
        if (added)
        {
            families.emplace_back();
            families.back().size = where.second;
        }

        Family& family = families[found->second];
        family.copies.push_back(where.first);
        family.size = family.size == where.second ? family.size : 0;
    }

    std::map<uint32_t, std::size_t> family_of;
    for (const auto& [file, up] : parent)
    {
        family_of[file] = family_of_root.at(root(file));
    }

    return family_of;
}

// The alignment that every file of the archive keeps, up to kMostAlignment.
std::size_t AlignmentOf(const Structure& structure)
{
    std::size_t alignment = kMostAlignment;
    for (const Location& file : structure.files)
    {
        while (file.start % alignment != 0)
        {
            alignment /= 2;
        }
    }

    return alignment;
}

// A position rounded up to a multiple of `alignment`.
std::size_t AlignUp(std::size_t position, std::size_t alignment)
{
    return (position + alignment - 1) / alignment * alignment;
}

// Where the padding after a copy that ends at `end` ends, in an archive of `size` bytes. A copy that ends the archive
// has no padding after it.
std::size_t PaddedEnd(std::size_t end, std::size_t alignment, std::size_t size)
{
    return end == size ? end : AlignUp(end, alignment);
}

// A copy of a sequence file among the pieces, or another file that becomes a run of its own: where it starts, ends and
// its padding ends, and its pieces.
struct Copy
{
    std::size_t family = 0; // a sequence file's family
    std::size_t start = 0;
    std::size_t end = 0;
    std::size_t padded = 0;
    uint32_t piece = 0;
    std::optional<uint32_t> padding = std::nullopt;
    uint32_t next = 0; // the piece after the copy and its padding
    bool run = false;  // a file that becomes a run, with no family
};

// A run of bytes among the pieces.
struct Run
{
    std::size_t start = 0;
    std::size_t end = 0;
    uint32_t piece = 0;
};

// The copies of the families kept that lie in source `source`, in order, with each copy's padding in a source of `size`
// bytes.
std::vector<Copy> CopiesOf(const std::vector<Family>& families, std::size_t source, std::size_t alignment,
                           std::size_t size)
{
    std::vector<Copy> copies;
    for (std::size_t i = 0; i < families.size(); ++i)
    {
        for (const CopyAt& copy : families[i].copies)
        {
            if (families[i].kept && copy.source == source)
            {
                const std::size_t end = copy.start + families[i].size;
                copies.push_back(
                    {.family = i, .start = copy.start, .end = end, .padded = PaddedEnd(end, alignment, size)});
            }
        }
    }
    std::ranges::sort(copies, {}, &Copy::start);

    return copies;
}

// Whether position `x`, of the kind given, falls inside `copy` or its padding, where no place can hold it. Only the end
// of a size may lie where the padding starts.
bool FallsInside(const Copy& copy, std::size_t x, bool size)
{
    if (x <= copy.start || x >= copy.padded)
    {
        return false;
    }

    return x != copy.end || !size;
}

// Whether `archive` holds the range from `end` to `padded`, and zeros fill it.
bool PaddedWithZeros(std::span<const uint8_t> archive, std::size_t end, std::size_t padded)
{
    if (padded > archive.size())
    {
        return false;
    }

    return std::all_of(archive.begin() + static_cast<std::ptrdiff_t>(end),
                       archive.begin() + static_cast<std::ptrdiff_t>(padded), [](uint8_t byte) { return byte == 0; });
}

// Calls `leave_out` with the index of each of `copies`, in order of their starts, that a field of `structure` lies in
// or points inside. A copy that starts further before a position than the longest copy with its padding can't reach
// it.
template <class LeaveOutCopy>
void ForEachInside(const Structure& structure, const std::vector<Copy>& copies, const LeaveOutCopy& leave_out)
{
    std::size_t longest = 0;
    for (const Copy& copy : copies)
    {
        longest = std::max(longest, copy.padded - copy.start);
    }

    // The `length` bytes at `x`, or a position of the kind given when `length` is 0.
    const auto check = [&](std::size_t x, std::size_t length, bool size)
    {
        auto copy = std::ranges::upper_bound(copies, x + length, {}, &Copy::start);
        while (copy != copies.begin() && std::prev(copy)->start + longest >= x)
        {
            --copy;
            const bool overlaps = length > 0 && x < copy->padded && x + length > copy->start;
            if (overlaps || (length == 0 && FallsInside(*copy, x, size)))
            {
                leave_out(static_cast<std::size_t>(copy - copies.begin()));
            }
        }
    };

    for (const Distance& distance : structure.distances)
    {
        check(distance.at, 4, false);
        check(distance.from, 0, false);
        check(distance.to, 0, distance.size);
    }
}

// Leaves out the families that the layout of a source keeps in the bytes: a copy whose padding isn't zeros, a field
// that points inside a copy or lies in one, and copies that overlap. Each step covers every source before the next.
// `alignments` gives each source's alignment.
void LeaveOut(std::span<const Source> sources, std::span<const std::size_t> alignments, std::vector<Family>& families)
{
    for (std::size_t s = 0; s < sources.size(); ++s)
    {
        const std::span<const uint8_t> bytes = sources[s].bytes;
        for (const Copy& copy : CopiesOf(families, s, alignments[s], bytes.size()))
        {
            families[copy.family].kept = families[copy.family].kept && PaddedWithZeros(bytes, copy.end, copy.padded);
        }
    }

    for (std::size_t s = 0; s < sources.size(); ++s)
    {
        const std::vector<Copy> copies = CopiesOf(families, s, alignments[s], sources[s].bytes.size());
        ForEachInside(sources[s].structure, copies, [&](std::size_t i) { families[copies[i].family].kept = false; });
    }

    // Of two copies that overlap, the later one stays in the bytes.
    for (std::size_t s = 0; s < sources.size(); ++s)
    {
        std::size_t end = 0;
        for (const Copy& copy : CopiesOf(families, s, alignments[s], sources[s].bytes.size()))
        {
            if (copy.start < end)
            {
                families[copy.family].kept = false;
                continue;
            }

            end = copy.end;
        }
    }
}

// Adds to `copies`, the copies of the sequence files kept, each other file that becomes a run of its own, and sorts
// them by their starts. A file does when the archive holds it whole, when zeros pad it to `alignment` or it ends the
// archive, and when no field lies in it or points inside it. A group's fields lie in it. Each file's offset and size
// are fields too. A field therefore points inside a file that holds another, or overlaps one. Several file IDs can name
// one file.
void AddRuns(std::span<const uint8_t> archive, const Structure& structure, std::size_t alignment,
             std::vector<Copy>& copies)
{
    // Each file once, by where it lies, but for the sequence pieces.
    std::set<std::pair<std::size_t, std::size_t>> files;
    for (const Location& file : structure.files)
    {
        if (file.size > 0)
        {
            files.insert({file.start, file.size});
        }
    }
    for (const Copy& copy : copies)
    {
        files.erase({copy.start, copy.end - copy.start});
    }

    std::vector<Copy> runs;
    for (const auto& [start, size] : files)
    {
        const std::size_t end = start + size;
        const std::size_t padded = PaddedEnd(end, alignment, archive.size());
        if (PaddedWithZeros(archive, end, padded))
        {
            runs.push_back({.start = start, .end = end, .padded = padded, .run = true});
        }
    }

    std::vector<bool> inside(runs.size(), false);
    ForEachInside(structure, runs, [&](std::size_t i) { inside[i] = true; });
    for (std::size_t i = 0; i < runs.size(); ++i)
    {
        if (!inside[i])
        {
            copies.push_back(runs[i]);
        }
    }
    std::ranges::sort(copies, {}, &Copy::start);
}

// The pieces of one source of a cut, and the places of the source's positions in them.
class Pieces
{
public:
    // Cuts `bytes`, a source of the cut, into `pieces`. The first copy of each family adds the family's sequence to
    // `sequences`. The archive's pieces come first, and hold each family's first copy.
    Pieces(std::span<const uint8_t> bytes, std::vector<Family>& families, std::vector<Copy> copies,
           std::size_t alignment, std::vector<commands::Sequence>& sequences, std::vector<Piece>& pieces)
        : copies_(std::move(copies)), size_(bytes.size())
    {
        std::size_t cursor = 0;
        for (Copy& copy : copies_)
        {
            if (copy.start > cursor)
            {
                AddRun(bytes, cursor, copy.start, pieces);
            }

            copy.piece = static_cast<uint32_t>(pieces.size());
            if (copy.run)
            {
                files_.insert(copy.piece);
                pieces.emplace_back(std::vector<uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(copy.start),
                                                         bytes.begin() + static_cast<std::ptrdiff_t>(copy.end)));
            }
            else
            {
                Family& family = families[copy.family];
                if (!family.placed)
                {
                    family.placed = true;
                    family.sequence = static_cast<uint32_t>(sequences.size());
                    family.piece = copy.piece;
                    sequences.push_back(std::move(family.decoded->sequence));
                }
                pieces.emplace_back(SequencePiece{family.sequence, family.decoded->version});
            }
            // The padding after a copy that ends the source adds no bytes.
            if (alignment > 1)
            {
                copy.padding = static_cast<uint32_t>(pieces.size());
                pieces.emplace_back(Padding{copy.end == size_ ? uint32_t{1} : static_cast<uint32_t>(alignment)});
            }
            copy.next = static_cast<uint32_t>(pieces.size());
            cursor = copy.padded;
        }
        if (cursor < bytes.size())
        {
            AddRun(bytes, cursor, bytes.size(), pieces);
        }

        end_ = static_cast<uint32_t>(pieces.size());
    }

    // The place of position `x`. A size ends at `x`, and starts at `from`: a range that starts at a copy ends before
    // its padding. Nothing for a position inside a copy or its padding.
    std::optional<Place> PlaceOf(std::size_t x, bool size, std::size_t from) const
    {
        auto copy = std::ranges::upper_bound(copies_, x, {}, &Copy::start);
        if (copy != copies_.begin())
        {
            const Copy& before = *std::prev(copy);
            if (x == before.start)
            {
                // The copy before it may end there, with padding of no bytes.
                if (std::prev(copy) != copies_.begin())
                {
                    const Copy& last = *std::prev(copy, 2);
                    if (last.padded == x && last.end == x && size && from == last.start && last.padding)
                    {
                        return Place{*last.padding, 0, false};
                    }
                }

                return Place{before.piece, 0, false};
            }
            if (x <= before.padded)
            {
                return PlaceAtEnd(before, x, size, from);
            }
        }

        auto run = std::ranges::upper_bound(runs_, x, {}, &Run::start);
        if (run != runs_.begin() && x < std::prev(run)->end)
        {
            return Place{std::prev(run)->piece, static_cast<uint32_t>(x - std::prev(run)->start), false};
        }
        if (x >= size_ && x - size_ <= 0xFFFFFFFF)
        {
            return Place{end_, static_cast<uint32_t>(x - size_), false};
        }

        return std::nullopt;
    }

    // The place of the field at `at`, inside a run.
    std::optional<Place> FieldAt(std::size_t at) const
    {
        auto run = std::ranges::upper_bound(runs_, at, {}, &Run::start);
        if (run == runs_.begin() || at + 4 > std::prev(run)->end)
        {
            return std::nullopt;
        }

        return Place{std::prev(run)->piece, static_cast<uint32_t>(at - std::prev(run)->start), false};
    }

    // Whether piece `piece` is a run that holds one file.
    bool IsFile(std::size_t piece) const
    {
        return files_.contains(static_cast<uint32_t>(piece));
    }

private:
    void AddRun(std::span<const uint8_t> bytes, std::size_t start, std::size_t end, std::vector<Piece>& pieces)
    {
        runs_.push_back({start, end, static_cast<uint32_t>(pieces.size())});
        pieces.emplace_back(std::vector<uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(start),
                                                 bytes.begin() + static_cast<std::ptrdiff_t>(end)));
    }

    // The place of position `x`, past the start of `copy` and no further than the end of its padding.
    static std::optional<Place> PlaceAtEnd(const Copy& copy, std::size_t x, bool size, std::size_t from)
    {
        if (x == copy.end && copy.padding && size && (copy.padded > copy.end || from == copy.start))
        {
            return Place{*copy.padding, 0, false};
        }
        if (x == copy.padded)
        {
            return Place{copy.next, 0, false};
        }

        return std::nullopt;
    }

    std::vector<Copy> copies_;
    std::vector<Run> runs_;
    std::set<uint32_t> files_; // the runs that hold one file
    std::size_t size_ = 0;
    uint32_t end_ = 0;
};

// The fields of `structure` that an edit can change, placed among `pieces`. `placed` cut the pieces. Throws
// xsflib::FormatError for a field that the pieces can't place.
std::vector<Field> FieldsOf(const Structure& structure, const Pieces& placed, std::span<const Piece> pieces)
{
    // A field whose places lie in one run, or in runs with no sequence, file or padding between them, can't change.
    std::vector<uint32_t> changing(pieces.size() + 1, 0);
    for (std::size_t i = 0; i < pieces.size(); ++i)
    {
        const bool fixed = std::holds_alternative<std::vector<uint8_t>>(pieces[i]) && !placed.IsFile(i);
        changing[i + 1] = changing[i] + (fixed ? 0 : 1);
    }

    std::vector<Field> fields;
    for (const Distance& distance : structure.distances)
    {
        const std::optional<Place> at = placed.FieldAt(distance.at);
        const std::optional<Place> from = placed.PlaceOf(distance.from, false, distance.from);
        const std::optional<Place> to = placed.PlaceOf(distance.to, distance.size, distance.from);
        if (!at || !from || !to)
        {
            throw xsflib::FormatError("a field that the pieces can't place");
        }

        const auto [low, high] = std::minmax(from->piece, to->piece);
        if (changing[high] != changing[low])
        {
            fields.push_back({*at, *from, *to});
        }
    }

    return fields;
}

// Cuts `sources`, the archive and the group files beside it, by their structures: the families of sequence files that
// decode, the other files that become runs, each with its padding, and the fields. Throws xsflib::FormatError for a
// field that the pieces can't place.
Layout CutBy(std::span<const Source> sources, Flavor flavor)
{
    // Each family's starts, and its commands when every copy holds the same bytes and they decode.
    std::vector<Family> families;
    const std::map<uint32_t, std::size_t> family_of = FamiliesOf(sources, flavor, families);
    const Structure& archive = sources.front().structure;
    for (const Start& start : archive.starts)
    {
        const auto found = family_of.find(start.file);
        if (found != family_of.end())
        {
            families[found->second].starts.insert(start.offset);
        }
    }

    const nwsnd::Grammar grammar = GrammarFor(flavor);
    for (Family& family : families)
    {
        // A sequence file that only group files hold stays in their bytes. The archive's tables hold its sounds'
        // starts, and no field reaches from the archive into a group file.
        if (family.size == 0 || family.copies.front().source != 0)
        {
            continue;
        }

        const CopyAt& front = family.copies.front();
        const std::span<const uint8_t> first = sources[front.source].bytes.subspan(front.start, family.size);
        auto same = [&](const CopyAt& copy)
        {
            return std::ranges::equal(sources[copy.source].bytes.subspan(copy.start, family.size), first);
        };
        if (std::ranges::all_of(family.copies, same))
        {
            family.decoded = Decode(first, family.starts, flavor, grammar);
            family.kept = family.decoded.has_value();
        }
    }

    std::vector<std::size_t> alignments;
    for (const Source& source : sources)
    {
        alignments.push_back(AlignmentOf(source.structure));
    }
    LeaveOut(sources, alignments, families);

    // The archive comes first, and its copies give the families their sequences.
    Layout layout;
    layout.flavor = flavor;
    layout.group_files.resize(sources.size() - 1);
    for (std::size_t s = 0; s < sources.size(); ++s)
    {
        const Source& source = sources[s];
        std::vector<Piece>& pieces = s == 0 ? layout.pieces : layout.group_files[s - 1].pieces;
        std::vector<Copy> copies = CopiesOf(families, s, alignments[s], source.bytes.size());
        AddRuns(source.bytes, source.structure, alignments[s], copies);
        const Pieces placed(source.bytes, families, std::move(copies), alignments[s], layout.sequences, pieces);
        std::vector<Field> fields = FieldsOf(source.structure, placed, pieces);
        if (s > 0)
        {
            layout.group_files[s - 1].fields = std::move(fields);
            continue;
        }

        layout.fields = std::move(fields);

        // A sound's start counts from the bytecode's first item.
        for (const Start& start : archive.starts)
        {
            const auto found = family_of.find(start.file);
            if (!start.at || found == family_of.end() || !families[found->second].kept)
            {
                continue;
            }

            const Family& family = families[found->second];
            const std::optional<Place> at = placed.FieldAt(*start.at);
            if (!at)
            {
                throw xsflib::FormatError("a sound's start that the pieces can't place");
            }

            layout.fields.push_back({*at, Place{family.piece, 0, true},
                                     Place{family.piece, family.decoded->entries.at(start.offset), true}});
        }
    }

    return layout;
}

// A piece that Join has laid out: where it starts, its size, and where its items lie.
struct Laid
{
    std::size_t start = 0;
    std::size_t size = 0;
    std::vector<std::size_t> items = {}; // a sequence piece's items, and the end of its bytecode
    bool run = false;
};

// The position of `place` among the pieces laid out in `laid`, of which the last is the archive's end.
std::size_t PositionOf(const Place& place, const std::vector<Laid>& laid)
{
    const std::string name = "a place in piece " + std::to_string(place.piece);
    if (place.piece >= laid.size())
    {
        throw std::invalid_argument(name + ", and there are " + std::to_string(laid.size() - 1));
    }

    const Laid& piece = laid[place.piece];
    const bool end = place.piece + 1 == laid.size();
    if (place.item)
    {
        if (place.offset >= piece.items.size())
        {
            throw std::invalid_argument(name + " names item " + std::to_string(place.offset) + ", past its sequence");
        }

        return piece.items[place.offset];
    }
    if (place.offset != 0 && !end && (!piece.run || place.offset >= piece.size))
    {
        throw std::invalid_argument(name + " lies past the piece's start");
    }

    return piece.start + place.offset;
}

// The bytes of `location` that `archive` holds: all of them, or those before its end.
std::span<const uint8_t> BytesOf(std::span<const uint8_t> archive, const Location& location)
{
    const std::size_t start = std::min(location.start, archive.size());

    return archive.subspan(start, std::min(location.size, archive.size() - start));
}

// Whether `joined`, whose tables `after` reads, holds `bytes` in each copy of file `file` that `copies` lists, and the
// bytes of `archive`, whose tables `before` reads, in every other file that holds no copy.
bool Follows(std::span<const uint8_t> archive, const Structure& before, std::span<const uint8_t> joined,
             const Structure& after, uint32_t file, const std::vector<Location>& copies, std::span<const uint8_t> bytes)
{
    if (after.files.size() != before.files.size())
    {
        return false;
    }

    for (std::size_t i = 0; i < before.files.size(); ++i)
    {
        const Location& old = before.files[i];
        const auto holds = [&](const Location& copy)
        {
            return old.start <= copy.start && copy.start + copy.size <= old.start + old.size;
        };
        const std::span<const uint8_t> now = BytesOf(joined, after.files[i]);
        if (old.file == file ? !std::ranges::equal(now, bytes)
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

// The pieces in which each copy of file `file` holds `bytes`: the pieces of the archive, or of group file `group`
// beside it when `group` gives the group file's place. Nothing when they hold no copy, and for a group file whose
// tables can't be read. Throws the exceptions of ReplaceFile.
std::optional<std::vector<Piece>> ReplaceCopies(Flavor flavor, std::span<const commands::Sequence> sequences,
                                                std::span<const Piece> pieces, std::span<const Field> fields,
                                                std::optional<std::size_t> group, uint32_t file,
                                                std::span<const uint8_t> bytes)
{
    const std::vector<uint8_t> joined = Join(flavor, sequences, pieces, fields);
    const std::optional<Structure> before = StructureOf(joined, flavor, group);
    if (!before)
    {
        return std::nullopt;
    }
    const std::vector<std::size_t> starts = Starts(joined, flavor, pieces);

    // Each copy of the file, in the FILE block and in the groups, is a run of its own. The run takes the bytes. Several
    // pieces can start where the copy starts: padding of no bytes, then the run.
    const std::string name = "file " + std::to_string(file);
    const std::string where = group ? " in group file " + std::to_string(*group) : "";
    std::vector<Piece> replaced(pieces.begin(), pieces.end());
    std::vector<Location> copies;
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

        replaced[piece] = std::vector<uint8_t>(bytes.begin(), bytes.end());
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
    if (!read || !Follows(joined, *before, after, *read, file, copies, bytes))
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
        if (location.file != file)
        {
            continue;
        }

        FileCopy& copy = copies.emplace_back();
        copy.start = location.start;
        copy.group_file = group;
        const std::span<const uint8_t> bytes = BytesOf(joined, location);
        copy.bytes.assign(bytes.begin(), bytes.end());
        for (const Location& other : structure.files)
        {
            const bool holds =
                other.start <= location.start && location.start + location.size <= other.start + other.size;
            if (other.file != file && holds && other.size > location.size)
            {
                copy.group = other.file;
            }
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

Layout Cut(std::span<const uint8_t> archive, std::span<const std::vector<uint8_t>> group_files, const Options& options)
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

        return CutBy(sources, options.flavor);
    }
    catch (const xsflib::FormatError&)
    {
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
    // The pieces, one after another.
    const nwsnd::Grammar grammar = GrammarFor(flavor);
    std::vector<uint8_t> bytes;
    std::vector<Laid> laid;
    for (const Piece& piece : pieces)
    {
        Laid& place = laid.emplace_back(Laid{.start = bytes.size()});
        if (const auto* run = std::get_if<std::vector<uint8_t>>(&piece))
        {
            bytes.insert(bytes.end(), run->begin(), run->end());
            place.run = true;
        }
        else if (const auto* file = std::get_if<SequencePiece>(&piece))
        {
            if (file->sequence >= sequences.size())
            {
                throw std::invalid_argument("a piece names sequence " + std::to_string(file->sequence) +
                                            ", and there are " + std::to_string(sequences.size()));
            }

            const Written written =
                WriteFile(sequences[file->sequence], file->version, flavor, grammar, file->sequence);
            for (const uint32_t offset : written.offsets)
            {
                place.items.push_back(bytes.size() + written.bytecode + offset);
            }
            bytes.insert(bytes.end(), written.bytes.begin(), written.bytes.end());
        }
        else
        {
            const uint32_t alignment = std::get<Padding>(piece).alignment;
            if (alignment == 0)
            {
                throw std::invalid_argument("padding to a multiple of 0 bytes");
            }

            bytes.resize(AlignUp(bytes.size(), alignment));
        }
        place.size = bytes.size() - place.start;
    }
    laid.push_back({.start = bytes.size()});

    // Each field's distance, in the archive's byte order.
    for (const Field& field : fields)
    {
        const std::size_t at = PositionOf(field.at, laid);
        if (field.at.item || field.at.piece >= pieces.size() || !laid[field.at.piece].run ||
            field.at.offset + std::size_t{4} > laid[field.at.piece].size)
        {
            throw std::invalid_argument("a field lies outside a run of bytes");
        }

        const std::size_t from = PositionOf(field.from, laid);
        const std::size_t to = PositionOf(field.to, laid);
        if (to < from || to - from > 0xFFFFFFFF)
        {
            throw std::invalid_argument("a field's distance, from " + std::to_string(from) + " to " +
                                        std::to_string(to) + ", doesn't fit 32 bits");
        }

        const auto distance = static_cast<uint32_t>(to - from);
        flavor == Flavor::kCafe ? xsflib::StoreBe32(&bytes[at], distance) : xsflib::StoreLe32(&bytes[at], distance);
    }

    return bytes;
}

std::vector<std::size_t> Starts(std::span<const uint8_t> archive, Flavor flavor, std::span<const Piece> pieces)
{
    // A file's header gives the file's size in its 32 bits at 0x0C.
    constexpr std::size_t kSizeField = 0x0C;
    const xsflib::Reader reader(archive, flavor == Flavor::kCafe);
    std::vector<std::size_t> starts;
    std::size_t position = 0;
    for (const Piece& piece : pieces)
    {
        starts.push_back(position);
        if (const auto* run = std::get_if<std::vector<uint8_t>>(&piece))
        {
            position += run->size();
        }
        else if (std::holds_alternative<SequencePiece>(piece))
        {
            if (position + kSizeField + 4 > archive.size())
            {
                throw std::invalid_argument("a sequence piece starts past the archive's end");
            }

            position += reader.U32(position + kSizeField);
        }
        else
        {
            position = AlignUp(position, std::max<uint32_t>(std::get<Padding>(piece).alignment, 1));
        }

        if (position > archive.size())
        {
            throw std::invalid_argument("a piece runs past the archive's end");
        }
    }
    starts.push_back(position);

    return starts;
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
    ReplacedPieces replaced;
    std::optional<std::vector<Piece>> archive =
        ReplaceCopies(flavor, sequences, pieces, fields, std::nullopt, file, bytes);
    bool found = archive.has_value();
    replaced.pieces = archive ? std::move(*archive) : std::vector<Piece>(pieces.begin(), pieces.end());
    for (std::size_t g = 0; g < group_files.size(); ++g)
    {
        const GroupFileLayout& group_file = group_files[g];
        std::optional<std::vector<Piece>> group =
            ReplaceCopies(flavor, sequences, group_file.pieces, group_file.fields, g, file, bytes);
        found = found || group.has_value();
        replaced.group_files.push_back(group ? std::move(*group) : group_file.pieces);
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
    const bool ctr = flavor == Flavor::kCtr;
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
        if (!ctr)
        {
            put(p + 4, sound.player_id, 0xFFFFFFFF);
        }
        put(p + 8, big_endian ? uint32_t{sound.volume} << 24 : sound.volume, big_endian ? 0xFF000000 : 0xFF);
        if (const std::optional<std::size_t> pan = OptionAt(r, p + 20, 1))
        {
            put(*pan, sound.pan_mode | uint32_t{sound.pan_curve} << 8, 0xFFFF);
        }
        if (const std::optional<std::size_t> priority = ctr ? std::nullopt : OptionAt(r, p + 20, 2))
        {
            put(*priority, sound.player_priority | uint32_t{sound.actor_player_id} << 8, 0xFFFF);
        }

        const Reference detail = ReadRef(r, p + 12);
        if (detail.type != kSequenceSound)
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

    // Each player's sound limit. The reader reads the 3DS's players with its other rules.
    if (!ctr)
    {
        const std::vector<std::size_t> rows = EntriesOf(r, tables, kPlayerTable);
        for (std::size_t i = 0; i < rows.size() && i < players.size(); ++i)
        {
            put(rows[i], players[i].playable_sound_max, 0xFFFFFFFF);
        }
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
        if (detail.type == kSequenceSound)
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
        place.priority = flavor != Flavor::kCtr && OptionAt(r, sound + 20, 2).has_value();
        const Reference detail = ReadRef(r, sound + 12);
        if (detail.type == kSequenceSound)
        {
            place.channel = OptionAt(r, sound + static_cast<uint32_t>(detail.offset) + 12, 1).has_value();
        }
    }

    return places;
}

} // namespace ntdlib::sar
