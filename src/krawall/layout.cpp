// SPDX-License-Identifier: MIT

#include "ntdlib/krawall/layout.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/commands/units.h"
#include "ntdlib/krawall/commands.h"
#include "ntdlib/krawall/instruments.h"

namespace ntdlib::krawall
{
namespace
{

using agb::kRomBase;

// The bytes of the player's tables: four waves of 64 halfwords, the periods of 120 notes, the factors of 64 fine tunes
// and the frequencies of 768 steps of an octave.
constexpr uint32_t kWaveBytes = 64 * 2;
constexpr uint32_t kPeriodBytes = 120 * 2;
constexpr uint32_t kFineTuneBytes = 64 * 2;
constexpr uint32_t kLinearBytes = 768 * 4;

// An entry of the game's table of what it plays.
constexpr uint32_t kPlayEntryBytes = 8;

// A sequence's place on a multiple of this, where it moves.
constexpr uint32_t kSequenceAlignment = 4;

uint64_t AlignUp(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

// A sequence that the cut looks at, what the decoder made of it, and the module whose patterns it holds.
struct CutSequence
{
    Decoded decoded;
    uint32_t size = 0;   // the bytes that its items encode to, or the bytes of its data where they don't encode
    bool usable = false; // the decoder read it, and its encoding gives its bytes back
    uint32_t module = 0; // the module's header
};

// Reads each module's sequence, by its address.
std::map<uint32_t, CutSequence> ReadSequences(const Rom& rom, const DriverInfo& info)
{
    const Traits traits = TraitsOf(info);
    std::map<uint32_t, CutSequence> sequences;
    for (int s = 0; s < SequenceCount(info); s++)
    {
        const uint32_t address = SequenceAddress(rom, info, s);
        if (!address)
        {
            continue;
        }

        CutSequence sequence;
        sequence.module = info.modules[size_t(s)];
        try
        {
            sequence.decoded = DecodeRomSequence(rom, info, s);
        }
        catch (const std::invalid_argument&)
        {
            // The module's patterns don't read. They stay among the ROM's bytes.
            continue;
        }

        // Two modules whose patterns start at one place both stay among the ROM's bytes. A module that the game lists
        // twice is one sequence.
        const auto found = sequences.find(address);
        if (found != sequences.end())
        {
            found->second.usable = found->second.usable && found->second.module == sequence.module;
            continue;
        }

        for (const commands::Command& item : sequence.decoded.sequence.items)
        {
            if (item.kind == commands::Kind::kData)
            {
                sequence.size += uint32_t(item.bytes.size());
            }
        }
        try
        {
            const Encoded encoded = EncodeSequence(sequence.decoded.sequence, sequence.decoded.entries, traits);
            sequence.size = uint32_t(encoded.bytes.size());
            sequence.usable = !sequence.decoded.opaque && rom.Contains(address, sequence.size) &&
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

// The spans of the ROM that hold the player's data other than the sequences, from 0x08000000.
std::vector<Span> DriverDataOf(const Rom& rom, const DriverInfo& info, const std::map<uint32_t, CutSequence>& sequences)
{
    std::vector<Span> spans;
    const auto add = [&spans](uint32_t address, uint64_t size)
    {
        if (address && size)
        {
            spans.push_back({address, uint32_t(std::min<uint64_t>(size, 0xFFFFFFFF))});
        }
    };

    // A sample of the game's table, with its entry.
    const Rom& source = info.unpacked ? *info.unpacked : rom;
    std::set<int> samples;
    const auto add_sample = [&](int number)
    {
        if (number < 0 || !samples.insert(number).second)
        {
            return;
        }

        add(info.samples + 4 * uint32_t(number), 4);
        if (const std::optional<Sample> sample = ReadSample(rom, info, number))
        {
            add(sample->header, uint64_t(sample->end - sample->header));
        }
    };

    add(info.module_table, 4 * uint64_t(info.table_count));
    if (info.play_table)
    {
        add(info.play_table, kPlayEntryBytes * uint64_t(info.table_count));
    }
    for (const uint32_t address : info.modules)
    {
        ModuleInfo module;
        if (ReadModule(source, info.build, address, module))
        {
            add(address, ModulePatterns(info.build) + 4 * uint64_t(module.patterns.size()));
        }
    }

    // The instruments and samples that each module's rows name.
    std::set<std::pair<uint32_t, int>> instruments;
    for (const auto& [address, sequence] : sequences)
    {
        ModuleInfo module;
        if (!ReadModule(source, info.build, sequence.module, module))
        {
            continue;
        }

        for (const commands::Command& item : sequence.decoded.sequence.items)
        {
            int instrument = 0;
            if (item.kind == commands::Kind::kProgram)
            {
                instrument = int(commands::FromUnit(item.value.value, {}));
            }
            else if (item.kind == commands::Kind::kNote || item.kind == commands::Kind::kNoteOff)
            {
                instrument = item.sample.value_or(0);
            }
            if (instrument <= 0 || !instruments.emplace(sequence.module, instrument).second)
            {
                continue;
            }

            const std::optional<Instrument> found = ReadInstrument(rom, info, module, uint16_t(instrument));
            if (!found)
            {
                continue;
            }
            if (found->address)
            {
                if (!module.instruments_at)
                {
                    add(info.instruments + 4 * uint32_t(instrument - 1), 4);
                }
                add(found->address, kInstrumentSize);
            }
            for (const Region& region : found->regions)
            {
                add_sample(region.sample);
            }
        }
    }

    add(info.sine, kWaveBytes);
    add(info.ramp, kWaveBytes);
    add(info.square, kWaveBytes);
    add(info.random, kWaveBytes);
    add(info.periods, kPeriodBytes);
    add(info.fine_tunes, kFineTuneBytes);
    add(info.linear, kLinearBytes);

    return spans;
}

// Whether `size` bytes from `address` meet one of `spans`.
bool Touches(std::span<const Span> spans, uint32_t address, uint32_t size)
{
    for (const Span& span : spans)
    {
        if (address < uint64_t(span.start) + span.size && span.start < uint64_t(address) + size)
        {
            return true;
        }
    }

    return false;
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

    // The offset just past the last byte that the driver reads past the ROM's end and before kLargestRom, or 0.
    uint64_t Beyond() const
    {
        return beyond_;
    }

    // Marks `size` bytes from `address`, where they lie in the ROM, and notes those past its end.
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
        if (from + size > bytes_.size() && from < kLargestRom)
        {
            beyond_ = std::max(beyond_, std::min<uint64_t>(from + size, kLargestRom));
        }
    }

private:
    std::vector<bool> bytes_;
    uint64_t beyond_ = 0;
};

// The bytes that the driver reads, with `sequences`.
Reached ReachedBytes(const Rom& rom, const DriverInfo& info, const std::map<uint32_t, CutSequence>& sequences)
{
    Reached reached(rom);
    for (const Span& span : DriverDataOf(rom, info, sequences))
    {
        reached.Mark(span.start, span.size);
    }
    for (const auto& [address, sequence] : sequences)
    {
        reached.Mark(address, sequence.size);
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
        free.start = uint32_t(AlignUp(std::max<uint64_t>(run, reached.Beyond()) + kFreeSpaceMargin, kFreeSpaceMargin));
        free.padding = last;
    }
    else
    {
        free.start = uint32_t(AlignUp(std::max<uint64_t>(size, reached.Beyond()), kSequenceAlignment));
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
    const std::vector<Span> data = DriverDataOf(rom, info, sequences);

    Layout layout;
    layout.traits = TraitsOf(info);
    layout.free = FreeSpaceOf(rom, reached);
    layout.free.filler = FillerRunsOf(rom, reached);

    // Each sequence once, in the order of the sequences' bytes. A sequence stays among the ROM's bytes when another
    // reads any of its bytes, or when it holds the player's other data: the other reader would lose them when the
    // sequence moved. The ROM's first bytes stay in a run, which the fields count from.
    auto reader = sequences.end(); // the sequence that reads furthest
    uint32_t read_to = 0;          // where that sequence's bytes end
    for (auto it = sequences.begin(); it != sequences.end(); ++it)
    {
        const uint32_t end = it->first + it->second.size;
        if (Touches(data, it->first, it->second.size) || it->first == kRomBase)
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
        layout.sequences.push_back(sequence.decoded.sequence);
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
        throw std::invalid_argument("a table of patterns lies outside the ROM's runs");
    };

    // The fields of each module's table of patterns, in the order of the modules.
    std::set<uint32_t> modules;
    for (const uint32_t module : info.modules)
    {
        if (!modules.insert(module).second)
        {
            continue;
        }

        for (const auto& [address, sequence] : sequences)
        {
            const auto piece = pieces_of.find(address);
            if (sequence.module != module || piece == pieces_of.end())
            {
                continue;
            }

            const uint32_t table = module + ModulePatterns(info.build);
            for (size_t p = 0; p < sequence.decoded.entries.size(); p++)
            {
                const sar::Place to = {piece->second, sequence.decoded.entries[p], true};
                layout.fields.push_back({run_place(table + 4 * uint32_t(p)), {}, to});
            }
        }
    }

    return layout;
}

std::vector<uint8_t> Join(const Traits& traits, std::span<const commands::Sequence> sequences,
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
    if (!pieces.empty() && !std::holds_alternative<std::vector<uint8_t>>(pieces.front()))
    {
        throw std::invalid_argument("a GBA ROM's layout starts with a sequence");
    }

    std::vector<uint8_t> image(size);
    for (size_t p = 0; p < pieces.size(); p++)
    {
        if (const auto* bytes = std::get_if<std::vector<uint8_t>>(&pieces[p]))
        {
            std::copy(bytes->begin(), bytes->end(), image.begin() + std::ptrdiff_t(starts[p]));
        }
    }

    // The items where the patterns of each sequence piece start: the fields name them.
    std::vector<std::vector<uint32_t>> entries(pieces.size());
    for (const sar::Field& field : fields)
    {
        if (field.to.item && field.to.piece < pieces.size() && !field.to.sequence)
        {
            entries[field.to.piece].push_back(field.to.offset);
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

        std::sort(entries[p].begin(), entries[p].end());
        entries[p].erase(std::unique(entries[p].begin(), entries[p].end()), entries[p].end());
        encoded[p] = EncodeSequence(sequences[sequence->sequence], entries[p], traits);
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
    const auto take_from = [](std::vector<Span>& spans, uint64_t bytes, uint64_t& at)
    {
        for (Span& span : spans)
        {
            const uint64_t place = AlignUp(span.start, kSequenceAlignment);
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
        if (!take_from(places, bytes, at))
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
            else if (!take_from(filler, bytes, at))
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
                          uint32_t(position(field.to) - position(field.from) + kRomBase));
    }

    return image;
}

std::vector<uint8_t> Join(const Layout& layout, const JoinOptions& options)
{
    return Join(layout.traits, layout.sequences, layout.pieces, layout.fields, layout.free, options);
}

std::vector<Span> FillerRuns(const Rom& rom, const DriverInfo& info)
{
    return FillerRunsOf(rom, ReachedBytes(rom, info, ReadSequences(rom, info)));
}

} // namespace ntdlib::krawall
