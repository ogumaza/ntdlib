// SPDX-License-Identifier: MIT

#include "ntdlib/mp2k/layout.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/mp2k/commands.h"
#include "ntdlib/mp2k/song.h"

namespace ntdlib::mp2k
{
namespace
{

using agb::kRomBase;

// A song that the cut takes: its first byte, its header and the decoder's sequence.
struct CutSong
{
    uint32_t base = 0;
    SongHeader header;
    Decoded decoded;
};

// The bytes of a header of `tracks` tracks.
uint32_t HeaderSize(uint32_t tracks)
{
    return 8 + 4 * tracks;
}

uint64_t AlignUp(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

// Where the free space of `image` starts (kFreeSpaceMargin).
uint64_t FreeSpace(const std::vector<uint8_t>& image)
{
    std::size_t run = image.size();
    while (run > 0 && image[run - 1] == 0xFF)
    {
        run--;
    }

    return run < image.size() ? AlignUp(run + kFreeSpaceMargin, kFreeSpaceMargin) : AlignUp(image.size(), 4);
}

// The bytes of a ROM that the driver reads, as FillerRuns finds them.
class Reached
{
public:
    explicit Reached(const agb::Rom& rom) : rom_(rom), bytes_(rom.Size(), false)
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

    // Marks the commands that a track from `start` reaches, with the patterns that it plays and the places that its
    // jumps go to, and the samples that its extended commands name. A jump's target starts with the running status
    // of the jump. Each command counts once in a song: `seen` holds the commands that the song's tracks have read.
    void MarkTrack(uint32_t start, std::set<uint32_t>& seen)
    {
        std::vector<std::pair<uint32_t, uint8_t>> pending = {{start, 0}}; // each place, with its running status
        while (!pending.empty())
        {
            auto [at, running] = pending.back();
            pending.pop_back();
            while (rom_.Contains(at) && seen.insert(at).second)
            {
                Event event;
                if (!DecodeEvent(rom_, at, running, event))
                {
                    break;
                }

                Mark(at, event.size);
                const uint8_t command = event.command;
                running = RunningStatus(event, running);
                if (command == kCmdGoto || command == kCmdPatt || command == kCmdRept ||
                    (command == kCmdMemAcc && event.args == 7))
                {
                    pending.emplace_back(event.target, running);
                }
                else if (command == kCmdXcmd && event.args == 5 && event.arg[0] == kXcmdWave)
                {
                    MarkWave(event.target);
                }
                if (command == kCmdGoto || EndsTrack(event))
                {
                    break;
                }
                at += event.size;
            }
        }
    }

    // Marks the voice at `address`, with what it plays. A key split's or a drum kit's voices count once, without the
    // tables of a split inside them.
    void MarkVoice(uint32_t address, bool inside = false)
    {
        Voice voice;
        if (!ReadVoice(rom_, address, voice))
        {
            return;
        }

        Mark(address, 12);
        if (voice.IsSplit())
        {
            if (inside)
            {
                return;
            }
            if (voice.type & kVoiceKeySplit)
            {
                Mark(voice.key_table, 128);
            }
            for (int key = 0; key < 128; key++)
            {
                const uint32_t played = SplitVoice(rom_, voice, key);
                if (played != 0)
                {
                    MarkVoice(played, true);
                }
            }
        }
        else if (voice.PsgChannel() == 0)
        {
            MarkWave(voice.wave);
        }
        else if (voice.PsgChannel() == 3)
        {
            Mark(voice.wave, 16);
        }
    }

private:
    // Marks the sample at `address`: its header and its data, with the byte after the data that the mixer can read.
    void MarkWave(uint32_t address)
    {
        Wave wave;
        if (ReadWave(rom_, address, wave))
        {
            Mark(address, 16 + uint64_t(wave.size) + 1);
        }
    }

    const agb::Rom& rom_;
    std::vector<bool> bytes_;
};

// Whether `field` names where a track of a song starts: it lies in the run after a sequence piece, at the place of a
// track's address in a header, and names an item of the piece.
bool IsTrackField(std::span<const sar::Piece> pieces, const sar::Field& field)
{
    const sar::Place& at = field.at;
    const sar::Place& to = field.to;
    return !at.item && !at.sequence && to.item && !to.sequence && to.piece + 1 == at.piece &&
           at.piece < pieces.size() && at.offset >= 8 && (at.offset - 8) % 4 == 0 &&
           std::holds_alternative<sar::SequencePiece>(pieces[to.piece]) &&
           std::holds_alternative<std::vector<uint8_t>>(pieces[at.piece]);
}

} // namespace

std::vector<uint32_t> HeaderTracks(std::span<const sar::Piece> pieces, std::span<const sar::Field> fields)
{
    std::vector<uint32_t> tracks(pieces.size());
    for (std::size_t p = 0; p + 1 < pieces.size(); p++)
    {
        const auto* run = std::get_if<std::vector<uint8_t>>(&pieces[p + 1]);
        if (std::holds_alternative<sar::SequencePiece>(pieces[p]) && run && !run->empty())
        {
            tracks[p] = (*run)[0];
        }
    }

    std::vector<bool> named(pieces.size());
    for (const sar::Field& field : fields)
    {
        if (!IsTrackField(pieces, field))
        {
            continue;
        }

        const uint32_t piece = field.to.piece;
        if (!named[piece])
        {
            named[piece] = true;
            tracks[piece] = 0;
        }
        tracks[piece] = std::max(tracks[piece], (field.at.offset - 8) / 4 + 1);
    }

    return tracks;
}

Layout Cut(const agb::Rom& rom, const DriverInfo& info)
{
    // Each song of the table once, by its header, in the order of the songs' first bytes.
    std::map<uint32_t, CutSong> songs;
    std::set<uint32_t> headers;
    std::multimap<uint32_t, int> entries; // the song table's entries, by the headers that they name
    for (int s = 0; s < info.song_count; s++)
    {
        SongHeader header;
        if (!ReadSong(rom, info, s, header))
        {
            continue;
        }

        entries.emplace(header.address, s);
        const uint32_t header_size = HeaderSize(uint32_t(header.tracks.size()));
        if (!headers.insert(header.address).second || !rom.Contains(header.address, header_size))
        {
            continue;
        }

        CutSong song;
        song.base = *std::min_element(header.tracks.begin(), header.tracks.end());
        try
        {
            song.decoded = DecodeSong(rom, header);
            if (song.decoded.opaque)
            {
                continue;
            }

            const Encoded encoded = EncodeSequence(song.decoded.sequence, song.base);
            if (!std::equal(encoded.bytes.begin(), encoded.bytes.end(), rom.Ptr(song.base),
                            rom.Ptr(song.base) + (header.address - song.base)))
            {
                continue;
            }
        }
        catch (const std::invalid_argument&)
        {
            continue;
        }

        song.header = std::move(header);
        songs.emplace(song.base, std::move(song));
    }

    // The songs that lie clear of one another and of the song table.
    const uint32_t table_end = info.song_table + 8 * uint32_t(info.song_count);
    std::vector<CutSong*> kept;
    uint32_t clear_from = kRomBase;
    for (auto& [base, song] : songs)
    {
        const uint32_t end = song.header.address + HeaderSize(uint32_t(song.header.tracks.size()));
        if (base < clear_from || (base < table_end && info.song_table < end))
        {
            continue;
        }

        kept.push_back(&song);
        clear_from = end;
    }

    // The runs between the songs, and the fields of the headers and of the song table.
    Layout layout;
    uint32_t at = kRomBase;                   // where the next run starts
    std::map<uint32_t, uint32_t> header_runs; // each header's run, by the header's address
    const auto add_run = [&](uint32_t end)
    {
        if (end > at)
        {
            const uint8_t* first = rom.Ptr(at);
            layout.pieces.emplace_back(std::vector<uint8_t>(first, first + (end - at)));
            at = end;
        }
    };
    for (CutSong* song : kept)
    {
        add_run(song->base);
        const auto piece = uint32_t(layout.pieces.size());
        layout.pieces.emplace_back(
            sar::SequencePiece{uint32_t(layout.sequences.size()), 0, song->header.address - song->base});
        layout.sequences.push_back(std::move(song->decoded.sequence));
        at = song->header.address;

        // The run that starts with the header follows the piece.
        const uint32_t run = piece + 1;
        header_runs[song->header.address] = run;
        for (std::size_t t = 0; t < song->header.tracks.size(); t++)
        {
            layout.fields.push_back({{run, 8 + 4 * uint32_t(t)}, {0, 0}, {piece, song->decoded.entries[t], true}});
        }
    }
    add_run(rom.End());

    // A song table entry's field lies in the run that holds the table: the songs lie clear of it.
    for (const auto& [header, run] : header_runs)
    {
        const auto [first, last] = entries.equal_range(header);
        for (auto e = first; e != last; e++)
        {
            const uint32_t entry = info.song_table + 8 * uint32_t(e->second);
            uint32_t start = kRomBase;
            for (uint32_t p = 0; p < layout.pieces.size(); p++)
            {
                const auto* bytes = std::get_if<std::vector<uint8_t>>(&layout.pieces[p]);
                const uint32_t size =
                    bytes ? uint32_t(bytes->size()) : std::get<sar::SequencePiece>(layout.pieces[p]).room;
                if (bytes && entry >= start && entry < start + size)
                {
                    layout.fields.push_back({{p, entry - start}, {0, 0}, {run, 0}});
                    break;
                }
                start += size;
            }
        }
    }

    return layout;
}

std::vector<Span> FillerRuns(const agb::Rom& rom, const DriverInfo& info)
{
    // The bytes that the driver reads: its tables, and each song with what it plays.
    Reached reached(rom);
    reached.Mark(info.song_table, 8 * uint64_t(std::max(info.song_count, 0)));
    reached.Mark(info.player_table, 12 * uint64_t(info.players.size()));
    std::set<uint32_t> groups; // the voice groups that the songs name
    for (int song = 0; song < info.song_count; song++)
    {
        SongHeader header;
        if (!ReadSongHeader(rom, SongAddress(rom, info, song), header))
        {
            continue;
        }

        reached.Mark(header.address, HeaderSize(uint32_t(header.tracks.size())));
        std::set<uint32_t> seen;
        for (const uint32_t track : header.tracks)
        {
            reached.MarkTrack(track, seen);
        }
        if (groups.insert(header.voices).second)
        {
            for (uint32_t program = 0; program < 128; program++)
            {
                reached.MarkVoice(header.voices + 12 * program);
            }
        }
    }

    // The runs of 0xFF before the ROM's end, without their margins, where the driver reads no byte.
    std::vector<Span> runs;
    const auto size = uint32_t(rom.Size());
    uint32_t at = 0;
    while (at < size)
    {
        uint32_t end = at;
        while (end < size && rom.U8(kRomBase + end) == 0xFF)
        {
            end++;
        }
        if (end == at)
        {
            at++;
            continue;
        }

        const auto start = uint32_t(AlignUp(uint64_t(at) + kFreeSpaceMargin, kFreeSpaceMargin));
        const bool taken = end - at >= kLeastFillerRun && end < size && start + kFreeSpaceMargin < end;
        if (taken)
        {
            bool read = false;
            for (uint32_t i = at; i < end && !read; i++)
            {
                read = reached.Holds(i);
            }
            if (!read)
            {
                runs.push_back({start, end - kFreeSpaceMargin - start});
            }
        }
        at = end;
    }

    return runs;
}

std::vector<uint8_t> Join(std::span<const commands::Sequence> sequences, std::span<const sar::Piece> pieces,
                          std::span<const sar::Field> fields, const JoinOptions& options)
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

    // Each song in its place, or in the free space after the songs that the ROM keeps in their places. Each song keeps
    // the places of its items, and where its header lies. A header in its place takes the count of its tracks, up to
    // the count that the run holds. A header that gains tracks past that count goes with its song into the free space.
    const std::vector<uint32_t> tracks = HeaderTracks(pieces, fields);
    std::vector<std::vector<uint64_t>> items(pieces.size());
    std::map<std::size_t, uint64_t> copies; // the copy of each moved song's header, by the run that holds the header
    std::vector<std::size_t> moving;
    for (std::size_t p = 0; p < pieces.size(); p++)
    {
        const auto* sequence = std::get_if<sar::SequencePiece>(&pieces[p]);
        if (!sequence)
        {
            continue;
        }

        const auto* header = p + 1 < pieces.size() ? std::get_if<std::vector<uint8_t>>(&pieces[p + 1]) : nullptr;
        const bool grows = header && !header->empty() && tracks[p] > (*header)[0];
        if (header && !header->empty() && tracks[p] < (*header)[0])
        {
            image[starts[p + 1]] = uint8_t(tracks[p]);
        }

        const Encoded encoded = EncodeSequence(sequences[sequence->sequence], uint32_t(kRomBase + starts[p]));
        if (encoded.bytes.size() > sequence->room || grows)
        {
            moving.push_back(p);
            continue;
        }

        std::copy(encoded.bytes.begin(), encoded.bytes.end(), image.begin() + std::ptrdiff_t(starts[p]));
        for (const uint32_t offset : encoded.offsets)
        {
            items[p].push_back(starts[p] + offset);
        }
    }

    // Each place's position from the ROM's start. A place at the end lies at the end of the pieces. A song that moves
    // has its items once it has moved.
    const auto position = [&](const sar::Place& place) -> uint64_t
    {
        if (place.sequence || place.piece > pieces.size())
        {
            throw std::invalid_argument("a field names a place that a GBA ROM's pieces lack");
        }
        if (place.piece == pieces.size())
        {
            return size + place.offset;
        }

        const sar::Piece& piece = pieces[place.piece];
        if (place.item)
        {
            if (!std::holds_alternative<sar::SequencePiece>(piece) || place.offset >= items[place.piece].size())
            {
                throw std::invalid_argument("a field names an item that a song lacks");
            }
            return items[place.piece][place.offset];
        }
        if (const auto* bytes = std::get_if<std::vector<uint8_t>>(&piece))
        {
            if (place.offset > bytes->size())
            {
                throw std::invalid_argument("a field names a byte past its run");
            }
            const auto copy = copies.find(place.piece);
            return place.offset == 0 && copy != copies.end() ? copy->second : starts[place.piece] + place.offset;
        }
        if (place.offset != 0)
        {
            throw std::invalid_argument("a field names a byte inside a song");
        }
        return items[place.piece].front();
    };

    // The filler runs, found once a song needs them, and the next free byte of each.
    std::optional<std::vector<Span>> filler;
    std::vector<uint64_t> filled;
    const auto find_filler = [&]
    {
        // The ROM with the songs that keep their places, and with their tracks' addresses in their headers.
        std::vector<uint8_t> base = image;
        for (const sar::Field& field : fields)
        {
            const auto* run = field.at.piece < pieces.size() && !field.at.item && !field.at.sequence
                                  ? std::get_if<std::vector<uint8_t>>(&pieces[field.at.piece])
                                  : nullptr;
            const bool moves = field.to.item && field.to.piece < pieces.size() && items[field.to.piece].empty();
            if (!run || uint64_t(field.at.offset) + 4 > run->size() || moves)
            {
                continue;
            }
            try
            {
                const uint64_t from = position(field.from);
                const uint64_t to = position(field.to);
                if (from <= to && to - from + kRomBase <= std::numeric_limits<uint32_t>::max())
                {
                    xsflib::StoreLe32(&base[starts[field.at.piece] + field.at.offset], uint32_t(to - from + kRomBase));
                }
            }
            catch (const std::invalid_argument&)
            {
            }
        }

        agb::Rom rom;
        rom.Assign(std::move(base));
        DriverInfo info;
        std::string error;
        if (!DetectDriver(rom, {}, info, error))
        {
            throw std::invalid_argument("the ROM's sound driver can't be found for its filler runs" +
                                        (error.empty() ? std::string() : ": " + error));
        }

        filler = FillerRuns(rom, info);
        for (const Span& run : *filler)
        {
            filled.push_back(run.start);
        }
    };

    uint64_t free = moving.empty() ? 0 : FreeSpace(image);
    for (const std::size_t p : moving)
    {
        const auto* header = p + 1 < pieces.size() ? std::get_if<std::vector<uint8_t>>(&pieces[p + 1]) : nullptr;
        if (!header || header->empty() || header->size() < HeaderSize((*header)[0]))
        {
            throw std::invalid_argument("a song that has to move has no header after it");
        }

        if (tracks[p] > kMaxTracks)
        {
            throw std::invalid_argument("a song's header can't hold more than 16 tracks");
        }

        // The free space at the ROM's end, or else the first filler run with room. A song's encoding takes the same
        // size wherever it lies.
        const auto& sequence = std::get<sar::SequencePiece>(pieces[p]);
        const uint64_t song_size = EncodeSequence(sequences[sequence.sequence], uint32_t(kRomBase + free)).bytes.size();
        const uint32_t header_size = HeaderSize(tracks[p]);
        uint64_t at = free;
        bool at_end = AlignUp(free + song_size, 4) + header_size <= kLargestRom;
        if (!at_end)
        {
            if (!options.filler)
            {
                throw std::invalid_argument("the songs that outgrow their places don't fit in a GBA ROM's 32 MiB");
            }
            if (!filler)
            {
                find_filler();
            }

            std::size_t run = 0;
            while (run < filler->size() && AlignUp(AlignUp(filled[run], 4) + song_size, 4) + header_size >
                                               uint64_t((*filler)[run].start) + (*filler)[run].size)
            {
                run++;
            }
            if (run == filler->size())
            {
                throw std::invalid_argument(
                    "the songs that outgrow their places don't fit in a GBA ROM's 32 MiB or in its filler runs");
            }
            at = AlignUp(filled[run], 4);
            filled[run] = AlignUp(at + song_size, 4) + header_size;
        }

        const Encoded encoded = EncodeSequence(sequences[sequence.sequence], uint32_t(kRomBase + at));
        const uint64_t copy = AlignUp(at + encoded.bytes.size(), 4);
        const uint64_t end = copy + header_size;
        if (end > image.size())
        {
            image.resize(end, 0xFF);
        }

        // Zeros fill the gap before the copy, as they fill the gap before a header in place.
        std::copy(encoded.bytes.begin(), encoded.bytes.end(), image.begin() + std::ptrdiff_t(at));
        std::fill(image.begin() + std::ptrdiff_t(at + encoded.bytes.size()), image.begin() + std::ptrdiff_t(copy), 0);

        // The copy takes the header's tracks. Its addresses of tracks past the run's header come from their fields.
        const uint32_t kept = HeaderSize(std::min<uint32_t>(tracks[p], (*header)[0]));
        std::copy_n(header->begin(), kept, image.begin() + std::ptrdiff_t(copy));
        std::fill(image.begin() + std::ptrdiff_t(copy + kept), image.begin() + std::ptrdiff_t(end), 0);
        image[copy] = uint8_t(tracks[p]);
        for (const uint32_t offset : encoded.offsets)
        {
            items[p].push_back(at + offset);
        }
        copies[p + 1] = copy;
        if (at_end)
        {
            free = end;
        }
    }

    for (const sar::Field& field : fields)
    {
        const auto* run = field.at.piece < pieces.size() && !field.at.item && !field.at.sequence
                              ? std::get_if<std::vector<uint8_t>>(&pieces[field.at.piece])
                              : nullptr;

        // A track past the run's header has its address only in the copy.
        const bool track = IsTrackField(pieces, field);
        const bool past = run && track && !run->empty() && (field.at.offset - 8) / 4 >= (*run)[0];
        if (!run || (!past && uint64_t(field.at.offset) + 4 > run->size()))
        {
            throw std::invalid_argument("a field doesn't lie in a run of a GBA ROM");
        }

        const uint64_t from = position(field.from);
        const uint64_t to = position(field.to);
        if (to < from || to - from + kRomBase > std::numeric_limits<uint32_t>::max())
        {
            throw std::invalid_argument("a field's address doesn't fit in 32 bits");
        }

        const auto address = uint32_t(to - from + kRomBase);
        if (!past)
        {
            xsflib::StoreLe32(&image[starts[field.at.piece] + field.at.offset], address);
        }

        // The copy holds the tracks' addresses, and any other field that lies in the header that it keeps.
        const auto copy = copies.find(field.at.piece);
        if (copy != copies.end() &&
            (track || field.at.offset + 4 <= HeaderSize(std::min<uint32_t>(tracks[field.at.piece - 1], (*run)[0]))))
        {
            xsflib::StoreLe32(&image[copy->second + field.at.offset], address);
        }
    }

    return image;
}

} // namespace ntdlib::mp2k
