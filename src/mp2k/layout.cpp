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
#include <span>
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

using agb::AlignUp;

// The bytes of a ROM that the driver reads, as FillerRuns finds them. A note reads up to `note_arguments` bytes.
class Reached : public agb::Reached
{
public:
    Reached(const agb::Rom& rom, int note_arguments) : agb::Reached(rom), rom_(rom), note_arguments_(note_arguments)
    {
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
                if (!DecodeEvent(rom_, at, running, event, note_arguments_))
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
    int note_arguments_ = 3;
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

namespace
{

// Each song of the table once, by its header, by the address of the song's first bytes. `entries` takes the song
// table's entries, by the headers that they name.
std::map<uint32_t, CutSong> ReadCutSongs(const agb::Rom& rom, const DriverInfo& info,
                                         std::multimap<uint32_t, int>& entries)
{
    std::map<uint32_t, CutSong> songs;
    std::set<uint32_t> headers;
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
            song.decoded = DecodeSong(rom, header, NoteArguments(info));
            if (song.decoded.opaque)
            {
                continue;
            }

            const Encoded encoded = EncodeSequence(song.decoded.sequence, song.base, NoteArguments(info));
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

    return songs;
}

// The songs that lie clear of one another and of the song table, in the order of their first bytes.
std::vector<CutSong*> ClearSongs(std::map<uint32_t, CutSong>& songs, const DriverInfo& info)
{
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

    return kept;
}

// Adds the fields of the song table's entries. An entry's field lies in the run that holds the table: the songs lie
// clear of it. `header_runs` gives each header's run, by the header's address.
void AddTableFields(const DriverInfo& info, const std::multimap<uint32_t, int>& entries,
                    const std::map<uint32_t, uint32_t>& header_runs, Layout& layout)
{
    for (const auto& [header, run] : header_runs)
    {
        const auto [first, last] = entries.equal_range(header);
        for (auto e = first; e != last; e++)
        {
            const uint32_t entry = info.song_table + 8 * uint32_t(e->second);
            if (const std::optional<sar::Place> place = agb::RunPlaceOf(layout.pieces, entry))
            {
                layout.fields.push_back({*place, {0, 0}, {run, 0}});
            }
        }
    }
}

} // namespace

uint64_t FreeSpaceStart(std::span<const uint8_t> end, uint64_t offset)
{
    std::size_t run = end.size();
    while (run > 0 && end[run - 1] == 0xFF)
    {
        run--;
    }

    return run < end.size() ? AlignUp(offset + run + kFreeSpaceMargin, kFreeSpaceMargin)
                            : AlignUp(offset + end.size(), 4);
}

Layout Cut(const agb::Rom& rom, const DriverInfo& info)
{
    std::multimap<uint32_t, int> entries;
    std::map<uint32_t, CutSong> songs = ReadCutSongs(rom, info, entries);
    const std::vector<CutSong*> kept = ClearSongs(songs, info);

    // The runs between the songs, and the fields of the headers and of the song table.
    Layout layout;
    layout.note_arguments = NoteArguments(info);
    uint32_t at = kRomBase;                   // where the next run starts
    std::map<uint32_t, uint32_t> header_runs; // each header's run, by the header's address
    for (CutSong* song : kept)
    {
        agb::AddRomRun(rom, at, song->base, layout.pieces);
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
    agb::AddRomRun(rom, at, rom.End(), layout.pieces);

    AddTableFields(info, entries, header_runs, layout);

    return layout;
}

std::vector<Span> FillerRuns(const agb::Rom& rom, const DriverInfo& info)
{
    // The bytes that the driver reads: its tables, and each song with what it plays.
    Reached reached(rom, NoteArguments(info));
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
    return agb::FillerRunsOf(rom, reached, kLeastFillerRun, false);
}

namespace
{

// The join of a ROM's pieces, a step at a time: the runs in their places, the songs that keep their places, the songs
// that move, then the fields.
class Joiner
{
public:
    Joiner(std::span<const commands::Sequence> sequences, std::span<const sar::Piece> pieces,
           std::span<const sar::Field> fields, const JoinOptions& options)
        : sequences_(sequences), pieces_(pieces), fields_(fields), options_(options), items_(pieces.size())
    {
    }

    // Each piece's place, from the ROM's start, and the runs' bytes in their places.
    void PlaceRuns()
    {
        for (const sar::Piece& piece : pieces_)
        {
            starts_.push_back(size_);
            if (const auto* bytes = std::get_if<std::vector<uint8_t>>(&piece))
            {
                size_ += bytes->size();
            }
            else if (const auto* sequence = std::get_if<sar::SequencePiece>(&piece))
            {
                if (sequence->sequence >= sequences_.size())
                {
                    throw std::invalid_argument("a song names a sequence that the layout lacks");
                }
                size_ += sequence->room;
            }
            else
            {
                throw std::invalid_argument("a GBA ROM's layout holds padding");
            }
        }
        if (size_ > kLargestRom)
        {
            throw std::invalid_argument("a GBA ROM can't hold more than 32 MiB");
        }

        image_.resize(size_);
        for (std::size_t p = 0; p < pieces_.size(); p++)
        {
            if (const auto* bytes = std::get_if<std::vector<uint8_t>>(&pieces_[p]))
            {
                std::copy(bytes->begin(), bytes->end(), image_.begin() + std::ptrdiff_t(starts_[p]));
            }
        }
    }

    // Each song in its place. Each song keeps the places of its items, and where its header lies. A header in its
    // place takes the count of its tracks, up to the count that the run holds. A header that gains tracks past that
    // count goes with its song into the free space. Returns the songs that move.
    std::vector<std::size_t> PlaceSongs()
    {
        tracks_ = HeaderTracks(pieces_, fields_);
        std::vector<std::size_t> moving;
        for (std::size_t p = 0; p < pieces_.size(); p++)
        {
            const auto* sequence = std::get_if<sar::SequencePiece>(&pieces_[p]);
            if (!sequence)
            {
                continue;
            }

            const std::vector<uint8_t>* header = HeaderAfter(p);
            const bool grows = header && !header->empty() && tracks_[p] > (*header)[0];
            if (header && !header->empty() && tracks_[p] < (*header)[0])
            {
                image_[starts_[p + 1]] = uint8_t(tracks_[p]);
            }

            const Encoded encoded = EncodeSequence(sequences_[sequence->sequence], uint32_t(kRomBase + starts_[p]),
                                                   options_.note_arguments);
            if (encoded.bytes.size() > sequence->room || grows)
            {
                moving.push_back(p);
                continue;
            }

            std::copy(encoded.bytes.begin(), encoded.bytes.end(), image_.begin() + std::ptrdiff_t(starts_[p]));
            for (const uint32_t offset : encoded.offsets)
            {
                items_[p].push_back(starts_[p] + offset);
            }
        }

        return moving;
    }

    // Moves each song of `moving` into the free space after the songs that the ROM keeps in their places, or else into
    // the first filler run with room.
    void MoveSongs(const std::vector<std::size_t>& moving)
    {
        uint64_t free = moving.empty() ? 0 : FreeSpaceStart(image_);
        for (const std::size_t p : moving)
        {
            const std::vector<uint8_t>* header = HeaderAfter(p);
            if (!header || header->empty() || header->size() < HeaderSize((*header)[0]))
            {
                throw std::invalid_argument("a song that has to move has no header after it");
            }

            if (tracks_[p] > kMaxTracks)
            {
                throw std::invalid_argument("a song's header can't hold more than 16 tracks");
            }

            // The free space at the ROM's end, or else the first filler run with room. A song's encoding takes the same
            // size wherever it lies.
            const auto& sequence = std::get<sar::SequencePiece>(pieces_[p]);
            const uint64_t song_size =
                EncodeSequence(sequences_[sequence.sequence], uint32_t(kRomBase + free), options_.note_arguments)
                    .bytes.size();
            const uint32_t header_size = HeaderSize(tracks_[p]);
            const bool at_end = AlignUp(free + song_size, 4) + header_size <= kLargestRom;
            const uint64_t at = at_end ? free : FillerPlace(song_size, header_size);
            const uint64_t end = MoveSong(p, *header, at);
            if (at_end)
            {
                free = end;
            }
        }
    }

    // Writes each field's address, in its run and in the copy of a moved song's header.
    std::vector<uint8_t> WriteFields()
    {
        for (const sar::Field& field : fields_)
        {
            const auto* run = field.at.piece < pieces_.size() && !field.at.item && !field.at.sequence
                                  ? std::get_if<std::vector<uint8_t>>(&pieces_[field.at.piece])
                                  : nullptr;

            // A track past the run's header has its address only in the copy.
            const bool track = IsTrackField(pieces_, field);
            const bool past = run && track && !run->empty() && (field.at.offset - 8) / 4 >= (*run)[0];
            if (!run || (!past && uint64_t(field.at.offset) + 4 > run->size()))
            {
                throw std::invalid_argument("a field doesn't lie in a run of a GBA ROM");
            }

            const uint64_t from = Position(field.from);
            const uint64_t to = Position(field.to);
            if (to < from || to - from + kRomBase > std::numeric_limits<uint32_t>::max())
            {
                throw std::invalid_argument("a field's address doesn't fit in 32 bits");
            }

            const auto address = uint32_t(to - from + kRomBase);
            if (!past)
            {
                xsflib::StoreLe32(&image_[starts_[field.at.piece] + field.at.offset], address);
            }

            // The copy holds the tracks' addresses, and any other field that lies in the header that it keeps.
            const auto copy = copies_.find(field.at.piece);
            if (copy != copies_.end() && (track || field.at.offset + 4 <= HeaderSize(std::min<uint32_t>(
                                                                              tracks_[field.at.piece - 1], (*run)[0]))))
            {
                xsflib::StoreLe32(&image_[copy->second + field.at.offset], address);
            }
        }

        return std::move(image_);
    }

private:
    // The run after song `p`, which holds its header, or nullptr.
    const std::vector<uint8_t>* HeaderAfter(std::size_t p) const
    {
        return p + 1 < pieces_.size() ? std::get_if<std::vector<uint8_t>>(&pieces_[p + 1]) : nullptr;
    }

    // Each place's position from the ROM's start. A place at the end lies at the end of the pieces. A song that moves
    // has its items once it has moved.
    uint64_t Position(const sar::Place& place) const
    {
        if (place.sequence || place.piece > pieces_.size())
        {
            throw std::invalid_argument("a field names a place that a GBA ROM's pieces lack");
        }
        if (place.piece == pieces_.size())
        {
            return size_ + place.offset;
        }

        const sar::Piece& piece = pieces_[place.piece];
        if (place.item)
        {
            if (!std::holds_alternative<sar::SequencePiece>(piece) || place.offset >= items_[place.piece].size())
            {
                throw std::invalid_argument("a field names an item that a song lacks");
            }
            return items_[place.piece][place.offset];
        }
        if (const auto* bytes = std::get_if<std::vector<uint8_t>>(&piece))
        {
            if (place.offset > bytes->size())
            {
                throw std::invalid_argument("a field names a byte past its run");
            }
            const auto copy = copies_.find(place.piece);
            return place.offset == 0 && copy != copies_.end() ? copy->second : starts_[place.piece] + place.offset;
        }
        if (place.offset != 0)
        {
            throw std::invalid_argument("a field names a byte inside a song");
        }
        return items_[place.piece].front();
    }

    // Finds the filler runs, once a song needs them, and the next free byte of each.
    void FindFiller()
    {
        // The ROM with the songs that keep their places, and with their tracks' addresses in their headers.
        std::vector<uint8_t> base = image_;
        for (const sar::Field& field : fields_)
        {
            const auto* run = field.at.piece < pieces_.size() && !field.at.item && !field.at.sequence
                                  ? std::get_if<std::vector<uint8_t>>(&pieces_[field.at.piece])
                                  : nullptr;
            const bool moves = field.to.item && field.to.piece < pieces_.size() && items_[field.to.piece].empty();
            if (!run || uint64_t(field.at.offset) + 4 > run->size() || moves)
            {
                continue;
            }
            try
            {
                const uint64_t from = Position(field.from);
                const uint64_t to = Position(field.to);
                if (from <= to && to - from + kRomBase <= std::numeric_limits<uint32_t>::max())
                {
                    xsflib::StoreLe32(&base[starts_[field.at.piece] + field.at.offset], uint32_t(to - from + kRomBase));
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

        filler_ = FillerRuns(rom, info);
        for (const Span& run : *filler_)
        {
            filled_.push_back(run.start);
        }
    }

    // The place in the first filler run with room for a song of `song_size` bytes and a header of `header_size`.
    uint64_t FillerPlace(uint64_t song_size, uint32_t header_size)
    {
        if (!options_.filler)
        {
            throw std::invalid_argument("the songs that outgrow their places don't fit in a GBA ROM's 32 MiB");
        }
        if (!filler_)
        {
            FindFiller();
        }

        std::size_t run = 0;
        while (run < filler_->size() && AlignUp(AlignUp(filled_[run], 4) + song_size, 4) + header_size >
                                            uint64_t((*filler_)[run].start) + (*filler_)[run].size)
        {
            run++;
        }
        if (run == filler_->size())
        {
            throw std::invalid_argument(
                "the songs that outgrow their places don't fit in a GBA ROM's 32 MiB or in its filler runs");
        }
        const uint64_t at = AlignUp(filled_[run], 4);
        filled_[run] = AlignUp(at + song_size, 4) + header_size;

        return at;
    }

    // Writes song `p` at `at`, with a copy of its header after it. Returns the end of the copy.
    uint64_t MoveSong(std::size_t p, const std::vector<uint8_t>& header, uint64_t at)
    {
        const auto& sequence = std::get<sar::SequencePiece>(pieces_[p]);
        const uint32_t header_size = HeaderSize(tracks_[p]);
        const Encoded encoded =
            EncodeSequence(sequences_[sequence.sequence], uint32_t(kRomBase + at), options_.note_arguments);
        const uint64_t copy = AlignUp(at + encoded.bytes.size(), 4);
        const uint64_t end = copy + header_size;
        if (end > image_.size())
        {
            image_.resize(end, 0xFF);
        }

        // Zeros fill the gap before the copy, as they fill the gap before a header in place.
        std::copy(encoded.bytes.begin(), encoded.bytes.end(), image_.begin() + std::ptrdiff_t(at));
        std::fill(image_.begin() + std::ptrdiff_t(at + encoded.bytes.size()), image_.begin() + std::ptrdiff_t(copy), 0);

        // The copy takes the header's tracks. Its addresses of tracks past the run's header come from their fields.
        const uint32_t kept = HeaderSize(std::min<uint32_t>(tracks_[p], header[0]));
        std::copy_n(header.begin(), kept, image_.begin() + std::ptrdiff_t(copy));
        std::fill(image_.begin() + std::ptrdiff_t(copy + kept), image_.begin() + std::ptrdiff_t(end), 0);
        image_[copy] = uint8_t(tracks_[p]);
        for (const uint32_t offset : encoded.offsets)
        {
            items_[p].push_back(at + offset);
        }
        copies_[p + 1] = copy;

        return end;
    }

    std::span<const commands::Sequence> sequences_;
    std::span<const sar::Piece> pieces_;
    std::span<const sar::Field> fields_;
    const JoinOptions& options_;
    std::vector<uint64_t> starts_;
    uint64_t size_ = 0;
    std::vector<uint8_t> image_;
    std::vector<uint32_t> tracks_;
    std::vector<std::vector<uint64_t>> items_;
    std::map<std::size_t, uint64_t> copies_; // the copy of each moved song's header, by the run that holds the header
    std::optional<std::vector<Span>> filler_;
    std::vector<uint64_t> filled_;
};

} // namespace

std::vector<uint8_t> Join(std::span<const commands::Sequence> sequences, std::span<const sar::Piece> pieces,
                          std::span<const sar::Field> fields, const JoinOptions& options)
{
    Joiner joiner(sequences, pieces, fields, options);
    joiner.PlaceRuns();
    joiner.MoveSongs(joiner.PlaceSongs());

    return joiner.WriteFields();
}

} // namespace ntdlib::mp2k
