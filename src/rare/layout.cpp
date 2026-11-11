// SPDX-License-Identifier: MIT

#include "ntdlib/rare/layout.h"

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

#include "ntdlib/rare/commands.h"
#include "ntdlib/rare/song.h"

namespace ntdlib::rare
{

using ntdlib::agb::kRomBase;

namespace
{

// The bytes of a tune's header, a program map and an instrument.
constexpr uint32_t kHeaderBytes = 20;
constexpr uint32_t kProgramMapBytes = 128;
constexpr uint32_t kInstrumentBytes = 68;

// The pitch table's entries from n = -64 up: the interpolation reads one past n = 63.
constexpr uint32_t kPitchTableBefore = 64 * 4;
constexpr uint32_t kPitchTableBytes = 129 * 4;
constexpr uint32_t kSineTableBytes = 257 * 4;

// A sequence's place on a multiple of this, where it moves.
constexpr uint32_t kSequenceAlignment = 4;

using agb::Reached;

// A sequence that the cut looks at, what the decoder made of it, and the tunes that play it.
struct CutSequence
{
    Decoded decoded;
    uint32_t size = 0;   // the bytes that its items encode to, or the bytes of its data where they don't encode
    bool usable = false; // the decoder read it, and its encoding gives its bytes back
    std::vector<std::pair<int, std::vector<uint32_t>>> tunes; // each tune and the items where its tracks start
};

// Reads each tune's sequence, by its address.
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
        sequence.decoded = DecodeRomSequence(rom, info, s);
        const auto found = sequences.find(address);
        if (found != sequences.end())
        {
            // Tunes that read one place share its piece only where they read the same commands.
            CutSequence& other = found->second;
            other.usable = other.usable && other.decoded.sequence == sequence.decoded.sequence;
            other.tunes.emplace_back(s, sequence.decoded.entries);
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
        sequence.tunes.emplace_back(s, sequence.decoded.entries);
        sequences.emplace(address, std::move(sequence));
    }

    return sequences;
}

// The spans of the ROM that hold the driver's data other than the sequences, from 0x08000000.
std::vector<Span> DriverDataOf(const Rom& rom, const DriverInfo& info)
{
    std::vector<Span> spans;
    const auto add = [&spans](uint32_t address, uint64_t size)
    {
        if (address && size)
        {
            spans.push_back({address, uint32_t(std::min<uint64_t>(size, 0xFFFFFFFF))});
        }
    };

    // An instrument, and for a drum kit or a key split, its key map and the instruments that the map names.
    std::set<uint32_t> instruments;
    const auto add_instrument = [&](const auto& self, uint32_t address, int depth) -> void
    {
        if (!rom.Contains(address, kInstrumentBytes) || !instruments.insert(address).second)
        {
            return;
        }

        add(address, kInstrumentBytes);
        const Instrument inst = InstrumentAt(rom, address);
        if (inst.IsSample())
        {
            if (inst.end > inst.start && rom.Contains(inst.start, inst.end - inst.start + 1))
            {
                add(inst.start, uint64_t(inst.end - inst.start) + 1);
            }
            return;
        }
        if (depth > 0 || !rom.Contains(inst.key_map, kProgramMapBytes))
        {
            return;
        }

        add(inst.key_map, kProgramMapBytes);
        int entries = 0;
        for (uint32_t key = 0; key < 128; key++)
        {
            const uint8_t index = rom.U8(inst.key_map + key);
            entries = index == 0xFF ? entries : std::max(entries, index + 1);
        }
        add(inst.key_table, 4 * uint64_t(entries));
        for (int i = 0; i < entries; i++)
        {
            if (rom.Contains(inst.key_table + 4 * uint32_t(i), 4))
            {
                self(self, rom.U32(inst.key_table + 4 * uint32_t(i)), depth + 1);
            }
        }
    };

    add(info.tune_table, 4 * uint64_t(info.tune_count));
    for (int s = 0; s < info.tune_count; s++)
    {
        TuneHeader header;
        if (!ReadTuneHeader(rom, TuneAddress(rom, info, s), header))
        {
            continue;
        }

        add(header.address, kHeaderBytes);
        add(header.track_list, 4 * uint64_t(header.track_count));
        add(header.program_map, kProgramMapBytes);
        int entries = 0;
        for (uint32_t program = 0; program < 128; program++)
        {
            const uint8_t index = rom.U8(header.program_map + program);
            entries = index == 0xFF ? entries : std::max(entries, index + 1);
        }
        add(header.instruments, 4 * uint64_t(entries));
        for (int i = 0; i < entries; i++)
        {
            if (rom.Contains(header.instruments + 4 * uint32_t(i), 4))
            {
                add_instrument(add_instrument, rom.U32(header.instruments + 4 * uint32_t(i)), 0);
            }
        }
    }
    if (info.pitch_table >= kRomBase + kPitchTableBefore)
    {
        add(info.pitch_table - kPitchTableBefore, kPitchTableBytes);
    }
    add(info.sine_table, kSineTableBytes);

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

// The bytes that the driver reads, with `sequences`.
Reached ReachedBytes(const Rom& rom, const DriverInfo& info, const std::map<uint32_t, CutSequence>& sequences)
{
    Reached reached(rom);
    for (const Span& span : DriverDataOf(rom, info))
    {
        reached.Mark(span.start, span.size);
    }
    for (const auto& [address, sequence] : sequences)
    {
        reached.Mark(address, sequence.size);
    }

    return reached;
}

// Marks the sequences that stay among the ROM's bytes as unusable. `data` gives the driver's other data.
void MarkUnmovable(std::map<uint32_t, CutSequence>& sequences, std::span<const Span> data)
{
    // Each sequence once, in the order of the sequences' bytes. A sequence stays among the ROM's bytes when another
    // reads any of its bytes, or when it holds the driver's other data: the other reader would lose them when the
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
}

// Adds the runs between the usable sequences and a piece for each of them to `layout`. Returns the piece of each
// sequence, by the sequence's address.
std::map<uint32_t, uint32_t> AddPieces(const Rom& rom, const std::map<uint32_t, CutSequence>& sequences, Layout& layout)
{
    uint32_t at = kRomBase;
    std::map<uint32_t, uint32_t> pieces_of;
    for (const auto& [address, sequence] : sequences)
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

    return pieces_of;
}

// The place in the pieces of `layout` of an address in a run.
sar::Place RunPlace(const Layout& layout, uint32_t address)
{
    const std::optional<sar::Place> place = agb::RunPlaceOf(layout.pieces, address);
    if (!place)
    {
        throw std::invalid_argument("a track list lies outside the ROM's runs");
    }

    return *place;
}

// Adds the fields of each tune's track list, in the order of the tunes. `pieces_of` gives the piece of each sequence
// that moves, by the sequence's address.
void AddTrackListFields(const Rom& rom, const DriverInfo& info, const std::map<uint32_t, CutSequence>& sequences,
                        const std::map<uint32_t, uint32_t>& pieces_of, Layout& layout)
{
    std::vector<std::pair<int, sar::Field>> fields;
    for (const auto& [address, sequence] : sequences)
    {
        const auto piece = pieces_of.find(address);
        if (piece == pieces_of.end())
        {
            continue;
        }

        for (const auto& [tune, entries] : sequence.tunes)
        {
            TuneHeader header;
            ReadTuneHeader(rom, TuneAddress(rom, info, tune), header);
            for (size_t t = 0; t < entries.size(); t++)
            {
                const sar::Place to = {piece->second, entries[t], true};
                fields.emplace_back(tune, sar::Field{RunPlace(layout, header.track_list + 4 * uint32_t(t)), {}, to});
            }
        }
    }
    std::stable_sort(fields.begin(), fields.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [tune, field] : fields)
    {
        layout.fields.push_back(field);
    }
}

} // namespace

Layout Cut(const Rom& rom, const DriverInfo& info)
{
    std::map<uint32_t, CutSequence> sequences = ReadSequences(rom, info);
    const Reached reached = ReachedBytes(rom, info, sequences);
    const std::vector<Span> data = DriverDataOf(rom, info);

    Layout layout;
    layout.traits = TraitsOf(info);
    layout.free = agb::FreeSpaceOf(rom, &reached);
    layout.free.filler = agb::FillerRunsOf(rom, reached, kLeastFillerRun);

    MarkUnmovable(sequences, data);
    const std::map<uint32_t, uint32_t> pieces_of = AddPieces(rom, sequences, layout);
    AddTrackListFields(rom, info, sequences, pieces_of, layout);

    return layout;
}

std::vector<uint8_t> Join(const Traits& traits, std::span<const commands::Sequence> sequences,
                          std::span<const sar::Piece> pieces, std::span<const sar::Field> fields, const FreeSpace& free,
                          const JoinOptions& options)
{
    // The items where the tracks of each sequence piece start: the fields name them.
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

} // namespace ntdlib::rare
