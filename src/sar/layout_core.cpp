// SPDX-License-Identifier: MIT

#include "sar/layout_core.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
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
#include "ntdlib/rounding.h"
#include "ntdlib/sar/layout.h"
#include "sar/file_bytes.h"

namespace ntdlib::sar
{
namespace
{

using core::Container;
using core::Distance;
using core::Location;
using core::Source;
using core::Start;
using core::Structure;

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

// The file of sequence `index`.
Written WriteFile(const commands::Sequence& sequence, uint32_t version, const Container& container, std::size_t index)
{
    nwsnd::Encoded encoded = nwsnd::EncodeSequence(sequence, container.grammar);
    core::SequenceParts parts;
    parts.version = version;
    parts.data = encoded.bytes;
    for (const commands::Label& label : sequence.labels)
    {
        if (label.item >= encoded.offsets.size())
        {
            throw std::invalid_argument("sequence " + std::to_string(index) + ": label " + label.name +
                                        " is past the end");
        }

        parts.labels.push_back({label.name, encoded.offsets[label.item]});
    }

    core::WrittenFile file = container.write(parts);
    Written written;
    written.bytes = std::move(file.bytes);
    written.bytecode = file.bytecode;
    written.offsets = std::move(encoded.offsets);

    return written;
}

// Whether `decoded` gives `bytes` back, with each start at its offset.
bool GivesBack(const Decoded& decoded, const Container& container, std::span<const uint8_t> bytes)
{
    try
    {
        const Written written = WriteFile(decoded.sequence, decoded.version, container, 0);
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
std::optional<Decoded> Decode(std::span<const uint8_t> bytes, const std::set<uint32_t>& starts,
                              const Container& container)
{
    try
    {
        const core::SequenceParts file = container.read(bytes);
        const std::vector<uint32_t> entries(starts.begin(), starts.end());
        nwsnd::Decoded read = nwsnd::DecodeSequence(file.data, file.labels, container.grammar, entries);
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
            if (GivesBack(trimmed, container, bytes))
            {
                return trimmed;
            }
        }
        if (GivesBack(decoded, container, bytes))
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

// `sequence` as the commands of the sequence file `bytes`, when they give its bytes back with each of `starts` at an
// item's start. The file's labels are the labels of `sequence`.
std::optional<Decoded> Known(std::span<const uint8_t> bytes, const std::set<uint32_t>& starts,
                             const commands::Sequence& sequence, const Container& container)
{
    try
    {
        Decoded known;
        known.sequence = sequence;
        known.version = container.read(bytes).version;
        const std::vector<uint32_t> offsets = nwsnd::EncodeSequence(sequence, container.grammar).offsets;
        for (const uint32_t start : starts)
        {
            const auto found = std::ranges::find(offsets.begin(), offsets.end() - 1, start);
            if (found == offsets.end() - 1)
            {
                return std::nullopt;
            }
            known.entries[start] = static_cast<uint32_t>(found - offsets.begin());
        }
        if (GivesBack(known, container, bytes))
        {
            return known;
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
    uint32_t piece = 0;    // the first copy's piece, in the source that holds it
};

// The sequence files of `sources`. Copies that share a file ID belong to one family, in the archive and in the group
// files. Returns each file ID's family.
std::map<uint32_t, std::size_t> FamiliesOf(std::span<const Source> sources, const Container& container,
                                           std::vector<Family>& families)
{
    // The sequence files, by where they lie, and the file IDs that name each.
    const char* magic = container.sequence_magic.data();
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

// The alignment that every file of the archive keeps, up to `most`.
std::size_t AlignmentOf(const Structure& structure, std::size_t most)
{
    std::size_t alignment = most;
    for (const Location& file : structure.files)
    {
        while (file.start % alignment != 0)
        {
            alignment /= 2;
        }
    }

    return alignment;
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

// Decodes each family of `families` whose copies in `sources` all hold the same bytes, with the commands that
// `known_of` gives the family when they fit, or else from the bytes. A family that decodes is kept.
void DecodeFamilies(std::span<const Source> sources, const Container& container,
                    const std::map<std::size_t, std::vector<const commands::Sequence*>>& known_of,
                    std::vector<Family>& families)
{
    for (std::size_t f = 0; f < families.size(); ++f)
    {
        Family& family = families[f];
        if (family.size == 0)
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
            const auto given = known_of.find(f);
            for (std::size_t k = 0; given != known_of.end() && !family.decoded && k < given->second.size(); ++k)
            {
                family.decoded = Known(first, family.starts, *given->second[k], container);
            }
            if (!family.decoded)
            {
                family.decoded = Decode(first, family.starts, container);
            }
            family.kept = family.decoded.has_value();
        }
    }
}

// Where `placed`, the pieces of the archive `archive`, holds the field of each sound's start in the tables. Nothing for
// a sound without such a field, or one whose family isn't kept. Throws xsflib::FormatError for a start that the pieces
// can't place.
std::vector<std::optional<Place>> StartFields(const Structure& archive, const std::vector<Family>& families,
                                              const std::map<uint32_t, std::size_t>& family_of, const Pieces& placed)
{
    std::vector<std::optional<Place>> start_fields(archive.starts.size());
    for (std::size_t i = 0; i < archive.starts.size(); ++i)
    {
        const Start& start = archive.starts[i];
        const auto found = family_of.find(start.file);
        if (!start.at || found == family_of.end() || !families[found->second].kept)
        {
            continue;
        }

        start_fields[i] = placed.FieldAt(*start.at);
        if (!start_fields[i])
        {
            throw xsflib::FormatError("a sound's start that the pieces can't place");
        }
    }

    return start_fields;
}

// Adds to `cut` the field of each sound's start that `start_fields` places.
void AddStartFields(const Structure& archive, const std::vector<Family>& families,
                    const std::map<uint32_t, std::size_t>& family_of,
                    const std::vector<std::optional<Place>>& start_fields, core::Cutting& cut)
{
    // A sound's start counts from the bytecode's first item. The field names items of the archive's copy of the file.
    // Without such a copy, the field names items of the file's sequence.
    for (std::size_t i = 0; i < archive.starts.size(); ++i)
    {
        if (!start_fields[i])
        {
            continue;
        }

        const Start& start = archive.starts[i];
        const Family& family = families[family_of.at(start.file)];
        const bool in_archive = family.copies.front().source == 0;
        const uint32_t piece = in_archive ? family.piece : family.sequence;
        const uint32_t item = family.decoded->entries.at(start.offset);
        cut.fields.push_back(
            {*start_fields[i], Place{piece, 0, true, !in_archive}, Place{piece, item, true, !in_archive}});
    }
}

// The bytes of `pieces`, one after another, with `container`'s rules for the files of `sequences`. Appends to `laid`
// where each piece lies. Throws std::invalid_argument for a piece that names a sequence past `sequences`, and for
// padding to a multiple of 0 bytes.
std::vector<uint8_t> LayPieces(const Container& container, std::span<const commands::Sequence> sequences,
                               std::span<const Piece> pieces, std::vector<Laid>& laid)
{
    std::vector<uint8_t> bytes;
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

            const Written written = WriteFile(sequences[file->sequence], file->version, container, file->sequence);
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

    return bytes;
}

// Sets each field of `fields` in `bytes`, the pieces that `laid` lays out, to its distance, in the archive's byte
// order. Throws std::invalid_argument for a field outside a run of bytes, for places that name items of a sequence and
// something else or a place past `sequences`, and for a distance that doesn't fit 32 bits.
void SetFields(const Container& container, std::span<const commands::Sequence> sequences, std::span<const Piece> pieces,
               std::span<const Field> fields, const std::vector<Laid>& laid, std::vector<uint8_t>& bytes)
{
    // Where an item of a sequence lies in the sequence's bytecode, for a place that names a sequence.
    std::map<uint32_t, std::vector<uint32_t>> encoded;
    const auto item_at = [&](const Place& place) -> std::size_t
    {
        if (place.piece >= sequences.size())
        {
            throw std::invalid_argument("a place names sequence " + std::to_string(place.piece) + ", and there are " +
                                        std::to_string(sequences.size()));
        }

        auto found = encoded.find(place.piece);
        if (found == encoded.end())
        {
            std::vector<uint32_t> offsets = nwsnd::EncodeSequence(sequences[place.piece], container.grammar).offsets;
            found = encoded.emplace(place.piece, std::move(offsets)).first;
        }
        if (place.offset >= found->second.size())
        {
            throw std::invalid_argument("a place names item " + std::to_string(place.offset) +
                                        ", past the end of sequence " + std::to_string(place.piece));
        }

        return found->second[place.offset];
    };

    // Each field's distance, in the archive's byte order. A field whose places name items of a sequence takes their
    // distance in the sequence's bytecode.
    for (const Field& field : fields)
    {
        const std::size_t at = PositionOf(field.at, laid);
        if (field.at.item || field.at.sequence || field.at.piece >= pieces.size() || !laid[field.at.piece].run ||
            field.at.offset + std::size_t{4} > laid[field.at.piece].size)
        {
            throw std::invalid_argument("a field lies outside a run of bytes");
        }

        const bool items = field.from.sequence || field.to.sequence;
        if (items && !(field.from.sequence && field.to.sequence && field.from.item && field.to.item &&
                       field.from.piece == field.to.piece))
        {
            throw std::invalid_argument("a field's places name items of a sequence and something else");
        }

        const std::size_t from = items ? item_at(field.from) : PositionOf(field.from, laid);
        const std::size_t to = items ? item_at(field.to) : PositionOf(field.to, laid);
        if (to < from || to - from > 0xFFFFFFFF)
        {
            throw std::invalid_argument("a field's distance, from " + std::to_string(from) + " to " +
                                        std::to_string(to) + ", doesn't fit 32 bits");
        }

        const auto distance = static_cast<uint32_t>(to - from);
        container.big_endian ? xsflib::StoreBe32(&bytes[at], distance) : xsflib::StoreLe32(&bytes[at], distance);
    }
}

} // namespace

namespace core
{

Cutting CutBy(std::span<const Source> sources, const Container& container, const KnownSequences& known)
{
    // Each family's starts, and its commands when every copy holds the same bytes and they decode, or the commands that
    // `known` gives a file of the family.
    std::vector<Family> families;
    const std::map<uint32_t, std::size_t> family_of = FamiliesOf(sources, container, families);
    std::map<std::size_t, std::vector<const commands::Sequence*>> known_of;
    for (const auto& [file, sequence] : known)
    {
        const auto found = family_of.find(file);
        if (found != family_of.end())
        {
            known_of[found->second].push_back(&sequence);
        }
    }
    const Structure& archive = sources.front().structure;
    for (const Start& start : archive.starts)
    {
        const auto found = family_of.find(start.file);
        if (found != family_of.end())
        {
            families[found->second].starts.insert(start.offset);
        }
    }
    DecodeFamilies(sources, container, known_of, families);

    std::vector<std::size_t> alignments;
    for (const Source& source : sources)
    {
        alignments.push_back(AlignmentOf(source.structure, container.most_alignment));
    }
    LeaveOut(sources, alignments, families);

    // The archive comes first, and its copies give the families their sequences. A family that only group files hold
    // takes its sequence from the first group file's copy, after the archive's sequences.
    Cutting cut;
    cut.group_files.resize(sources.size() - 1);
    std::vector<std::optional<Place>> start_fields(archive.starts.size());
    for (std::size_t s = 0; s < sources.size(); ++s)
    {
        const Source& source = sources[s];
        std::vector<Piece>& pieces = s == 0 ? cut.pieces : cut.group_files[s - 1].pieces;
        std::vector<Copy> copies = CopiesOf(families, s, alignments[s], source.bytes.size());
        AddRuns(source.bytes, source.structure, alignments[s], copies);
        const Pieces placed(source.bytes, families, std::move(copies), alignments[s], cut.sequences, pieces);
        std::vector<Field> fields = FieldsOf(source.structure, placed, pieces);
        if (s > 0)
        {
            cut.group_files[s - 1].fields = std::move(fields);
            continue;
        }

        cut.fields = std::move(fields);
        start_fields = StartFields(archive, families, family_of, placed);
    }
    AddStartFields(archive, families, family_of, start_fields, cut);

    return cut;
}

std::vector<uint8_t> JoinBy(const Container& container, std::span<const commands::Sequence> sequences,
                            std::span<const Piece> pieces, std::span<const Field> fields)
{
    std::vector<Laid> laid;
    std::vector<uint8_t> bytes = LayPieces(container, sequences, pieces, laid);
    laid.push_back({.start = bytes.size()});
    SetFields(container, sequences, pieces, fields, laid, bytes);

    return bytes;
}

std::vector<std::size_t> StartsBy(const Container& container, std::span<const uint8_t> archive,
                                  std::span<const Piece> pieces)
{
    // A sequence file's header gives the file's size.
    const xsflib::Reader reader(archive, container.big_endian);
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
            if (position + container.size_field + 4 > archive.size())
            {
                throw std::invalid_argument("a sequence piece starts past the archive's end");
            }

            position += reader.U32(position + container.size_field);
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

} // namespace core

} // namespace ntdlib::sar
