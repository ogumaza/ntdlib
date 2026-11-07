// SPDX-License-Identifier: MIT

#include "ntdlib/quintet/layout.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/quintet/song.h"

namespace ntdlib::quintet
{
namespace
{

using ntdlib::agb::kRomBase;

// A list of songs back to back, and its room.
struct List
{
    std::vector<uint32_t> songs; // each song's header, from the ROM's start
    uint32_t end = 0;            // the end of the last song
    bool terminator = false;     // a length of 0 follows the last song, and ends the list for the driver's loader
    uint32_t room_end = 0;       // the end of the list's room
    uint8_t fill = 0;
};

// The end of part `part` of the music file whose first word is at `file`, from the ROM's start: where the next part's
// size lies. Returns 0 if the file doesn't hold the part.
uint32_t PartEnd(const Rom& rom, uint32_t file, int part)
{
    uint32_t at = file;
    for (int p = 0; p <= part; p++)
    {
        if (!rom.Contains(at, 4))
        {
            return 0;
        }
        at += 4 + (rom.U32(at) & ~3u);
    }

    return at - kRomBase;
}

// The list of `songs`, whose room ends no later than `limit` from the ROM's start, or at the list's end without it.
// Returns nothing unless each song's header reads and the songs lie back to back.
std::optional<List> ListOf(const Rom& rom, const std::vector<uint32_t>& songs, uint32_t limit)
{
    List list;
    SongHeader header;
    for (size_t i = 0; i < songs.size(); i++)
    {
        if (!ReadSongHeader(rom, songs[i], header) ||
            (i > 0 && songs[i] != songs[i - 1] + (rom.U16(songs[i - 1]) & ~1u)))
        {
            return std::nullopt;
        }
        list.songs.push_back(songs[i] - kRomBase);
    }

    list.end = songs.back() - kRomBase + header.length;
    list.terminator = rom.Contains(kRomBase + list.end, 2) && rom.U16(kRomBase + list.end) == 0;
    list.room_end = list.end + (list.terminator ? 2 : 0);
    if (limit > list.room_end && limit <= rom.Size())
    {
        // The bytes up to the limit have to be one byte, repeated.
        const uint8_t fill = rom.U8(kRomBase + list.room_end);
        bool same = true;
        for (uint32_t at = list.room_end; at < limit && same; at++)
        {
            same = rom.U8(kRomBase + at) == fill;
        }
        if (same)
        {
            list.room_end = limit;
            list.fill = fill;
        }
    }

    return list;
}

// The lists of the music files, in the order of their places. Lists that overlap are left out.
std::vector<List> ListsOf(const Rom& rom, std::span<const DriverInfo> files)
{
    std::vector<List> lists;
    std::set<uint32_t> files_seen;
    for (const DriverInfo& info : files)
    {
        // The file's first word is 4 bytes before its songs. Without a file, the lists end at their last songs.
        const bool in_file = !info.music_files.empty();
        const uint32_t file = info.music_file - 4;
        if (in_file && !files_seen.insert(file).second)
        {
            continue;
        }

        // The songs are the file's first part, and the sound effects its second.
        const std::vector<uint32_t>* parts[] = {&info.song_addresses, &info.effect_addresses};
        for (int part = 0; part < 2; part++)
        {
            const std::vector<uint32_t>& songs = *parts[part];
            if (songs.empty())
            {
                continue;
            }
            if (std::optional<List> list = ListOf(rom, songs, in_file ? PartEnd(rom, file, part) : 0))
            {
                lists.push_back(std::move(*list));
            }
        }
    }

    std::sort(lists.begin(), lists.end(), [](const List& a, const List& b) { return a.songs[0] < b.songs[0]; });
    std::vector<List> apart;
    for (List& list : lists)
    {
        if (apart.empty() || apart.back().room_end <= list.songs[0])
        {
            apart.push_back(std::move(list));
        }
    }

    return apart;
}

// Adds `bytes` to the run at the end of `pieces`, or starts one.
void AddRun(std::vector<sar::Piece>& pieces, std::span<const uint8_t> bytes)
{
    if (bytes.empty())
    {
        return;
    }
    if (pieces.empty() || !std::holds_alternative<std::vector<uint8_t>>(pieces.back()))
    {
        pieces.emplace_back(std::vector<uint8_t>());
    }

    std::vector<uint8_t>& run = std::get<std::vector<uint8_t>>(pieces.back());
    run.insert(run.end(), bytes.begin(), bytes.end());
}

// The places of a song's header's fields.
sar::Place PieceStart(uint32_t piece)
{
    return {piece, 0, false, false};
}

} // namespace

Layout Cut(const Rom& rom, std::span<const DriverInfo> files)
{
    Layout layout;
    if (files.empty())
    {
        layout.pieces.emplace_back(std::vector<uint8_t>(rom.Ptr(kRomBase), rom.Ptr(kRomBase) + rom.Size()));
        return layout;
    }

    layout.traits = TraitsOf(rom, files[0]);
    const std::span<const uint8_t> bytes(rom.Ptr(kRomBase), rom.Size());
    uint32_t at = 0;
    for (const List& list : ListsOf(rom, files))
    {
        AddRun(layout.pieces, bytes.subspan(at, list.songs[0] - at));
        for (const uint32_t song : list.songs)
        {
            SongHeader header;
            ReadSongHeader(rom, kRomBase + song, header);
            const bool even = (rom.U16(kRomBase + song) & 1) == 0;
            Decoded decoded;
            bool cut = false;
            if (even)
            {
                const std::span<const uint8_t> body = bytes.subspan(song + kHeaderBytes, header.length - kHeaderBytes);
                std::vector<uint32_t> starts;
                for (const uint32_t channel : header.channels)
                {
                    starts.push_back(channel - kRomBase - song - kHeaderBytes);
                }
                decoded = DecodeSequence(body, starts, layout.traits);
                if (!decoded.opaque)
                {
                    const Encoded encoded = EncodeSequence(decoded.sequence, decoded.entries, layout.traits);
                    cut = std::equal(encoded.bytes.begin(), encoded.bytes.end(), body.begin(), body.end());
                }
            }

            if (!cut)
            {
                AddRun(layout.pieces, bytes.subspan(song, header.length));
                continue;
            }

            // The header, as a run of its own, and the commands.
            const uint32_t header_piece = uint32_t(layout.pieces.size());
            layout.pieces.emplace_back(std::vector<uint8_t>(bytes.begin() + song, bytes.begin() + song + kHeaderBytes));
            const uint32_t body_piece = uint32_t(layout.pieces.size());
            const uint32_t sequence = uint32_t(layout.sequences.size());
            layout.pieces.emplace_back(sar::SequencePiece{sequence, 0, 0});

            sar::Field length;
            length.at = PieceStart(header_piece);
            length.from = PieceStart(header_piece);
            length.to = {body_piece, uint32_t(decoded.sequence.items.size()), true, false};
            length.bytes = 2;
            layout.fields.push_back(length);
            for (size_t c = 0; c < decoded.entries.size(); c++)
            {
                sar::Field offset;
                offset.at = {header_piece, uint32_t(2 + 2 * c), false, false};
                offset.from = PieceStart(header_piece);
                offset.to = {body_piece, decoded.entries[c], true, false};
                offset.bytes = 2;
                layout.fields.push_back(offset);
            }
            layout.sequences.push_back(std::move(decoded.sequence));
        }

        // The length of 0 after the list moves with it. The room's fill follows, and the next piece starts at the
        // room's end.
        AddRun(layout.pieces, bytes.subspan(list.end, list.terminator ? 2 : 0));
        layout.rooms.push_back({uint32_t(layout.pieces.size()), list.room_end, list.fill});
        layout.pieces.emplace_back(std::vector<uint8_t>());
        at = list.room_end;
    }
    AddRun(layout.pieces, bytes.subspan(at));

    if (layout.sequences.empty())
    {
        layout = Layout();
        layout.pieces.emplace_back(std::vector<uint8_t>(bytes.begin(), bytes.end()));
    }

    return layout;
}

std::vector<uint8_t> Join(const Traits& traits, std::span<const commands::Sequence> sequences,
                          std::span<const sar::Piece> pieces, std::span<const sar::Field> fields,
                          std::span<const Room> rooms)
{
    // Each sequence's entries: the items that the fields name.
    std::map<uint32_t, std::set<uint32_t>> entries;
    for (const sar::Field& field : fields)
    {
        if (field.to.item && field.to.piece < pieces.size())
        {
            if (const auto* piece = std::get_if<sar::SequencePiece>(&pieces[field.to.piece]))
            {
                if (piece->sequence < sequences.size() && field.to.offset < sequences[piece->sequence].items.size())
                {
                    entries[field.to.piece].insert(field.to.offset);
                }
            }
        }
    }

    // The pieces' bytes and places.
    std::vector<uint8_t> out;
    std::vector<uint32_t> starts;
    std::vector<uint32_t> ends;                        // each piece's end, before a room's fill
    std::map<uint32_t, std::vector<uint32_t>> offsets; // each sequence piece's items' offsets
    std::map<uint32_t, const Room*> room_at;
    for (const Room& room : rooms)
    {
        room_at[room.piece] = &room;
    }

    for (uint32_t p = 0; p <= pieces.size(); p++)
    {
        const auto room = room_at.find(p);
        if (room != room_at.end())
        {
            if (out.size() > room->second->end)
            {
                throw std::invalid_argument("a list of songs outgrows its room");
            }
            out.resize(room->second->end, room->second->fill);
        }
        starts.push_back(uint32_t(out.size()));
        if (p == pieces.size())
        {
            break;
        }

        const sar::Piece& piece = pieces[p];
        if (const auto* run = std::get_if<std::vector<uint8_t>>(&piece))
        {
            out.insert(out.end(), run->begin(), run->end());
            ends.push_back(uint32_t(out.size()));
            continue;
        }
        if (std::holds_alternative<sar::Padding>(piece))
        {
            throw std::invalid_argument("a ROM of Quintet's driver holds no padding");
        }

        const sar::SequencePiece& sequence = std::get<sar::SequencePiece>(piece);
        if (sequence.sequence >= sequences.size())
        {
            throw std::invalid_argument("a sequence piece names a sequence that the layout lacks");
        }
        const std::set<uint32_t>& named = entries[p];
        const std::vector<uint32_t> starts_of(named.begin(), named.end());
        const Encoded encoded = EncodeSequence(sequences[sequence.sequence], starts_of, traits);
        out.insert(out.end(), encoded.bytes.begin(), encoded.bytes.end());
        if (out.size() % 2 != 0)
        {
            out.push_back(0);
        }
        ends.push_back(uint32_t(out.size()));
        offsets[p] = encoded.offsets;
    }

    // The position of a place.
    const auto position = [&](const sar::Place& place) -> uint32_t
    {
        if (place.sequence || place.piece >= pieces.size())
        {
            throw std::invalid_argument("a field names a place that the pieces lack");
        }
        if (!place.item)
        {
            if (place.offset > ends[place.piece] - starts[place.piece])
            {
                throw std::invalid_argument("a field names a place past its piece");
            }
            return starts[place.piece] + place.offset;
        }

        const auto found = offsets.find(place.piece);
        if (found == offsets.end() || place.offset >= found->second.size())
        {
            throw std::invalid_argument("a field names an item that its sequence lacks");
        }

        // The end of the items is the end of the piece, with its zero.
        return place.offset + 1 == found->second.size() ? ends[place.piece]
                                                        : starts[place.piece] + found->second[place.offset];
    };

    for (const sar::Field& field : fields)
    {
        if (field.bytes != 2 || field.at.item || field.at.piece >= pieces.size() ||
            !std::holds_alternative<std::vector<uint8_t>>(pieces[field.at.piece]) ||
            field.at.offset + 2 > std::get<std::vector<uint8_t>>(pieces[field.at.piece]).size())
        {
            throw std::invalid_argument("a field doesn't lie in a run");
        }

        const uint32_t from = position(field.from);
        const uint32_t to = position(field.to);
        if (to < from || to - from > 0xFFFF)
        {
            throw std::invalid_argument("a song's header can't hold the distance");
        }

        const uint32_t at = starts[field.at.piece] + field.at.offset;
        out[at] = uint8_t(to - from);
        out[at + 1] = uint8_t((to - from) >> 8);
    }

    return out;
}

std::vector<uint8_t> Join(const Layout& layout)
{
    return Join(layout.traits, layout.sequences, layout.pieces, layout.fields, layout.rooms);
}

} // namespace ntdlib::quintet
