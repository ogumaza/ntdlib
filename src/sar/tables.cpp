// SPDX-License-Identifier: MIT

#include "ntdlib/sar/tables.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/info_block.h"
#include "ntdlib/sar/string_block.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "sar/file_bytes.h"

namespace ntdlib::sar
{
namespace
{

constexpr uint16_t kStringBlock = 0x2000;
constexpr uint16_t kInfoBlock = 0x2001;
constexpr uint16_t kFileBlock = 0x2002;
constexpr uint16_t kInternalFile = 0x220C;  // from a file's entry to its place in the FILE block
constexpr uint16_t kDataReference = 0x1F00; // a file's place in the FILE block
constexpr uint16_t kListReference = 0x0100; // a list of 32-bit values
constexpr uint16_t kWaveSoundDetails = 0x2202;
constexpr uint16_t kSequenceDetails = 0x2203;
constexpr uint32_t kSequencePriority = 2;  // the option of a sequence sound's details that gives its channel priority
constexpr uint32_t kWaveSoundPriority = 1; // the option of a wave sound's details that gives its channel priority
constexpr uint32_t kNone = 0xFFFFFFFF;
constexpr std::size_t kHeaderSizeField = 0x06;
constexpr std::size_t kSizeField = 0x0C; // the archive's size
constexpr std::size_t kBlockCount = 0x10;
constexpr std::size_t kBlockTable = 0x14;
constexpr std::size_t kBlockEntrySize = 12; // a type, two bytes of padding, an offset and a size
constexpr std::size_t kBlockHeaderSize = 8; // a block's magic and size
constexpr std::size_t kSoundOptions = 20;   // where a sound's entry holds its options
constexpr std::size_t kSoundDetails = 12;   // where a sound's entry refers to its details
constexpr std::size_t kMaxBanks = 4;
constexpr uint32_t kNameOption = 1; // the option that gives the string index

// Where an entry of each table that holds names keeps its options, in the order of InfoBlock's tables: the sounds, the
// sound groups, the banks, the wave archives, the groups and the players.
constexpr std::array<std::size_t, 6> kOptionsAt = {kSoundOptions, 24, 12, 8, 4, 4};

// The places of the tables of sounds, banks and wave archives in kOptionsAt.
constexpr std::size_t kSoundTable = 0;
constexpr std::size_t kBankTable = 2;
constexpr std::size_t kWaveArchiveTable = 3;

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

// Stores `value` at `at` in `bytes`, in big-endian order or in little-endian.
void Store32(std::vector<uint8_t>& bytes, std::size_t at, bool big_endian, uint32_t value)
{
    big_endian ? xsflib::StoreBe32(&bytes[at], value) : xsflib::StoreLe32(&bytes[at], value);
}

// Stores the 16-bit `value` at `at` in `bytes`, in big-endian order or in little-endian.
void Store16(std::vector<uint8_t>& bytes, std::size_t at, bool big_endian, uint16_t value)
{
    big_endian ? xsflib::StoreBe16(&bytes[at], value) : xsflib::StoreLe16(&bytes[at], value);
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

        Store32(entry, at, big_endian, static_cast<uint32_t>(offset + shift));
    }
}

// The bytes of `archive`, an archive of `flavor`, with `bytes` after its last byte, from `start` on. Zeros fill the
// gap. The FILE block that ends the archive takes the bytes, and the archive's size and the block's follow.
std::vector<uint8_t> WithFileAppended(std::span<const uint8_t> archive, Flavor flavor, std::size_t start,
                                      std::span<const uint8_t> bytes)
{
    const bool big_endian = flavor == Flavor::kCafe;
    const std::optional<BlockEntry> files = EntryOf(xsflib::Reader(archive, big_endian), kFileBlock);
    if (!files || files->offset > archive.size())
    {
        throw xsflib::FormatError("an archive without a FILE block");
    }

    Out out(big_endian);
    out.Append(archive);
    out.Zeros(start - archive.size());
    out.Append(bytes);
    const auto size = static_cast<uint32_t>(out.Size() - files->offset);
    out.U32(files->offset + 4, size);
    out.U32(files->entry + 8, size);
    out.U32(kSizeField, static_cast<uint32_t>(out.Size()));

    return out.Take();
}

// The entry of an internal file at `offset` from the end of the FILE block's size, of `size` bytes, as the tools write
// it for `flavor`. The Wii U and the Switch list the groups that hold the file. The list is empty.
std::vector<uint8_t> InternalFileEntry(Flavor flavor, uint32_t offset, uint32_t size)
{
    const bool ctr = flavor == Flavor::kCtr;
    Out out(flavor == Flavor::kCafe);
    out.Zeros(ctr ? 24 : 36);
    out.Reference(0, kInternalFile, 12);
    out.Reference(12, kDataReference, offset);
    out.U32(20, size);
    if (!ctr)
    {
        out.Reference(24, kListReference, 20);
    }

    return out.Take();
}

// Where `entry`, whose options start at `options`, holds its string index. Nothing for an entry without a name.
// Throws xsflib::FormatError for an entry that ends before them.
std::optional<std::size_t> StringIndexAt(std::span<const uint8_t> entry, std::size_t options, bool big_endian)
{
    if (entry.size() < options + 4)
    {
        throw OtherLayout("an INFO entry ends before its options");
    }
    if ((xsflib::Reader(entry, big_endian).U32(options) & kNameOption) == 0)
    {
        return std::nullopt;
    }
    if (entry.size() < options + 8)
    {
        throw OtherLayout("an INFO entry ends before its string index");
    }

    return options + 4;
}

// An archive's STRG block, if it has one, and its INFO block, read.
struct ReadBlocks
{
    std::optional<StringBlock> strings;
    InfoBlock info;
};

// The blocks of `archive`, an archive of `flavor`. Throws the exceptions of TablesOf and of the blocks' readers.
ReadBlocks ReadTables(std::span<const uint8_t> archive, Flavor flavor)
{
    const Tables tables = TablesOf(archive, flavor);
    ReadBlocks blocks;
    if (tables.strings)
    {
        blocks.strings = StringBlock::Read(*tables.strings, flavor);
    }
    blocks.info = InfoBlock::Read(tables.info, flavor);

    return blocks;
}

// The count of names of `strings`, or nothing for an archive without names.
std::optional<std::size_t> NameCount(const std::optional<StringBlock>& strings)
{
    return strings ? std::optional<std::size_t>(strings->names.size()) : std::nullopt;
}

// The tables of `info` whose entries hold names, in the order of InfoBlock's tables.
template <class Info>
auto NamedTables(Info& info)
{
    return std::array{&info.sounds, &info.sound_groups, &info.banks, &info.wave_archives, &info.groups, &info.players};
}

// The string index of a new name for an item at the end of table `table` of `info`, in the order of NamedTables: one
// past the last string index that the entries of that table and the tables before it hold. Throws xsflib::FormatError
// for an index past `names`, the string table's count of names, in an archive with names.
uint32_t NewStringIndex(const InfoBlock& info, std::size_t table, std::optional<std::size_t> names)
{
    const bool big_endian = info.flavor == Flavor::kCafe;
    const auto tables = NamedTables(info);
    uint32_t string = 0;
    for (std::size_t k = 0; k <= table; ++k)
    {
        for (const std::vector<uint8_t>& entry : *tables[k])
        {
            if (const std::optional<std::size_t> at = StringIndexAt(entry, kOptionsAt[k], big_endian))
            {
                const uint32_t index = xsflib::Reader(entry, big_endian).U32(*at);
                string = index != kNone ? std::max(string, index + 1) : string;
            }
        }
    }
    if (names && string > *names)
    {
        throw OtherLayout("a string index past the string table");
    }

    return string;
}

// Checks the name of a new item for an archive with the names `strings`, or for one without names. With `required`,
// an archive with names requires one. Throws std::invalid_argument for a name for an archive without names, no name
// where one is required, and a name that the archive holds already.
void CheckName(const std::optional<StringBlock>& strings, const std::string& name, bool required)
{
    if (!strings && !name.empty())
    {
        throw std::invalid_argument("a name for an archive without them");
    }
    if (strings && required && name.empty())
    {
        throw std::invalid_argument("no name for an archive with names");
    }
    const auto taken = [&name](const ItemName& other)
    {
        return other.name == name;
    };
    if (strings && !name.empty() && std::ranges::any_of(strings->names, taken))
    {
        throw std::invalid_argument("a name that the archive holds already");
    }
}

// Moves up by one each string index of `info` from `first` on, for a new name at `first` in the string table.
void MakeRoomForName(InfoBlock& info, uint32_t first)
{
    const bool big_endian = info.flavor == Flavor::kCafe;
    const auto tables = NamedTables(info);
    for (std::size_t k = 0; k < tables.size(); ++k)
    {
        for (std::vector<uint8_t>& entry : *tables[k])
        {
            const std::optional<std::size_t> at = StringIndexAt(entry, kOptionsAt[k], big_endian);
            const uint32_t index = at ? xsflib::Reader(entry, big_endian).U32(*at) : kNone;
            if (index != kNone && index >= first)
            {
                Store32(entry, *at, big_endian, index + 1);
            }
        }
    }
}

// Where a sequence sound's details start in its entry, and where its list of banks starts.
struct DetailsPlaces
{
    std::size_t details = 0;
    std::size_t banks = 0;
};

// The places of the details of `entry`, a sequence sound's entry in the byte order that `big_endian` gives. Throws
// xsflib::FormatError for details that don't start with the list of banks, and for an entry that the list doesn't end.
DetailsPlaces PlacesOfDetails(std::span<const uint8_t> entry, bool big_endian)
{
    const xsflib::Reader r(entry, big_endian);
    const std::size_t details = r.U32(kSoundDetails + 4);
    if (r.U16(details) != kListReference)
    {
        throw OtherLayout("a sequence sound's details that don't start with its list of banks");
    }
    const std::size_t banks = details + r.U32(details + 4);
    if (banks + 4 + std::size_t{4} * r.U32(banks) != entry.size())
    {
        throw OtherLayout("a sequence sound's entry that its list of banks doesn't end");
    }

    return {details, banks};
}

// Whether `entry` is the entry of a sequence sound, in the byte order that `big_endian` gives.
bool IsSequenceSound(std::span<const uint8_t> entry, bool big_endian)
{
    return entry.size() >= kSoundDetails + 8 &&
           xsflib::Reader(entry, big_endian).U16(kSoundDetails) == kSequenceDetails;
}

// Whether `entry` is the entry of a wave sound, in the byte order that `big_endian` gives.
bool IsWaveSound(std::span<const uint8_t> entry, bool big_endian)
{
    return entry.size() >= kSoundDetails + 8 &&
           xsflib::Reader(entry, big_endian).U16(kSoundDetails) == kWaveSoundDetails;
}

// `entry`, a sequence sound's entry whose list of banks starts at `banks`, with `list` as its list, in the byte order
// that `big_endian` gives.
void WriteBanks(std::vector<uint8_t>& entry, std::size_t banks, bool big_endian, std::span<const uint32_t> list)
{
    entry.resize(banks + 4 + 4 * list.size());
    Store32(entry, banks, big_endian, static_cast<uint32_t>(list.size()));
    for (std::size_t k = 0; k < list.size(); ++k)
    {
        Store32(entry, banks + 4 + 4 * k, big_endian, list[k]);
    }
}

// The entry of `sound` in an archive of `flavor`, from `model`, the entry of a sequence sound. NewSequenceSound
// describes the new entry. The sound takes string index `string`. Throws xsflib::FormatError for a model whose details
// are laid out another way than the tools lay them out, and std::invalid_argument for a start that the model has no
// place for.
std::vector<uint8_t> SequenceSoundEntry(std::span<const uint8_t> model, Flavor flavor, const NewSequenceSound& sound,
                                        std::optional<uint32_t> string)
{
    // The model's details, and its list of banks at their end.
    const bool big_endian = flavor == Flavor::kCafe;
    const auto [details, banks] = PlacesOfDetails(model, big_endian);
    const bool has_start = (xsflib::Reader(model, big_endian).U32(details + 12) & 1) != 0;
    if (!has_start && sound.start != 0)
    {
        throw std::invalid_argument("a start for a sound whose model leaves its start out");
    }

    // The copy, with the sound's file, name, start and banks.
    std::vector<uint8_t> entry(model.begin(), model.begin() + static_cast<std::ptrdiff_t>(banks));
    WriteBanks(entry, banks, big_endian, sound.banks);
    Store32(entry, 0, big_endian, sound.file);
    if (const std::optional<std::size_t> at = StringIndexAt(entry, kSoundOptions, big_endian))
    {
        Store32(entry, *at, big_endian, string.value_or(kNone));
    }
    if (has_start)
    {
        Store32(entry, details + 16, big_endian, sound.start);
    }

    return entry;
}

// The entry of `sound` in an archive of `flavor`, from `model`, the entry of a wave sound or of a sequence sound.
// NewWaveSound describes the new entry. The sound takes string index `string`. Throws xsflib::FormatError for a model
// whose details are laid out another way than the tools lay them out.
std::vector<uint8_t> WaveSoundEntry(std::span<const uint8_t> model, Flavor flavor, const NewWaveSound& sound,
                                    std::optional<uint32_t> string)
{
    const bool big_endian = flavor == Flavor::kCafe;
    const xsflib::Reader r(model, big_endian);
    const std::size_t details = r.U32(kSoundDetails + 4);
    std::vector<uint8_t> entry(model.begin(), model.end());
    if (IsSequenceSound(model, big_endian))
    {
        // A sequence sound's channel priority follows its start, if it holds one. A wave sound's details take the
        // place of its details.
        PlacesOfDetails(model, big_endian);
        const uint32_t options = r.U32(details + 12);
        if ((options & kSequencePriority) == 0)
        {
            throw OtherLayout("a sequence sound's details without its channel priority");
        }
        const uint32_t priority = r.U32(details + 16 + ((options & 1) != 0 ? 4 : 0));
        entry.resize(details + 16);
        Store16(entry, kSoundDetails, big_endian, kWaveSoundDetails);
        Store32(entry, details + 4, big_endian, 1);
        Store32(entry, details + 8, big_endian, kWaveSoundPriority);
        Store32(entry, details + 12, big_endian, priority);
    }
    else if (details + 4 > model.size())
    {
        throw OtherLayout("a wave sound's details past its entry");
    }

    // The sound's file, index and name.
    Store32(entry, 0, big_endian, sound.file);
    Store32(entry, details, big_endian, sound.index);
    if (const std::optional<std::size_t> at = StringIndexAt(entry, kSoundOptions, big_endian))
    {
        Store32(entry, *at, big_endian, string.value_or(kNone));
    }

    return entry;
}

// Throws std::invalid_argument for more than four banks, or for a bank past the bank table of `info`.
void CheckBanks(const InfoBlock& info, std::span<const uint32_t> banks)
{
    const auto past = [&info](uint32_t bank)
    {
        return bank != kNoItem && !ItemIndex(bank, kBankItem, info.banks.size());
    };
    if (banks.size() > kMaxBanks || std::ranges::any_of(banks, past))
    {
        throw std::invalid_argument("more than four banks, or a bank past the bank table");
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

ArchiveWithFile WithNewFile(std::span<const uint8_t> archive, Flavor flavor, std::span<const uint8_t> bytes)
{
    // The file's place: the first multiple of the alignment from the archive's end.
    const std::size_t alignment = flavor == Flavor::kNx ? 64 : 32;
    const std::size_t start = (archive.size() + alignment - 1) / alignment * alignment;
    if (start + bytes.size() > kNone)
    {
        throw std::invalid_argument("an archive that would pass 4 GB");
    }

    // The file's entry, with its offset in the archive as it is. WithTables moves it with the other files.
    auto [strings, info] = ReadTables(archive, flavor);
    const std::optional<BlockEntry> files = EntryOf(xsflib::Reader(archive, flavor == Flavor::kCafe), kFileBlock);
    if (!files || files->offset + kBlockHeaderSize > start)
    {
        throw xsflib::FormatError("an archive without a FILE block");
    }
    const auto offset = static_cast<uint32_t>(start - files->offset - kBlockHeaderSize);
    info.files.push_back(InternalFileEntry(flavor, offset, static_cast<uint32_t>(bytes.size())));

    ArchiveWithFile result;
    result.file = static_cast<uint32_t>(info.files.size() - 1);
    result.archive = WithTables(WithFileAppended(archive, flavor, start, bytes), flavor, strings, std::move(info));

    return result;
}

std::vector<uint8_t> WithNewSequenceSound(std::span<const uint8_t> archive, Flavor flavor,
                                          const NewSequenceSound& sound)
{
    const bool big_endian = flavor == Flavor::kCafe;
    auto [strings, info] = ReadTables(archive, flavor);

    // The sound's model, file, banks and name.
    if (sound.model >= info.sounds.size())
    {
        throw std::invalid_argument("a model past the sound table");
    }
    const std::vector<uint8_t>& model = info.sounds[sound.model];
    if (!IsSequenceSound(model, big_endian))
    {
        throw std::invalid_argument("a model that isn't a sequence sound");
    }
    if (sound.file >= info.files.size())
    {
        throw std::invalid_argument("a file past the file table");
    }
    CheckBanks(info, sound.banks);
    CheckName(strings, sound.name, true);
    if (strings && !StringIndexAt(model, kSoundOptions, big_endian))
    {
        throw OtherLayout("a model without a string index in an archive with names");
    }

    // The entry, the room for the name after the sounds' names, and the name.
    const uint32_t string = NewStringIndex(info, kSoundTable, NameCount(strings));
    std::vector<uint8_t> entry = SequenceSoundEntry(model, flavor, sound, string);
    MakeRoomForName(info, string);
    const auto index = static_cast<uint32_t>(info.sounds.size());
    info.sounds.push_back(std::move(entry));
    if (strings)
    {
        strings->names.insert(strings->names.begin() + string, ItemName{sound.name, ItemId(kSoundItem, index)});
    }

    return WithTables(archive, flavor, strings, std::move(info));
}

std::vector<uint8_t> WithNewWaveSounds(std::span<const uint8_t> archive, Flavor flavor,
                                       std::span<const NewWaveSound> sounds)
{
    const bool big_endian = flavor == Flavor::kCafe;
    auto [strings, info] = ReadTables(archive, flavor);
    const std::size_t count = info.sounds.size();
    for (const NewWaveSound& sound : sounds)
    {
        // The sound's model, file and name.
        if (sound.model >= count)
        {
            throw std::invalid_argument("a model past the sound table");
        }
        const std::vector<uint8_t>& model = info.sounds[sound.model];
        if (!IsWaveSound(model, big_endian) && !IsSequenceSound(model, big_endian))
        {
            throw std::invalid_argument("a model that isn't a wave sound or a sequence sound");
        }
        if (sound.file >= info.files.size())
        {
            throw std::invalid_argument("a file past the file table");
        }
        CheckName(strings, sound.name, true);
        if (strings && !StringIndexAt(model, kSoundOptions, big_endian))
        {
            throw OtherLayout("a model without a string index in an archive with names");
        }

        // The entry, the room for the name after the sounds' names, and the name.
        const uint32_t string = NewStringIndex(info, kSoundTable, NameCount(strings));
        std::vector<uint8_t> entry = WaveSoundEntry(model, flavor, sound, string);
        MakeRoomForName(info, string);
        const auto index = static_cast<uint32_t>(info.sounds.size());
        info.sounds.push_back(std::move(entry));
        if (strings)
        {
            strings->names.insert(strings->names.begin() + string, ItemName{sound.name, ItemId(kSoundItem, index)});
        }
    }

    return WithTables(archive, flavor, strings, std::move(info));
}

std::vector<uint8_t> WithBankLists(std::span<const uint8_t> archive, Flavor flavor,
                                   const std::map<uint32_t, std::vector<uint32_t>>& banks)
{
    const bool big_endian = flavor == Flavor::kCafe;
    auto [strings, info] = ReadTables(archive, flavor);
    for (const auto& [sound, list] : banks)
    {
        if (sound >= info.sounds.size())
        {
            throw std::invalid_argument("a sound past the sound table");
        }
        std::vector<uint8_t>& entry = info.sounds[sound];
        if (!IsSequenceSound(entry, big_endian))
        {
            throw std::invalid_argument("a sound that isn't a sequence sound");
        }
        CheckBanks(info, list);

        WriteBanks(entry, PlacesOfDetails(entry, big_endian).banks, big_endian, list);
    }

    return WithTables(archive, flavor, strings, std::move(info));
}

std::vector<uint8_t> WithNewBank(std::span<const uint8_t> archive, Flavor flavor, const NewBank& bank)
{
    auto [strings, info] = ReadTables(archive, flavor);

    // The bank's file, wave archives and name.
    if (bank.file >= info.files.size())
    {
        throw std::invalid_argument("a file past the file table");
    }
    const auto past = [&info](uint32_t wave_archive)
    {
        return !ItemIndex(wave_archive, kWaveArchiveItem, info.wave_archives.size());
    };
    if (std::ranges::any_of(bank.wave_archives, past))
    {
        throw std::invalid_argument("a wave archive past the wave archive table");
    }
    CheckName(strings, bank.name, true);

    // The entry: the file, the reference to the list, the options with the string index after the banks' names, and
    // the list.
    const uint32_t string = NewStringIndex(info, kBankTable, NameCount(strings));
    const std::size_t list = kOptionsAt[kBankTable] + 8;
    Out entry(flavor == Flavor::kCafe);
    entry.Zeros(list + 4 + 4 * bank.wave_archives.size());
    entry.U32(0, bank.file);
    entry.Reference(4, kListReference, static_cast<uint32_t>(list));
    entry.U32(kOptionsAt[kBankTable], kNameOption);
    entry.U32(kOptionsAt[kBankTable] + 4, string);
    entry.U32(list, static_cast<uint32_t>(bank.wave_archives.size()));
    for (std::size_t k = 0; k < bank.wave_archives.size(); ++k)
    {
        entry.U32(list + 4 + 4 * k, bank.wave_archives[k]);
    }

    // The room for the name, and the name.
    MakeRoomForName(info, string);
    const auto index = static_cast<uint32_t>(info.banks.size());
    info.banks.push_back(entry.Take());
    if (strings)
    {
        strings->names.insert(strings->names.begin() + string, ItemName{bank.name, ItemId(kBankItem, index)});
    }

    return WithTables(archive, flavor, strings, std::move(info));
}

std::vector<uint8_t> WithNewWaveArchive(std::span<const uint8_t> archive, Flavor flavor,
                                        const NewWaveArchive& wave_archive)
{
    auto [strings, info] = ReadTables(archive, flavor);
    if (wave_archive.file >= info.files.size())
    {
        throw std::invalid_argument("a file past the file table");
    }
    CheckName(strings, wave_archive.name, false);

    // The entry: the file, a word of zeros for a wave archive that loads whole, and the options. A name's string index
    // goes after the wave archives' names.
    const bool named = !wave_archive.name.empty();
    const std::size_t options = kOptionsAt[kWaveArchiveTable];
    Out entry(flavor == Flavor::kCafe);
    entry.Zeros(options + (named ? 8 : 4));
    entry.U32(0, wave_archive.file);
    const auto index = static_cast<uint32_t>(info.wave_archives.size());
    if (named)
    {
        const uint32_t string = NewStringIndex(info, kWaveArchiveTable, NameCount(strings));
        entry.U32(options, kNameOption);
        entry.U32(options + 4, string);
        MakeRoomForName(info, string);
        strings->names.insert(strings->names.begin() + string,
                              ItemName{wave_archive.name, ItemId(kWaveArchiveItem, index)});
    }
    info.wave_archives.push_back(entry.Take());

    return WithTables(archive, flavor, strings, std::move(info));
}

} // namespace ntdlib::sar
