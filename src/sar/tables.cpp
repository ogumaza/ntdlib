// SPDX-License-Identifier: MIT

#include "ntdlib/sar/tables.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/info_block.h"
#include "ntdlib/sar/string_block.h"
#include "sar/file_bytes.h"

namespace ntdlib::sar
{
namespace
{

constexpr uint16_t kStringBlock = 0x2000;
constexpr uint16_t kInfoBlock = 0x2001;
constexpr uint16_t kFileBlock = 0x2002;
constexpr uint16_t kInternalFile = 0x220C; // from a file's entry to its place in the FILE block
constexpr uint32_t kNone = 0xFFFFFFFF;
constexpr std::size_t kHeaderSizeField = 0x06;
constexpr std::size_t kSizeField = 0x0C; // the archive's size
constexpr std::size_t kBlockCount = 0x10;
constexpr std::size_t kBlockTable = 0x14;
constexpr std::size_t kBlockEntrySize = 12; // a type, two bytes of padding, an offset and a size
constexpr std::size_t kBlockHeaderSize = 8; // a block's magic and size

using detail::Out;

// A block that the header's table lists: where its entry is in the table, and the block's offset and size.
struct BlockEntry
{
    std::size_t entry = 0;
    uint32_t offset = kNone;
    uint32_t size = 0;
};

// The entry of the header's table for the block of type `type`, in the archive that `r` reads. Nothing for a block
// that the table doesn't list.
std::optional<BlockEntry> EntryOf(const xsflib::Reader& r, uint16_t type)
{
    for (std::size_t i = 0; i < r.U16(kBlockCount); ++i)
    {
        const std::size_t entry = kBlockTable + i * kBlockEntrySize;
        if (r.U16(entry) == type)
        {
            return BlockEntry{entry, r.U32(entry + 4), r.U32(entry + 8)};
        }
    }

    return std::nullopt;
}

// The bytes of the block that `entry` lists in `archive`. Throws xsflib::FormatError for a block past the archive's
// end. `name` names the block in the message.
std::span<const uint8_t> BytesOf(std::span<const uint8_t> archive, const BlockEntry& entry, const char* name)
{
    if (entry.offset > archive.size() || entry.size > archive.size() - entry.offset)
    {
        throw xsflib::FormatError(std::string("an archive whose ") + name + " block runs past its end");
    }

    return archive.subspan(entry.offset, entry.size);
}

// Where the first file of a FILE block at `block` starts in an archive of `flavor`: the first multiple of the files'
// alignment after the block's size.
std::size_t FirstFile(std::size_t block, Flavor flavor)
{
    const std::size_t alignment = flavor == Flavor::kNx ? 64 : 32;

    return (block + kBlockHeaderSize + alignment - 1) / alignment * alignment;
}

xsflib::FormatError OtherLayout(const std::string& what)
{
    return xsflib::FormatError("an archive laid out another way: " + what);
}

// Adds `shift` to the offset of each file of `info` that the FILE block holds. Throws xsflib::FormatError for a file
// whose location lies outside its entry, or before `first`, the first file's offset.
void MoveFiles(InfoBlock& info, int64_t shift, uint32_t first)
{
    const bool big_endian = info.flavor == Flavor::kCafe;
    for (std::size_t i = 0; i < info.files.size(); ++i)
    {
        std::vector<uint8_t>& entry = info.files[i];
        const xsflib::Reader r(entry, big_endian);
        if (r.U16(0) != kInternalFile)
        {
            continue;
        }

        const std::size_t at = std::size_t{r.U32(4)} + 4; // the offset in the location
        const uint32_t offset = r.U32(at);
        if (offset == kNone)
        {
            continue;
        }
        if (offset < first)
        {
            throw xsflib::FormatError("an archive whose file " + std::to_string(i) + " starts before its first file");
        }

        const auto moved = static_cast<uint32_t>(offset + shift);
        big_endian ? xsflib::StoreBe32(&entry[at], moved) : xsflib::StoreLe32(&entry[at], moved);
    }
}

} // namespace

Tables TablesOf(std::span<const uint8_t> archive, Flavor flavor)
{
    const xsflib::Reader r(archive, flavor == Flavor::kCafe);
    const std::optional<BlockEntry> strings = EntryOf(r, kStringBlock);
    const std::optional<BlockEntry> info = EntryOf(r, kInfoBlock);
    if (!info)
    {
        throw xsflib::FormatError("an archive without an INFO block");
    }

    Tables tables;
    if (strings && strings->offset != kNone)
    {
        tables.strings = BytesOf(archive, *strings, "STRG");
    }
    tables.info = BytesOf(archive, *info, "INFO");

    return tables;
}

std::vector<uint8_t> WithTables(std::span<const uint8_t> archive, Flavor flavor,
                                const std::optional<StringBlock>& strings, InfoBlock info)
{
    if (info.flavor != flavor || (strings && strings->flavor != flavor))
    {
        throw std::invalid_argument("tables of another flavor than the archive's");
    }

    // The header lists the STRG, INFO and FILE blocks in that order. They follow it in that order, without gaps.
    const bool big_endian = flavor == Flavor::kCafe;
    const xsflib::Reader r(archive, big_endian);
    const std::size_t header_size = r.U16(kHeaderSizeField);
    const std::optional<BlockEntry> old_strings = EntryOf(r, kStringBlock);
    const std::optional<BlockEntry> old_info = EntryOf(r, kInfoBlock);
    const std::optional<BlockEntry> files = EntryOf(r, kFileBlock);
    if (r.U16(kBlockCount) != 3 || !old_strings || !old_info || !files || old_strings->entry != kBlockTable ||
        old_info->entry != kBlockTable + kBlockEntrySize || files->entry != kBlockTable + 2 * kBlockEntrySize ||
        header_size < kBlockTable + 3 * kBlockEntrySize)
    {
        throw OtherLayout("its header doesn't list its STRG, INFO and FILE blocks in that order");
    }
    const bool named = old_strings->offset != kNone;
    if (named != strings.has_value())
    {
        throw std::invalid_argument(named ? "no STRG block for an archive with one"
                                          : "a STRG block for an archive without one");
    }

    std::size_t at = header_size;
    if (named)
    {
        if (old_strings->offset != at)
        {
            throw OtherLayout("its STRG block doesn't follow its header");
        }
        at += old_strings->size;
    }
    if (old_info->offset != at)
    {
        throw OtherLayout("its INFO block doesn't follow its STRG block");
    }
    if (files->offset != at + old_info->size)
    {
        throw OtherLayout("its FILE block doesn't follow its INFO block");
    }
    if (r.U32(kSizeField) != archive.size() || files->offset + std::size_t{files->size} != archive.size())
    {
        throw xsflib::FormatError("an archive cut short, or one that doesn't end with its FILE block");
    }

    // The FILE block: its magic and size, then zeros up to its first file.
    const std::size_t first = FirstFile(files->offset, flavor);
    if (!r.Magic(files->offset, "FILE") || r.U32(files->offset + 4) != files->size)
    {
        throw OtherLayout("its FILE block has another magic, or another size than its header gives");
    }
    if (first > archive.size())
    {
        throw OtherLayout("its FILE block ends before its first file's place");
    }
    const std::span<const uint8_t> lead =
        archive.subspan(files->offset + kBlockHeaderSize, first - files->offset - kBlockHeaderSize);
    if (std::ranges::any_of(lead, [](uint8_t byte) { return byte != 0; }))
    {
        throw OtherLayout("its FILE block holds bytes other than zeros before its first file");
    }

    // The new blocks. The files keep their offsets from the archive's start modulo the alignment. Their offsets in the
    // FILE block change as the zeros before the first file change.
    const std::vector<uint8_t> new_strings = named ? strings->Write() : std::vector<uint8_t>();
    std::vector<uint8_t> new_info = info.Write();
    const std::size_t new_files = header_size + new_strings.size() + new_info.size();
    const std::size_t new_first = FirstFile(new_files, flavor);
    const auto shift = static_cast<int64_t>(new_first - new_files) - static_cast<int64_t>(first - files->offset);
    if (shift != 0)
    {
        MoveFiles(info, shift, static_cast<uint32_t>(lead.size()));
        new_info = info.Write();
    }

    Out out(big_endian);
    out.Append(archive.first(header_size));
    out.Append(new_strings);
    out.Append(new_info);
    out.Append(std::span(reinterpret_cast<const uint8_t*>("FILE"), 4));
    out.Zeros(new_first - new_files - 4);
    out.Append(archive.subspan(first));

    if (named)
    {
        out.U32(old_strings->entry + 4, static_cast<uint32_t>(header_size));
        out.U32(old_strings->entry + 8, static_cast<uint32_t>(new_strings.size()));
    }
    out.U32(old_info->entry + 4, static_cast<uint32_t>(header_size + new_strings.size()));
    out.U32(old_info->entry + 8, static_cast<uint32_t>(new_info.size()));
    out.U32(files->entry + 4, static_cast<uint32_t>(new_files));
    out.U32(files->entry + 8, static_cast<uint32_t>(out.Size() - new_files));
    out.U32(new_files + 4, static_cast<uint32_t>(out.Size() - new_files));
    out.U32(kSizeField, static_cast<uint32_t>(out.Size()));

    return out.Take();
}

} // namespace ntdlib::sar
