// SPDX-License-Identifier: MIT

#include "ntdlib/agb/layout.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/rounding.h"

namespace ntdlib::agb
{

Reached::Reached(const Rom& rom) : bytes_(rom.Size(), false)
{
}

bool Reached::Holds(uint32_t offset) const
{
    return bytes_[offset];
}

bool Reached::Touches(uint32_t address, uint32_t size) const
{
    if (address < kRomBase)
    {
        return false;
    }

    const uint64_t from = address - kRomBase;
    const uint64_t to = std::min<uint64_t>(from + size, bytes_.size());
    for (uint64_t i = from; i < to; i++)
    {
        if (bytes_[i])
        {
            return true;
        }
    }

    return false;
}

uint64_t Reached::Beyond() const
{
    return beyond_;
}

void Reached::Mark(uint32_t address, uint64_t size)
{
    if (address < kRomBase)
    {
        return;
    }

    const uint64_t from = address - kRomBase;
    const uint64_t to = std::min<uint64_t>(from + size, bytes_.size());
    for (uint64_t i = from; i < to; i++)
    {
        bytes_[i] = true;
    }

    if (from + size > bytes_.size() && from < kLargestRom)
    {
        beyond_ = std::max(beyond_, std::min<uint64_t>(from + size, kLargestRom));
    }
}

uint64_t AlignUp(uint64_t value, uint64_t alignment)
{
    return ntdlib::AlignUp(value, alignment);
}

uint32_t EndRunOf(const Rom& rom, uint8_t byte)
{
    auto run = uint32_t(rom.Size());
    while (run > 0 && rom.U8(kRomBase + run - 1) == byte)
    {
        run--;
    }

    return run;
}

FreeSpace FreeSpaceOf(const Rom& rom, const Reached* reached, uint64_t floor)
{
    FreeSpace free;
    const auto size = uint32_t(rom.Size());
    const uint8_t last = size ? rom.U8(kRomBase + size - 1) : 0;
    if (size && (last == 0xFF || last == 0))
    {
        // The free space starts past the last byte of the run that the driver reads.
        uint32_t run = EndRunOf(rom, last);
        for (uint32_t i = size; reached && i > run; i--)
        {
            if (reached->Holds(i - 1))
            {
                run = i;
                break;
            }
        }
        free.start = uint32_t(AlignUp(std::max<uint64_t>(run, floor) + kFreeSpaceMargin, kFreeSpaceMargin));
        free.padding = last;
    }
    else
    {
        free.start = uint32_t(AlignUp(std::max<uint64_t>(size, floor), 4));
    }

    return free;
}

std::vector<Span> FillerRunsOf(const Rom& rom, const Reached& reached, uint32_t least_run, bool zeros)
{
    std::vector<Span> full;
    std::vector<Span> empty;
    const auto size = uint32_t(rom.Size());
    const uint8_t* bytes = rom.Ptr(kRomBase);
    uint32_t at = 0;
    while (at < size)
    {
        const uint8_t byte = bytes[at];
        uint32_t end = at + 1;
        if (byte != 0xFF && (byte != 0 || !zeros))
        {
            at = end;
            continue;
        }

        while (end < size && bytes[end] == byte)
        {
            end++;
        }

        const uint64_t start = AlignUp(uint64_t(at) + kFreeSpaceMargin, kFreeSpaceMargin);
        if (end - at >= least_run && end < size && start + kFreeSpaceMargin < end)
        {
            bool read = false;
            for (uint32_t i = at; i < end && !read; i++)
            {
                read = reached.Holds(i);
            }
            if (!read)
            {
                (byte == 0xFF ? full : empty).push_back({uint32_t(start), end - kFreeSpaceMargin - uint32_t(start)});
            }
        }

        at = end;
    }

    full.insert(full.end(), empty.begin(), empty.end());

    return full;
}

namespace
{

// The size of `piece` in the ROM: its bytes, or its room.
uint64_t PieceSize(const sar::Piece& piece)
{
    if (const auto* bytes = std::get_if<std::vector<uint8_t>>(&piece))
    {
        return bytes->size();
    }

    return std::get<sar::SequencePiece>(piece).room;
}

// The place of each of `pieces` from the ROM's start. Throws std::invalid_argument for a sequence piece that names a
// sequence past `sequences`, and for padding.
std::vector<uint64_t> StartsOf(std::span<const sar::Piece> pieces, std::size_t sequences)
{
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
            if (sequence->sequence >= sequences)
            {
                throw std::invalid_argument("a sequence piece names a sequence missing from the layout");
            }
            size += sequence->room;
        }
        else
        {
            throw std::invalid_argument("a GBA ROM's layout holds padding");
        }
    }

    return starts;
}

// Where each sequence that moves can start at the earliest. With `after_tables`, a sequence that moves stays after
// each place in a run that a field counts from.
std::vector<uint64_t> FloorsOf(std::size_t pieces, std::span<const sar::Field> fields, std::span<const uint64_t> starts,
                               bool after_tables)
{
    std::vector<uint64_t> floors(pieces, 0);
    for (const sar::Field& field : fields)
    {
        if (after_tables && field.to.piece < pieces && field.from.piece < pieces && !field.from.item)
        {
            floors[field.to.piece] = std::max(floors[field.to.piece], starts[field.from.piece] + field.from.offset);
        }
    }

    return floors;
}

// The places of the sequences that move, where neighbours count as one.
std::vector<Span> MovingPlaces(std::span<const sar::Piece> pieces, std::span<const uint64_t> starts,
                               std::span<const std::size_t> moving)
{
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
            places.push_back({uint32_t(starts[p]), room});
        }
    }

    return places;
}

// Takes `bytes` from the first of `spans` that has room for them after `floor`, on a multiple of `alignment`. Sets
// `at` to their place. Returns false when no span has room.
bool TakeFrom(std::vector<Span>& spans, uint64_t bytes, uint64_t floor, uint32_t alignment, uint64_t& at)
{
    for (Span& span : spans)
    {
        const uint64_t place = AlignUp(std::max<uint64_t>(span.start, floor), alignment);
        if (place + bytes <= uint64_t(span.start) + span.size)
        {
            at = place;
            span.size -= uint32_t(place + bytes - span.start);
            span.start = uint32_t(place + bytes);
            return true;
        }
    }

    return false;
}

} // namespace

JoinedRom::JoinedRom(std::span<const sar::Piece> pieces, std::size_t sequences, std::span<const sar::Field> fields,
                     const FreeSpace& free, const JoinRules& rules, const Encode& encode)
    : pieces_(pieces), starts_(StartsOf(pieces, sequences))
{
    const uint64_t size = starts_.empty() ? 0 : starts_.back() + PieceSize(pieces.back());
    if (size > kLargestRom)
    {
        throw std::invalid_argument("a GBA ROM can't hold more than 32 MiB");
    }
    if (rules.run_first && !pieces.empty() && !std::holds_alternative<std::vector<uint8_t>>(pieces.front()))
    {
        throw std::invalid_argument("a GBA ROM's layout starts with a sequence");
    }

    image_.resize(size);
    for (std::size_t p = 0; p < pieces.size(); p++)
    {
        if (const auto* bytes = std::get_if<std::vector<uint8_t>>(&pieces[p]))
        {
            std::copy(bytes->begin(), bytes->end(), image_.begin() + std::ptrdiff_t(starts_[p]));
        }
    }

    const std::vector<uint64_t> floors = FloorsOf(pieces.size(), fields, starts_, rules.after_tables);

    // Each sequence in its place, or later in the places of the sequences that move, or in the free space.
    std::vector<Encoded> encoded(pieces.size());
    items_.resize(pieces.size());
    const auto place_at = [&](std::size_t p, uint64_t at)
    {
        std::copy(encoded[p].bytes.begin(), encoded[p].bytes.end(), image_.begin() + std::ptrdiff_t(at));
        for (const uint32_t offset : encoded[p].offsets)
        {
            items_[p].push_back(at + offset);
        }
    };

    std::vector<std::size_t> moving;
    for (std::size_t p = 0; p < pieces.size(); p++)
    {
        const auto* sequence = std::get_if<sar::SequencePiece>(&pieces[p]);
        if (!sequence)
        {
            continue;
        }

        encoded[p] = encode(p, sequence->sequence);
        if (encoded[p].bytes.size() > sequence->room)
        {
            moving.push_back(p);
            continue;
        }

        place_at(p, starts_[p]);
    }

    std::vector<Span> places = MovingPlaces(pieces, starts_, moving);

    // The largest sequence first, then the earlier of two of a size.
    const auto larger = [&encoded](std::size_t a, std::size_t b)
    {
        return encoded[a].bytes.size() > encoded[b].bytes.size();
    };
    std::stable_sort(moving.begin(), moving.end(), larger);

    std::vector<Span> filler = free.filler;
    uint64_t end_of_sequences = free.start;

    const auto take_from = [&rules](std::vector<Span>& spans, uint64_t bytes, uint64_t floor, uint64_t& at)
    {
        return TakeFrom(spans, bytes, floor, rules.alignment, at);
    };

    const std::string outgrowing = "the " + std::string(rules.sequences) + " that outgrow their places don't fit in";
    for (const std::size_t p : moving)
    {
        const uint64_t bytes = encoded[p].bytes.size();
        uint64_t at = 0;
        if (!take_from(places, bytes, floors[p], at))
        {
            const uint64_t place = AlignUp(end_of_sequences, rules.alignment);
            if (place + bytes <= kLargestRom)
            {
                at = place;
                end_of_sequences = place + bytes;
                if (at + bytes > image_.size())
                {
                    image_.resize(at + bytes, free.padding);
                }
            }
            else if (!rules.filler)
            {
                throw std::invalid_argument(outgrowing + " a GBA ROM's 32 MiB");
            }
            else if (!take_from(filler, bytes, floors[p], at))
            {
                throw std::invalid_argument(outgrowing + " a GBA ROM's 32 MiB or in its filler runs");
            }
        }

        place_at(p, at);
    }
}

uint64_t JoinedRom::Position(const sar::Place& place) const
{
    if (place.sequence || place.piece >= pieces_.size())
    {
        throw std::invalid_argument("a field names a place missing from a GBA ROM's pieces");
    }
    if (place.item)
    {
        if (!std::holds_alternative<sar::SequencePiece>(pieces_[place.piece]) ||
            place.offset >= items_[place.piece].size())
        {
            throw std::invalid_argument("a field names an item missing from its sequence");
        }

        return items_[place.piece][place.offset];
    }

    return starts_[place.piece] + place.offset;
}

uint8_t* JoinedRom::FieldBytes(const sar::Field& field, uint32_t width)
{
    const auto* run = field.at.piece < pieces_.size() && !field.at.item && !field.at.sequence
                          ? std::get_if<std::vector<uint8_t>>(&pieces_[field.at.piece])
                          : nullptr;
    if (!run || uint64_t(field.at.offset) + width > run->size())
    {
        throw std::invalid_argument("a field is outside the runs of a GBA ROM");
    }

    return &image_[starts_[field.at.piece] + field.at.offset];
}

std::vector<uint8_t> JoinedRom::TakeImage()
{
    return std::move(image_);
}

void AddRomRun(const Rom& rom, uint32_t& at, uint32_t end, std::vector<sar::Piece>& pieces)
{
    if (end > at)
    {
        pieces.emplace_back(std::vector<uint8_t>(rom.Ptr(at), rom.Ptr(at) + (end - at)));
        at = end;
    }
}

std::optional<sar::Place> RunPlaceOf(std::span<const sar::Piece> pieces, uint32_t address)
{
    uint32_t start = kRomBase;
    for (uint32_t p = 0; p < pieces.size(); p++)
    {
        const auto* bytes = std::get_if<std::vector<uint8_t>>(&pieces[p]);
        const uint32_t size = bytes ? uint32_t(bytes->size()) : std::get<sar::SequencePiece>(pieces[p]).room;
        if (bytes && address >= start && address - start < size)
        {
            return sar::Place{p, address - start};
        }
        start += size;
    }

    return std::nullopt;
}

std::vector<std::vector<uint32_t>> NamedItems(std::span<const sar::Piece> pieces, std::span<const sar::Field> fields)
{
    std::vector<std::vector<uint32_t>> items(pieces.size());
    for (const sar::Field& field : fields)
    {
        if (field.to.item && field.to.piece < pieces.size() && !field.to.sequence)
        {
            items[field.to.piece].push_back(field.to.offset);
        }
    }

    for (std::vector<uint32_t>& offsets : items)
    {
        std::sort(offsets.begin(), offsets.end());
        offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
    }

    return items;
}

} // namespace ntdlib::agb
