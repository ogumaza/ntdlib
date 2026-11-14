// SPDX-License-Identifier: MIT

#include "ntdlib/sdat/cut.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/ntr/snd/commands.h"
#include "ntdlib/rounding.h"
#include "ntdlib/sdat/formats.h"

namespace ntdlib::sdat
{
namespace
{

// The bytes of a sequence file's headers: the file's header, the DATA block's header and the bytecode's offset.
constexpr uint32_t kSequenceHeaders = 0x1C;

// The bytes of a sequence archive's headers before its table: the file's header, the DATA block's header, the
// bytecode's offset and the number of sequences. Each sequence's record in the table takes 12 bytes. Its first word
// gives the sequence's start in the bytecode, or kNoSequence for a record without a sequence.
constexpr uint32_t kSequenceArchiveHeaders = 0x20;
constexpr uint32_t kRecordBytes = 12;
constexpr uint32_t kNoSequence = 0xFFFFFFFF;

// The bytes of the FILE block before its first file: its magic word, its size and the number of files. The tools put
// the first file on the next multiple of 32 bytes. That can come right after these 12 bytes.
constexpr uint32_t kFileBlockHeader = 12;

// The alignment of a file's start, from the archive's start, and of a sequence file's end.
constexpr uint32_t kFileAlignment = 32;
constexpr uint32_t kSequenceAlignment = 4;

bool Zeros(std::span<const uint8_t> bytes)
{
    return std::all_of(bytes.begin(), bytes.end(), [](uint8_t byte) { return byte == 0; });
}

// A file that the cut takes, with its places in the archive. Only a sequence file or a sequence archive holds a
// sequence.
struct CutFile
{
    uint32_t start = 0;
    uint32_t end = 0;  // the file's end
    uint32_t next = 0; // where the padding to a multiple of 32 bytes ends, or `end` without it
    bool padded = false;
    bool has_sequence = false;
    uint16_t version = 0;
    uint32_t bytecode = 0; // the bytecode's offset from the file's start
    commands::Sequence sequence;

    // A sequence archive's records, in the table's order: the item where each record's sequence starts, or nothing
    // for a record without a sequence. Empty for a sequence file.
    std::vector<std::optional<uint32_t>> records;
};

// A sequence of a file's bytecode: the bytes of the bytecode that the sequence holds, and the item at each entry.
struct ReadCode
{
    commands::Sequence sequence;
    uint32_t size = 0;
    std::vector<uint32_t> entries;
};

// The bytecode's sequence from `entries`, when the decoder reads the bytecode without keeping it as data and the
// encoding gives it back. The zeros at the bytecode's end come off when the decoder then ends on a command: as many as
// 3 of them, the most first.
std::optional<ReadCode> ReadBytecode(std::span<const uint8_t> bytecode, std::span<const uint32_t> entries)
{
    const auto read = [&](uint32_t size, bool ends_on_command) -> std::optional<ReadCode>
    {
        try
        {
            const std::span<const uint8_t> code = bytecode.first(size);
            ntr::snd::Decoded decoded = ntr::snd::DecodeSequence(code, {}, entries);
            if (decoded.opaque || decoded.sequence.items.empty() ||
                (ends_on_command && decoded.sequence.items.back().kind == commands::Kind::kData))
            {
                return std::nullopt;
            }

            const ntr::snd::Encoded encoded = ntr::snd::EncodeSequence(decoded.sequence);
            if (!std::equal(encoded.bytes.begin(), encoded.bytes.end(), code.begin(), code.end()))
            {
                return std::nullopt;
            }
            return ReadCode{std::move(decoded.sequence), size, std::move(decoded.entries)};
        }
        catch (const std::exception&)
        {
            return std::nullopt;
        }
    };

    const auto size = static_cast<uint32_t>(bytecode.size());
    uint32_t zeros = 0;
    while (zeros < 3 && zeros < size && bytecode[size - 1 - zeros] == 0)
    {
        zeros++;
    }
    for (uint32_t cut = zeros; cut > 0; cut--)
    {
        if (std::optional<ReadCode> code = read(size - cut, true))
        {
            return code;
        }
    }

    return read(size, false);
}

// The sequence of `file`'s bytecode, from `offset` in the file to its end, by ReadBytecode's rules, when only zeros
// follow the bytes that the sequence holds.
std::optional<ReadCode> ReadFileBytecode(std::span<const uint8_t> file, uint32_t offset,
                                         std::span<const uint32_t> entries)
{
    const std::span<const uint8_t> bytecode = file.subspan(offset);
    std::optional<ReadCode> code = ReadBytecode(bytecode, entries);
    if (!code || !Zeros(bytecode.subspan(code->size)))
    {
        return std::nullopt;
    }

    return code;
}

// Whether `file` starts with the header and the DATA block's header that the tools write for a file of `magic` and of
// `file`'s size. The DATA block reaches the file's end, and the file ends on a multiple of 4 bytes.
bool HasFileHeaders(std::span<const uint8_t> file, const char* magic)
{
    const auto size = static_cast<uint32_t>(file.size());
    return size >= kSequenceHeaders && size % kSequenceAlignment == 0 &&
           std::equal(file.begin(), file.begin() + 4, magic) && xsflib::LoadLe16(&file[4]) == 0xFEFF &&
           xsflib::LoadLe32(&file[8]) == size && xsflib::LoadLe16(&file[0xC]) == 0x10 &&
           xsflib::LoadLe16(&file[0xE]) == 1 && std::equal(file.begin() + 0x10, file.begin() + 0x14, "DATA") &&
           xsflib::LoadLe32(&file[0x14]) == size - 0x10;
}

// Whether `file` starts with the headers that the tools write for a sequence file of `file`'s size.
bool HasSequenceHeaders(std::span<const uint8_t> file)
{
    return file.size() > kSequenceHeaders && HasFileHeaders(file, "SSEQ") &&
           xsflib::LoadLe32(&file[0x18]) == kSequenceHeaders;
}

// The sequence of `file`, a sequence archive (SSAR), with the item where each record's sequence starts. The file has
// the headers that the tools write: its table follows its headers, and its bytecode follows its table. The bytecode
// reads by ReadFileBytecode's rules from the starts that the records give. At least one record has a sequence. Nothing
// for any other bytes.
std::optional<ReadCode> ReadSequenceArchive(std::span<const uint8_t> file,
                                            std::vector<std::optional<uint32_t>>& records)
{
    if (file.size() < kSequenceArchiveHeaders || !HasFileHeaders(file, "SSAR"))
    {
        return std::nullopt;
    }
    const uint32_t count = xsflib::LoadLe32(&file[0x1C]);
    const uint64_t bytecode = kSequenceArchiveHeaders + uint64_t{count} * kRecordBytes;
    if (xsflib::LoadLe32(&file[0x18]) != bytecode || bytecode >= file.size())
    {
        return std::nullopt;
    }

    // The starts of the records with sequences. A record without one stays as it is.
    std::vector<uint32_t> entries;
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t offset = xsflib::LoadLe32(&file[kSequenceArchiveHeaders + i * kRecordBytes]);
        if (offset != kNoSequence)
        {
            entries.push_back(offset);
        }
    }
    if (entries.empty())
    {
        return std::nullopt;
    }

    std::optional<ReadCode> code = ReadFileBytecode(file, static_cast<uint32_t>(bytecode), entries);
    if (!code)
    {
        return std::nullopt;
    }
    records.clear();
    std::size_t entry = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t offset = xsflib::LoadLe32(&file[kSequenceArchiveHeaders + i * kRecordBytes]);
        records.push_back(offset == kNoSequence ? std::nullopt : std::optional(code->entries[entry++]));
    }

    return code;
}

// The pieces and fields of `archive`, whose files in `files` become sequence pieces or runs of their own, or nothing
// when a field names a byte that no run holds.
std::optional<Layout> LayOut(const Archive& archive, std::vector<CutFile> files)
{
    const std::span<const uint8_t> bytes = archive.bytes;
    const auto size = static_cast<uint32_t>(bytes.size());
    Layout layout;

    // The runs, by their first bytes in the archive. A sequence file's end lies at the start of the piece after its
    // padding.
    struct Run
    {
        uint32_t start = 0;
        uint32_t end = 0;
        uint32_t piece = 0;
    };
    std::vector<Run> runs;

    // For each file in `files` with a sequence, its sequence piece, and its end: the piece after its padding to 4
    // bytes.
    struct SequencePlaces
    {
        uint32_t piece = 0;
        uint32_t end = 0;
    };
    std::vector<SequencePlaces> sequence_places;
    uint32_t at = 0;
    const auto add_run = [&](uint32_t end)
    {
        runs.push_back(Run{at, end, static_cast<uint32_t>(layout.pieces.size())});
        layout.pieces.emplace_back(std::vector<uint8_t>(bytes.begin() + at, bytes.begin() + end));
        at = end;
    };
    for (CutFile& file : files)
    {
        if (file.has_sequence)
        {
            add_run(file.start + file.bytecode);
            const auto piece = static_cast<uint32_t>(layout.pieces.size());
            layout.pieces.emplace_back(
                sar::SequencePiece{static_cast<uint32_t>(layout.sequences.size()), file.version});
            layout.sequences.push_back(std::move(file.sequence));
            layout.pieces.emplace_back(sar::Padding{kSequenceAlignment});
            sequence_places.push_back({piece, static_cast<uint32_t>(layout.pieces.size())});
        }
        else
        {
            if (at < file.start)
            {
                add_run(file.start);
            }
            add_run(file.end);
        }
        if (file.padded)
        {
            layout.pieces.emplace_back(sar::Padding{kFileAlignment});
        }
        at = file.next;
    }
    if (at < size)
    {
        add_run(size);
    }

    const sar::Place end{static_cast<uint32_t>(layout.pieces.size()), 0};
    // A place names a byte of a run, or the start of the piece after a run's end.
    const auto place = [&](uint32_t offset) -> std::optional<sar::Place>
    {
        for (const Run& run : runs)
        {
            if (run.start <= offset && offset < run.end)
            {
                return sar::Place{run.piece, offset - run.start};
            }
            if (offset == run.end)
            {
                return sar::Place{run.piece + 1, 0};
            }
        }
        return offset == size ? std::optional(end) : std::nullopt;
    };
    // Where a file starts: past padding without bytes that the file before it ends in.
    const auto start_place = [&](uint32_t offset) -> std::optional<sar::Place>
    {
        for (const Run& run : runs)
        {
            if (run.start <= offset && offset < run.end)
            {
                return sar::Place{run.piece, offset - run.start};
            }
        }
        return place(offset);
    };
    bool placed = true;
    const auto field =
        [&](uint32_t at_offset, const std::optional<sar::Place>& from, const std::optional<sar::Place>& to)
    {
        const std::optional<sar::Place> at_place = place(at_offset);
        if (!at_place || !from || !to || at_place->piece == end.piece)
        {
            placed = false;
            return;
        }
        layout.fields.push_back(sar::Field{*at_place, *from, *to});
    };

    // The archive's size, and the FILE block's size in the header and in the block, where they reach the archive's
    // end.
    const sar::Place start{0, 0};
    field(0x08, start, end);
    const uint32_t file_block = xsflib::LoadLe32(&bytes[0x28]);
    if (uint64_t(file_block) + kFileBlockHeader <= size &&
        std::equal(bytes.begin() + file_block, bytes.begin() + file_block + 4, "FILE"))
    {
        if (uint64_t(file_block) + xsflib::LoadLe32(&bytes[0x2C]) == size)
        {
            field(0x2C, place(file_block), end);
        }
        if (uint64_t(file_block) + xsflib::LoadLe32(&bytes[file_block + 4]) == size)
        {
            field(file_block + 4, place(file_block), end);
        }
    }

    // Each file's offset and size in the FAT. The file's size and its DATA block's size in the header of a file with a
    // sequence. Each sequence's start in a sequence archive's table: the distance from the bytecode's first item to
    // the item where the sequence starts.
    std::vector<const CutFile*> sequence_files;
    for (const CutFile& file : files)
    {
        if (file.has_sequence)
        {
            sequence_files.push_back(&file);
        }
    }
    const uint32_t fat = xsflib::LoadLe32(&bytes[0x20]);
    for (std::size_t id = 0; id < archive.files.size(); id++)
    {
        const FileEntry& entry = archive.files[id];
        const auto entry_at = static_cast<uint32_t>(fat + 12 + id * 16);
        const auto cut = std::find_if(sequence_files.begin(), sequence_files.end(), [&](const CutFile* file)
                                      { return file->start == entry.offset && file->end - file->start == entry.size; });
        const SequencePlaces* places =
            cut != sequence_files.end() ? &sequence_places[std::size_t(cut - sequence_files.begin())] : nullptr;
        const std::optional<sar::Place> file_start = start_place(entry.offset);
        const std::optional<sar::Place> file_end = places            ? std::optional(sar::Place{places->end, 0})
                                                   : entry.size == 0 ? file_start
                                                                     : place(entry.offset + entry.size);
        field(entry_at, start, file_start);
        field(entry_at + 4, file_start, file_end);
        if (places == nullptr)
        {
            continue;
        }

        field(entry.offset + 8, file_start, file_end);
        field(entry.offset + 0x14, place(entry.offset + 0x10), file_end);
        const std::vector<std::optional<uint32_t>>& records = (*cut)->records;
        for (std::size_t i = 0; i < records.size(); i++)
        {
            if (records[i])
            {
                field(static_cast<uint32_t>(entry.offset + kSequenceArchiveHeaders + i * kRecordBytes),
                      sar::Place{places->piece, 0, true}, sar::Place{places->piece, *records[i], true});
            }
        }
    }

    if (!placed)
    {
        return std::nullopt;
    }
    return layout;
}

// Each piece's start, with the archive's end after the last, and each sequence piece's bytes and items.
struct LaidPieces
{
    std::vector<uint64_t> starts;
    std::vector<ntr::snd::Encoded> encoded;
};

LaidPieces LayPieces(std::span<const commands::Sequence> sequences, std::span<const sar::Piece> pieces)
{
    LaidPieces laid;
    laid.starts.resize(pieces.size() + 1);
    laid.encoded.resize(pieces.size());
    uint64_t size = 0;
    for (std::size_t p = 0; p < pieces.size(); p++)
    {
        laid.starts[p] = size;
        if (const auto* run = std::get_if<std::vector<uint8_t>>(&pieces[p]))
        {
            size += run->size();
        }
        else if (const auto* sequence = std::get_if<sar::SequencePiece>(&pieces[p]))
        {
            if (sequence->sequence >= sequences.size())
            {
                throw std::invalid_argument("a sequence piece names a sequence that the layout lacks");
            }
            laid.encoded[p] = ntr::snd::EncodeSequence(sequences[sequence->sequence]);
            size += laid.encoded[p].bytes.size();
        }
        else
        {
            const uint32_t alignment = std::get<sar::Padding>(pieces[p]).alignment;
            size = AlignUp(size, std::max<uint32_t>(alignment, 1));
        }
    }
    laid.starts[pieces.size()] = size;
    if (size > std::numeric_limits<uint32_t>::max())
    {
        throw std::invalid_argument("a DS sound archive can't hold more than 4 GiB");
    }

    return laid;
}

} // namespace

Layout Cut(std::span<const uint8_t> sdat)
{
    const Archive archive = Archive::Parse(sdat);
    const std::span<const uint8_t> bytes = archive.bytes;
    const auto size = static_cast<uint32_t>(bytes.size());

    // The files can't move the blocks before them: the sequence files that the cut takes lie after the header's blocks.
    // The FILE block counts up to its first file.
    uint64_t blocks_end = 0x40;
    for (uint32_t block = 0; block < 4; block++)
    {
        const uint32_t offset = xsflib::LoadLe32(&bytes[0x10 + block * 8]);
        const uint32_t block_size = xsflib::LoadLe32(&bytes[0x14 + block * 8]);
        blocks_end = std::max<uint64_t>(blocks_end, offset + uint64_t(block == 3 ? kFileBlockHeader : block_size));
    }

    // The files by their starts, each FAT entry once.
    std::vector<std::pair<uint32_t, uint32_t>> entries; // start and end
    for (const FileEntry& entry : archive.files)
    {
        entries.emplace_back(entry.offset, uint32_t(std::min<uint64_t>(uint64_t(entry.offset) + entry.size, ~0u)));
    }
    std::sort(entries.begin(), entries.end());

    std::vector<CutFile> files;
    uint32_t reached = 0; // the furthest end of the files before
    for (std::size_t e = 0; e < entries.size(); e++)
    {
        const auto [start, end] = entries[e];
        const bool clear = reached <= start && (e + 1 == entries.size() || entries[e + 1].first >= end) &&
                           (e + 1 == entries.size() || entries[e + 1] != entries[e]) &&
                           (e == 0 || entries[e - 1] != entries[e]);
        reached = std::max(reached, end);
        if (!clear || start < blocks_end || end > size || end == start)
        {
            continue;
        }

        // A file that ends on the multiple takes the padding too, without bytes. A new size then keeps what follows on
        // its multiple.
        CutFile file{.start = start, .end = end, .next = end};
        const uint32_t next = e + 1 < entries.size() ? entries[e + 1].first : size;
        if (next == AlignUp(uint64_t{end}, kFileAlignment) && Zeros(bytes.subspan(end, next - end)))
        {
            file.next = next;
            file.padded = true;
        }

        // A sequence file or a sequence archive whose bytecode the decoder reads becomes a sequence piece, and any
        // other file a run. Both kinds give the bytecode's offset at 0x18.
        const std::span<const uint8_t> file_bytes = bytes.subspan(start, end - start);
        std::optional<ReadCode> code;
        if (start % kSequenceAlignment == 0 && HasSequenceHeaders(file_bytes))
        {
            const uint32_t entry = 0;
            code = ReadFileBytecode(file_bytes, kSequenceHeaders, std::span(&entry, 1));
        }
        else if (start % kSequenceAlignment == 0)
        {
            code = ReadSequenceArchive(file_bytes, file.records);
        }
        if (code)
        {
            file.has_sequence = true;
            file.version = xsflib::LoadLe16(&file_bytes[6]);
            file.bytecode = xsflib::LoadLe32(&file_bytes[0x18]);
            file.sequence = std::move(code->sequence);
        }
        files.push_back(std::move(file));
    }

    const auto whole = [&]
    {
        return Layout{{}, {std::vector<uint8_t>(bytes.begin(), bytes.end())}, {}};
    };
    if (files.empty())
    {
        return whole();
    }

    std::optional<Layout> layout = LayOut(archive, std::move(files));
    try
    {
        if (layout && std::ranges::equal(Join(layout->sequences, layout->pieces, layout->fields), bytes))
        {
            return std::move(*layout);
        }
    }
    catch (const std::exception&)
    {
    }

    return whole();
}

std::vector<uint8_t> Join(std::span<const commands::Sequence> sequences, std::span<const sar::Piece> pieces,
                          std::span<const sar::Field> fields)
{
    const LaidPieces laid = LayPieces(sequences, pieces);
    const std::vector<uint64_t>& starts = laid.starts;
    const std::vector<ntr::snd::Encoded>& encoded = laid.encoded;
    const uint64_t size = starts.back();

    std::vector<uint8_t> archive(size);
    for (std::size_t p = 0; p < pieces.size(); p++)
    {
        const auto* run = std::get_if<std::vector<uint8_t>>(&pieces[p]);
        const std::vector<uint8_t>& bytes = run ? *run : encoded[p].bytes;
        std::copy(bytes.begin(), bytes.end(), archive.begin() + std::ptrdiff_t(starts[p]));
    }

    const auto position = [&](const sar::Place& place) -> uint64_t
    {
        if (place.sequence)
        {
            throw std::invalid_argument("a field of a DS sound archive names a sequence without a piece");
        }
        if (place.piece > pieces.size())
        {
            throw std::invalid_argument("a field names a place that the archive's pieces lack");
        }
        if (place.item)
        {
            if (place.piece == pieces.size() || !std::holds_alternative<sar::SequencePiece>(pieces[place.piece]))
            {
                throw std::invalid_argument("a field names an item outside a sequence piece");
            }
            const std::vector<uint32_t>& offsets = encoded[place.piece].offsets;
            if (place.offset >= offsets.size())
            {
                throw std::invalid_argument("a field names an item past its sequence");
            }
            return starts[place.piece] + offsets[place.offset];
        }
        if (place.piece == pieces.size())
        {
            return size + place.offset;
        }

        const auto* run = std::get_if<std::vector<uint8_t>>(&pieces[place.piece]);
        if (run ? place.offset > run->size() : place.offset != 0)
        {
            throw std::invalid_argument(run ? "a field names a byte past its run"
                                            : "a field names a byte inside a "
                                              "sequence or padding");
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
            throw std::invalid_argument("a field doesn't lie in a run of a DS sound archive");
        }
        if ((field.from.item || field.to.item) &&
            (field.from.item != field.to.item || field.from.piece != field.to.piece))
        {
            throw std::invalid_argument("a field names an item of a sequence and another place");
        }

        const uint64_t from = position(field.from);
        const uint64_t to = position(field.to);
        if (to < from || to - from > std::numeric_limits<uint32_t>::max())
        {
            throw std::invalid_argument("a field's distance doesn't fit in 32 bits");
        }
        xsflib::StoreLe32(&archive[starts[field.at.piece] + field.at.offset], uint32_t(to - from));
    }

    return archive;
}

std::optional<commands::Sequence> ReadSequenceFile(std::span<const uint8_t> file)
{
    if (!HasSequenceHeaders(file))
    {
        return std::nullopt;
    }

    const uint32_t entry = 0;
    std::optional<ReadCode> code = ReadFileBytecode(file, kSequenceHeaders, std::span(&entry, 1));
    if (!code)
    {
        return std::nullopt;
    }

    return std::move(code->sequence);
}

std::vector<sar::Piece> ReplaceFile(std::span<const commands::Sequence> sequences, std::span<const sar::Piece> pieces,
                                    std::span<const sar::Field> fields, uint32_t file, std::span<const uint8_t> bytes)
{
    const std::vector<uint8_t> joined = Join(sequences, pieces, fields);
    const Archive before = Archive::Parse(joined);
    if (file >= before.files.size())
    {
        throw std::invalid_argument("the archive holds no file " + std::to_string(file));
    }

    // The run that holds the file, and nothing else.
    const FileEntry& entry = before.files[file];
    const LaidPieces laid = LayPieces(sequences, pieces);
    std::optional<std::size_t> own_run;
    for (std::size_t p = 0; p < pieces.size() && !own_run; p++)
    {
        const auto* run = std::get_if<std::vector<uint8_t>>(&pieces[p]);
        if (run != nullptr && laid.starts[p] == entry.offset && run->size() == entry.size && entry.size > 0)
        {
            own_run = p;
        }
    }
    if (!own_run)
    {
        throw std::invalid_argument("file " + std::to_string(file) + " isn't a run of its own");
    }

    std::vector<sar::Piece> replaced(pieces.begin(), pieces.end());
    replaced[*own_run] = std::vector<uint8_t>(bytes.begin(), bytes.end());

    // Every other file has to keep its bytes, and the tables have to give the new bytes.
    const std::vector<uint8_t> new_joined = Join(sequences, replaced, fields);
    const Archive after = Archive::Parse(new_joined);
    if (after.files.size() != before.files.size() || !std::ranges::equal(after.File(file), bytes))
    {
        throw std::invalid_argument("the archive's tables don't give file " + std::to_string(file) + "'s new bytes");
    }
    for (uint32_t id = 0; id < before.files.size(); id++)
    {
        if (id != file && !std::ranges::equal(after.File(id), before.File(id)))
        {
            throw std::invalid_argument("file " + std::to_string(id) + " shares bytes with file " +
                                        std::to_string(file));
        }
    }

    return replaced;
}

} // namespace ntdlib::sdat
