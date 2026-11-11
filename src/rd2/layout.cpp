// SPDX-License-Identifier: MIT

#include "ntdlib/rd2/layout.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <utility>
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

using agb::Reached;

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

// Marks the sequences that stay among the ROM's bytes, each once, in the order of the sequences' bytes. A sequence
// stays when another reads any of its bytes, or when it reads the driver's other data: the other reader would lose them
// when the sequence moved.
void MarkFixed(const DriverData& data, std::map<uint32_t, CutSequence>& sequences)
{
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
}

// Returns the place in the pieces of `layout` of an address in a run.
sar::Place RunPlace(const Layout& layout, uint32_t address)
{
    const std::optional<sar::Place> place = agb::RunPlaceOf(layout.pieces, address);
    if (!place)
    {
        throw std::invalid_argument("a table of the driver lies outside the ROM's runs");
    }

    return *place;
}

} // namespace

Layout Cut(const Rom& rom, const DriverInfo& info)
{
    std::map<uint32_t, CutSequence> sequences = ReadSequences(rom, info);
    const Reached reached = ReachedBytes(rom, info, sequences);
    const DriverData data = DriverDataOf(rom, info);

    Layout layout;
    layout.revision = info.revision;
    layout.free = agb::FreeSpaceOf(rom, &reached);
    layout.free.filler = agb::FillerRunsOf(rom, reached, kLeastFillerRun);

    // The sequences that can't move.
    MarkFixed(data, sequences);

    // The runs between the sequences.
    uint32_t at = kRomBase;
    std::map<uint32_t, uint32_t> pieces_of; // the piece of each sequence, by the sequence's address
    for (auto& [address, sequence] : sequences)
    {
        if (!sequence.usable)
        {
            continue;
        }

        agb::AddRomRun(rom, at, address, layout.pieces);
        pieces_of[address] = uint32_t(layout.pieces.size());
        layout.pieces.emplace_back(sar::SequencePiece{uint32_t(layout.sequences.size()), 0, sequence.size});
        layout.sequences.push_back(std::move(sequence.decoded.sequence));
        at = address + sequence.size;
    }
    agb::AddRomRun(rom, at, rom.End(), layout.pieces);

    // The fields of a table's entries lie in the run that holds the table: the sequences lie clear of it.
    const auto add_fields = [&](uint32_t table, std::span<const uint32_t> addresses)
    {
        for (size_t s = 0; s < addresses.size(); s++)
        {
            const auto piece = pieces_of.find(addresses[s]);
            if (piece != pieces_of.end())
            {
                layout.fields.push_back(
                    {RunPlace(layout, table + 4 * uint32_t(s)), RunPlace(layout, table), {piece->second, 0, true}});
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
    const auto encode = [&](std::size_t, uint32_t sequence)
    {
        return EncodeSequence(sequences[sequence], revision);
    };
    agb::JoinedRom rom(pieces, sequences.size(), fields, free,
                       {.alignment = kSequenceAlignment, .after_tables = true, .filler = options.filler}, encode);
    for (const sar::Field& field : fields)
    {
        uint8_t* at = rom.FieldBytes(field, 4);
        xsflib::StoreLe32(at, uint32_t(rom.Position(field.to) - rom.Position(field.from)));
    }

    return rom.TakeImage();
}

std::vector<uint8_t> Join(const Layout& layout, const JoinOptions& options)
{
    return Join(layout.revision, layout.sequences, layout.pieces, layout.fields, layout.free, options);
}

std::vector<Span> FillerRuns(const Rom& rom, const DriverInfo& info)
{
    return agb::FillerRunsOf(rom, ReachedBytes(rom, info, ReadSequences(rom, info)), kLeastFillerRun);
}

} // namespace ntdlib::rd2
