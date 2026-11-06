// SPDX-License-Identifier: MIT

#include "ntdlib/konami/layout.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/konami/commands.h"

namespace ntdlib::konami
{
namespace
{

using agb::kRomBase;

// The sample numbers that the driver's commands reach: 12 bits.
constexpr int kSampleNumbers = 0x1000;

// The invalid entries of a row that end a walk of a sample table.
constexpr int kInvalidRow = 64;

// The bytes that each of the driver's other tables takes up at most, by the indices that its commands can give it.
constexpr uint32_t kVolumeTableBytes = 256 * 64;
// The PSG's table holds 128 semitones, but a note's key byte reaches 255, and a pitch bend adds to it. Some songs play
// such a key, and the driver reads the table's entry past its end. Bytes there change how the note sounds.
constexpr uint32_t kFrequencyTableBytes = (256 + 16) * 32 * 2;
constexpr uint32_t kNoiseTableBytes = 256 * 2;
constexpr uint32_t kWaveTableBytes = 128 * 16 * 16;
constexpr uint32_t kTimerTableBytes = 16 * 4;
constexpr uint32_t kSampleMapBytes = 256;
constexpr uint32_t kPeriodTableBytes = 128 * 16 * 2;
constexpr uint32_t kWaveVolumeTableBytes = 16 * 2;

uint64_t AlignUp(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

// A song that the cut looks at: its header and what the decoder made of it.
struct CutSong
{
    SongHeader header;
    Decoded decoded;
    uint32_t size = 0;   // the bytes that its items encode to, or the bytes of its data where they don't encode
    bool usable = false; // the decoder read it, and its encoding gives its bytes back
};

// Returns true if the two entries name the same bytes with the same tracks.
bool SameSong(const CutSong& a, const CutSong& b)
{
    return a.header.base == b.header.base && a.header.tracks == b.header.tracks &&
           std::equal(a.header.offsets, a.header.offsets + a.header.tracks, b.header.offsets);
}

// Reads each song of the song table that names bytes in the ROM, by its number.
std::map<int, CutSong> ReadSongs(const Rom& rom, const DriverInfo& info)
{
    std::map<int, CutSong> songs;
    for (int s = 0; s < info.song_count; s++)
    {
        SongHeader header;
        if (!ReadSongHeader(rom, info.song_table, s, info.revision, header) || !rom.Contains(header.base))
        {
            continue;
        }

        CutSong song;
        song.header = header;
        song.decoded = DecodeSong(rom, info, s);
        for (const commands::Command& item : song.decoded.sequence.items)
        {
            if (item.kind == commands::Kind::kData)
            {
                song.size += uint32_t(item.bytes.size());
            }
        }
        try
        {
            const Encoded encoded = EncodeSequence(song.decoded.sequence, song.decoded.entries, info.revision);
            song.size = uint32_t(encoded.bytes.size());
            song.usable = !song.decoded.opaque && rom.Contains(header.base, song.size) &&
                          std::equal(encoded.bytes.begin(), encoded.bytes.end(), rom.Ptr(header.base));
        }
        catch (const std::invalid_argument&)
        {
            // The song stays among the ROM's bytes, up to the end of its data.
        }
        songs.emplace(s, std::move(song));
    }

    return songs;
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

    // Marks a sample table's entries, with the samples that they name.
    void MarkSamples(const Rom& rom, const DriverInfo& info, uint32_t table, int first, uint32_t entry_size)
    {
        int invalid = 0;
        for (int n = 0; n < kSampleNumbers && invalid < kInvalidRow; n++)
        {
            const SampleInfo sample = info.Sample(rom, first + n);
            if (!sample.valid)
            {
                invalid++;
                continue;
            }

            invalid = 0;
            Mark(table, uint64_t(n + 1) * entry_size);
            if (info.revision != Revision::kDungeonDiceMonsters)
            {
                Mark(rom.U32(table + uint32_t(n) * entry_size), 12);
            }
            Mark(sample.data, sample.length);
            if (sample.packed)
            {
                Mark(rom.U32(table + uint32_t(n) * entry_size), sample.data - rom.U32(table + uint32_t(n) * entry_size));
            }
        }
    }

private:
    std::vector<bool> bytes_;
};

// The bytes that the driver reads, with `songs`.
Reached ReachedBytes(const Rom& rom, const DriverInfo& info, const std::map<int, CutSong>& songs)
{
    Reached reached(rom);
    reached.Mark(info.song_table, uint64_t(std::max(info.song_count, 0)) * SongEntrySize(info.revision));
    for (const auto& [number, song] : songs)
    {
        reached.Mark(song.header.base, song.size);
    }

    const bool dungeon = info.revision == Revision::kDungeonDiceMonsters;
    if (info.sample_table)
    {
        reached.MarkSamples(rom, info, info.sample_table, 0, dungeon ? 8 : 4);
    }
    if (info.live_sample_table)
    {
        reached.MarkSamples(rom, info, info.live_sample_table, 0x8000, 4);
    }

    const auto mark_table = [&reached](uint32_t address, uint32_t size)
    {
        if (address)
        {
            reached.Mark(address, size);
        }
    };
    mark_table(info.volume_table, kVolumeTableBytes);
    mark_table(info.psg_freq_table, kFrequencyTableBytes);
    mark_table(info.noise_table, kNoiseTableBytes);
    mark_table(info.wave_table, kWaveTableBytes);
    mark_table(info.sfx_wave_table, kWaveTableBytes);
    mark_table(info.timer_table, kTimerTableBytes);
    mark_table(info.sample_map, kSampleMapBytes);
    mark_table(info.sample_period_table, kPeriodTableBytes);
    mark_table(info.wave_volume_table, kWaveVolumeTableBytes);

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
        free.start = uint32_t(AlignUp(size, 4));
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
    std::map<int, CutSong> songs = ReadSongs(rom, info);
    const Reached reached = ReachedBytes(rom, info, songs);

    Layout layout;
    layout.revision = info.revision;
    layout.free = FreeSpaceOf(rom, reached);
    layout.free.filler = FillerRunsOf(rom, reached);

    // Each song once, in the order of the songs' bytes. A song stays among the ROM's bytes when another song reads any
    // of its bytes, or when it reads the song table: the other reader would lose them when the song moved. Entries with
    // the same base and the same tracks are one song.
    std::vector<int> order;
    for (const auto& [number, song] : songs)
    {
        order.push_back(number);
    }
    std::stable_sort(order.begin(), order.end(),
                     [&songs](int a, int b) { return songs.at(a).header.base < songs.at(b).header.base; });

    const uint32_t table_end = info.song_table + uint32_t(info.song_count) * SongEntrySize(info.revision);
    std::vector<int> kept;
    int reader = -1;      // the song that reads furthest
    uint32_t read_to = 0; // where that song's bytes end
    for (const int number : order)
    {
        CutSong& song = songs.at(number);
        const uint32_t end = song.header.base + song.size;
        if (reader >= 0 && SameSong(songs.at(reader), song))
        {
            song.usable = song.usable && songs.at(reader).usable;
            continue;
        }

        if (song.header.base < table_end && info.song_table < end)
        {
            song.usable = false;
        }
        if (reader >= 0 && song.header.base < read_to)
        {
            song.usable = false;
            songs.at(reader).usable = false;
        }
        if (reader < 0 || end > read_to)
        {
            reader = number;
            read_to = end;
        }
        kept.push_back(number);
    }
    std::erase_if(kept, [&songs](int number) { return !songs.at(number).usable; });

    // The runs between the songs.
    uint32_t at = kRomBase;
    const auto add_run = [&](uint32_t end)
    {
        if (end > at)
        {
            layout.pieces.emplace_back(std::vector<uint8_t>(rom.Ptr(at), rom.Ptr(at) + (end - at)));
            at = end;
        }
    };
    std::map<uint32_t, uint32_t> pieces_of; // the piece of each song, by the song's base
    for (const int number : kept)
    {
        CutSong& song = songs.at(number);
        add_run(song.header.base);
        pieces_of[song.header.base] = uint32_t(layout.pieces.size());
        layout.pieces.emplace_back(sar::SequencePiece{uint32_t(layout.sequences.size()), 0, song.size});
        layout.sequences.push_back(std::move(song.decoded.sequence));
        at = song.header.base + song.size;
    }
    add_run(rom.End());

    // A song table entry's fields lie in the run that holds the table: the songs lie clear of it.
    for (const auto& [number, song] : songs)
    {
        const auto piece = pieces_of.find(song.header.base);
        if (piece == pieces_of.end())
        {
            continue;
        }

        const uint32_t entry = info.song_table + uint32_t(number) * SongEntrySize(info.revision);
        uint32_t start = kRomBase;
        for (uint32_t p = 0; p < layout.pieces.size(); p++)
        {
            const auto* bytes = std::get_if<std::vector<uint8_t>>(&layout.pieces[p]);
            const uint32_t size =
                bytes ? uint32_t(bytes->size()) : std::get<sar::SequencePiece>(layout.pieces[p]).room;
            if (bytes && entry >= start && entry < start + size)
            {
                const uint32_t offset = entry - start;
                layout.fields.push_back({{p, offset}, {0, 0}, {piece->second, 0, true}});
                for (int t = 0; t < song.header.tracks; t++)
                {
                    layout.fields.push_back({{p, offset + 4 + 2 * uint32_t(t)},
                                             {piece->second, 0, true},
                                             {piece->second, song.decoded.entries[size_t(t)], true}});
                }
                break;
            }
            start += size;
        }
    }

    return layout;
}

std::vector<std::vector<uint32_t>> TrackEntries(std::span<const sar::Piece> pieces, std::span<const sar::Field> fields,
                                                Revision revision)
{
    const auto tracks = std::size_t(TrackCount(revision));
    std::vector<std::vector<uint32_t>> entries(pieces.size());
    for (const sar::Field& field : fields)
    {
        if (field.from.item && field.to.item && field.to.piece < pieces.size() && entries[field.to.piece].size() < tracks)
        {
            entries[field.to.piece].push_back(field.to.offset);
        }
    }

    return entries;
}

std::vector<uint8_t> Join(Revision revision, std::span<const commands::Sequence> sequences,
                          std::span<const sar::Piece> pieces, std::span<const sar::Field> fields,
                          const FreeSpace& free, const JoinOptions& options)
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
                throw std::invalid_argument("a song names a sequence that the layout lacks");
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
    for (std::size_t p = 0; p < pieces.size(); p++)
    {
        if (const auto* bytes = std::get_if<std::vector<uint8_t>>(&pieces[p]))
        {
            std::copy(bytes->begin(), bytes->end(), image.begin() + std::ptrdiff_t(starts[p]));
        }
    }

    // Each song in its place, or later in the places of the songs that move, or in the free space.
    const std::vector<std::vector<uint32_t>> entries = TrackEntries(pieces, fields, revision);
    std::vector<Encoded> encoded(pieces.size());
    std::vector<std::vector<uint64_t>> items(pieces.size());
    std::vector<std::size_t> moving;
    for (std::size_t p = 0; p < pieces.size(); p++)
    {
        const auto* sequence = std::get_if<sar::SequencePiece>(&pieces[p]);
        if (!sequence)
        {
            continue;
        }

        encoded[p] = EncodeSequence(sequences[sequence->sequence], entries[p], revision);
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

    // The places of the songs that move, where neighbours count as one.
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

    // The largest song first, then the earlier of two of a size.
    std::stable_sort(moving.begin(), moving.end(), [&encoded](std::size_t a, std::size_t b)
                     { return encoded[a].bytes.size() > encoded[b].bytes.size(); });

    std::vector<Span> filler = free.filler;
    uint64_t end_of_songs = free.start;
    const auto take_from = [](std::vector<Span>& spans, uint64_t bytes, uint64_t& at)
    {
        for (Span& span : spans)
        {
            if (span.size >= bytes)
            {
                at = span.start;
                span.start += uint32_t(bytes);
                span.size -= uint32_t(bytes);
                return true;
            }
        }
        return false;
    };
    for (const std::size_t p : moving)
    {
        const uint64_t bytes = encoded[p].bytes.size();
        uint64_t at = 0;
        if (!take_from(places, bytes, at))
        {
            if (end_of_songs + bytes <= kLargestRom)
            {
                at = end_of_songs;
                end_of_songs += bytes;
                if (at + bytes > image.size())
                {
                    image.resize(at + bytes, free.padding);
                }
            }
            else if (!options.filler)
            {
                throw std::invalid_argument("the songs that outgrow their places don't fit in a GBA ROM's 32 MiB");
            }
            else if (!take_from(filler, bytes, at))
            {
                throw std::invalid_argument(
                    "the songs that outgrow their places don't fit in a GBA ROM's 32 MiB or in its filler runs");
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
                throw std::invalid_argument("a field names an item that a song lacks");
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
        const uint32_t width = field.from.item ? 2 : 4;
        if (!run || uint64_t(field.at.offset) + width > run->size())
        {
            throw std::invalid_argument("a field doesn't lie in a run of a GBA ROM");
        }

        uint8_t* at = &image[starts[field.at.piece] + field.at.offset];
        const uint64_t to = position(field.to);
        if (field.from.item)
        {
            const uint64_t from = position(field.from);
            if (to < from || to - from > 0xFFFF)
            {
                throw std::invalid_argument("a track starts past 0xFFFF bytes from its song's base");
            }
            xsflib::StoreLe16(at, uint16_t(to - from));
        }
        else
        {
            xsflib::StoreLe32(at, uint32_t(kRomBase + to));
        }
    }

    return image;
}

std::vector<uint8_t> Join(const Layout& layout, const JoinOptions& options)
{
    return Join(layout.revision, layout.sequences, layout.pieces, layout.fields, layout.free, options);
}

std::vector<Span> FillerRuns(const Rom& rom, const DriverInfo& info)
{
    return FillerRunsOf(rom, ReachedBytes(rom, info, ReadSongs(rom, info)));
}

} // namespace ntdlib::konami
