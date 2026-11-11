// SPDX-License-Identifier: MIT

#include "ntdlib/sar/tables.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/binary.h"

#include "ntdlib/rounding.h"
#include "ntdlib/sar/archive_model.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/groups.h"
#include "ntdlib/sar/info_block.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/sar/string_block.h"
#include "sar/file_bytes.h"
#include "sar/file_parts.h"
#include "sar/format_ids.h"
#include "sar/info_entries.h"
#include "sar/stream_details.h"

namespace ntdlib::sar
{
namespace
{

constexpr std::size_t kMaxStreamTracks = 8;   // the tracks of a stream sound that ntdlib's reader reads
constexpr std::size_t kMaxTrackChannels = 16; // the channels of a stream sound's track that ntdlib's reader reads
constexpr uint32_t kSequencePriority = 2;  // the option of a sequence sound's details that gives its channel priority
constexpr uint32_t kWaveSoundPriority = 1; // the option of a wave sound's details that gives its channel priority

// The option of a wave sound's details in a 3DS archive of CSAR 2.0.0.0 that gives the offset of its sends, and the
// sends that the tools write there: 127 to the main bus, a count of 2 aux buses, and 0 to each.
constexpr uint32_t kCtrWaveSends = 1u << 8;
constexpr std::array<uint8_t, 8> kCtrWaveSoundSends = {0x7F, 0x02, 0, 0, 0, 0, 0, 0};

constexpr std::size_t kSoundOptions = 20; // where a sound's entry holds its options
constexpr std::size_t kSoundDetails = 12; // where a sound's entry refers to its details
constexpr uint32_t kNameOption = 1;       // the option that gives the string index

// The options of a sound's entry that give its pan and its priorities, and the one that gives the offset of its 3D
// settings.
constexpr uint32_t kPanOption = 1u << 1;
constexpr uint32_t kPriorityOption = 1u << 2;
constexpr uint32_t kSettingsOption = 1u << 8;

// What the reader reads for a sound's priorities and its channel priority without their options: a priority of 64 in
// the low byte, and an actor player of 0 or no release priority fix in the next.
constexpr uint32_t kUnsetPriority = 64;

// Where an entry of each table that holds names keeps its options, in the order of InfoTables: the sounds, the sound
// groups, the banks, the wave archives, the groups and the players.
constexpr std::array<std::size_t, 6> kOptionsAt = {kSoundOptions, 24, 12, 8, 4, 4};

// The places of the tables of sounds, banks, wave archives and groups in InfoTables and kOptionsAt.
constexpr std::size_t kSoundsPlace = 0;
constexpr std::size_t kBanksPlace = 2;
constexpr std::size_t kWaveArchivesPlace = 3;
constexpr std::size_t kGroupsPlace = 4;

using detail::BankEntry;
using detail::GroupEntry;
using detail::InfoTables;
using detail::InternalOffsetAt;
using detail::kBlockCount;
using detail::kBlockEntrySize;
using detail::kBlockHeaderSize;
using detail::kBlockTable;
using detail::kDataReference;
using detail::kExternalFile;
using detail::kFileBlock;
using detail::kFileSizeField;
using detail::kHeaderSizeField;
using detail::kInfoBlock;
using detail::kInternalFile;
using detail::kKnownSequenceOptions;
using detail::kKnownSoundOptions;
using detail::kListReference;
using detail::kNone;
using detail::kNothingVersion;
using detail::kSendsVersion;
using detail::kSequenceDetails;
using detail::kSettings3dSize;
using detail::kStreamDetails;
using detail::kStringBlock;
using detail::kTableReference;
using detail::kVersionField;
using detail::kWaveSoundDetails;
using detail::OtherLayout;
using detail::Out;
using detail::Store16;
using detail::Store32;
using detail::StreamDetailsOf;
using detail::StreamHeader;
using detail::WaveArchiveEntry;
using detail::WriteList;

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
    const std::optional<std::size_t> entry = detail::FindBlockEntry(r, type);
    if (!entry)
    {
        return std::nullopt;
    }

    return BlockEntry{*entry, r.U32(*entry + 4), r.U32(*entry + 8)};
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
    return AlignUp(block + kBlockHeaderSize, detail::FileAlignment(flavor));
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
        const std::optional<std::size_t> at = InternalOffsetAt(r);
        if (!at)
        {
            continue;
        }
        const uint32_t offset = r.U32(*at);
        if (offset < first)
        {
            throw xsflib::FormatError("an archive whose file " + std::to_string(i) + " starts before its first file");
        }

        Store32(entry, *at, big_endian, static_cast<uint32_t>(offset + shift));
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
    out.U32(kFileSizeField, static_cast<uint32_t>(out.Size()));

    return out.Take();
}

// The entry of an internal file at `offset` from the end of the FILE block's size, of `size` bytes, as the tools write
// it for `flavor`. The location's reference has the type `data`: kDataReference, or 0 for a group's file. The Wii U and
// the Switch list the groups that hold the file. The list is empty.
std::vector<uint8_t> InternalFileEntry(Flavor flavor, uint32_t offset, uint32_t size, uint16_t data)
{
    const bool ctr = flavor == Flavor::kCtr;
    Out out(flavor == Flavor::kCafe);
    out.Zeros(ctr ? 24 : 36);
    out.Reference(0, kInternalFile, 12);
    out.Reference(12, data, offset);
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

// The string index of a new name for an item at the end of table `table` of `info`, in the order of InfoTables: one
// past the last string index that the entries of that table and the tables before it hold. Throws xsflib::FormatError
// for an index past `names`, the string table's count of names, in an archive with names.
uint32_t NewStringIndex(const InfoBlock& info, std::size_t table, std::optional<std::size_t> names)
{
    const bool big_endian = info.flavor == Flavor::kCafe;
    const auto tables = InfoTables(info);
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
    const auto tables = InfoTables(info);
    for (std::size_t k = 0; k < kOptionsAt.size(); ++k)
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

// Adds `entry` to the end of `table`, a table of `info`, as the entry of a new item of `type` whose name has the string
// index `string`. The string indices from `string` on move up by one first. In an archive with names, `name` then goes
// in at `string` in `strings`.
void AddNamedEntry(InfoBlock& info, std::optional<StringBlock>& strings, std::vector<std::vector<uint8_t>>& table,
                   uint32_t type, uint32_t string, const std::string& name, std::vector<uint8_t> entry)
{
    MakeRoomForName(info, string);
    const auto index = static_cast<uint32_t>(table.size());
    table.push_back(std::move(entry));
    if (strings)
    {
        strings->names.insert(strings->names.begin() + string, ItemName{name, ItemId(type, index)});
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

// The entry of `sound` in an archive of `flavor`, from `model`, the entry of a sequence sound. NewSequenceSound
// describes the new entry. The sound takes string index `string`. Throws xsflib::FormatError for a model whose details
// are laid out another way than the tools lay them out, and std::invalid_argument for a start that the model has no
// place for.
std::vector<uint8_t> SequenceSoundEntry(std::span<const uint8_t> model, Flavor flavor, const NewSequenceSound& sound,
                                        uint32_t string)
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
    WriteList(entry, banks, big_endian, sound.banks);
    Store32(entry, 0, big_endian, sound.file);
    if (const std::optional<std::size_t> at = StringIndexAt(entry, kSoundOptions, big_endian))
    {
        Store32(entry, *at, big_endian, string);
    }
    if (has_start)
    {
        Store32(entry, details + 16, big_endian, sound.start);
    }

    return entry;
}

// The entry of `sound` in an archive of `flavor` and `version`, from `model`, the entry of a wave sound or of a
// sequence sound. NewWaveSound describes the new entry. The sound takes string index `string`. Throws
// xsflib::FormatError for a model whose details are laid out another way than the tools lay them out.
std::vector<uint8_t> WaveSoundEntry(std::span<const uint8_t> model, Flavor flavor, uint32_t version,
                                    const NewWaveSound& sound, uint32_t string)
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
        const bool sends = flavor == Flavor::kCtr && version == kCtrFirstVersion;
        entry.resize(details + (sends ? 28 : 16));
        Store16(entry, kSoundDetails, big_endian, kWaveSoundDetails);
        Store32(entry, details + 4, big_endian, 1);
        Store32(entry, details + 8, big_endian, kWaveSoundPriority | (sends ? kCtrWaveSends : 0));
        Store32(entry, details + 12, big_endian, priority);
        if (sends)
        {
            Store32(entry, details + 16, big_endian, 20);
            std::ranges::copy(kCtrWaveSoundSends, entry.begin() + static_cast<std::ptrdiff_t>(details + 20));
        }
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
        Store32(entry, *at, big_endian, string);
    }

    return entry;
}

// Where the value of option `option`, a bit of the field of bits at `options` in `entry`, lies or would lie among the
// options' values, in the byte order that `big_endian` gives.
std::size_t OptionPlace(std::span<const uint8_t> entry, std::size_t options, uint32_t option, bool big_endian)
{
    const uint32_t bits = xsflib::Reader(entry, big_endian).U32(options);

    return options + 4 + 4 * static_cast<std::size_t>(std::popcount(bits & (option - 1)));
}

// Puts `value` in among the options' values of `entry` at `options` for option `option`, a bit that the field of bits
// leaves out. The bit goes on, and the bytes from the value's place on move 4 bytes on.
void InsertOption(std::vector<uint8_t>& entry, std::size_t options, uint32_t option, uint32_t value, bool big_endian)
{
    const uint32_t bits = xsflib::Reader(entry, big_endian).U32(options);
    const std::size_t at = OptionPlace(entry, options, option, big_endian);
    entry.insert(entry.begin() + static_cast<std::ptrdiff_t>(at), 4, 0);
    Store32(entry, options, big_endian, bits | option);
    Store32(entry, at, big_endian, value);
}

// Gives `entry`, a sound's entry, option `option` of the sound with `value`: the pan or the priorities. The value lies
// where the tools lay it out in an entry that holds the option. The 3D settings and the details move with the bytes
// after the new value, and the offset of the 3D settings and the reference to the details follow them. An entry that
// holds the option keeps it. Throws xsflib::FormatError for unknown options, and for 3D settings or details that don't
// follow the options.
void GiveSoundOption(std::vector<uint8_t>& entry, uint32_t option, uint32_t value, bool big_endian)
{
    if (entry.size() < kSoundOptions + 4)
    {
        throw OtherLayout("a sound's entry that ends before its options");
    }
    const xsflib::Reader r(entry, big_endian);
    const uint32_t bits = r.U32(kSoundOptions);
    if ((bits & option) != 0)
    {
        return;
    }
    if ((bits & ~kKnownSoundOptions) != 0)
    {
        throw OtherLayout("an unknown option of a sound");
    }

    // The values of the options end where the 3D settings or the details start.
    const std::size_t end = kSoundOptions + 4 + 4 * static_cast<std::size_t>(std::popcount(bits));
    const std::size_t details = r.U32(kSoundDetails + 4);
    const bool settings = (bits & kSettingsOption) != 0;
    if (end > entry.size() || details < end || details > entry.size() ||
        (settings && r.U32(OptionPlace(entry, kSoundOptions, kSettingsOption, big_endian)) < end))
    {
        throw OtherLayout("a sound's 3D settings or details that don't follow its options");
    }

    InsertOption(entry, kSoundOptions, option, value, big_endian);
    Store32(entry, kSoundDetails + 4, big_endian, static_cast<uint32_t>(details + 4));
    if (settings)
    {
        const std::size_t at = OptionPlace(entry, kSoundOptions, kSettingsOption, big_endian);
        Store32(entry, at, big_endian, xsflib::Reader(entry, big_endian).U32(at) + 4);
    }
}

// Gives `entry`, a sequence sound's entry, the option of its details that gives its channel priority. The value lies
// where the tools lay it out in details that hold the option. The list of banks moves with the bytes after the new
// value, and the details' reference to it follows. Details that hold the option keep it. Throws xsflib::FormatError for
// details that the tools don't lay out so.
void GiveChannelOption(std::vector<uint8_t>& entry, bool big_endian)
{
    const auto [details, banks] = PlacesOfDetails(entry, big_endian);
    const std::size_t options = details + 12;
    const uint32_t bits = xsflib::Reader(entry, big_endian).U32(options);
    if ((bits & kSequencePriority) != 0)
    {
        return;
    }
    if ((bits & ~kKnownSequenceOptions) != 0)
    {
        throw OtherLayout("an unknown option of a sequence sound's details");
    }
    if (banks < options + 4 + 4 * static_cast<std::size_t>(std::popcount(bits)))
    {
        throw OtherLayout("a sequence sound's list of banks that doesn't follow its details' options");
    }

    InsertOption(entry, options, kSequencePriority, kUnsetPriority, big_endian);
    Store32(entry, details + 4, big_endian, static_cast<uint32_t>(banks + 4 - details));
}

// Whether `entry` is the entry of a stream sound, in the byte order that `big_endian` gives.
bool IsStreamSound(std::span<const uint8_t> entry, bool big_endian)
{
    return entry.size() >= kSoundDetails + 8 && xsflib::Reader(entry, big_endian).U16(kSoundDetails) == kStreamDetails;
}

// Throws std::invalid_argument for the tracks of `stream` that a stream sound can't hold (WithNewStreamSound).
void CheckStream(const StreamSoundInfo& stream)
{
    if (stream.tracks.size() > kMaxStreamTracks)
    {
        throw std::invalid_argument("more than 8 tracks");
    }
    if ((stream.valid_tracks >> stream.tracks.size()) != 0)
    {
        throw std::invalid_argument("a bit of the tracks that play past the tracks");
    }
    for (const StreamTrackInfo& track : stream.tracks)
    {
        const auto past = [&stream](uint8_t channel)
        {
            return channel >= stream.channel_count;
        };
        if (track.channels.empty() || track.channels.size() > kMaxTrackChannels ||
            std::ranges::any_of(track.channels, past))
        {
            throw std::invalid_argument(
                "a track that plays no channel, more than 16 channels, or a channel past the stream's channels");
        }
    }
}

// Throws std::invalid_argument for the tracks of `stream` in an archive of the 3DS before 2.3.0.0: any track, and a
// bit of the tracks that play past 8 tracks. The stream file holds the tracks there (WithNewStreamSound).
void CheckCounts(const StreamSoundInfo& stream)
{
    if (!stream.tracks.empty())
    {
        throw std::invalid_argument("tracks in an archive of the 3DS whose stream sounds hold none");
    }
    if ((stream.valid_tracks >> kMaxStreamTracks) != 0)
    {
        throw std::invalid_argument("a bit of the tracks that play past 8 tracks");
    }
}

// Where a stream sound's details start in a copy of `entry`, a sound's entry: after its 3D settings, or after the
// values of its options without them. The tools write a 32-bit value between the 3D settings and the details of a
// sequence sound or a wave sound, but none for a stream sound. Throws xsflib::FormatError for an entry that ends
// before them, and for 3D settings that don't follow the options.
std::size_t StreamDetailsStart(std::span<const uint8_t> entry, bool big_endian)
{
    if (entry.size() < kSoundOptions + 4)
    {
        throw OtherLayout("a sound's entry that ends before its options");
    }
    const xsflib::Reader r(entry, big_endian);
    const uint32_t bits = r.U32(kSoundOptions);
    const std::size_t end = kSoundOptions + 4 + 4 * static_cast<std::size_t>(std::popcount(bits));
    if (end > entry.size())
    {
        throw OtherLayout("a sound's entry that ends before the values of its options");
    }
    if ((bits & kSettingsOption) == 0)
    {
        return end;
    }
    const std::size_t settings = r.U32(OptionPlace(entry, kSoundOptions, kSettingsOption, big_endian));
    if (settings < end || settings + kSettings3dSize > entry.size())
    {
        throw OtherLayout("a sound's 3D settings that don't follow its options");
    }

    return settings + kSettings3dSize;
}

// A new stream sound's details for `stream` in an archive of `flavor` and `version`, from `model`, the entry of the
// sound that it copies. A stream sound model keeps the fields of its details before its table of tracks. The reference
// after the sends then leads to no extension, and the sound has no prefetch file: both describe the model's stream,
// such as the loop of a stream that the stream file doesn't give. A model of another kind takes the version's fields.
// Throws xsflib::FormatError for a stream sound model whose details hold no table of tracks or end before their pitch
// and sends.
std::vector<uint8_t> NewStreamDetails(std::span<const uint8_t> model, Flavor flavor, uint32_t version,
                                      const StreamSoundInfo& stream)
{
    const bool big_endian = flavor == Flavor::kCafe;
    if (flavor == Flavor::kCtr && version < kCtrTracksVersion)
    {
        Out out(false);
        out.Zeros(8);
        out.U16(0, stream.valid_tracks);
        out.U16(2, stream.channel_count);
        return out.Take();
    }
    if (flavor == Flavor::kCtr && version < kCtrFieldsVersion)
    {
        // The field of options holds no value of an extension.
        Out out(false);
        out.Zeros(16);
        return StreamDetailsOf(out.Take(), stream, false, flavor);
    }

    const uint32_t layout = flavor == Flavor::kCtr ? kNothingVersion : version;
    const bool sends = layout >= kSendsVersion;
    std::vector<uint8_t> header = StreamHeader(layout, big_endian);
    if (IsStreamSound(model, big_endian))
    {
        const xsflib::Reader r(model, big_endian);
        const std::size_t details = r.U32(kSoundDetails + 4);
        if (details + 12 > model.size() || r.U16(details + 4) != kTableReference ||
            details + std::size_t{r.U32(details + 8)} > model.size())
        {
            throw OtherLayout("a stream sound's details without a table of tracks in its entry");
        }
        header.assign(model.begin() + static_cast<std::ptrdiff_t>(details),
                      model.begin() + static_cast<std::ptrdiff_t>(details + r.U32(details + 8)));
        if (sends && header.size() >= 32)
        {
            Store32(header, 24, big_endian, 0);
            Store32(header, 28, big_endian, kNone);
        }
        if (sends && header.size() >= 36)
        {
            Store32(header, 32, big_endian, kNone);
        }
    }
    if (header.size() < (sends ? 24 : 12))
    {
        throw OtherLayout("a stream sound's details that end before their pitch and sends");
    }

    return StreamDetailsOf(header, stream, sends, flavor);
}

// Throws std::invalid_argument for more than four banks, or for a bank past the bank table of `info`.
void CheckBanks(const InfoBlock& info, std::span<const uint32_t> banks)
{
    const auto past = [&info](uint32_t bank)
    {
        return bank != kNoItem && !ItemIndex(bank, kBankItem, info.banks.size());
    };
    if (banks.size() > kBankSlots || std::ranges::any_of(banks, past))
    {
        throw std::invalid_argument("more than four banks, or a bank past the bank table");
    }
}

// The highest version of the groups' files that the FILE block of `archive` holds, or nothing without one. `info` is
// the archive's INFO block.
std::optional<uint32_t> GroupFileVersion(std::span<const uint8_t> archive, Flavor flavor, const InfoBlock& info)
{
    const bool big_endian = flavor == Flavor::kCafe;
    const xsflib::Reader r(archive, big_endian);
    const std::optional<BlockEntry> files = EntryOf(r, kFileBlock);
    if (!files)
    {
        return std::nullopt;
    }

    std::optional<uint32_t> highest;
    for (const std::vector<uint8_t>& group : info.groups)
    {
        const uint32_t file = xsflib::Reader(group, big_endian).U32(0);
        if (file >= info.files.size())
        {
            continue;
        }
        const xsflib::Reader entry(info.files[file], big_endian);
        const std::optional<std::size_t> place = InternalOffsetAt(entry);
        if (!place)
        {
            continue;
        }
        const std::size_t at = std::size_t{files->offset} + kBlockHeaderSize + entry.U32(*place);
        if (at + kVersionField + 4 <= archive.size())
        {
            highest = std::max(highest.value_or(0), r.U32(at + kVersionField));
        }
    }

    return highest;
}

// `archive` with `bytes` as a new file after its last one, as WithNewFile adds one, with the type `data` in the
// reference of the entry's location (InternalFileEntry).
ArchiveWithFile WithFileAdded(std::span<const uint8_t> archive, Flavor flavor, std::span<const uint8_t> bytes,
                              uint16_t data)
{
    // The file's place: the first multiple of the alignment from the archive's end.
    const std::size_t start = AlignUp(archive.size(), detail::FileAlignment(flavor));
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
    info.files.push_back(InternalFileEntry(flavor, offset, static_cast<uint32_t>(bytes.size()), data));

    ArchiveWithFile result;
    result.file = static_cast<uint32_t>(info.files.size() - 1);
    result.archive = WithTables(WithFileAppended(archive, flavor, start, bytes), flavor, strings, std::move(info));

    return result;
}

// The size of an archive's header, and the entries of its STRG, INFO and FILE blocks in the header's table.
struct ArchiveBlocks
{
    std::size_t header_size = 0;
    BlockEntry strings;
    BlockEntry info;
    BlockEntry files;
};

// The blocks of the archive of `size` bytes that `r` reads. The header lists the STRG, INFO and FILE blocks in that
// order. They follow it in that order, without gaps. An archive without names lists a STRG block whose offset is none.
// Throws OtherLayout for other blocks or another order, std::invalid_argument for an archive with names without
// `has_strings` or one without names with it, and xsflib::FormatError for an archive cut short or one that doesn't
// end with its FILE block.
ArchiveBlocks ListedBlocks(const xsflib::Reader& r, std::size_t size, bool has_strings)
{
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
    if (named != has_strings)
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
    if (r.U32(kFileSizeField) != size || files->offset + std::size_t{files->size} != size)
    {
        throw xsflib::FormatError("an archive cut short, or one that doesn't end with its FILE block");
    }

    return {header_size, *old_strings, *old_info, *files};
}

// Where a file's entry holds the file's offset in the FILE block, with the offset and the size.
struct FileLocation
{
    std::size_t at = 0;
    uint32_t offset = 0;
    uint32_t size = 0;
};

// The location that `entry`, a file's entry, gives. Nothing for a file that the FILE block doesn't hold.
std::optional<FileLocation> LocationOf(std::span<const uint8_t> entry, bool big_endian)
{
    const xsflib::Reader r(entry, big_endian);
    const std::optional<std::size_t> at = InternalOffsetAt(r);
    if (!at)
    {
        return std::nullopt;
    }

    return FileLocation{*at, r.U32(*at), r.U32(*at + 4)};
}

// The places in an archive of a file that new bytes replace: where the file starts and ends and where its padding
// ends, and where the new bytes and their padding end.
struct Replacement
{
    std::size_t start = 0;
    std::size_t end = 0;
    std::size_t padded = 0;
    std::size_t new_end = 0;
    std::size_t new_padded = 0;
};

// Moves the entries of the files of `info` for `new_size` new bytes in place of file `name` at `old`, at `places`. The
// FILE block's body starts at `body`. The files after it move, and the file IDs that name the same bytes take the new
// size. The files inside it move where `inner` says. Throws std::invalid_argument for a file that would run past the
// new bytes, and OtherLayout for a file that overlaps the old bytes otherwise.
void MoveFileEntries(InfoBlock& info, bool big_endian, std::size_t body, const FileLocation& old,
                     const Replacement& places, std::size_t new_size, std::span<const InnerFile> inner,
                     const std::string& name)
{
    const auto shift = static_cast<int64_t>(places.new_padded) - static_cast<int64_t>(places.padded);
    const auto inner_at = [&](std::size_t at, uint32_t size) -> std::optional<uint32_t>
    {
        if (at < places.start || at + size > places.end)
        {
            return std::nullopt;
        }

        const auto found = std::ranges::find(inner, at - places.start, &InnerFile::from);
        if (found == inner.end())
        {
            return std::nullopt;
        }

        return found->to;
    };
    for (std::size_t i = 0; i < info.files.size(); ++i)
    {
        const std::optional<FileLocation> location = LocationOf(info.files[i], big_endian);
        if (!location)
        {
            continue;
        }

        const std::size_t at = body + location->offset;
        if (location->offset == old.offset && location->size == old.size)
        {
            Store32(info.files[i], location->at + 4, big_endian, static_cast<uint32_t>(new_size));
        }
        else if (at >= places.padded)
        {
            Store32(info.files[i], location->at, big_endian, static_cast<uint32_t>(location->offset + shift));
        }
        else if (const std::optional<uint32_t> to = inner_at(at, location->size))
        {
            if (std::size_t{*to} + location->size > new_size)
            {
                throw std::invalid_argument("file " + std::to_string(i) + " would run past the new bytes of " + name);
            }

            Store32(info.files[i], location->at, big_endian, old.offset + *to);
        }
        else if (at >= places.start || at + location->size > places.start)
        {
            throw OtherLayout("file " + std::to_string(i) + " overlaps " + name);
        }
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

    const bool big_endian = flavor == Flavor::kCafe;
    const xsflib::Reader r(archive, big_endian);
    const ArchiveBlocks blocks = ListedBlocks(r, archive.size(), strings.has_value());
    const std::size_t header_size = blocks.header_size;
    const BlockEntry& files = blocks.files;
    const bool named = blocks.strings.offset != kNone;

    // The FILE block: its magic and size, then zeros up to its first file.
    const std::size_t first = FirstFile(files.offset, flavor);
    if (!r.Magic(files.offset, "FILE") || r.U32(files.offset + 4) != files.size)
    {
        throw OtherLayout("its FILE block has another magic, or another size than its header gives");
    }
    if (first > archive.size())
    {
        throw OtherLayout("its FILE block ends before its first file's place");
    }
    const std::span<const uint8_t> lead =
        archive.subspan(files.offset + kBlockHeaderSize, first - files.offset - kBlockHeaderSize);
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
    const auto shift = static_cast<int64_t>(new_first - new_files) - static_cast<int64_t>(first - files.offset);
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
        out.U32(blocks.strings.entry + 4, static_cast<uint32_t>(header_size));
        out.U32(blocks.strings.entry + 8, static_cast<uint32_t>(new_strings.size()));
    }
    out.U32(blocks.info.entry + 4, static_cast<uint32_t>(header_size + new_strings.size()));
    out.U32(blocks.info.entry + 8, static_cast<uint32_t>(new_info.size()));
    out.U32(files.entry + 4, static_cast<uint32_t>(new_files));
    out.U32(files.entry + 8, static_cast<uint32_t>(out.Size() - new_files));
    out.U32(new_files + 4, static_cast<uint32_t>(out.Size() - new_files));
    out.U32(kFileSizeField, static_cast<uint32_t>(out.Size()));

    return out.Take();
}

ArchiveWithFile WithNewFile(std::span<const uint8_t> archive, Flavor flavor, std::span<const uint8_t> bytes)
{
    return WithFileAdded(archive, flavor, bytes, kDataReference);
}

ArchiveWithFile WithNewExternalFile(std::span<const uint8_t> archive, Flavor flavor, const std::string& path)
{
    if (path.empty() || path.find('\0') != std::string::npos)
    {
        throw std::invalid_argument("an empty path, or a path with a zero byte");
    }

    // The entry: the reference to the path, no options, then the path and a zero, padded to 4 bytes.
    auto [strings, info] = ReadTables(archive, flavor);
    Out entry(flavor == Flavor::kCafe);
    entry.Zeros(12);
    entry.Reference(0, kExternalFile, 12);
    entry.Append(std::span(reinterpret_cast<const uint8_t*>(path.data()), path.size()));
    entry.Zeros(1);
    entry.Align(4);
    info.files.push_back(entry.Take());

    ArchiveWithFile result;
    result.file = static_cast<uint32_t>(info.files.size() - 1);
    result.archive = WithTables(archive, flavor, strings, std::move(info));

    return result;
}

std::vector<uint8_t> WithFileBytes(std::span<const uint8_t> archive, Flavor flavor, uint32_t file,
                                   std::span<const uint8_t> bytes, std::span<const InnerFile> inner)
{
    auto [strings, info] = ReadTables(archive, flavor);
    const std::string name = "file " + std::to_string(file);
    if (file >= info.files.size())
    {
        throw std::invalid_argument("the archive holds no " + name);
    }
    const bool big_endian = flavor == Flavor::kCafe;
    const std::optional<BlockEntry> files = EntryOf(xsflib::Reader(archive, big_endian), kFileBlock);
    if (!files || files->offset + kBlockHeaderSize > archive.size())
    {
        throw xsflib::FormatError("an archive without a FILE block");
    }
    const std::optional<FileLocation> old = LocationOf(info.files[file], big_endian);
    if (!old)
    {
        throw std::invalid_argument("the archive's FILE block holds no copy of " + name);
    }

    // The file and its padding, then the new bytes and theirs. The padding of a file that ends the archive is empty.
    const std::size_t alignment = detail::FileAlignment(flavor);
    const std::size_t body = files->offset + kBlockHeaderSize;
    Replacement places;
    places.start = body + old->offset;
    places.end = places.start + old->size;
    const bool last = places.end == archive.size();
    places.padded = last ? places.end : AlignUp(places.end, alignment);
    if (places.padded > archive.size() || std::ranges::any_of(archive.subspan(places.end, places.padded - places.end),
                                                              [](uint8_t byte) { return byte != 0; }))
    {
        throw OtherLayout(name + " runs past its end, or bytes other than zeros pad it");
    }
    places.new_end = places.start + bytes.size();
    places.new_padded = last ? places.new_end : AlignUp(places.new_end, alignment);
    if (archive.size() - places.padded + places.new_padded > kNone)
    {
        throw std::invalid_argument("an archive that would pass 4 GB");
    }

    MoveFileEntries(info, big_endian, body, *old, places, bytes.size(), inner, name);

    Out out(big_endian);
    out.Append(archive.first(places.start));
    out.Append(bytes);
    out.Zeros(places.new_padded - places.new_end);
    out.Append(archive.subspan(places.padded));
    const auto size = static_cast<uint32_t>(out.Size() - files->offset);
    out.U32(files->offset + 4, size);
    out.U32(files->entry + 8, size);
    out.U32(kFileSizeField, static_cast<uint32_t>(out.Size()));

    return WithTables(out.Take(), flavor, strings, std::move(info));
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
    const uint32_t string = NewStringIndex(info, kSoundsPlace, NameCount(strings));
    AddNamedEntry(info, strings, info.sounds, kSoundItem, string, sound.name,
                  SequenceSoundEntry(model, flavor, sound, string));

    return WithTables(archive, flavor, strings, std::move(info));
}

std::vector<uint8_t> WithNewWaveSounds(std::span<const uint8_t> archive, Flavor flavor,
                                       std::span<const NewWaveSound> sounds)
{
    const bool big_endian = flavor == Flavor::kCafe;
    auto [strings, info] = ReadTables(archive, flavor);
    const uint32_t version = xsflib::Reader(archive, big_endian).U32(kVersionField);
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
        const uint32_t string = NewStringIndex(info, kSoundsPlace, NameCount(strings));
        AddNamedEntry(info, strings, info.sounds, kSoundItem, string, sound.name,
                      WaveSoundEntry(model, flavor, version, sound, string));
    }

    return WithTables(archive, flavor, strings, std::move(info));
}

std::vector<uint8_t> WithNewStreamSound(std::span<const uint8_t> archive, Flavor flavor, const NewStreamSound& sound)
{
    const bool big_endian = flavor == Flavor::kCafe;
    const bool ctr = flavor == Flavor::kCtr;
    auto [strings, info] = ReadTables(archive, flavor);
    const uint32_t version = xsflib::Reader(archive, big_endian).U32(kVersionField);
    if (ctr && !LaysOutStreamSounds(version))
    {
        throw std::invalid_argument("an archive of the 3DS of a version whose stream sounds ntdlib doesn't lay out");
    }

    // The sound's model, file, tracks and name.
    if (sound.model >= info.sounds.size())
    {
        throw std::invalid_argument("a model past the sound table");
    }
    const std::vector<uint8_t>& model = info.sounds[sound.model];
    if (sound.file >= info.files.size())
    {
        throw std::invalid_argument("a file past the file table");
    }
    if (ctr && version < kCtrTracksVersion)
    {
        CheckCounts(sound.stream);
    }
    else
    {
        CheckStream(sound.stream);
    }
    CheckName(strings, sound.name, true);
    if (strings && !StringIndexAt(model, kSoundOptions, big_endian))
    {
        throw OtherLayout("a model without a string index in an archive with names");
    }

    // The new details start after the model's 3D settings. In a 3DS archive of 2.0.0.0, a stream sound holds the value
    // after them that every sound holds there, and the copy keeps the model's.
    const xsflib::Reader r(model, big_endian);
    if (model.size() < kSoundDetails + 8)
    {
        throw OtherLayout("a sound's entry that ends before its details");
    }
    const std::size_t details = r.U32(kSoundDetails + 4);
    const std::size_t start = StreamDetailsStart(model, big_endian) + (ctr && version == kCtrFirstVersion ? 4 : 0);
    if (start > details || details > model.size())
    {
        throw OtherLayout("a sound's details that start before the end of its 3D settings");
    }

    // The entry: the model's fields up to the new details, then the details.
    std::vector<uint8_t> entry(model.begin(), model.begin() + static_cast<std::ptrdiff_t>(start));
    const std::vector<uint8_t> stream = NewStreamDetails(model, flavor, version, sound.stream);
    entry.insert(entry.end(), stream.begin(), stream.end());
    Store16(entry, kSoundDetails, big_endian, kStreamDetails);
    Store32(entry, kSoundDetails + 4, big_endian, static_cast<uint32_t>(start));
    Store32(entry, 0, big_endian, sound.file);
    const uint32_t string = NewStringIndex(info, kSoundsPlace, NameCount(strings));
    if (const std::optional<std::size_t> at = StringIndexAt(entry, kSoundOptions, big_endian))
    {
        Store32(entry, *at, big_endian, string);
    }

    // The room for the name after the sounds' names, and the name.
    AddNamedEntry(info, strings, info.sounds, kSoundItem, string, sound.name, std::move(entry));

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

        WriteList(entry, PlacesOfDetails(entry, big_endian).banks, big_endian, list);
    }

    return WithTables(archive, flavor, strings, std::move(info));
}

std::vector<uint8_t> WithValuePlaces(std::span<const uint8_t> archive, Flavor flavor,
                                     const std::map<uint32_t, ValuePlaces>& places)
{
    const bool big_endian = flavor == Flavor::kCafe;
    auto [strings, info] = ReadTables(archive, flavor);
    bool changed = false;
    for (const auto& [sound, wanted] : places)
    {
        if (sound >= info.sounds.size())
        {
            throw std::invalid_argument("a sound past the sound table");
        }
        std::vector<uint8_t>& entry = info.sounds[sound];
        if (wanted.channel && !IsSequenceSound(entry, big_endian))
        {
            throw std::invalid_argument("a channel priority of a sound that isn't a sequence sound");
        }

        const std::size_t size = entry.size();
        if (wanted.pan)
        {
            GiveSoundOption(entry, kPanOption, 0, big_endian);
        }
        if (wanted.priority)
        {
            GiveSoundOption(entry, kPriorityOption, kUnsetPriority, big_endian);
        }
        if (wanted.channel)
        {
            GiveChannelOption(entry, big_endian);
        }
        changed = changed || entry.size() != size;
    }
    if (!changed)
    {
        return std::vector<uint8_t>(archive.begin(), archive.end());
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

    // The entry with the string index after the banks' names, the room for the name, and the name.
    const uint32_t string = NewStringIndex(info, kBanksPlace, NameCount(strings));
    AddNamedEntry(
        info, strings, info.banks, kBankItem, string, bank.name,
        BankEntry({.file_id = bank.file, .wave_archives = bank.wave_archives}, string, flavor == Flavor::kCafe));

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

    // The entry of a wave archive that loads whole. A name's string index goes after the wave archives' names.
    std::optional<uint32_t> string;
    if (!wave_archive.name.empty())
    {
        string = NewStringIndex(info, kWaveArchivesPlace, NameCount(strings));
    }
    std::vector<uint8_t> entry = WaveArchiveEntry({.file_id = wave_archive.file}, string, flavor == Flavor::kCafe);
    if (string)
    {
        AddNamedEntry(info, strings, info.wave_archives, kWaveArchiveItem, *string, wave_archive.name,
                      std::move(entry));
    }
    else
    {
        info.wave_archives.push_back(std::move(entry));
    }

    return WithTables(archive, flavor, strings, std::move(info));
}

std::vector<uint8_t> WithNewGroup(std::span<const uint8_t> archive, Flavor flavor, const std::string& name)
{
    // The name first: the file mustn't go in for a group that can't. On the 3DS, the group's file takes the version of
    // the archive's groups' files. The FILE block holds a group's file up to the end of its INFX block's entries, or
    // whole in a version without INFX.
    const ReadBlocks tables = ReadTables(archive, flavor);
    CheckName(tables.strings, name, true);
    std::optional<uint32_t> version;
    if (flavor == Flavor::kCtr)
    {
        version = GroupFileVersion(archive, flavor, tables.info);
    }
    const std::vector<uint8_t> group = EmptyGroupFile(flavor, version);
    const ArchiveWithFile with =
        WithFileAdded(archive, flavor, std::span(group).first(GroupSizeInArchive(group, flavor)), 0);
    auto [strings, info] = ReadTables(with.archive, flavor);

    // The entry with the string index after the groups' names, the room for the name, and the name.
    const uint32_t string = NewStringIndex(info, kGroupsPlace, NameCount(strings));
    AddNamedEntry(info, strings, info.groups, kGroupItem, string, name,
                  GroupEntry({.file_id = with.file}, string, flavor == Flavor::kCafe));

    return WithTables(with.archive, flavor, strings, std::move(info));
}

} // namespace ntdlib::sar
