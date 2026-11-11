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

using agb::Reached;

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

// The spans of the ROM that hold the player's data, collected one piece at a time. A sample comes with its entry of
// the game's table, once.
class DataSpans
{
public:
    DataSpans(const Rom& rom, const DriverInfo& info) : rom_(rom), info_(info)
    {
    }

    void Add(uint32_t address, uint64_t size)
    {
        if (address && size)
        {
            spans_.push_back({address, uint32_t(std::min<uint64_t>(size, 0xFFFFFFFF))});
        }
    }

    void AddSample(int number)
    {
        if (number < 0 || !samples_.insert(number).second)
        {
            return;
        }

        Add(info_.samples + 4 * uint32_t(number), 4);
        if (const std::optional<Sample> sample = ReadSample(rom_, info_, number))
        {
            Add(sample->header, uint64_t(sample->end - sample->header));
        }
    }

    std::vector<Span> Take()
    {
        return std::move(spans_);
    }

private:
    const Rom& rom_;
    const DriverInfo& info_;
    std::vector<Span> spans_;
    std::set<int> samples_;
};

// Adds the instruments and samples that each module's rows name.
void AddNamedInstruments(const Rom& rom, const Rom& source, const DriverInfo& info,
                         const std::map<uint32_t, CutSequence>& sequences, DataSpans& spans)
{
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
                    spans.Add(info.instruments + 4 * uint32_t(instrument - 1), 4);
                }
                spans.Add(found->address, kInstrumentSize);
            }
            for (const Region& region : found->regions)
            {
                spans.AddSample(region.sample);
            }
        }
    }
}

// What a channel of a module plays: the entries of the instruments' maps that its notes and note offs with an
// instrument read, and its notes without an instrument.
struct ChannelNotes
{
    std::map<int, std::set<int>> entries;
    std::set<int> notes;
};

// What each channel of each module plays, by the module's address and the channel's number.
std::map<uint32_t, std::map<int, ChannelNotes>> NotesOf(const std::map<uint32_t, CutSequence>& sequences)
{
    std::map<uint32_t, std::map<int, ChannelNotes>> channels;
    for (const auto& [address, sequence] : sequences)
    {
        for (const commands::Command& item : sequence.decoded.sequence.items)
        {
            ChannelNotes& channel = channels[sequence.module][item.index];
            const int instrument = item.sample.value_or(0);
            if (item.kind == commands::Kind::kNote && instrument > 0)
            {
                channel.entries[instrument].insert(item.key - 1);
            }
            else if (item.kind == commands::Kind::kNote)
            {
                channel.notes.insert(item.key);
            }
            else if (item.kind == commands::Kind::kNoteOff && instrument > 0)
            {
                channel.entries[instrument].insert(kNoteOff - 1);
            }
        }
    }

    return channels;
}

// Adds the entries of the map of `instrument` at `address` that `channel` reads, and the samples they pick. A note
// without an instrument looks its sample up again in the channel's instrument. The entry is the note less 1 plus the
// relative note of the channel's sample, modulo 256. Past the map's 96 notes, the player reads the instrument's other
// bytes, or the bytes after it, as a sample's number. A note off with an instrument reads entry 126: its note of 127,
// less 1. Each sample that an entry picks becomes the channel's sample. Its relative note moves the channel's next
// notes.
void AddMapEntries(const Rom& source, const DriverInfo& info, uint32_t address, const ChannelNotes& channel,
                   const std::set<int>& entries, DataSpans& spans)
{
    std::set<int> reached;
    std::set<int> looked;
    std::vector<int> pending;
    const auto read = [&](int entry)
    {
        if (!reached.insert(entry).second)
        {
            return;
        }

        const uint32_t at = address + 2 * uint32_t(entry);
        if (entry >= kInstrumentNotes)
        {
            spans.Add(at, 2);
            spans.AddSample(source.U16(at));
        }
        pending.push_back(source.U16(at));
    };
    for (const int entry : entries)
    {
        read(entry);
    }

    while (!pending.empty())
    {
        const int sample = pending.back();
        pending.pop_back();
        const uint32_t header = source.U32(info.samples + 4 * uint32_t(sample));
        if (!header || !looked.insert(sample).second)
        {
            continue;
        }

        const auto relative = int8_t(source.U8(header + kSampleRelativeNote));
        for (const int note : channel.notes)
        {
            read(uint8_t(note - 1 + relative));
        }
    }
}

// The spans of the ROM that hold the player's data other than the sequences, from 0x08000000.
std::vector<Span> DriverDataOf(const Rom& rom, const DriverInfo& info, const std::map<uint32_t, CutSequence>& sequences)
{
    DataSpans spans(rom, info);
    const Rom& source = info.unpacked ? *info.unpacked : rom;
    spans.Add(info.module_table, 4 * uint64_t(info.table_count));
    if (info.play_table)
    {
        spans.Add(info.play_table, kPlayEntryBytes * uint64_t(info.table_count));
    }
    for (const uint32_t address : info.modules)
    {
        ModuleInfo module;
        if (ReadModule(source, info.build, address, module))
        {
            spans.Add(address, ModulePatterns(info.build) + 4 * uint64_t(module.patterns.size()));
        }
    }

    AddNamedInstruments(rom, source, info, sequences, spans);
    for (const auto& [address, of_module] : NotesOf(sequences))
    {
        ModuleInfo module;
        if (!ReadModule(source, info.build, address, module))
        {
            continue;
        }

        for (const auto& [index, channel] : of_module)
        {
            for (const auto& [instrument, entries] : channel.entries)
            {
                const std::optional<Instrument> found = ReadInstrument(rom, info, module, uint16_t(instrument));
                if (found && found->address)
                {
                    AddMapEntries(source, info, found->address, channel, entries, spans);
                }
            }
        }
    }

    spans.Add(info.sine, kWaveBytes);
    spans.Add(info.ramp, kWaveBytes);
    spans.Add(info.square, kWaveBytes);
    spans.Add(info.random, kWaveBytes);
    spans.Add(info.periods, kPeriodBytes);
    spans.Add(info.fine_tunes, kFineTuneBytes);
    spans.Add(info.linear, kLinearBytes);

    return spans.Take();
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

// Marks the sequences that stay among the ROM's bytes, in the order of the sequences' bytes. A sequence stays when
// another reads any of its bytes, or when it holds the player's other data (`data`): the other reader would lose them
// when the sequence moved. The ROM's first bytes stay in a run, which the fields count from.
void MarkUnmovable(std::map<uint32_t, CutSequence>& sequences, std::span<const Span> data)
{
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
}

// The place in the pieces of `layout` of an address in a run.
sar::Place RunPlace(const Layout& layout, uint32_t address)
{
    const std::optional<sar::Place> place = agb::RunPlaceOf(layout.pieces, address);
    if (!place)
    {
        throw std::invalid_argument("a table of patterns lies outside the ROM's runs");
    }

    return *place;
}

// Adds the fields of each module's table of patterns, in the order of the modules. `pieces_of` gives the piece of each
// sequence that moves, by the sequence's address.
void AddPatternFields(const DriverInfo& info, const std::map<uint32_t, CutSequence>& sequences,
                      const std::map<uint32_t, uint32_t>& pieces_of, Layout& layout)
{
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
                layout.fields.push_back({RunPlace(layout, table + 4 * uint32_t(p)), {}, to});
            }
        }
    }
}

} // namespace

Layout Cut(const Rom& rom, const DriverInfo& info)
{
    std::map<uint32_t, CutSequence> sequences = ReadSequences(rom, info);
    const Reached reached = ReachedBytes(rom, info, sequences);
    const std::vector<Span> data = DriverDataOf(rom, info, sequences);

    Layout layout;
    layout.traits = TraitsOf(info);
    layout.free = agb::FreeSpaceOf(rom, &reached, reached.Beyond());
    layout.free.filler = agb::FillerRunsOf(rom, reached, kLeastFillerRun);

    MarkUnmovable(sequences, data);

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
        layout.sequences.push_back(sequence.decoded.sequence);
        at = address + sequence.size;
    }
    agb::AddRomRun(rom, at, rom.End(), layout.pieces);

    AddPatternFields(info, sequences, pieces_of, layout);

    return layout;
}

std::vector<uint8_t> Join(const Traits& traits, std::span<const commands::Sequence> sequences,
                          std::span<const sar::Piece> pieces, std::span<const sar::Field> fields, const FreeSpace& free,
                          const JoinOptions& options)
{
    // The items where the patterns of each sequence piece start: the fields name them.
    const std::vector<std::vector<uint32_t>> entries = agb::NamedItems(pieces, fields);
    const auto encode = [&](std::size_t piece, uint32_t sequence)
    {
        return EncodeSequence(sequences[sequence], entries[piece], traits);
    };
    agb::JoinedRom rom(pieces, sequences.size(), fields, free,
                       {.alignment = kSequenceAlignment, .run_first = true, .filler = options.filler}, encode);
    for (const sar::Field& field : fields)
    {
        uint8_t* at = rom.FieldBytes(field, 4);
        xsflib::StoreLe32(at, uint32_t(rom.Position(field.to) - rom.Position(field.from) + kRomBase));
    }

    return rom.TakeImage();
}

std::vector<uint8_t> Join(const Layout& layout, const JoinOptions& options)
{
    return Join(layout.traits, layout.sequences, layout.pieces, layout.fields, layout.free, options);
}

std::vector<Span> FillerRuns(const Rom& rom, const DriverInfo& info)
{
    return agb::FillerRunsOf(rom, ReachedBytes(rom, info, ReadSequences(rom, info)), kLeastFillerRun);
}

} // namespace ntdlib::krawall
