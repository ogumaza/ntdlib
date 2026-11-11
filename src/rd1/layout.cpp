// SPDX-License-Identifier: MIT

#include "ntdlib/rd1/layout.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/rd1/commands.h"
#include "ntdlib/rd1/instruments.h"
#include "table_bytes.h"

namespace ntdlib::rd1
{
namespace
{

using agb::kRomBase;

// A file's place on a multiple of this.
constexpr uint32_t kFileAlignment = 4;

// The most bytes of a name that a record names.
constexpr uint32_t kNameBytes = 65;

// The bytes of each of the driver's tables.
constexpr uint32_t kPitchTableBytes = 128 * 2;
constexpr uint32_t kSineTableBytes = 256 * 2;
constexpr uint32_t kNoiseTableBytes = 0x50 - 0x15 + 1;
constexpr uint32_t kWaveLevelBytes = 4;

// The bytes of a bank: an instrument for each of 128 programs.
constexpr uint32_t kBankBytes = 128 * 4;

// The bytes of the instrument records of each kind.
constexpr uint32_t kSampleRecordBytes = 32;
constexpr uint32_t kPsgRecordBytes = 36;
constexpr uint32_t kKitRecordBytes = 8;
constexpr uint32_t kSplitRecordBytes = 12;

// The bytes of a sample's header and of the wave channel's pattern.
constexpr uint32_t kSampleHeaderBytes = 24;
constexpr uint32_t kWaveBytes = 16;

// The PSG channel that plays a pattern of the wave RAM.
constexpr int kWaveChannel = 2;

using agb::Reached;

// The driver's data other than the MIDI files: its tables, the song table, the records and their names, the banks, the
// instrument records, the kits' and the splits' tables and the samples.
class OtherData
{
public:
    OtherData(const Rom& rom, const DriverInfo& info) : rom_(rom), reached_(rom)
    {
        const auto players = uint32_t(info.players.size());
        reached_.Mark(info.song_table, uint64_t(kSongEntryBytes) * uint32_t(info.song_count));
        reached_.Mark(info.player_table, uint64_t(kPlayerEntryBytes) * players);
        reached_.Mark(info.player_setup, uint64_t(kSetupBytes) * players);
        reached_.Mark(info.pitch_table, kPitchTableBytes);
        reached_.Mark(info.sine_table, kSineTableBytes);
        reached_.Mark(info.noise_table, kNoiseTableBytes);
        reached_.Mark(info.wave_levels, kWaveLevelBytes);
        reached_.Mark(info.bank_table, uint64_t(4) * uint32_t(info.bank_count));

        for (int song = 0; song < info.song_count; song++)
        {
            MarkRecord(rom.U32(info.song_table + kSongEntryBytes * uint32_t(song)));
        }

        for (int bank = 0; bank < info.bank_count; bank++)
        {
            const uint32_t table = rom.U32(info.bank_table + 4 * uint32_t(bank));
            if (!table || !rom.Contains(table))
            {
                continue;
            }

            reached_.Mark(table, kBankBytes);
            for (uint32_t program = 0; program < 128; program++)
            {
                MarkInstrument(rom.U32(table + 4 * program), false);
            }
        }
    }

    const Reached& Bytes() const
    {
        return reached_;
    }

private:
    void MarkRecord(uint32_t record)
    {
        if (!record || !rom_.Contains(record) || !records_.insert(record).second)
        {
            return;
        }

        reached_.Mark(record, kSongRecordBytes);

        const uint32_t name = rom_.U32(record + 12);
        uint32_t length = 0;
        while (length < kNameBytes && rom_.Contains(name + length) && rom_.U8(name + length))
        {
            length++;
        }
        reached_.Mark(name, std::min(length + 1, kNameBytes));
    }

    // Marks the instrument record at `address`. A drum kit's or a key split's part plays only as itself.
    void MarkInstrument(uint32_t address, bool part)
    {
        if (!address || !rom_.Contains(address) || !instruments_.insert(address).second)
        {
            return;
        }

        const InstrumentRecord r = ReadInstrument(rom_, address);
        switch (r.kind)
        {
        case InstrumentKind::kDrums:
            reached_.Mark(address, kKitRecordBytes);
            if (!part)
            {
                const uint32_t keys = 128 - uint32_t(r.key);
                reached_.Mark(r.table, 4 * uint64_t(keys));
                for (uint32_t k = 0; k < keys; k++)
                {
                    MarkInstrument(rom_.U32(r.table + 4 * k), true);
                }
            }
            break;

        case InstrumentKind::kKeySplit:
            reached_.Mark(address, kSplitRecordBytes);
            if (!part)
            {
                const uint32_t keys = 128 - uint32_t(r.key);
                reached_.Mark(r.table, keys);
                uint32_t parts = 0;
                for (uint32_t k = 0; k < keys; k++)
                {
                    parts = std::max<uint32_t>(parts, uint32_t(rom_.U8(r.table + k)) + 1);
                }
                reached_.Mark(r.parts, 4 * uint64_t(parts));
                for (uint32_t p = 0; p < parts; p++)
                {
                    MarkInstrument(rom_.U32(r.parts + 4 * p), true);
                }
            }
            break;

        case InstrumentKind::kPsg:
            reached_.Mark(address, kPsgRecordBytes);
            if (r.psg_channel == kWaveChannel)
            {
                reached_.Mark(r.sample, kWaveBytes);
            }
            break;

        case InstrumentKind::kSample:
        case InstrumentKind::kFixed:
            reached_.Mark(address, kSampleRecordBytes);
            MarkSample(r.sample);
            break;

        default:
            // A record past the ROM's end: the driver reads its first bytes.
            reached_.Mark(address, kKitRecordBytes);
            break;
        }
    }

    // Marks a sample's header and its points: up to the longer of its length and its loop's end.
    void MarkSample(uint32_t header)
    {
        if (!rom_.Contains(header, kSampleHeaderBytes) || !samples_.insert(header).second)
        {
            return;
        }

        reached_.Mark(header, kSampleHeaderBytes);
        const uint32_t points = std::min(std::max(rom_.U32(header), rom_.U32(header + 16)), kMaxSamplePoints);
        reached_.Mark(rom_.U32(header + 20), points);
    }

    const Rom& rom_;
    Reached reached_;
    std::set<uint32_t> records_;
    std::set<uint32_t> instruments_;
    std::set<uint32_t> samples_;
};

// A MIDI file that the cut looks at, and what the decoder made of it.
struct CutFile
{
    Decoded decoded;
    bool usable = false; // the decoder read it, and it starts on a multiple of 4 bytes
};

// Reads each MIDI file that the records name, by its address.
std::map<uint32_t, CutFile> ReadFiles(const Rom& rom, const DriverInfo& info)
{
    std::map<uint32_t, CutFile> files;
    for (const uint32_t address : SequenceAddresses(rom, info))
    {
        CutFile file;
        file.decoded = DecodeSequence(std::span<const uint8_t>(rom.Ptr(address), rom.End() - address));
        file.usable = !file.decoded.opaque && address % kFileAlignment == 0;
        files.emplace(address, std::move(file));
    }

    return files;
}

// The bytes that the driver reads: its other data, `other`, and `files`.
Reached ReachedBytes(const Reached& other, const std::map<uint32_t, CutFile>& files)
{
    Reached reached = other;
    for (const auto& [address, file] : files)
    {
        reached.Mark(address, file.decoded.size);
    }

    return reached;
}

// Marks the files that stay among the ROM's bytes as unusable. `data` gives the driver's other data.
void MarkUnmovable(std::map<uint32_t, CutFile>& files, const Reached& data)
{
    // Each file once, in the order of the files' bytes. A file stays among the ROM's bytes when another file reads any
    // of its bytes, or when it holds the driver's other data: the other reader would lose them when the file moved.
    auto reader = files.end(); // the file that reads furthest
    uint32_t read_to = 0;      // where that file's bytes end
    for (auto it = files.begin(); it != files.end(); ++it)
    {
        const uint32_t end = it->first + it->second.decoded.size;
        if (data.Touches(it->first, it->second.decoded.size))
        {
            it->second.usable = false;
        }
        if (reader != files.end() && it->first < read_to)
        {
            it->second.usable = false;
            reader->second.usable = false;
        }
        if (reader == files.end() || end > read_to)
        {
            reader = it;
            read_to = end;
        }
    }
}

// Adds the runs between the usable files and a piece for each of them to `layout`. Returns the piece of each file, by
// the file's address.
std::map<uint32_t, uint32_t> AddPieces(const Rom& rom, std::map<uint32_t, CutFile>& files, Layout& layout)
{
    uint32_t at = kRomBase;
    std::map<uint32_t, uint32_t> pieces_of;
    for (auto& [address, file] : files)
    {
        if (!file.usable)
        {
            continue;
        }

        agb::AddRomRun(rom, at, address, layout.pieces);
        pieces_of[address] = uint32_t(layout.pieces.size());
        layout.pieces.emplace_back(sar::SequencePiece{uint32_t(layout.sequences.size()), 0, file.decoded.size});
        layout.sequences.push_back(std::move(file.decoded.sequence));
        at = address + file.decoded.size;
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
        throw std::invalid_argument("a record of the driver is outside the ROM's runs");
    }

    return *place;
}

// Adds the field of each song record that names a file that moves. `pieces_of` gives the piece of each such file, by
// its address.
void AddRecordFields(const Rom& rom, const DriverInfo& info, const std::map<uint32_t, uint32_t>& pieces_of,
                     Layout& layout)
{
    // Each record's field, in the order of the records' places. The records are clear of the files. A record that names
    // a player or a bank that the driver doesn't have gets a field too: it may name a file that another record moves.
    std::set<uint32_t> records;
    for (int song = 0; song < info.song_count; song++)
    {
        SongRecord record;
        ReadSongRecord(rom, info, song, record);
        if (record.address != 0)
        {
            records.insert(record.address);
        }
    }

    for (const uint32_t record : records)
    {
        const auto piece = pieces_of.find(rom.U32(record));
        if (piece != pieces_of.end())
        {
            layout.fields.push_back({RunPlace(layout, record), {0, 0}, {piece->second, 0, true}});
        }
    }
}

} // namespace

Layout Cut(const Rom& rom, const DriverInfo& info)
{
    std::map<uint32_t, CutFile> files = ReadFiles(rom, info);
    const OtherData other(rom, info);
    const Reached reached = ReachedBytes(other.Bytes(), files);

    Layout layout;
    layout.free = agb::FreeSpaceOf(rom, &reached);
    layout.free.filler = agb::FillerRunsOf(rom, reached, kLeastFillerRun);

    MarkUnmovable(files, other.Bytes());
    const std::map<uint32_t, uint32_t> pieces_of = AddPieces(rom, files, layout);
    AddRecordFields(rom, info, pieces_of, layout);

    return layout;
}

std::vector<uint8_t> Join(std::span<const commands::Sequence> sequences, std::span<const sar::Piece> pieces,
                          std::span<const sar::Field> fields, const FreeSpace& free, const JoinOptions& options)
{
    const auto encode = [&](std::size_t, uint32_t sequence)
    {
        return EncodeSequence(sequences[sequence]);
    };
    agb::JoinedRom rom(pieces, sequences.size(), fields, free,
                       {.alignment = kFileAlignment, .filler = options.filler, .sequences = "MIDI files"}, encode);
    for (const sar::Field& field : fields)
    {
        uint8_t* at = rom.FieldBytes(field, 4);
        const uint64_t from = rom.Position(field.from);
        const uint64_t to = rom.Position(field.to);
        if (to < from || to - from + kRomBase > std::numeric_limits<uint32_t>::max())
        {
            throw std::invalid_argument("a field's address is outside the GBA's address space");
        }

        xsflib::StoreLe32(at, uint32_t(to - from + kRomBase));
    }

    return rom.TakeImage();
}

std::vector<uint8_t> Join(const Layout& layout, const JoinOptions& options)
{
    return Join(layout.sequences, layout.pieces, layout.fields, layout.free, options);
}

std::vector<Span> FillerRuns(const Rom& rom, const DriverInfo& info)
{
    return agb::FillerRunsOf(rom, ReachedBytes(OtherData(rom, info).Bytes(), ReadFiles(rom, info)), kLeastFillerRun);
}

} // namespace ntdlib::rd1
