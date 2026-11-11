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

// A file that the cut takes, with its places in the archive. Only a sequence file holds a sequence.
struct CutFile
{
    uint32_t start = 0;
    uint32_t end = 0;  // the file's end
    uint32_t next = 0; // where the padding to a multiple of 32 bytes ends, or `end` without it
    bool padded = false;
    bool has_sequence = false;
    uint16_t version = 0;
    commands::Sequence sequence;
};

// The bytecode's sequence, and the bytes of it that the sequence holds, when the decoder reads the bytecode without
// keeping it as data and the encoding gives it back. The zeros at the bytecode's end come off when the decoder then
// ends on a command: as many as 3 of them, the most first.
std::optional<std::pair<commands::Sequence, uint32_t>> ReadBytecode(std::span<const uint8_t> bytecode)
{
    const auto read = [&](uint32_t size, bool ends_on_command) -> std::optional<commands::Sequence>
    {
        try
        {
            const std::span<const uint8_t> code = bytecode.first(size);
            const uint32_t entry = 0;
            ntr::snd::Decoded decoded = ntr::snd::DecodeSequence(code, {}, std::span(&entry, 1));
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
            return std::move(decoded.sequence);
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
        if (std::optional<commands::Sequence> sequence = read(size - cut, true))
        {
            return std::pair(std::move(*sequence), size - cut);
        }
    }
    if (std::optional<commands::Sequence> sequence = read(size, false))
    {
        return std::pair(std::move(*sequence), size);
    }

    return std::nullopt;
}

// Whether `file` starts with the headers that the tools write for a sequence file of `size` bytes.
bool HasSequenceHeaders(std::span<const uint8_t> file)
{
    const auto size = static_cast<uint32_t>(file.size());
    return size > kSequenceHeaders && size % kSequenceAlignment == 0 &&
           std::equal(file.begin(), file.begin() + 4, "SSEQ") && xsflib::LoadLe16(&file[4]) == 0xFEFF &&
           xsflib::LoadLe32(&file[8]) == size && xsflib::LoadLe16(&file[0xC]) == 0x10 &&
           xsflib::LoadLe16(&file[0xE]) == 1 && std::equal(file.begin() + 0x10, file.begin() + 0x14, "DATA") &&
           xsflib::LoadLe32(&file[0x14]) == size - 0x10 && xsflib::LoadLe32(&file[0x18]) == kSequenceHeaders;
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
    std::vector<uint32_t> ends; // for each sequence file in `files`, its end: the piece after its padding to 4 bytes
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
            add_run(file.start + kSequenceHeaders);
            layout.pieces.emplace_back(
                sar::SequencePiece{static_cast<uint32_t>(layout.sequences.size()), file.version});
            layout.sequences.push_back(std::move(file.sequence));
            layout.pieces.emplace_back(sar::Padding{kSequenceAlignment});
            ends.push_back(static_cast<uint32_t>(layout.pieces.size()));
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

    // Each file's offset and size in the FAT. A sequence file's size and its DATA block's size in its header.
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
        const std::optional<sar::Place> file_start = start_place(entry.offset);
        const std::optional<sar::Place> file_end =
            cut != sequence_files.end() ? std::optional(sar::Place{ends[std::size_t(cut - sequence_files.begin())], 0})
            : entry.size == 0           ? file_start
                                        : place(entry.offset + entry.size);
        field(entry_at, start, file_start);
        field(entry_at + 4, file_start, file_end);
        if (cut != sequence_files.end())
        {
            field(entry.offset + 8, file_start, file_end);
            field(entry.offset + 0x14, place(entry.offset + 0x10), file_end);
        }
    }

    if (!placed)
    {
        return std::nullopt;
    }
    return layout;
}

// Each piece's start, with the archive's end after the last, and each sequence's bytes.
struct LaidPieces
{
    std::vector<uint64_t> starts;
    std::vector<std::vector<uint8_t>> encoded;
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
            laid.encoded[p] = ntr::snd::EncodeSequence(sequences[sequence->sequence]).bytes;
            size += laid.encoded[p].size();
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
        CutFile file{start, end, end, false, false, 0, {}};
        const uint32_t next = e + 1 < entries.size() ? entries[e + 1].first : size;
        if (next == AlignUp(uint64_t{end}, kFileAlignment) && Zeros(bytes.subspan(end, next - end)))
        {
            file.next = next;
            file.padded = true;
        }

        // A sequence file whose bytecode the decoder reads becomes a sequence piece, and any other file a run.
        if (start % kSequenceAlignment == 0 && HasSequenceHeaders(bytes.subspan(start, end - start)))
        {
            auto read = ReadBytecode(bytes.subspan(start + kSequenceHeaders, end - start - kSequenceHeaders));
            if (read && Zeros(bytes.subspan(start + kSequenceHeaders + read->second,
                                            end - start - kSequenceHeaders - read->second)))
            {
                file.has_sequence = true;
                file.version = xsflib::LoadLe16(&bytes[start + 6]);
                file.sequence = std::move(read->first);
            }
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
    const std::vector<std::vector<uint8_t>>& encoded = laid.encoded;
    const uint64_t size = starts.back();

    std::vector<uint8_t> archive(size);
    for (std::size_t p = 0; p < pieces.size(); p++)
    {
        const auto* run = std::get_if<std::vector<uint8_t>>(&pieces[p]);
        const std::vector<uint8_t>& bytes = run ? *run : encoded[p];
        std::copy(bytes.begin(), bytes.end(), archive.begin() + std::ptrdiff_t(starts[p]));
    }

    const auto position = [&](const sar::Place& place) -> uint64_t
    {
        if (place.sequence || place.item)
        {
            throw std::invalid_argument("a field of a DS sound archive names an item of a sequence");
        }
        if (place.piece > pieces.size())
        {
            throw std::invalid_argument("a field names a place that the archive's pieces lack");
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

    const std::span<const uint8_t> bytecode = file.subspan(kSequenceHeaders);
    auto read = ReadBytecode(bytecode);
    if (!read || !Zeros(bytecode.subspan(read->second)))
    {
        return std::nullopt;
    }

    return std::move(read->first);
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
