// SPDX-License-Identifier: MIT

#include "ntdlib/ubimilan/layout.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/ubimilan/commands.h"
#include "ntdlib/ubimilan/song.h"
#include "xsflib/bytes.h"

namespace ntdlib::ubimilan
{
namespace
{

using agb::kRomBase;

// The bank's header up to the resource table's first entry, at the least: its counts and the offsets of its tables.
constexpr uint32_t kBankHeader = 16;

// The kit table's entries that the driver's init reads: the first.
constexpr uint32_t kKitTableBytes = 2;

// A kit's entries: a halfword for each of 256 keys and 256 offsets.
constexpr uint32_t kKitBytes = 2 * (256 + 256);

uint64_t AlignUp(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

// `size` bytes of a ROM from `start`, counted from the ROM's start.
struct Span
{
    uint32_t start = 0;
    uint32_t size = 0;
};

// A sequence that the cut reads.
struct CutSequence
{
    int resource = 0; // the first resource that names it
    Decoded decoded;
    uint32_t size = 0;
    bool usable = false;
};

// Reads each sequence that a resource names, by its address.
std::map<uint32_t, CutSequence> ReadSequences(const Rom& rom, const DriverInfo& info)
{
    std::map<uint32_t, CutSequence> sequences;
    for (int id = 0; id < info.resource_count; id++)
    {
        const uint32_t address = ResourceAddress(rom, info, id);
        if (!address || rom.U8(address) != kSequence || sequences.count(address))
        {
            continue;
        }

        CutSequence sequence;
        sequence.resource = id;
        sequence.decoded = DecodeResource(rom, info, id);
        sequence.size = sequence.decoded.read;
        if (!sequence.decoded.opaque)
        {
            const Encoded encoded = EncodeSequence(sequence.decoded.sequence);
            sequence.usable = encoded.bytes.size() == sequence.size &&
                              std::equal(encoded.bytes.begin(), encoded.bytes.end(), rom.Ptr(address));
        }

        sequences.emplace(address, std::move(sequence));
    }

    return sequences;
}

// The address past the last byte of the ROM that the driver reads.
uint32_t LastRead(const Rom& rom, const DriverInfo& info, const std::map<uint32_t, CutSequence>& sequences)
{
    uint32_t last = info.resource_table + 4 * uint32_t(info.resource_count);
    const auto mark = [&last](uint32_t address, uint32_t size)
    {
        last = std::max(last, address + size);
    };
    mark(info.kit_table, kKitTableBytes);
    mark(info.kit, kKitBytes);
    for (const auto& [address, sequence] : sequences)
    {
        mark(address, sequence.size);
    }

    for (int id = 0; id < info.resource_count; id++)
    {
        const uint32_t address = ResourceAddress(rom, info, id);
        if (!address)
        {
            continue;
        }

        GameSample sample;
        if (rom.U8(address) == kSample && ReadSample(rom, address, sample))
        {
            mark(sample.data, sample.length);
        }
        else
        {
            mark(address, 1);
        }
    }

    for (size_t s = 0; s < info.songs.size(); s++)
    {
        MusicPiece piece;
        if (ReadMusic(rom, info, int(s), piece))
        {
            mark(piece.address, 8 + 2 * uint32_t(piece.sequences.size()));
        }
    }

    return last;
}

// Where the free space of `rom` starts.
FreeSpace FreeSpaceOf(const Rom& rom, uint32_t last_read)
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

        run = std::max(run, last_read > kRomBase ? std::min(last_read - kRomBase, size) : 0);
        free.start = uint32_t(AlignUp(uint64_t(run) + kFreeSpaceMargin, kFreeSpaceMargin));
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
    std::map<uint32_t, CutSequence> sequences = ReadSequences(rom, info);
    Layout layout;
    layout.free = FreeSpaceOf(rom, LastRead(rom, info, sequences));

    // A sequence stays among the ROM's bytes when another reads any of its bytes, or when it reads the bank's header or
    // the resource table: the other reader would lose them when the sequence moved.
    const uint32_t table_end = info.resource_table + 4 * uint32_t(info.resource_count);
    const auto clear_of = [](uint32_t address, uint32_t size, uint32_t from, uint32_t to)
    {
        return address + size <= from || address >= to;
    };
    CutSequence* reader = nullptr;
    uint32_t read_to = 0;
    for (auto& [address, sequence] : sequences)
    {
        if (!clear_of(address, sequence.size, info.resource_table, table_end) ||
            !clear_of(address, sequence.size, info.bank, info.bank + kBankHeader))
        {
            sequence.usable = false;
        }

        if (reader && address < read_to)
        {
            sequence.usable = false;
            reader->usable = false;
        }

        if (!reader || address + sequence.size > read_to)
        {
            reader = &sequence;
            read_to = address + sequence.size;
        }
    }

    // The runs between the sequences.
    uint32_t at = kRomBase;
    const auto add_run = [&](uint32_t end)
    {
        if (end > at)
        {
            layout.pieces.emplace_back(std::vector<uint8_t>(rom.Ptr(at), rom.Ptr(at) + (end - at)));
            at = end;
        }
    };

    std::map<uint32_t, uint32_t> pieces_of;
    for (auto& [address, sequence] : sequences)
    {
        if (!sequence.usable)
        {
            continue;
        }

        add_run(address);
        pieces_of[address] = uint32_t(layout.pieces.size());
        layout.pieces.emplace_back(sar::SequencePiece{uint32_t(layout.sequences.size()), 0, sequence.size});
        layout.sequences.push_back(std::move(sequence.decoded.sequence));
        layout.resources.push_back(sequence.resource);
        at = address + sequence.size;
    }

    add_run(rom.End());

    // A place of the ROM's bytes in the runs.
    const auto place_of = [&layout](uint32_t address)
    {
        uint32_t start = kRomBase;
        for (uint32_t p = 0; p < layout.pieces.size(); p++)
        {
            const auto* bytes = std::get_if<std::vector<uint8_t>>(&layout.pieces[p]);
            const uint32_t size = bytes ? uint32_t(bytes->size()) : std::get<sar::SequencePiece>(layout.pieces[p]).room;
            if (address >= start && address < start + size)
            {
                return sar::Place{p, address - start};
            }

            start += size;
        }

        return sar::Place{uint32_t(layout.pieces.size()), 0};
    };

    const sar::Place bank = place_of(info.bank);
    for (int id = 0; id < info.resource_count; id++)
    {
        const auto piece = pieces_of.find(ResourceAddress(rom, info, id));
        if (piece != pieces_of.end())
        {
            layout.fields.push_back({place_of(info.resource_table + 4 * uint32_t(id)), bank, {piece->second, 0, true}});
        }
    }

    return layout;
}

std::vector<uint8_t> Join(std::span<const commands::Sequence> sequences, std::span<const sar::Piece> pieces,
                          std::span<const sar::Field> fields, const FreeSpace& free)
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
                throw std::invalid_argument("a piece names a sequence that the layout lacks");
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
    for (size_t p = 0; p < pieces.size(); p++)
    {
        if (const auto* bytes = std::get_if<std::vector<uint8_t>>(&pieces[p]))
        {
            std::copy(bytes->begin(), bytes->end(), image.begin() + std::ptrdiff_t(starts[p]));
        }
    }

    // Each sequence in its place, or later in the places of the sequences that move, or in the free space.
    std::vector<Encoded> encoded(pieces.size());
    std::vector<uint64_t> first_items(pieces.size());
    std::vector<size_t> moving;
    for (size_t p = 0; p < pieces.size(); p++)
    {
        const auto* sequence = std::get_if<sar::SequencePiece>(&pieces[p]);
        if (!sequence)
        {
            continue;
        }

        encoded[p] = EncodeSequence(sequences[sequence->sequence]);
        if (encoded[p].bytes.size() > sequence->room)
        {
            moving.push_back(p);
            continue;
        }

        std::copy(encoded[p].bytes.begin(), encoded[p].bytes.end(), image.begin() + std::ptrdiff_t(starts[p]));
        first_items[p] = starts[p];
    }

    // The places of the sequences that move, where neighbours count as one.
    std::vector<Span> places;
    for (const size_t p : moving)
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

    // The largest sequence first, then the earlier of two of a size.
    std::stable_sort(moving.begin(), moving.end(),
                     [&encoded](size_t a, size_t b) { return encoded[a].bytes.size() > encoded[b].bytes.size(); });

    uint64_t end_of_sequences = free.start;
    for (const size_t p : moving)
    {
        const uint64_t bytes = encoded[p].bytes.size();
        uint64_t place = 0;
        const auto room =
            std::find_if(places.begin(), places.end(), [bytes](const Span& s) { return s.size >= bytes; });
        if (room != places.end())
        {
            place = room->start;
            room->start += uint32_t(bytes);
            room->size -= uint32_t(bytes);
        }
        else
        {
            if (end_of_sequences + bytes > kLargestRom)
            {
                throw std::invalid_argument("the sequences that outgrow their places don't fit in a GBA ROM's 32 MiB");
            }

            place = end_of_sequences;
            end_of_sequences += bytes;
            if (place + bytes > image.size())
            {
                image.resize(place + bytes, free.padding);
            }
        }

        std::copy(encoded[p].bytes.begin(), encoded[p].bytes.end(), image.begin() + std::ptrdiff_t(place));
        first_items[p] = place;
    }

    const auto position = [&](const sar::Place& place) -> uint64_t
    {
        if (place.sequence || place.piece >= pieces.size())
        {
            throw std::invalid_argument("a field names a place that a GBA ROM's pieces lack");
        }

        if (place.item)
        {
            if (!std::holds_alternative<sar::SequencePiece>(pieces[place.piece]) || place.offset != 0)
            {
                throw std::invalid_argument("a field names an item other than a sequence's first");
            }

            return first_items[place.piece];
        }

        return starts[place.piece] + place.offset;
    };

    for (const sar::Field& field : fields)
    {
        const auto* run = field.at.piece < pieces.size() && !field.at.item && !field.at.sequence
                              ? std::get_if<std::vector<uint8_t>>(&pieces[field.at.piece])
                              : nullptr;
        if (!run || uint64_t(field.at.offset) + 4 > run->size())
        {
            throw std::invalid_argument("a field doesn't lie in a run of a GBA ROM");
        }

        xsflib::StoreLe32(&image[starts[field.at.piece] + field.at.offset],
                          uint32_t(position(field.to) - position(field.from)));
    }

    return image;
}

std::vector<uint8_t> Join(const Layout& layout)
{
    return Join(layout.sequences, layout.pieces, layout.fields, layout.free);
}

} // namespace ntdlib::ubimilan
