// SPDX-License-Identifier: MIT

#include "ntdlib/dse/layout.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "ntdlib/dse/commands.h"
#include "ntdlib/dse/formats.h"

namespace ntdlib::dse
{
namespace
{

using sar::Field;
using sar::Padding;
using sar::Piece;
using sar::Place;
using sar::SequencePiece;

constexpr char kSetMagic[4] = {'D', 'S', 'E', 'T'};
constexpr uint32_t kSetHeader = 16;
constexpr uint32_t kSetEntry = 12;
constexpr uint32_t kFileAlignment = 16;

constexpr uint32_t kFileSizeField = 8;
constexpr uint32_t kSongTracks = 0x80;       // a song's header, its song chunk and its settings
constexpr uint32_t kEffectsChunkSize = 0x4C; // the seq chunk's size in a bank of sound effects
constexpr uint32_t kEffectsBase = 0x50;      // what the seq chunk's table counts from
constexpr uint32_t kEndChunk = 16;           // an eoc chunk

uint64_t AlignUp(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

bool HasMagic(std::span<const uint8_t> bytes, const char* magic)
{
    return bytes.size() >= 4 && std::memcmp(bytes.data(), magic, 4) == 0;
}

bool IsZeros(std::span<const uint8_t> bytes)
{
    return std::ranges::all_of(bytes, [](uint8_t byte) { return byte == 0; });
}

Place ByteOf(std::size_t piece, std::size_t offset = 0)
{
    return {.piece = static_cast<uint32_t>(piece), .offset = static_cast<uint32_t>(offset)};
}

std::vector<uint8_t> RunOf(std::span<const uint8_t> bytes)
{
    return {bytes.begin(), bytes.end()};
}

// A sequence, and the bytes that it takes.
struct SizedSequence
{
    commands::Sequence sequence;
    std::size_t size = 0;
};

// The sequence of `tracks`, when the encoder gives back `file`'s bytes at `at`. Nothing otherwise.
std::optional<SizedSequence> SequenceAt(std::span<const Track> tracks, std::span<const uint8_t> file, std::size_t at)
{
    commands::Sequence sequence = DecodeTracks(tracks);
    const std::vector<uint8_t> bytes = EncodeTracks(sequence).bytes;
    if (at + bytes.size() > file.size() || !std::ranges::equal(bytes, file.subspan(at, bytes.size())))
    {
        return std::nullopt;
    }

    return SizedSequence{std::move(sequence), bytes.size()};
}

// A file's part of the layout. Its places count from the file's first piece. A place one past its last piece names
// the padding that follows the file.
struct CutFile
{
    std::vector<commands::Sequence> sequences = {};
    std::vector<Piece> pieces = {};
    std::vector<Field> fields = {};
};

std::optional<CutFile> CutSong(const NamedFile& file)
{
    const Song song = Song::Parse(file.bytes);
    std::optional<SizedSequence> sized = SequenceAt(song.tracks, file.bytes, kSongTracks);
    if (!sized)
    {
        return std::nullopt;
    }

    sized->sequence.labels.push_back({SequenceLabel({.path = file.path}), 0});
    CutFile cut;
    cut.sequences.push_back(std::move(sized->sequence));
    cut.pieces.emplace_back(RunOf(file.bytes.first(kSongTracks)));
    cut.pieces.emplace_back(SequencePiece{});
    cut.pieces.emplace_back(RunOf(file.bytes.subspan(kSongTracks + sized->size)));
    cut.fields.push_back({ByteOf(0, kFileSizeField), ByteOf(0), ByteOf(3)});

    return cut;
}

std::optional<CutFile> CutEffects(const NamedFile& file)
{
    const SoundEffects effects = SoundEffects::Parse(file.bytes);
    const xsflib::Reader r(file.bytes);
    const std::size_t chunk_end = kEffectsBase + r.U32(kEffectsChunkSize);

    CutFile cut;
    std::size_t run_start = 0;
    std::vector<std::pair<std::size_t, Place>> slots;
    for (std::size_t slot = 0; slot < effects.sequences.size(); slot++)
    {
        if (!effects.sequences[slot])
        {
            continue;
        }

        const std::size_t info = kEffectsBase + std::size_t{r.U16(kEffectsBase + 2 * slot)};
        const std::size_t tracks = info + SequenceInfo::kSize;
        std::optional<SizedSequence> sized = SequenceAt(effects.sequences[slot]->tracks, file.bytes, tracks);
        if (!sized)
        {
            return std::nullopt;
        }

        sized->sequence.labels.push_back(
            {SequenceLabel({.path = file.path, .effect = static_cast<uint16_t>(slot)}), 0});
        slots.emplace_back(slot, ByteOf(cut.pieces.size(), info - run_start));
        cut.pieces.emplace_back(RunOf(file.bytes.subspan(run_start, tracks - run_start)));
        cut.pieces.emplace_back(SequencePiece{.sequence = static_cast<uint32_t>(cut.sequences.size())});
        cut.sequences.push_back(std::move(sized->sequence));
        run_start = tracks + sized->size;
    }
    if (cut.sequences.empty() || run_start + kEndChunk != chunk_end)
    {
        return std::nullopt;
    }

    // The last eoc chunk ends the seq chunk. Zeros bring what follows to a multiple of 16 bytes.
    const std::size_t next = AlignUp(chunk_end, kFileAlignment);
    if (chunk_end % 4 != 0 || next > file.bytes.size() || !IsZeros(file.bytes.subspan(chunk_end, next - chunk_end)))
    {
        return std::nullopt;
    }

    cut.pieces.emplace_back(RunOf(file.bytes.subspan(run_start, chunk_end - run_start)));
    const std::size_t padding = cut.pieces.size();
    cut.pieces.emplace_back(Padding{kFileAlignment});
    cut.pieces.emplace_back(RunOf(file.bytes.subspan(next)));
    const std::size_t end = cut.pieces.size();

    cut.fields.push_back({ByteOf(0, kFileSizeField), ByteOf(0), ByteOf(end)});
    cut.fields.push_back({ByteOf(0, kEffectsChunkSize), ByteOf(0, kEffectsBase), ByteOf(padding)});
    for (const auto& [slot, place] : slots)
    {
        cut.fields.push_back({ByteOf(0, kEffectsBase + 2 * slot), ByteOf(0, kEffectsBase), place, 2});
    }

    return cut;
}

// A song or a bank of sound effects as pieces, or nothing for any other file and for one that the readers or the
// encoder can't keep.
std::optional<CutFile> CutSequences(const NamedFile& file)
{
    try
    {
        if (HasMagic(file.bytes, "smdl"))
        {
            return CutSong(file);
        }
        if (HasMagic(file.bytes, "sedl"))
        {
            return CutEffects(file);
        }
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }

    return std::nullopt;
}

// The bytes of each sequence piece of `pieces`, by the piece.
std::vector<std::vector<uint8_t>> EncodedPieces(std::span<const commands::Sequence> sequences,
                                                std::span<const Piece> pieces)
{
    std::vector<std::vector<uint8_t>> encoded(pieces.size());
    for (std::size_t p = 0; p < pieces.size(); p++)
    {
        if (const auto* sequence = std::get_if<SequencePiece>(&pieces[p]))
        {
            if (sequence->sequence >= sequences.size())
            {
                throw std::invalid_argument("a sequence piece names a sequence that the set lacks");
            }
            encoded[p] = EncodeTracks(sequences[sequence->sequence]).bytes;
        }
    }

    return encoded;
}

std::vector<uint64_t> StartsOf(std::span<const Piece> pieces, std::span<const std::vector<uint8_t>> encoded)
{
    std::vector<uint64_t> starts(pieces.size() + 1);
    uint64_t size = 0;
    for (std::size_t p = 0; p < pieces.size(); p++)
    {
        starts[p] = size;
        if (const auto* run = std::get_if<std::vector<uint8_t>>(&pieces[p]))
        {
            size += run->size();
        }
        else if (std::holds_alternative<SequencePiece>(pieces[p]))
        {
            size += encoded[p].size();
        }
        else
        {
            size = AlignUp(size, std::max<uint32_t>(std::get<Padding>(pieces[p]).alignment, 1));
        }
    }
    starts[pieces.size()] = size;

    return starts;
}

} // namespace

Layout Cut(std::span<const NamedFile> files)
{
    Layout layout;

    // The directory, with the paths.
    std::vector<uint8_t> directory(kSetHeader + kSetEntry * files.size());
    std::memcpy(directory.data(), kSetMagic, sizeof(kSetMagic));
    xsflib::StoreLe32(&directory[8], static_cast<uint32_t>(files.size()));
    for (std::size_t i = 0; i < files.size(); i++)
    {
        xsflib::StoreLe32(&directory[kSetHeader + kSetEntry * i + 8], static_cast<uint32_t>(directory.size()));
        directory.insert(directory.end(), files[i].path.begin(), files[i].path.end());
        directory.push_back(0);
    }
    layout.pieces.emplace_back(std::move(directory));
    layout.pieces.emplace_back(Padding{kFileAlignment});

    for (std::size_t i = 0; i < files.size(); i++)
    {
        const std::size_t first = layout.pieces.size();
        const auto first_sequence = static_cast<uint32_t>(layout.sequences.size());
        if (std::optional<CutFile> cut = CutSequences(files[i]))
        {
            for (Piece& piece : cut->pieces)
            {
                if (auto* sequence = std::get_if<SequencePiece>(&piece))
                {
                    sequence->sequence += first_sequence;
                }
                layout.pieces.push_back(std::move(piece));
            }
            for (commands::Sequence& sequence : cut->sequences)
            {
                layout.sequences.push_back(std::move(sequence));
            }
            for (Field field : cut->fields)
            {
                field.at.piece += static_cast<uint32_t>(first);
                field.from.piece += static_cast<uint32_t>(first);
                field.to.piece += static_cast<uint32_t>(first);
                layout.fields.push_back(field);
            }
        }
        else
        {
            layout.pieces.emplace_back(RunOf(files[i].bytes));
        }

        const std::size_t end = layout.pieces.size();
        layout.pieces.emplace_back(Padding{kFileAlignment});

        const uint32_t entry = kSetHeader + kSetEntry * static_cast<uint32_t>(i);
        layout.fields.push_back({ByteOf(0, entry), ByteOf(0), ByteOf(first)});
        layout.fields.push_back({ByteOf(0, entry + 4), ByteOf(first), ByteOf(end)});
    }
    layout.fields.push_back({ByteOf(0, 4), ByteOf(0), ByteOf(layout.pieces.size())});

    return layout;
}

std::vector<uint64_t> Starts(std::span<const commands::Sequence> sequences, std::span<const Piece> pieces)
{
    return StartsOf(pieces, EncodedPieces(sequences, pieces));
}

std::vector<uint8_t> Join(std::span<const commands::Sequence> sequences, std::span<const Piece> pieces,
                          std::span<const Field> fields)
{
    const std::vector<std::vector<uint8_t>> encoded = EncodedPieces(sequences, pieces);
    const std::vector<uint64_t> starts = StartsOf(pieces, encoded);
    const uint64_t size = starts.back();
    if (size > std::numeric_limits<uint32_t>::max())
    {
        throw std::invalid_argument("a set of DSE files can't hold more than 4 GiB");
    }

    std::vector<uint8_t> set(size);
    for (std::size_t p = 0; p < pieces.size(); p++)
    {
        const auto* run = std::get_if<std::vector<uint8_t>>(&pieces[p]);
        const std::vector<uint8_t>& bytes = run ? *run : encoded[p];
        std::ranges::copy(bytes, set.begin() + static_cast<std::ptrdiff_t>(starts[p]));
    }

    const auto position = [&](const Place& place) -> uint64_t
    {
        if (place.sequence || place.item || place.piece > pieces.size())
        {
            throw std::invalid_argument("a field names a place that the set's pieces lack");
        }
        if (place.piece == pieces.size())
        {
            return size + place.offset;
        }

        const auto* run = std::get_if<std::vector<uint8_t>>(&pieces[place.piece]);
        if (run ? place.offset > run->size() : place.offset != 0)
        {
            throw std::invalid_argument("a field names a byte past its run, or inside a sequence or padding");
        }
        return starts[place.piece] + place.offset;
    };

    for (const Field& field : fields)
    {
        const auto* run = field.at.piece < pieces.size() && !field.at.item && !field.at.sequence
                              ? std::get_if<std::vector<uint8_t>>(&pieces[field.at.piece])
                              : nullptr;
        if ((field.bytes != 2 && field.bytes != 4) || !run || uint64_t{field.at.offset} + field.bytes > run->size())
        {
            throw std::invalid_argument("a field doesn't lie in a run of a set of DSE files");
        }

        const uint64_t from = position(field.from);
        const uint64_t to = position(field.to);
        const uint64_t limit = field.bytes == 2 ? 0xFFFF : 0xFFFFFFFF;
        if (to - from > limit)
        {
            throw std::invalid_argument("a distance of " + std::to_string(int64_t(to) - int64_t(from)) +
                                        " bytes doesn't fit its field in a DSE file");
        }

        uint8_t* at = &set[starts[field.at.piece] + field.at.offset];
        if (field.bytes == 2)
        {
            xsflib::StoreLe16(at, static_cast<uint16_t>(to - from));
        }
        else
        {
            xsflib::StoreLe32(at, static_cast<uint32_t>(to - from));
        }
    }

    return set;
}

bool IsSet(std::span<const uint8_t> bytes)
{
    return bytes.size() >= kSetHeader && std::memcmp(bytes.data(), kSetMagic, sizeof(kSetMagic)) == 0;
}

std::vector<NamedFile> ReadSet(std::span<const uint8_t> set)
{
    if (!IsSet(set))
    {
        throw xsflib::FormatError("the bytes aren't a set of DSE files");
    }

    const xsflib::Reader r(set);
    const uint32_t count = r.U32(8);
    std::vector<NamedFile> files;
    for (uint32_t i = 0; i < count; i++)
    {
        const std::size_t entry = kSetHeader + std::size_t{kSetEntry} * i;
        files.push_back({r.CString(r.U32(entry + 8), set.size()), r.Sub(r.U32(entry), r.U32(entry + 4))});
    }

    return files;
}

std::string SequenceLabel(const SequenceRef& sequence)
{
    const auto is_digit = [](char c)
    {
        return c >= '0' && c <= '9';
    };
    const auto is_name = [&is_digit](char c)
    {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || is_digit(c) || c == '_';
    };

    std::string label = sequence.path;
    if (sequence.effect)
    {
        label += "_" + std::to_string(*sequence.effect);
    }
    std::ranges::replace_if(label, [&is_name](char c) { return !is_name(c); }, '_');
    if (label.empty() || is_digit(label.front()))
    {
        label.insert(label.begin(), '_');
    }

    return label;
}

std::optional<SequenceRef> SequenceNamed(std::string_view label, std::span<const NamedFile> files)
{
    for (const NamedFile& file : files)
    {
        const std::string name = SequenceLabel({.path = file.path});
        if (label == name)
        {
            return SequenceRef{.path = file.path};
        }

        if (!label.starts_with(name + "_"))
        {
            continue;
        }

        uint16_t slot = 0;
        const std::string_view digits = label.substr(name.size() + 1);
        const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), slot);
        if (error == std::errc{} && end == digits.data() + digits.size())
        {
            return SequenceRef{.path = file.path, .effect = slot};
        }
    }

    return std::nullopt;
}

std::optional<uint32_t> TracksOffset(std::span<const uint8_t> file, std::optional<uint16_t> effect)
{
    if (!effect)
    {
        return kSongTracks;
    }
    if (file.size() < kEffectsBase || *effect >= xsflib::LoadLe16(&file[0x30]) ||
        kEffectsBase + 2 * std::size_t{*effect} + 2 > file.size())
    {
        return std::nullopt;
    }

    const uint16_t offset = xsflib::LoadLe16(&file[kEffectsBase + 2 * std::size_t{*effect}]);
    if (offset == 0)
    {
        return std::nullopt;
    }

    return kEffectsBase + uint32_t{offset} + static_cast<uint32_t>(SequenceInfo::kSize);
}

} // namespace ntdlib::dse
