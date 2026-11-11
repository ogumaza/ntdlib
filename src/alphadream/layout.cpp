// SPDX-License-Identifier: MIT

#include "ntdlib/alphadream/layout.h"

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

#include "ntdlib/alphadream/commands.h"

namespace ntdlib::alphadream
{
namespace
{

using agb::kRomBase;

// A song that stays in its place starts on a multiple of this: the driver reads its header in halfwords. A song that
// moves goes onto a multiple of kMovedAlignment.
constexpr uint32_t kSongAlignment = 2;
constexpr uint32_t kMovedAlignment = 4;

// The bytes of each of the driver's tables.
constexpr uint32_t kStepTableBytes = 13 * 4;
constexpr uint32_t kKeyTableBytes = 128;
constexpr uint32_t kPsgPitchBytes = (128 - 36 + 1) * 2; // keys 36 to 127, and the key above for the step between
constexpr uint32_t kNoiseTableBytes = 0x50 - 0x15 + 1;
constexpr uint32_t kWaveLevelBytes = 4;

// The bytes of a sample's header and of a wave pattern.
constexpr uint32_t kSampleHeaderBytes = 16;
constexpr uint32_t kWaveBytes = 16;

// The most entries of the table of sound effects that the cut reads.
constexpr uint32_t kMaxEffects = 4096;

using agb::Reached;

// The words of the table of sound effects: up to the first word that names no byte of the ROM, or up to the next of
// the driver's tables.
uint32_t EffectCount(const Rom& rom, const DriverInfo& info)
{
    if (!info.effect_table)
    {
        return 0;
    }

    uint32_t end = std::numeric_limits<uint32_t>::max();
    for (const uint32_t table : {info.song_table, info.instrument_table, info.sample_table})
    {
        if (table > info.effect_table)
        {
            end = std::min(end, table);
        }
    }

    uint32_t count = 0;
    while (count < kMaxEffects && info.effect_table + 4 * count < end &&
           rom.Contains(info.effect_table + 4 * count, 4) && rom.Contains(rom.U32(info.effect_table + 4 * count)))
    {
        count++;
    }

    return count;
}

// The driver's data other than the songs: its tables, the song table, the table of sound effects and the effects, the
// instrument table and its regions, the sample table and the samples, and the wave patterns.
Reached OtherData(const Rom& rom, const DriverInfo& info)
{
    Reached reached(rom);
    reached.Mark(info.song_table, uint64_t(4) * uint32_t(info.song_count));
    reached.Mark(info.step_table, kStepTableBytes);
    reached.Mark(info.key_table, kKeyTableBytes);
    reached.Mark(info.psg_pitch_table, kPsgPitchBytes);
    reached.Mark(info.noise_table, kNoiseTableBytes);
    reached.Mark(info.wave_levels, kWaveLevelBytes);

    // The effects are songs in the same form.
    const uint32_t effects = EffectCount(rom, info);
    reached.Mark(info.effect_table, uint64_t(4) * effects);
    std::set<uint32_t> seen;
    for (uint32_t e = 0; e < effects; e++)
    {
        const uint32_t address = rom.U32(info.effect_table + 4 * e);
        if (seen.insert(address).second)
        {
            reached.Mark(address, DecodeSequence(rom, info.version, address).size);
        }
    }

    // The instrument table's last offset ends the last instrument's regions.
    const auto offsets = uint32_t(info.instrument_count + 1);
    if (rom.Contains(info.instrument_table, 2 * offsets))
    {
        reached.Mark(info.instrument_table, rom.U16(info.instrument_table + 2 * (offsets - 1)));
    }

    // Every wave pattern that a region can name: the driver picks a region's kind by the track that plays it.
    for (int instrument = 0; instrument < info.instrument_count; instrument++)
    {
        for (const Region& region : Regions(rom, info, instrument))
        {
            reached.Mark(info.wave_table + kWaveBytes * (region.sample & 0xFFu), kWaveBytes);
        }
    }

    reached.Mark(info.sample_table, uint64_t(4) * uint32_t(info.sample_count));
    for (int sample = 0; sample < info.sample_count; sample++)
    {
        SampleHeader header;
        if (ReadSample(rom, info, sample, header))
        {
            reached.Mark(header.address, kSampleHeaderBytes);
            reached.Mark(header.data, header.end);
        }
        else if (rom.Contains(header.address))
        {
            reached.Mark(header.address, kSampleHeaderBytes);
        }
    }

    return reached;
}

// A song that the cut looks at, and what the decoder made of it.
struct CutSong
{
    Decoded decoded;
    bool usable = false; // the decoder read it, and it starts on a multiple of 2 bytes
};

// Reads each song that the song table names, by its address.
std::map<uint32_t, CutSong> ReadSongs(const Rom& rom, const DriverInfo& info)
{
    std::map<uint32_t, CutSong> songs;
    for (const uint32_t address : SequenceAddresses(rom, info))
    {
        CutSong song;
        song.decoded = DecodeSequence(rom, info.version, address);
        song.usable = !song.decoded.opaque && address % kSongAlignment == 0;
        songs.emplace(address, std::move(song));
    }

    return songs;
}

// The bytes that the driver reads: its other data, `other`, and `songs`.
Reached ReachedBytes(const Reached& other, const std::map<uint32_t, CutSong>& songs)
{
    Reached reached = other;
    for (const auto& [address, song] : songs)
    {
        reached.Mark(address, song.decoded.size);
    }

    return reached;
}

} // namespace

Layout Cut(const Rom& rom, const DriverInfo& info)
{
    std::map<uint32_t, CutSong> songs = ReadSongs(rom, info);
    const Reached other = OtherData(rom, info);
    const Reached reached = ReachedBytes(other, songs);

    Layout layout;
    layout.version = info.version;
    layout.free = agb::FreeSpaceOf(rom, &reached);
    layout.free.filler = agb::FillerRunsOf(rom, reached, kLeastFillerRun);

    // Each song once, in the order of the songs' bytes. A song stays among the ROM's bytes when another song reads any
    // of its bytes, or when it holds the driver's other data: the other reader would lose them when the song moved.
    auto reader = songs.end(); // the song that reads furthest
    uint32_t read_to = 0;      // where that song's bytes end
    for (auto it = songs.begin(); it != songs.end(); ++it)
    {
        const uint32_t end = it->first + it->second.decoded.size;
        if (other.Touches(it->first, it->second.decoded.size))
        {
            it->second.usable = false;
        }
        if (reader != songs.end() && it->first < read_to)
        {
            it->second.usable = false;
            reader->second.usable = false;
        }
        if (reader == songs.end() || end > read_to)
        {
            reader = it;
            read_to = end;
        }
    }

    // The runs between the songs.
    uint32_t at = kRomBase;
    std::map<uint32_t, uint32_t> pieces_of; // the piece of each song, by the song's address
    for (auto& [address, song] : songs)
    {
        if (!song.usable)
        {
            continue;
        }

        agb::AddRomRun(rom, at, address, layout.pieces);
        pieces_of[address] = uint32_t(layout.pieces.size());
        layout.pieces.emplace_back(sar::SequencePiece{uint32_t(layout.sequences.size()), 0, song.decoded.size});
        layout.sequences.push_back(std::move(song.decoded.sequence));
        at = address + song.decoded.size;
    }
    agb::AddRomRun(rom, at, rom.End(), layout.pieces);

    // The place in the pieces of an address in a run.
    const auto run_place = [&layout](uint32_t address) -> sar::Place
    {
        const std::optional<sar::Place> place = agb::RunPlaceOf(layout.pieces, address);
        if (!place)
        {
            throw std::invalid_argument("AlphaDream's song table is outside the ROM's runs");
        }

        return *place;
    };

    // Each word of the song table that names a song piece, in order. The table is clear of the songs.
    for (int song = 0; song < info.song_count; song++)
    {
        const uint32_t word = info.song_table + 4 * uint32_t(song);
        const auto piece = pieces_of.find(rom.U32(word));
        if (piece != pieces_of.end())
        {
            layout.fields.push_back({run_place(word), {0, 0}, {piece->second, 0, true}});
        }
    }

    return layout;
}

std::vector<uint8_t> Join(Version version, std::span<const commands::Sequence> sequences,
                          std::span<const sar::Piece> pieces, std::span<const sar::Field> fields, const FreeSpace& free,
                          const JoinOptions& options)
{
    const auto encode = [&](std::size_t, uint32_t sequence)
    {
        return EncodeSequence(sequences[sequence], version);
    };
    agb::JoinedRom rom(pieces, sequences.size(), fields, free,
                       {.alignment = kMovedAlignment, .filler = options.filler, .sequences = "songs"}, encode);
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
    return Join(layout.version, layout.sequences, layout.pieces, layout.fields, layout.free, options);
}

std::vector<Span> FillerRuns(const Rom& rom, const DriverInfo& info)
{
    return agb::FillerRunsOf(rom, ReachedBytes(OtherData(rom, info), ReadSongs(rom, info)), kLeastFillerRun);
}

} // namespace ntdlib::alphadream
