// SPDX-License-Identifier: MIT

#include "ntdlib/ubimilan/layout.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/ubimilan/commands.h"
#include "ntdlib/ubimilan/song.h"

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
    const auto size = uint32_t(rom.Size());
    const uint32_t read_end = last_read > kRomBase ? std::min(last_read - kRomBase, size) : 0;
    const agb::FreeSpace free = agb::FreeSpaceOf(rom, nullptr, read_end);

    return {.start = free.start, .padding = free.padding};
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

    std::map<uint32_t, uint32_t> pieces_of;
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
        layout.resources.push_back(sequence.resource);
        at = address + sequence.size;
    }

    agb::AddRomRun(rom, at, rom.End(), layout.pieces);

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
    // The fields name only the first item of a sequence.
    const auto encode = [&](std::size_t, uint32_t sequence)
    {
        Encoded encoded = EncodeSequence(sequences[sequence]);
        encoded.offsets = {0};
        return encoded;
    };
    agb::JoinedRom rom(pieces, sequences.size(), fields, {.start = free.start, .padding = free.padding}, {}, encode);
    for (const sar::Field& field : fields)
    {
        uint8_t* at = rom.FieldBytes(field, 4);
        xsflib::StoreLe32(at, uint32_t(rom.Position(field.to) - rom.Position(field.from)));
    }

    return rom.TakeImage();
}

std::vector<uint8_t> Join(const Layout& layout)
{
    return Join(layout.sequences, layout.pieces, layout.fields, layout.free);
}

} // namespace ntdlib::ubimilan
