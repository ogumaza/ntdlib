// SPDX-License-Identifier: MIT

#include "ntdlib/jaudio/aaf.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

namespace ntdlib::jaudio
{
namespace
{

// The chunk types that the readers take, and the lists of blocks. A chunk of another type up to type 8 gives one block.
// The sound library skips the words of a later type up to a 0.
constexpr uint32_t kEnd = 0;
constexpr uint32_t kSoundTable = 1;
constexpr uint32_t kBankList = 2;
constexpr uint32_t kWaveSystemList = 3;
constexpr uint32_t kSequenceIndex = 4;
constexpr uint32_t kLastBlockType = 8;

// The sound table: a header, then 16-byte entries. The header's first three bytes give the formats of the effects', the
// sequences' and the streams' entries. Its words from +6 give each of 18 categories its count of sounds and the index
// of its first entry: 16 categories of effects, then the sequences, then the streams. An entry gives the sound's volume
// and, with bit 0 of its format, its offset number (for a sequence, its file's number in the sequence archive).
// Without that bit, the offset number is the sound's index in its category.
constexpr std::size_t kTableHeader = 0x50;
constexpr std::size_t kEntrySize = 0x10;
constexpr std::size_t kSequenceFormat = 1;
constexpr uint8_t kGivesOffsetNumber = 1;
constexpr uint32_t kEffectCategories = 16;
constexpr uint32_t kSequenceCategory = 16;
constexpr uint32_t kMaxPerCategory = 0x400;

// The sequence index (BARC): "BARC", four bytes, a word, the count of sequences, and the name of the archive's file in
// 16 bytes. Then a 32-byte entry for each sequence: its name in up to 14 bytes, the sequence whose data it shares, two
// words, and the sequence's offset and size in the archive.
constexpr std::size_t kIndexHeader = 0x20;
constexpr std::size_t kIndexEntrySize = 0x20;
constexpr std::size_t kIndexNameSize = 14;
constexpr std::size_t kIndexOffset = 0x18;
constexpr std::size_t kIndexSize = 0x1c;
constexpr uint32_t kMaxIndexEntries = 0x10000;

// A wave system of this load type loads for a scene. The others load at start-up.
constexpr uint32_t kSceneLoad = 2;

// Every read past the end of the archive gives one message.
constexpr xsflib::ReaderOptions kReaderOptions{.bounds_message = xsflib::BoundsMessage::kFixed,
                                               .fixed_message = "the AAF archive is damaged"};

// The archive's blocks that the readers take.
struct Blocks
{
    std::size_t table = 0;
    std::size_t table_size = 0;
    std::optional<std::size_t> sequence_index;
    std::vector<std::size_t> wave_systems;
    std::vector<uint32_t> load_types; // the load type of each wave system
};

Blocks FindBlocks(const xsflib::Reader& r)
{
    Blocks blocks;
    for (std::size_t at = 0;;)
    {
        const uint32_t type = r.U32(at);
        at += 4;
        if (type == kEnd)
        {
            return blocks;
        }

        if (type == kBankList || type == kWaveSystemList)
        {
            // Blocks of offset, size and value, up to a 0 word.
            for (; r.U32(at) != 0; at += 12)
            {
                if (type == kWaveSystemList)
                {
                    blocks.wave_systems.push_back(r.U32(at));
                    blocks.load_types.push_back(r.U32(at + 8));
                }
            }

            at += 4;
        }
        else if (type <= kLastBlockType)
        {
            if (type == kSoundTable)
            {
                blocks.table = r.U32(at);
                blocks.table_size = r.U32(at + 4);
            }
            else if (type == kSequenceIndex)
            {
                blocks.sequence_index = r.U32(at);
            }

            at += 12;
        }
        else
        {
            while (r.U32(at) != 0)
            {
                at += 4;
            }

            at += 4;
        }
    }
}

// The sound table's block, checked to hold its header. Throws FormatError for an archive without a table.
std::size_t SoundTable(const xsflib::Reader& r, const Blocks& blocks)
{
    if (!blocks.table)
    {
        throw xsflib::FormatError("the AAF archive has no sound table");
    }

    r.Sub(blocks.table, std::max(blocks.table_size, kTableHeader));

    return blocks.table;
}

// The archives of the AAF's wave systems, or of those that load at start-up.
std::vector<std::string> ArchiveNames(std::span<const uint8_t> aaf, bool staying_only)
{
    const Blocks blocks = FindBlocks(xsflib::Reader(aaf, true, kReaderOptions));
    const std::vector<WaveSystem> systems = ReadAafWaveSystems(aaf);
    std::vector<std::string> names;
    for (std::size_t s = 0; s < systems.size(); s++)
    {
        if (staying_only && blocks.load_types[s] == kSceneLoad)
        {
            continue;
        }

        for (const WaveGroup& group : systems[s].groups)
        {
            if (!group.archive.empty() && std::find(names.begin(), names.end(), group.archive) == names.end())
            {
                names.push_back(group.archive);
            }
        }
    }

    return names;
}

} // namespace

std::vector<SoundEntry> ReadAafSounds(std::span<const uint8_t> aaf)
{
    const xsflib::Reader r(aaf, true, kReaderOptions);
    const std::size_t table = SoundTable(r, FindBlocks(r));
    const bool gives_offset_number = (r.U8(table + kSequenceFormat) & kGivesOffsetNumber) != 0;
    std::vector<SoundEntry> sounds;
    for (uint32_t category = 0; category <= kSequenceCategory; category++)
    {
        // The sound library plays the first sequence itself: it's the effects sequence.
        const uint32_t count = r.U16(table + 6 + 4 * std::size_t{category});
        const uint32_t first = r.U16(table + 8 + 4 * std::size_t{category});
        for (uint32_t index = category == kSequenceCategory ? 1 : 0; index < count && index < kMaxPerCategory; index++)
        {
            const std::size_t entry = table + kTableHeader + (std::size_t{first} + index) * kEntrySize;
            r.Sub(entry, kEntrySize);

            const uint32_t id =
                category < kEffectCategories ? kAafEffect | category << 12 | kAafToEnd | index : kAafSequence | index;
            SoundEntry sound;
            sound.section = static_cast<uint8_t>(id >> 24);
            sound.group = static_cast<uint8_t>(id >> 16);
            sound.index = static_cast<uint16_t>(id);
            sound.type = sound.section;
            sound.volume = r.U8(entry + kAafItemLayout.volume);
            sound.offset = static_cast<uint32_t>(entry);
            if (category == kSequenceCategory)
            {
                sound.source = SoundEntry::Source::kArchive;
                sound.resource = gives_offset_number ? r.U16(entry + *kAafItemLayout.file) : index;
            }

            sounds.push_back(std::move(sound));
        }
    }

    if (sounds.empty())
    {
        throw xsflib::FormatError("the AAF archive's sound table lists no sounds");
    }

    return sounds;
}

uint16_t AafEffectsSequenceFile(std::span<const uint8_t> aaf)
{
    const xsflib::Reader r(aaf, true, kReaderOptions);
    const std::size_t table = SoundTable(r, FindBlocks(r));
    if (r.U16(table + 6 + 4 * std::size_t{kSequenceCategory}) == 0)
    {
        throw xsflib::FormatError("the AAF archive's sound table lists no sequences");
    }

    // The effects sequence is the first sequence: its index is 0.
    if ((r.U8(table + kSequenceFormat) & kGivesOffsetNumber) == 0)
    {
        return 0;
    }

    const uint32_t first = r.U16(table + 8 + 4 * std::size_t{kSequenceCategory});
    return r.U16(table + kTableHeader + std::size_t{first} * kEntrySize + *kAafItemLayout.file);
}

std::optional<std::vector<ArchiveFile>> ReadAafSequenceFiles(std::span<const uint8_t> aaf,
                                                             std::span<const uint8_t> archive)
{
    const xsflib::Reader r(aaf, true, kReaderOptions);
    const std::optional<std::size_t> block = FindBlocks(r).sequence_index;
    if (!block)
    {
        return std::nullopt;
    }

    r.Sub(*block, kIndexHeader);
    const uint32_t count = r.U32(*block + 12);
    if (!r.Magic(*block, "BARC") || count > kMaxIndexEntries)
    {
        throw xsflib::FormatError("the AAF archive's sequence index is damaged");
    }

    std::vector<ArchiveFile> files;
    for (uint32_t number = 0; number < count; number++)
    {
        const std::size_t entry = *block + kIndexHeader + std::size_t{number} * kIndexEntrySize;
        r.Sub(entry, kIndexEntrySize);
        const uint32_t offset = r.U32(entry + kIndexOffset);
        const uint32_t size = r.U32(entry + kIndexSize);
        if (offset > archive.size() || size > archive.size() - offset)
        {
            throw xsflib::FormatError("the AAF archive's sequence index names a sequence outside the sequence archive");
        }

        files.push_back(
            {static_cast<uint16_t>(number), r.CString(entry, kIndexNameSize), archive.subspan(offset, size)});
    }

    return files;
}

std::vector<WaveSystem> ReadAafWaveSystems(std::span<const uint8_t> aaf)
{
    const xsflib::Reader r(aaf, true, kReaderOptions);
    std::vector<WaveSystem> systems;
    for (const std::size_t offset : FindBlocks(r).wave_systems)
    {
        r.Sub(offset, 1);
        systems.push_back(ReadWaveSystem(aaf.subspan(offset), static_cast<uint32_t>(systems.size())));
    }

    return systems;
}

std::vector<std::string> AafWaveArchiveNames(std::span<const uint8_t> aaf)
{
    return ArchiveNames(aaf, false);
}

std::vector<std::string> AafStayingWaveArchiveNames(std::span<const uint8_t> aaf)
{
    return ArchiveNames(aaf, true);
}

} // namespace ntdlib::jaudio
