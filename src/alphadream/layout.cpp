// SPDX-License-Identifier: MIT

#include "ntdlib/alphadream/layout.h"

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
        free.start = uint32_t(AlignUp(size, kMovedAlignment));
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
    std::map<uint32_t, CutSong> songs = ReadSongs(rom, info);
    const Reached other = OtherData(rom, info);
    const Reached reached = ReachedBytes(other, songs);

    Layout layout;
    layout.version = info.version;
    layout.free = FreeSpaceOf(rom, reached);
    layout.free.filler = FillerRunsOf(rom, reached);

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
    const auto add_run = [&](uint32_t end)
    {
        if (end > at)
        {
            layout.pieces.emplace_back(std::vector<uint8_t>(rom.Ptr(at), rom.Ptr(at) + (end - at)));
            at = end;
        }
    };
    std::map<uint32_t, uint32_t> pieces_of; // the piece of each song, by the song's address
    for (auto& [address, song] : songs)
    {
        if (!song.usable)
        {
            continue;
        }

        add_run(address);
        pieces_of[address] = uint32_t(layout.pieces.size());
        layout.pieces.emplace_back(sar::SequencePiece{uint32_t(layout.sequences.size()), 0, song.decoded.size});
        layout.sequences.push_back(std::move(song.decoded.sequence));
        at = address + song.decoded.size;
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
        throw std::invalid_argument("AlphaDream's song table is outside the ROM's runs");
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

    // Each song in its place, or later in the places of the songs that move, or in the free space.
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

        encoded[p] = EncodeSequence(sequences[sequence->sequence], version);
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

    // The largest song first, then the earlier of two of a size.
    std::stable_sort(moving.begin(), moving.end(),
                     [&encoded](size_t a, size_t b) { return encoded[a].bytes.size() > encoded[b].bytes.size(); });

    std::vector<Span> filler = free.filler;
    uint64_t end_of_songs = free.start;
    const auto take_from = [](std::vector<Span>& spans, uint64_t bytes, uint64_t& at)
    {
        for (Span& span : spans)
        {
            const uint64_t place = AlignUp(span.start, kMovedAlignment);
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
            const uint64_t place = AlignUp(end_of_songs, kMovedAlignment);
            if (place + bytes <= kLargestRom)
            {
                at = place;
                end_of_songs = place + bytes;
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
    return Join(layout.version, layout.sequences, layout.pieces, layout.fields, layout.free, options);
}

std::vector<Span> FillerRuns(const Rom& rom, const DriverInfo& info)
{
    return FillerRunsOf(rom, ReachedBytes(OtherData(rom, info), ReadSongs(rom, info)));
}

} // namespace ntdlib::alphadream
