// SPDX-License-Identifier: MIT

#include "ntdlib/rd1/layout.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>
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

uint64_t AlignUp(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
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

    // Whether any of `size` bytes from `address` is marked.
    bool Touches(uint32_t address, uint32_t size) const
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

    // Marks the bytes of the ROM among the `size` bytes from `address`.
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
        free.start = uint32_t(AlignUp(size, kFileAlignment));
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
    std::map<uint32_t, CutFile> files = ReadFiles(rom, info);
    const OtherData other(rom, info);
    const Reached reached = ReachedBytes(other.Bytes(), files);

    Layout layout;
    layout.free = FreeSpaceOf(rom, reached);
    layout.free.filler = FillerRunsOf(rom, reached);

    // Each file once, in the order of the files' bytes. A file stays among the ROM's bytes when another file reads any
    // of its bytes, or when it holds the driver's other data: the other reader would lose them when the file moved.
    auto reader = files.end(); // the file that reads furthest
    uint32_t read_to = 0;      // where that file's bytes end
    for (auto it = files.begin(); it != files.end(); ++it)
    {
        const uint32_t end = it->first + it->second.decoded.size;
        if (other.Bytes().Touches(it->first, it->second.decoded.size))
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

    // The runs between the files.
    uint32_t at = kRomBase;
    const auto add_run = [&](uint32_t end)
    {
        if (end > at)
        {
            layout.pieces.emplace_back(std::vector<uint8_t>(rom.Ptr(at), rom.Ptr(at) + (end - at)));
            at = end;
        }
    };

    std::map<uint32_t, uint32_t> pieces_of; // the piece of each file, by the file's address
    for (auto& [address, file] : files)
    {
        if (!file.usable)
        {
            continue;
        }

        add_run(address);
        pieces_of[address] = uint32_t(layout.pieces.size());
        layout.pieces.emplace_back(sar::SequencePiece{uint32_t(layout.sequences.size()), 0, file.decoded.size});
        layout.sequences.push_back(std::move(file.decoded.sequence));
        at = address + file.decoded.size;
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

        throw std::invalid_argument("a record of the driver is outside the ROM's runs");
    };

    // Each record's field, in the order of the records' places. The records are clear of the files.
    std::set<uint32_t> records;
    for (int song = 0; song < info.song_count; song++)
    {
        SongRecord record;
        if (ReadSongRecord(rom, info, song, record))
        {
            records.insert(record.address);
        }
    }

    for (const uint32_t record : records)
    {
        const auto piece = pieces_of.find(rom.U32(record));
        if (piece != pieces_of.end())
        {
            layout.fields.push_back({run_place(record), {0, 0}, {piece->second, 0, true}});
        }
    }

    return layout;
}

std::vector<uint8_t> Join(std::span<const commands::Sequence> sequences, std::span<const sar::Piece> pieces,
                          std::span<const sar::Field> fields, const FreeSpace& free, const JoinOptions& options)
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
                throw std::invalid_argument("a sequence piece names a sequence missing from the layout");
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

    // Each file in its place, or later in the places of the files that move, or in the free space.
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

        encoded[p] = EncodeSequence(sequences[sequence->sequence]);
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

    // The places of the files that move, where neighbours count as one.
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

    // The largest file first, then the earlier of two of a size.
    std::stable_sort(moving.begin(), moving.end(),
                     [&encoded](size_t a, size_t b) { return encoded[a].bytes.size() > encoded[b].bytes.size(); });

    std::vector<Span> filler = free.filler;
    uint64_t end_of_files = free.start;
    const auto take_from = [](std::vector<Span>& spans, uint64_t bytes, uint64_t& at)
    {
        for (Span& span : spans)
        {
            const uint64_t place = AlignUp(span.start, kFileAlignment);
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
            const uint64_t place = AlignUp(end_of_files, kFileAlignment);
            if (place + bytes <= kLargestRom)
            {
                at = place;
                end_of_files = place + bytes;
                if (at + bytes > image.size())
                {
                    image.resize(at + bytes, free.padding);
                }
            }
            else if (!options.filler)
            {
                throw std::invalid_argument("the MIDI files that outgrow their places don't fit in a GBA ROM's 32 MiB");
            }
            else if (!take_from(filler, bytes, at))
            {
                throw std::invalid_argument(
                    "the MIDI files that outgrow their places don't fit in a GBA ROM's 32 MiB or in its filler runs");
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
            throw std::invalid_argument("a field names a place missing from a GBA ROM's pieces");
        }

        if (place.item)
        {
            if (!std::holds_alternative<sar::SequencePiece>(pieces[place.piece]) ||
                place.offset >= items[place.piece].size())
            {
                throw std::invalid_argument("a field names an item missing from its sequence");
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
            throw std::invalid_argument("a field is outside the runs of a GBA ROM");
        }

        const uint64_t from = position(field.from);
        const uint64_t to = position(field.to);
        if (to < from || to - from + kRomBase > std::numeric_limits<uint32_t>::max())
        {
            throw std::invalid_argument("a field's address is outside the GBA's address space");
        }

        xsflib::StoreLe32(&image[starts[field.at.piece] + field.at.offset], uint32_t(to - from + kRomBase));
    }

    return image;
}

std::vector<uint8_t> Join(const Layout& layout, const JoinOptions& options)
{
    return Join(layout.sequences, layout.pieces, layout.fields, layout.free, options);
}

std::vector<Span> FillerRuns(const Rom& rom, const DriverInfo& info)
{
    return FillerRunsOf(rom, ReachedBytes(OtherData(rom, info).Bytes(), ReadFiles(rom, info)));
}

} // namespace ntdlib::rd1
