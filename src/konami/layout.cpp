// SPDX-License-Identifier: MIT

#include "ntdlib/konami/layout.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
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

using agb::Reached;

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

// Marks a sample table's entries, with the samples that they name.
void MarkSamples(Reached& reached, const Rom& rom, const DriverInfo& info, uint32_t table, int first,
                 uint32_t entry_size)
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
        reached.Mark(table, uint64_t(n + 1) * entry_size);
        if (info.revision != Revision::kDungeonDiceMonsters)
        {
            reached.Mark(rom.U32(table + uint32_t(n) * entry_size), 12);
        }
        reached.Mark(sample.data, sample.length);
        if (sample.packed)
        {
            reached.Mark(rom.U32(table + uint32_t(n) * entry_size),
                         sample.data - rom.U32(table + uint32_t(n) * entry_size));
        }
    }
}

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
        MarkSamples(reached, rom, info, info.sample_table, 0, dungeon ? 8 : 4);
    }
    if (info.live_sample_table)
    {
        MarkSamples(reached, rom, info, info.live_sample_table, 0x8000, 4);
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

// The songs whose bytes the layout cuts out, in the order of their bytes. Marks the songs that have to stay in place
// as unusable.
std::vector<int> KeptSongs(std::map<int, CutSong>& songs, const DriverInfo& info)
{
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

    return kept;
}

// Adds the runs between the songs of `kept` and a piece for each song to `layout`. Returns the piece of each song, by
// the song's base.
std::map<uint32_t, uint32_t> AddPieces(const Rom& rom, std::map<int, CutSong>& songs, std::span<const int> kept,
                                       Layout& layout)
{
    // The runs between the songs.
    uint32_t at = kRomBase;
    std::map<uint32_t, uint32_t> pieces_of;
    for (const int number : kept)
    {
        CutSong& song = songs.at(number);
        agb::AddRomRun(rom, at, song.header.base, layout.pieces);
        pieces_of[song.header.base] = uint32_t(layout.pieces.size());
        layout.pieces.emplace_back(sar::SequencePiece{uint32_t(layout.sequences.size()), 0, song.size});
        layout.sequences.push_back(std::move(song.decoded.sequence));
        at = song.header.base + song.size;
    }
    agb::AddRomRun(rom, at, rom.End(), layout.pieces);

    return pieces_of;
}

// Adds the fields of each song's entry in the song table to `layout`.
void AddEntryFields(const std::map<int, CutSong>& songs, const DriverInfo& info,
                    const std::map<uint32_t, uint32_t>& pieces_of, Layout& layout)
{
    // A song table entry's fields lie in the run that holds the table: the songs lie clear of it.
    for (const auto& [number, song] : songs)
    {
        const auto piece = pieces_of.find(song.header.base);
        if (piece == pieces_of.end())
        {
            continue;
        }

        const uint32_t entry = info.song_table + uint32_t(number) * SongEntrySize(info.revision);
        const std::optional<sar::Place> place = agb::RunPlaceOf(layout.pieces, entry);
        if (!place)
        {
            continue;
        }

        layout.fields.push_back({*place, {0, 0}, {piece->second, 0, true}});
        for (int t = 0; t < song.header.tracks; t++)
        {
            layout.fields.push_back({{place->piece, place->offset + 4 + 2 * uint32_t(t)},
                                     {piece->second, 0, true},
                                     {piece->second, song.decoded.entries[size_t(t)], true}});
        }
    }
}

} // namespace

Layout Cut(const Rom& rom, const DriverInfo& info)
{
    std::map<int, CutSong> songs = ReadSongs(rom, info);
    const Reached reached = ReachedBytes(rom, info, songs);

    Layout layout;
    layout.revision = info.revision;
    layout.free = agb::FreeSpaceOf(rom, &reached);
    layout.free.filler = agb::FillerRunsOf(rom, reached, kLeastFillerRun);

    const std::vector<int> kept = KeptSongs(songs, info);
    const std::map<uint32_t, uint32_t> pieces_of = AddPieces(rom, songs, kept, layout);
    AddEntryFields(songs, info, pieces_of, layout);

    return layout;
}

std::vector<std::vector<uint32_t>> TrackEntries(std::span<const sar::Piece> pieces, std::span<const sar::Field> fields,
                                                Revision revision)
{
    const auto tracks = std::size_t(TrackCount(revision));
    std::vector<std::vector<uint32_t>> entries(pieces.size());
    for (const sar::Field& field : fields)
    {
        if (field.from.item && field.to.item && field.to.piece < pieces.size() &&
            entries[field.to.piece].size() < tracks)
        {
            entries[field.to.piece].push_back(field.to.offset);
        }
    }

    return entries;
}

std::vector<uint8_t> Join(Revision revision, std::span<const commands::Sequence> sequences,
                          std::span<const sar::Piece> pieces, std::span<const sar::Field> fields, const FreeSpace& free,
                          const JoinOptions& options)
{
    const std::vector<std::vector<uint32_t>> entries = TrackEntries(pieces, fields, revision);
    const auto encode = [&](std::size_t piece, uint32_t sequence)
    {
        return EncodeSequence(sequences[sequence], entries[piece], revision);
    };
    agb::JoinedRom rom(pieces, sequences.size(), fields, free, {.filler = options.filler, .sequences = "songs"},
                       encode);
    for (const sar::Field& field : fields)
    {
        uint8_t* at = rom.FieldBytes(field, field.from.item ? 2 : 4);
        const uint64_t to = rom.Position(field.to);
        if (field.from.item)
        {
            const uint64_t from = rom.Position(field.from);
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

    return rom.TakeImage();
}

std::vector<uint8_t> Join(const Layout& layout, const JoinOptions& options)
{
    return Join(layout.revision, layout.sequences, layout.pieces, layout.fields, layout.free, options);
}

std::vector<Span> FillerRuns(const Rom& rom, const DriverInfo& info)
{
    return agb::FillerRunsOf(rom, ReachedBytes(rom, info, ReadSongs(rom, info)), kLeastFillerRun);
}

} // namespace ntdlib::konami
