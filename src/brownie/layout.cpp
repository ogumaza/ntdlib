// SPDX-License-Identifier: MIT

#include "ntdlib/brownie/layout.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/agb/rom.h"
#include "ntdlib/brownie/commands.h"

namespace ntdlib::brownie
{
namespace
{

using agb::kRomBase;

// The sample sets and samples that a byte of E1 can name.
constexpr uint32_t kTableEntries = 256;

// The semitones of a sample set of the Sword of Mana revision.
constexpr uint32_t kSetEntries = 12;

uint64_t AlignUp(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

// The place past the last byte that the driver reads of what the regions and the driver's sample table name.
uint64_t ReachedEnd(const Rom& rom, const DriverInfo& info, std::span<const Region> regions)
{
    uint64_t end = info.song_table - kRomBase + uint64_t(std::max(info.song_count, 0)) * 4;
    for (const Region& region : regions)
    {
        end = std::max(end, uint64_t(region.start) - kRomBase + region.size);
    }

    const auto keep_sample = [&](uint32_t entry)
    {
        if (rom.Contains(entry, 12) && rom.Contains(rom.U32(entry + 4)))
        {
            end = std::max(end, uint64_t(rom.U32(entry + 4)) - kRomBase);
        }
    };

    for (uint32_t i = 0; i < kTableEntries; i++)
    {
        const uint32_t entry = rom.U32(info.sample_table + 4 * i);
        if (info.revision == Revision::kMagicalVacation)
        {
            keep_sample(entry);
            continue;
        }

        for (uint32_t k = 0; rom.Contains(entry, kSetEntries * 4) && k < kSetEntries; k++)
        {
            keep_sample(rom.U32(entry + 4 * k));
        }
    }

    return end;
}

FreeSpace FreeSpaceOf(const Rom& rom, const DriverInfo& info, std::span<const Region> regions)
{
    FreeSpace free;
    const auto size = uint32_t(rom.Size());
    const uint8_t last = size ? rom.U8(kRomBase + size - 1) : 0;
    if (size && (last == 0xFF || last == 0))
    {
        uint32_t run = size;
        while (run > 0 && rom.U8(kRomBase + run - 1) == last)
        {
            run--;
        }

        const uint64_t from = std::max(uint64_t(run), std::min(ReachedEnd(rom, info, regions), uint64_t(size)));
        free.start = uint32_t(AlignUp(from + kFreeSpaceMargin, kFreeSpaceMargin));
        free.padding = last;
    }
    else
    {
        free.start = uint32_t(AlignUp(size, 4));
    }

    return free;
}

} // namespace

Layout Cut(const Rom& rom, const DriverInfo& info)
{
    const std::vector<Region> regions = FindRegions(rom, info);
    Layout layout;
    layout.revision = info.revision;
    layout.free = FreeSpaceOf(rom, info, regions);

    // Each region that comes back from its items. The runs lie between them.
    const uint32_t table_end = info.song_table + uint32_t(info.song_count) * 4;
    struct Kept
    {
        uint32_t piece = 0;
        Decoded decoded;
    };

    std::vector<Kept> kept;
    uint32_t at = kRomBase;
    const auto add_run = [&](uint32_t end)
    {
        if (end > at)
        {
            layout.pieces.emplace_back(std::vector<uint8_t>(rom.Ptr(at), rom.Ptr(at) + (end - at)));
            at = end;
        }
    };

    for (const Region& region : regions)
    {
        if (region.start < table_end && info.song_table < region.start + region.size)
        {
            continue;
        }

        Decoded decoded = DecodeRegion(rom, info, region);
        if (decoded.opaque)
        {
            continue;
        }

        const Encoded encoded = EncodeSequence(decoded.sequence, info.revision, region.start);
        if (encoded.bytes.size() != region.size ||
            !std::equal(encoded.bytes.begin(), encoded.bytes.end(), rom.Ptr(region.start)))
        {
            continue;
        }

        add_run(region.start);
        layout.pieces.emplace_back(sar::SequencePiece{uint32_t(layout.sequences.size()), 0, region.size});
        layout.sequences.push_back(decoded.sequence);
        kept.push_back({uint32_t(layout.pieces.size() - 1), std::move(decoded)});
        at = region.start + region.size;
    }
    add_run(rom.End());

    // A song table entry's field lies in the run that holds the table: the regions lie clear of it.
    uint32_t start = kRomBase;
    for (uint32_t p = 0; p < layout.pieces.size(); p++)
    {
        const auto* bytes = std::get_if<std::vector<uint8_t>>(&layout.pieces[p]);
        const uint32_t size = bytes ? uint32_t(bytes->size()) : std::get<sar::SequencePiece>(layout.pieces[p]).room;
        if (bytes && info.song_table >= start && info.song_table < start + size)
        {
            std::map<int, sar::Place> lists;
            for (const Kept& region : kept)
            {
                for (size_t s = 0; s < region.decoded.sounds.size(); s++)
                {
                    lists[region.decoded.sounds[s]] = {region.piece, region.decoded.lists[s], true};
                }
            }

            for (const auto& [sound, list] : lists)
            {
                layout.fields.push_back({{p, info.song_table + 4 * uint32_t(sound) - start}, {0, 0}, list});
            }
            break;
        }
        start += size;
    }

    return layout;
}

std::map<int, SoundPlace> SoundPlaces(const Layout& layout, const DriverInfo& info)
{
    std::vector<uint32_t> starts;
    uint32_t start = kRomBase;
    for (const sar::Piece& piece : layout.pieces)
    {
        starts.push_back(start);
        const auto* bytes = std::get_if<std::vector<uint8_t>>(&piece);
        start += bytes ? uint32_t(bytes->size()) : std::get<sar::SequencePiece>(piece).room;
    }

    std::map<int, SoundPlace> places;
    for (const sar::Field& field : layout.fields)
    {
        if (field.at.piece >= starts.size() || field.to.piece >= layout.pieces.size())
        {
            continue;
        }

        const uint32_t address = starts[field.at.piece] + field.at.offset;
        const auto* sequence = std::get_if<sar::SequencePiece>(&layout.pieces[field.to.piece]);
        if (sequence && address >= info.song_table)
        {
            places[int((address - info.song_table) / 4)] = {sequence->sequence, field.to.offset};
        }
    }

    return places;
}

std::vector<uint8_t> Join(Revision revision, std::span<const commands::Sequence> sequences,
                          std::span<const sar::Piece> pieces, std::span<const sar::Field> fields, const FreeSpace& free)
{
    // Each piece's place, from the ROM's start.
    std::vector<uint64_t> starts;
    uint64_t size = 0;
    for (const sar::Piece& piece : pieces)
    {
        starts.push_back(size);
        if (const auto* bytes = std::get_if<std::vector<uint8_t>>(&piece))
        {
            size += bytes->size();
        }
        else if (const auto* sequence = std::get_if<sar::SequencePiece>(&piece))
        {
            if (sequence->sequence >= sequences.size())
            {
                throw std::invalid_argument("a region names a sequence that the layout lacks");
            }
            size += sequence->room;
        }
        else
        {
            throw std::invalid_argument("a GBA ROM's layout holds padding");
        }
    }
    if (size > kLargestRom)
    {
        throw std::invalid_argument("a GBA ROM can't hold more than 32 MiB");
    }

    std::vector<uint8_t> image(size);
    for (std::size_t p = 0; p < pieces.size(); p++)
    {
        if (const auto* bytes = std::get_if<std::vector<uint8_t>>(&pieces[p]))
        {
            std::copy(bytes->begin(), bytes->end(), image.begin() + std::ptrdiff_t(starts[p]));
        }
    }

    // Each region in its place, or later in the places of the regions that move, or in the free space.
    std::vector<Encoded> encoded(pieces.size());
    std::vector<std::vector<uint64_t>> items(pieces.size());
    std::vector<std::size_t> moving;
    const auto place_at = [&](std::size_t p, uint64_t at)
    {
        std::copy(encoded[p].bytes.begin(), encoded[p].bytes.end(), image.begin() + std::ptrdiff_t(at));
        items[p].clear();
        for (const uint32_t offset : encoded[p].offsets)
        {
            items[p].push_back(at + offset);
        }
    };

    for (std::size_t p = 0; p < pieces.size(); p++)
    {
        const auto* sequence = std::get_if<sar::SequencePiece>(&pieces[p]);
        if (!sequence)
        {
            continue;
        }

        encoded[p] = EncodeSequence(sequences[sequence->sequence], revision, uint32_t(kRomBase + starts[p]));
        if (encoded[p].bytes.size() > sequence->room)
        {
            moving.push_back(p);
            continue;
        }

        place_at(p, starts[p]);
    }

    // The places of the regions that move, where neighbours count as one.
    struct Span
    {
        uint64_t start = 0;
        uint64_t size = 0;
    };

    std::vector<Span> places;
    for (const std::size_t p : moving)
    {
        const uint32_t room = std::get<sar::SequencePiece>(pieces[p]).room;
        if (!places.empty() && places.back().start + places.back().size == starts[p])
        {
            places.back().size += room;
        }
        else if (room > 0)
        {
            places.push_back({starts[p], room});
        }
    }

    // The largest region first, then the earlier of two of a size.
    std::stable_sort(moving.begin(), moving.end(), [&encoded](std::size_t a, std::size_t b)
                     { return encoded[a].bytes.size() > encoded[b].bytes.size(); });

    uint64_t end_of_regions = free.start;
    for (const std::size_t p : moving)
    {
        const uint64_t bytes = encoded[p].bytes.size();
        const uint64_t remainder = starts[p] % 4;
        uint64_t at = 0;
        bool placed = false;
        for (Span& span : places)
        {
            const uint64_t first = AlignUp(span.start + 4 - remainder, 4) - 4 + remainder;
            if (first + bytes <= span.start + span.size)
            {
                at = first;
                span.size = span.start + span.size - (first + bytes);
                span.start = first + bytes;
                placed = true;
                break;
            }
        }

        if (!placed)
        {
            at = AlignUp(end_of_regions + 4 - remainder, 4) - 4 + remainder;
            if (at + bytes > kLargestRom)
            {
                throw std::invalid_argument("the regions that outgrow their places don't fit in a GBA ROM's 32 MiB");
            }

            end_of_regions = at + bytes;
            if (at + bytes > image.size())
            {
                image.resize(at + bytes, free.padding);
            }
        }

        // The commands hold absolute addresses. At the new place, they name the new place's bytes.
        const uint32_t sequence = std::get<sar::SequencePiece>(pieces[p]).sequence;
        encoded[p] = EncodeSequence(sequences[sequence], revision, uint32_t(kRomBase + at));
        place_at(p, at);
    }

    for (const sar::Field& field : fields)
    {
        const auto* run = field.at.piece < pieces.size() && !field.at.item && !field.at.sequence
                              ? std::get_if<std::vector<uint8_t>>(&pieces[field.at.piece])
                              : nullptr;
        if (!run || uint64_t(field.at.offset) + 4 > run->size())
        {
            throw std::invalid_argument("a field doesn't lie in a run of a GBA ROM");
        }

        const sar::Place& to = field.to;
        if (to.sequence || !to.item || to.piece >= pieces.size() ||
            !std::holds_alternative<sar::SequencePiece>(pieces[to.piece]) || to.offset >= items[to.piece].size())
        {
            throw std::invalid_argument("a field names an item that a region lacks");
        }

        xsflib::StoreLe32(&image[starts[field.at.piece] + field.at.offset],
                          uint32_t(kRomBase + items[to.piece][to.offset]));
    }

    return image;
}

std::vector<uint8_t> Join(const Layout& layout)
{
    return Join(layout.revision, layout.sequences, layout.pieces, layout.fields, layout.free);
}

} // namespace ntdlib::brownie
