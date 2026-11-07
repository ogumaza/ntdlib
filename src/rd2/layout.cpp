// SPDX-License-Identifier: MIT

#include "ntdlib/rd2/layout.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/rd2/commands.h"
#include "ntdlib/rd2/sequencer.h"

namespace ntdlib::rd2
{
namespace
{

using agb::kRomBase;

// The bytes of each of the driver's tables. A note's index reaches one entry past the pitch table and the frequency
// table. E2 can give a bend a range that reads further past the pitch table. That counts only as far as a note's index:
// a game's tables can lie just before a sequence of sound effects.
constexpr uint32_t kPitchTableBytes = (kLastIndex + 2) * 4;
constexpr uint32_t kFrequencyTableBytes = (kLastIndex + 2) * 2;
constexpr uint32_t kNoiseTableBytes = kLastIndex + 1;
constexpr uint32_t kLfoTableBytes = 256;
constexpr uint32_t kWaveVolumeBytes = 5;
constexpr uint32_t kVoiceClassBytes = 16;

// The bytes of a bank's list of banks, by the C7 command's byte.
constexpr uint32_t kBankListBytes = 256 * 2;

// The bytes that a bank's offsets reach.
constexpr uint32_t kBankBytes = 0x10000;

// A sequence's place on a multiple of this.
constexpr uint32_t kSequenceAlignment = 4;

uint64_t AlignUp(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

// A table of offsets from its start: the number of entries, from where its first entry points.
uint32_t TableEntries(const Rom& rom, uint32_t table)
{
    if (!table || !rom.Contains(table, 4))
    {
        return 0;
    }

    const uint32_t first = rom.U32(table);
    return first % 4 == 0 && rom.Contains(table, first) ? first / 4 : 0;
}

// A sequence that the cut looks at, and what the decoder made of it.
struct CutSequence
{
    Decoded decoded;
    uint32_t size = 0;   // the bytes that its items encode to, or the bytes of its data where they don't encode
    bool usable = false; // the decoder read it, and its encoding gives its bytes back
};

// Reads each sequence that the tables name, by its address.
std::map<uint32_t, CutSequence> ReadSequences(const Rom& rom, const DriverInfo& info)
{
    std::map<uint32_t, CutSequence> sequences;
    std::map<uint32_t, bool> kinds; // whether each address holds a sound effects' sequence
    for (int s = 0; s < SequenceCount(info); s++)
    {
        const uint32_t address = SequenceAddress(info, s);
        const bool effects = IsEffectSequence(info, s);
        const auto kind = kinds.find(address);
        if (kind != kinds.end())
        {
            // Two tables that read one place by other rules: neither can move.
            if (kind->second != effects)
            {
                sequences.at(address).usable = false;
            }
            continue;
        }

        kinds[address] = effects;
        CutSequence sequence;
        sequence.decoded = DecodeRomSequence(rom, info, s);
        for (const commands::Command& item : sequence.decoded.sequence.items)
        {
            if (item.kind == commands::Kind::kData)
            {
                sequence.size += uint32_t(item.bytes.size());
            }
        }
        try
        {
            const Encoded encoded = EncodeSequence(sequence.decoded.sequence, info.revision);
            sequence.size = uint32_t(encoded.bytes.size());
            sequence.usable = !sequence.decoded.opaque && address % kSequenceAlignment == 0 &&
                              rom.Contains(address, sequence.size) &&
                              std::equal(encoded.bytes.begin(), encoded.bytes.end(), rom.Ptr(address));
        }
        catch (const std::invalid_argument&)
        {
            // The sequence stays among the ROM's bytes, up to the end of its data.
        }
        sequences.emplace(address, std::move(sequence));
    }

    return sequences;
}

// The places in a ROM where the driver's data other than the sequences starts, and the tables' spans.
struct DriverData
{
    std::set<uint32_t> starts;
    std::vector<Span> tables; // from 0x08000000

    // Whether `size` bytes from `address` hold such a start, or lie in a table.
    bool Touches(uint32_t address, uint32_t size) const
    {
        const auto found = starts.lower_bound(address);
        if (found != starts.end() && *found - address < size)
        {
            return true;
        }

        for (const Span& table : tables)
        {
            if (address < table.start + table.size && table.start < address + size)
            {
                return true;
            }
        }

        return false;
    }
};

// The addresses that the entries of the table of offsets at `table` name.
std::set<uint32_t> EntriesOf(const Rom& rom, uint32_t table)
{
    std::set<uint32_t> entries;
    const uint32_t count = TableEntries(rom, table);
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t address = table + rom.U32(table + 4 * i);
        if (rom.Contains(address))
        {
            entries.insert(address);
        }
    }

    return entries;
}

DriverData DriverDataOf(const Rom& rom, const DriverInfo& info)
{
    DriverData data;
    const auto table = [&](uint32_t address, uint32_t size)
    {
        if (address && size)
        {
            data.tables.push_back({address, size});
        }
    };
    const auto offsets = [&](uint32_t address)
    {
        table(address, 4 * TableEntries(rom, address));
    };

    table(info.settings, kSettingsSize);
    table(info.sequences, 4 * uint32_t(info.sequence_addresses.size()));
    table(info.effects, 4 * uint32_t(info.effect_sequence_addresses.size()));
    table(info.bank_lists, 4 * uint32_t(info.bank_list_addresses.size()));
    table(info.effect_bank_lists, 4 * uint32_t(info.effect_bank_list_addresses.size()));
    offsets(info.banks);
    offsets(info.sample_sets);
    table(info.pitch_table, kPitchTableBytes);
    table(info.frequency_table, kFrequencyTableBytes);
    table(info.noise_table, kNoiseTableBytes);
    table(info.lfo_table, kLfoTableBytes);
    table(info.wave_volumes, kWaveVolumeBytes);
    table(info.voice_classes, kVoiceClassBytes);
    data.starts.insert(info.bank_sample_sets);
    data.starts.insert(info.key_envelope);
    for (const uint32_t list : info.bank_list_addresses)
    {
        data.starts.insert(list);
    }
    for (const uint32_t list : info.effect_bank_list_addresses)
    {
        data.starts.insert(list);
    }
    data.starts.merge(EntriesOf(rom, info.banks));
    for (const uint32_t set : EntriesOf(rom, info.sample_sets))
    {
        data.starts.insert(set);
        data.starts.merge(EntriesOf(rom, set));
    }

    return data;
}

// The bytes of a ROM that the driver reads.
class Reached
{
public:
    explicit Reached(const Rom& rom) : bytes_(rom.Size(), false)
    {
    }

    bool Holds(uint32_t offset) const
    {
        return bytes_[offset];
    }

    // Marks `size` bytes from `address`, where they lie in the ROM.
    void Mark(uint32_t address, uint64_t size)
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
    }

private:
    std::vector<bool> bytes_;
};

// The bytes that the driver reads, with `sequences`.
Reached ReachedBytes(const Rom& rom, const DriverInfo& info, const std::map<uint32_t, CutSequence>& sequences)
{
    Reached reached(rom);
    const DriverData data = DriverDataOf(rom, info);
    for (const Span& table : data.tables)
    {
        reached.Mark(table.start, table.size);
    }
    for (const auto& [address, sequence] : sequences)
    {
        reached.Mark(address, sequence.size);
    }
    for (const uint32_t list : info.bank_list_addresses)
    {
        reached.Mark(list, kBankListBytes);
    }
    for (const uint32_t list : info.effect_bank_list_addresses)
    {
        reached.Mark(list, kBankListBytes);
    }
    for (const uint32_t bank : EntriesOf(rom, info.banks))
    {
        reached.Mark(bank, kBankBytes);
    }
    for (const uint32_t set : EntriesOf(rom, info.sample_sets))
    {
        reached.Mark(set, 4 * uint64_t(TableEntries(rom, set)));
        for (const uint32_t sample : EntriesOf(rom, set))
        {
            // A header of 4 words holds the sample's length and its loop's end, in points.
            reached.Mark(sample, 16 + uint64_t(std::max(rom.U32(sample), rom.U32(sample + 12))));
        }
    }

    return reached;
}

// The run of `byte` at the ROM's end starts here, or at the ROM's end if there's none.
uint32_t EndRunOf(const Rom& rom, uint8_t byte)
{
    auto run = uint32_t(rom.Size());
    while (run > 0 && rom.U8(kRomBase + run - 1) == byte)
    {
        run--;
    }

    return run;
}

// Where the free space of `rom` starts.
FreeSpace FreeSpaceOf(const Rom& rom, const Reached& reached)
{
    FreeSpace free;
    const auto size = uint32_t(rom.Size());
    const uint8_t last = size ? rom.U8(kRomBase + size - 1) : 0;
    if (size && (last == 0xFF || last == 0))
    {
        uint32_t run = EndRunOf(rom, last);
        for (uint32_t i = size; i > run; i--)
        {
            if (reached.Holds(i - 1))
            {
                run = i;
                break;
            }
        }
        free.start = uint32_t(AlignUp(uint64_t(run) + kFreeSpaceMargin, kFreeSpaceMargin));
        free.padding = last;
    }
    else
    {
        free.start = uint32_t(AlignUp(size, kSequenceAlignment));
    }

    return free;
}

std::vector<Span> FillerRunsOf(const Rom& rom, const Reached& reached)
{
    std::vector<Span> full;
    std::vector<Span> zeros;
    const auto size = uint32_t(rom.Size());
    const uint8_t* bytes = rom.Ptr(kRomBase);
    uint32_t at = 0;
    while (at < size)
    {
        const uint8_t byte = bytes[at];
        uint32_t end = at + 1;
        if (byte != 0xFF && byte != 0)
        {
            at = end;
            continue;
        }
        while (end < size && bytes[end] == byte)
        {
            end++;
        }

        const uint64_t start = AlignUp(uint64_t(at) + kFreeSpaceMargin, kFreeSpaceMargin);
        if (end - at >= kLeastFillerRun && end < size && start + kFreeSpaceMargin < end)
        {
            bool read = false;
            for (uint32_t i = at; i < end && !read; i++)
            {
                read = reached.Holds(i);
            }
            if (!read)
            {
                (byte == 0xFF ? full : zeros).push_back({uint32_t(start), end - kFreeSpaceMargin - uint32_t(start)});
            }
        }
        at = end;
    }

    full.insert(full.end(), zeros.begin(), zeros.end());

    return full;
}

} // namespace

Layout Cut(const Rom& rom, const DriverInfo& info)
{
    std::map<uint32_t, CutSequence> sequences = ReadSequences(rom, info);
    const Reached reached = ReachedBytes(rom, info, sequences);
    const DriverData data = DriverDataOf(rom, info);

    Layout layout;
    layout.revision = info.revision;
    layout.free = FreeSpaceOf(rom, reached);
    layout.free.filler = FillerRunsOf(rom, reached);

    // Each sequence once, in the order of the sequences' bytes. A sequence stays among the ROM's bytes when another
    // reads any of its bytes, or when it reads the driver's other data: the other reader would lose them when the
    // sequence moved.
    auto reader = sequences.end(); // the sequence that reads furthest
    uint32_t read_to = 0;          // where that sequence's bytes end
    for (auto it = sequences.begin(); it != sequences.end(); ++it)
    {
        const uint32_t end = it->first + it->second.size;
        if (data.Touches(it->first, it->second.size))
        {
            it->second.usable = false;
        }
        if (reader != sequences.end() && it->first < read_to)
        {
            it->second.usable = false;
            reader->second.usable = false;
        }
        if (reader == sequences.end() || end > read_to)
        {
            reader = it;
            read_to = end;
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
    std::map<uint32_t, uint32_t> pieces_of; // the piece of each sequence, by the sequence's address
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
        at = address + sequence.size;
    }
    add_run(rom.End());

    // The place in the pieces of an address in a run.
    const auto run_place = [&layout](uint32_t address) -> sar::Place
    {
        uint32_t start = kRomBase;
        for (uint32_t p = 0; p < layout.pieces.size(); p++)
        {
            const auto* bytes = std::get_if<std::vector<uint8_t>>(&layout.pieces[p]);
            const uint32_t size = bytes ? uint32_t(bytes->size()) : std::get<sar::SequencePiece>(layout.pieces[p]).room;
            if (bytes && address >= start && address - start < size)
            {
                return {p, address - start};
            }
            start += size;
        }
        throw std::invalid_argument("a table of the driver lies outside the ROM's runs");
    };

    // The fields of a table's entries lie in the run that holds the table: the sequences lie clear of it.
    const auto add_fields = [&](uint32_t table, std::span<const uint32_t> addresses)
    {
        for (size_t s = 0; s < addresses.size(); s++)
        {
            const auto piece = pieces_of.find(addresses[s]);
            if (piece != pieces_of.end())
            {
                layout.fields.push_back(
                    {run_place(table + 4 * uint32_t(s)), run_place(table), {piece->second, 0, true}});
            }
        }
    };
    add_fields(info.sequences, info.sequence_addresses);
    add_fields(info.effects, info.effect_sequence_addresses);

    return layout;
}

std::vector<uint8_t> Join(Revision revision, std::span<const commands::Sequence> sequences,
                          std::span<const sar::Piece> pieces, std::span<const sar::Field> fields, const FreeSpace& free,
                          const JoinOptions& options)
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
                throw std::invalid_argument("a sequence piece names a sequence that the layout lacks");
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

    // A sequence that moves stays after the tables that name it: a table's offsets are positive.
    std::vector<uint64_t> floors(pieces.size(), 0);
    for (const sar::Field& field : fields)
    {
        if (field.to.piece < pieces.size() && field.from.piece < pieces.size() && !field.from.item)
        {
            floors[field.to.piece] = std::max(floors[field.to.piece], starts[field.from.piece] + field.from.offset);
        }
    }

    // Each sequence in its place, or later in the places of the sequences that move, or in the free space.
    std::vector<Encoded> encoded(pieces.size());
    std::vector<std::vector<uint64_t>> items(pieces.size());
    std::vector<size_t> moving;
    for (size_t p = 0; p < pieces.size(); p++)
    {
        const auto* sequence = std::get_if<sar::SequencePiece>(&pieces[p]);
        if (!sequence)
        {
            continue;
        }

        encoded[p] = EncodeSequence(sequences[sequence->sequence], revision);
        if (encoded[p].bytes.size() > sequence->room)
        {
            moving.push_back(p);
            continue;
        }

        std::copy(encoded[p].bytes.begin(), encoded[p].bytes.end(), image.begin() + std::ptrdiff_t(starts[p]));
        for (const uint32_t offset : encoded[p].offsets)
        {
            items[p].push_back(starts[p] + offset);
        }
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

    std::vector<Span> filler = free.filler;
    uint64_t end_of_sequences = free.start;
    const auto take_from = [](std::vector<Span>& spans, uint64_t bytes, uint64_t floor, uint64_t& at)
    {
        for (Span& span : spans)
        {
            const uint64_t place = AlignUp(std::max<uint64_t>(span.start, floor), kSequenceAlignment);
            if (place + bytes <= uint64_t(span.start) + span.size)
            {
                at = place;
                span.size -= uint32_t(place + bytes - span.start);
                span.start = uint32_t(place + bytes);
                return true;
            }
        }
        return false;
    };
    for (const size_t p : moving)
    {
        const uint64_t bytes = encoded[p].bytes.size();
        uint64_t at = 0;
        if (!take_from(places, bytes, floors[p], at))
        {
            const uint64_t place = AlignUp(end_of_sequences, kSequenceAlignment);
            if (place + bytes <= kLargestRom)
            {
                at = place;
                end_of_sequences = place + bytes;
                if (at + bytes > image.size())
                {
                    image.resize(at + bytes, free.padding);
                }
            }
            else if (!options.filler)
            {
                throw std::invalid_argument("the sequences that outgrow their places don't fit in a GBA ROM's 32 MiB");
            }
            else if (!take_from(filler, bytes, floors[p], at))
            {
                throw std::invalid_argument(
                    "the sequences that outgrow their places don't fit in a GBA ROM's 32 MiB or in its filler runs");
            }
        }

        std::copy(encoded[p].bytes.begin(), encoded[p].bytes.end(), image.begin() + std::ptrdiff_t(at));
        for (const uint32_t offset : encoded[p].offsets)
        {
            items[p].push_back(at + offset);
        }
    }

    const auto position = [&](const sar::Place& place) -> uint64_t
    {
        if (place.sequence || place.piece >= pieces.size())
        {
            throw std::invalid_argument("a field names a place that a GBA ROM's pieces lack");
        }
        if (place.item)
        {
            if (!std::holds_alternative<sar::SequencePiece>(pieces[place.piece]) ||
                place.offset >= items[place.piece].size())
            {
                throw std::invalid_argument("a field names an item that a sequence lacks");
            }
            return items[place.piece][place.offset];
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

std::vector<uint8_t> Join(const Layout& layout, const JoinOptions& options)
{
    return Join(layout.revision, layout.sequences, layout.pieces, layout.fields, layout.free, options);
}

std::vector<Span> FillerRuns(const Rom& rom, const DriverInfo& info)
{
    return FillerRunsOf(rom, ReachedBytes(rom, info, ReadSequences(rom, info)));
}

} // namespace ntdlib::rd2
