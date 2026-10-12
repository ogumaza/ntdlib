// SPDX-License-Identifier: MIT

#include "ntdlib/jaudio/aaf.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
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
constexpr uint32_t kLastBlockType = 8;

// The sound table: a header, then 16-byte entries. The header gives each of 18 categories its count of sounds and the
// index of its first entry: 16 categories of effects, then the sequences, then the streams. An entry gives the sound's
// offset number (for a sequence, its file's number in the sequence archive) and its volume (kAafItemLayout).
constexpr std::size_t kTableHeader = 0x50;
constexpr std::size_t kEntrySize = 0x10;
constexpr uint32_t kEffectCategories = 16;
constexpr uint32_t kSequenceCategory = 16;
constexpr uint32_t kMaxPerCategory = 0x400;

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
                sound.resource = r.U16(entry + *kAafItemLayout.file);
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

    const uint32_t first = r.U16(table + 8 + 4 * std::size_t{kSequenceCategory});
    return r.U16(table + kTableHeader + std::size_t{first} * kEntrySize + *kAafItemLayout.file);
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
