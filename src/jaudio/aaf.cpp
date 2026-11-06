// SPDX-License-Identifier: MIT

#include "ntdlib/jaudio/aaf.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "jaudio/barc_parse.h"
#include "ntdlib/jaudio/archive.h"
#include "ntdlib/jaudio/bank.h"
#include "ntdlib/jaudio/rarc.h"
#include "ntdlib/jaudio/wave_system.h"

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
constexpr uint32_t kSceneTable = 6;
constexpr uint32_t kLastBlockType = 8;

// The sizes of a list's entry and of a block, and where an entry holds its value. A little-endian archive widens the
// offsets of the list entries and of a block of type 5 to 64 bits.
constexpr std::size_t kEntryBytes = 12;
constexpr std::size_t kWideEntryBytes = 16;
constexpr std::size_t kEntryValue = 8;
constexpr std::size_t kWideEntryValue = 12;
constexpr uint32_t kWideBlockType = 5;

// The sound table: a header, then 16-byte entries. The header's first three bytes give the formats of the effects', the
// sequences' and the streams' entries. Its words from +6 give each of 18 categories its count of sounds and the index
// of its first entry: 16 categories of effects, then the sequences, then the streams. An entry gives the sound's volume
// and, with bit 0 of its format, its offset number (for a sequence, its file's number in the sequence archive).
// Without that bit, the offset number is the sound's index in its category. With bit 1, the sound library plays the
// sound at the pitch that the entry gives at +8, and with bit 2 at the volume that it gives at +0xc
// (JAIBasic::setSeExtParameter, 0x80291034 in The Wind Waker's main.dol). Without bit 2, the sound plays at full
// volume. Pac-Man Vs. (0x8003aaec) and Luigi's Mansion (0x801908d8) test the same bits. Luigi's Mansion's sequences
// have format 0, and every entry gives 255 there.
constexpr std::size_t kTableHeader = 0x50;
constexpr std::size_t kEntrySize = 0x10;
constexpr std::size_t kEffectFormat = 0;
constexpr std::size_t kSequenceFormat = 1;
constexpr std::size_t kPitch = 8;
constexpr uint8_t kGivesOffsetNumber = 1;
constexpr uint8_t kGivesPitch = 2;
constexpr uint8_t kGivesVolume = 4;
constexpr uint8_t kFullVolume = 127;
constexpr uint32_t kEffectCategories = 16;
constexpr uint32_t kSequenceCategory = 16;
constexpr uint32_t kMaxPerCategory = 0x400;

// A wave system of this load type loads for a scene. The others load at start-up.
constexpr uint32_t kSceneLoad = 2;

// Every read past the end of the archive gives one message.
constexpr xsflib::ReaderOptions kAafReaderOptions{.bounds_message = xsflib::BoundsMessage::kFixed,
                                                  .fixed_message = "the AAF archive is damaged"};

// Whether the archive's first word is a block's type in little-endian order.
bool IsLittleEndianAaf(std::span<const uint8_t> data)
{
    if (data.size() < 4)
    {
        return false;
    }

    const uint32_t first = xsflib::LoadLe32(data.data());
    return first >= kSoundTable && first <= kLastBlockType;
}

// A reader of the archive in its byte order.
xsflib::Reader AafReader(std::span<const uint8_t> aaf)
{
    return xsflib::Reader(aaf, !IsLittleEndianAaf(aaf), kAafReaderOptions);
}

// A block of the bank list or the wave system list: its offset, and its value. A bank's value is the wave system that
// it plays. A wave system's value is its load type.
struct ListedBlock
{
    std::size_t offset = 0;
    uint32_t value = 0;
};

// The archive's blocks that the readers take.
struct Blocks
{
    std::size_t table = 0;
    std::size_t table_size = 0;
    std::optional<std::size_t> sequence_index;
    std::optional<std::size_t> scene_table;
    std::size_t scene_table_size = 0;
    std::vector<ListedBlock> banks;
    std::vector<ListedBlock> wave_systems;
};

Blocks FindBlocks(const xsflib::Reader& r)
{
    const bool wide = !r.BigEndian();
    const std::size_t entry_bytes = wide ? kWideEntryBytes : kEntryBytes;
    const std::size_t entry_value = wide ? kWideEntryValue : kEntryValue;
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
            std::vector<ListedBlock>& list = type == kBankList ? blocks.banks : blocks.wave_systems;
            for (; r.U32(at) != 0; at += entry_bytes)
            {
                list.push_back({r.U32(at), r.U32(at + entry_value)});
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
            else if (type == kSceneTable)
            {
                blocks.scene_table = r.U32(at);
                blocks.scene_table_size = r.U32(at + 4);
            }

            at += wide && type == kWideBlockType ? kWideEntryBytes : kEntryBytes;
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

// A category's count of sounds in the sound table at `table`.
uint16_t CategoryCount(const xsflib::Reader& r, std::size_t table, uint32_t category)
{
    return r.U16(table + 6 + 4 * std::size_t{category});
}

// The index of a category's first entry in the sound table at `table`.
uint16_t CategoryFirst(const xsflib::Reader& r, std::size_t table, uint32_t category)
{
    return r.U16(table + 8 + 4 * std::size_t{category});
}

// The place of entry `index` of the sound table at `table`.
std::size_t TableEntry(std::size_t table, std::size_t index)
{
    return table + kTableHeader + index * kEntrySize;
}

// The archives of the AAF's wave systems, or of those that load at start-up.
std::vector<std::string> ArchiveNames(std::span<const uint8_t> aaf, bool staying_only)
{
    const Blocks blocks = FindBlocks(AafReader(aaf));
    const std::vector<WaveSystem> systems = ReadAafWaveSystems(aaf);
    std::vector<std::string> names;
    for (std::size_t s = 0; s < systems.size(); s++)
    {
        if (staying_only && blocks.wave_systems[s].value == kSceneLoad)
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

// The places in the list of the AAF's wave systems, by the id in each system's header. Only the earlier JAudio1 finds
// a wave's system that way. Luigi's Mansion's sound library registers each system under the id in its header, from 0
// to 0xff. The first system with an id keeps it (0x801a25a0 and 0x801a2d80 in the USA release's main.dol). The high
// half of a wave's id names the system. With 0xffff, the wave is in the bank's system (0x801a2680). The later sound
// libraries register each system under its place in the list. Each bank plays the system that its value in the list
// gives. A note takes its wave from its bank's system. The USA releases' main.dol files show it: Super Mario
// Sunshine's at 0x8030151c, 0x803015f8 and 0x8030dda8, The Wind Waker's at 0x80291388 and 0x80291410, Pac-Man Vs.'s
// at 0x8003b084 and 0x8003b108, and Pikmin 2's at 0x800abee4.
//
// The AAF doesn't name the library's build. Two conditions stand in for it. The archive has a sequence index, as
// Luigi's Mansion's and Sunshine's have. The others above have none. And no two systems share an id. Luigi's
// Mansion's systems give 0, 8, 1 and 2. Sunshine's three all give 0. Each of the five games gets its library's way.
// The Wind Waker's and Pac-Man Vs.'s systems give distinct ids that differ from their places. The sequence index alone
// keeps them to their places. An archive that fails either condition gets no places.
std::map<uint16_t, uint32_t> WaveSystemPlaces(const xsflib::Reader& r, const Blocks& blocks)
{
    constexpr std::size_t kIdAt = 8;
    constexpr uint32_t kIds = 0x100;
    std::map<uint16_t, uint32_t> places;
    for (std::size_t s = 0; blocks.sequence_index && s < blocks.wave_systems.size(); s++)
    {
        const uint32_t id = r.U32(blocks.wave_systems[s].offset + kIdAt);
        if (id < kIds && !places.try_emplace(static_cast<uint16_t>(id), static_cast<uint32_t>(s)).second)
        {
            return {};
        }
    }

    return places;
}

// Fills `bank`'s named_systems from `places` (WaveSystemPlaces) if a velocity region names a system other than the
// bank's.
void NameSystems(Bank& bank, const std::map<uint16_t, uint32_t>& places)
{
    bool other = false;
    const auto name = [&](const std::vector<VelocityRegion>& regions)
    {
        for (const VelocityRegion& region : regions)
        {
            if (const auto found = places.find(region.system_id); found != places.end())
            {
                bank.named_systems[region.system_id] = found->second;
                other = other || found->second != bank.wave_system;
            }
        }
    };
    for (const auto& [program, sound] : bank.programs)
    {
        if (const auto* inst = std::get_if<Instrument>(&sound))
        {
            for (const KeyRegion& keys : inst->keys)
            {
                name(keys.velocities);
            }
        }
        else
        {
            for (const std::optional<Drum>& drum : std::get<DrumSet>(sound).keys)
            {
                if (drum)
                {
                    name(drum->velocities);
                }
            }
        }
    }

    if (!other)
    {
        bank.named_systems.clear();
    }
}

} // namespace

bool IsAaf(std::span<const uint8_t> data)
{
    if (data.size() < 4)
    {
        return false;
    }

    const uint32_t first = xsflib::LoadBe32(data.data());
    return (first >= kSoundTable && first <= kLastBlockType) || IsLittleEndianAaf(data);
}

std::vector<SoundEntry> ReadAafSounds(std::span<const uint8_t> aaf)
{
    const xsflib::Reader r = AafReader(aaf);
    const std::size_t table = SoundTable(r, FindBlocks(r));
    const bool gives_offset_number = (r.U8(table + kSequenceFormat) & kGivesOffsetNumber) != 0;
    const bool gives_effect_number = (r.U8(table + kEffectFormat) & kGivesOffsetNumber) != 0;
    const bool gives_effect_pitch = (r.U8(table + kEffectFormat) & kGivesPitch) != 0;
    const bool gives_effect_volume = (r.U8(table + kEffectFormat) & kGivesVolume) != 0;
    const bool gives_sequence_volume = (r.U8(table + kSequenceFormat) & kGivesVolume) != 0;
    std::vector<SoundEntry> sounds;
    for (uint32_t category = 0; category <= kSequenceCategory; category++)
    {
        // The sound library plays the first sequence itself: it's the effects sequence.
        const uint32_t count = CategoryCount(r, table, category);
        const uint32_t first = CategoryFirst(r, table, category);
        for (uint32_t index = category == kSequenceCategory ? 1 : 0; index < count && index < kMaxPerCategory; index++)
        {
            const std::size_t entry = TableEntry(table, std::size_t{first} + index);
            r.Sub(entry, kEntrySize);

            const uint32_t id =
                category < kEffectCategories ? kAafEffect | category << 12 | kAafToEnd | index : kAafSequence | index;
            SoundEntry sound;
            sound.section = static_cast<uint8_t>(id >> 24);
            sound.group = static_cast<uint8_t>(id >> 16);
            sound.index = static_cast<uint16_t>(id);
            sound.type = sound.section;
            const bool gives_volume = category == kSequenceCategory ? gives_sequence_volume : gives_effect_volume;
            sound.volume = gives_volume ? r.U8(entry + kAafItemLayout.volume) : kFullVolume;
            sound.offset = static_cast<uint32_t>(entry);
            if (category == kSequenceCategory)
            {
                sound.source = SoundEntry::Source::kArchive;
                sound.resource = gives_offset_number ? r.U16(entry + *kAafItemLayout.file) : index;
            }
            else
            {
                sound.resource = gives_effect_number ? r.U16(entry + *kAafItemLayout.file) : index;
                // The sound keeps its notes' pitch if the entry's pitch isn't a positive number.
                const float pitch = gives_effect_pitch ? r.F32(entry + kPitch) : 1.0f;
                sound.pitch = std::isfinite(pitch) && pitch > 0.0f ? pitch : 1.0f;
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
    const xsflib::Reader r = AafReader(aaf);
    const std::size_t table = SoundTable(r, FindBlocks(r));
    if (CategoryCount(r, table, kSequenceCategory) == 0)
    {
        throw xsflib::FormatError("the AAF archive's sound table lists no sequences");
    }

    // The effects sequence is the first sequence: its index is 0.
    if ((r.U8(table + kSequenceFormat) & kGivesOffsetNumber) == 0)
    {
        return 0;
    }

    return r.U16(TableEntry(table, CategoryFirst(r, table, kSequenceCategory)) + *kAafItemLayout.file);
}

std::optional<std::vector<uint8_t>> AafEffectSlotCounts(std::span<const uint8_t> aaf)
{
    const xsflib::Reader r = AafReader(aaf);
    const Blocks blocks = FindBlocks(r);
    if (!blocks.scene_table)
    {
        return std::nullopt;
    }

    // Scene 0's row runs to scene 1's row, or to the table's end. A wide offset starts with its low word. The low word
    // holds the whole offset.
    const xsflib::Reader table(r.Sub(*blocks.scene_table, blocks.scene_table_size), r.BigEndian(), kAafReaderOptions);
    const std::size_t offset_bytes = r.BigEndian() ? 4 : 8;
    const uint32_t scenes = table.U32(0);
    if (scenes == 0)
    {
        throw xsflib::FormatError("the AAF archive's table of scenes lists no scenes");
    }

    const std::size_t row = table.U32(4);
    const std::size_t end = scenes > 1 ? table.U32(4 + offset_bytes) : table.Size();
    if (end < row || end > table.Size())
    {
        throw xsflib::FormatError("the AAF archive is damaged");
    }

    std::vector<uint8_t> counts;
    for (std::size_t at = row; at + 2 <= end; at += 2)
    {
        counts.push_back(table.U8(at));
    }

    return counts;
}

std::optional<std::vector<ArchiveFile>> ReadAafSequenceFiles(std::span<const uint8_t> aaf,
                                                             std::span<const uint8_t> archive)
{
    const xsflib::Reader r = AafReader(aaf);
    const std::optional<std::size_t> block = FindBlocks(r).sequence_index;
    if (!block)
    {
        return std::nullopt;
    }

    // No known little-endian archive has a sequence index. ReadAafSequenceFiles takes one in the GameCube's layout.
    return detail::ParseBarc(r, *block, detail::kBarc, archive, "the AAF archive's sequence index");
}

std::vector<WaveSystem> ReadAafWaveSystems(std::span<const uint8_t> aaf)
{
    const xsflib::Reader r = AafReader(aaf);
    std::vector<WaveSystem> systems;
    const std::size_t wave_count_at = r.BigEndian() ? kWaveCountAt : kWideWaveCountAt;
    for (const ListedBlock& block : FindBlocks(r).wave_systems)
    {
        r.Sub(block.offset, 1);
        systems.push_back(
            ReadWaveSystem(aaf.subspan(block.offset), static_cast<uint32_t>(systems.size()), wave_count_at));
    }

    return systems;
}

std::vector<Bank> ReadAafBanks(std::span<const uint8_t> aaf, std::vector<std::string>& warnings)
{
    const xsflib::Reader r = AafReader(aaf);
    const Blocks blocks = FindBlocks(r);
    const std::map<uint16_t, uint32_t> places = WaveSystemPlaces(r, blocks);
    std::vector<Bank> banks;
    for (const auto& [offset, wave_system] : blocks.banks)
    {
        try
        {
            r.Sub(offset, 1);
            Bank bank = Bank::Parse(aaf.subspan(offset), wave_system);
            NameSystems(bank, places);
            banks.push_back(std::move(bank));
        }
        catch (const xsflib::FormatError& e)
        {
            warnings.push_back("bank at offset " + std::to_string(offset) + ": " + e.what());
        }
    }

    return banks;
}

std::vector<std::string> AafWaveArchiveNames(std::span<const uint8_t> aaf)
{
    return ArchiveNames(aaf, false);
}

std::vector<std::string> AafStayingWaveArchiveNames(std::span<const uint8_t> aaf)
{
    return ArchiveNames(aaf, true);
}

std::vector<AsnSound> ReadAsnSounds(std::span<const uint8_t> asn, bool big_endian)
{
    constexpr std::size_t kHeaderBytes = 16;
    constexpr std::size_t kRecordBytes = 32;
    constexpr std::size_t kNameBytes = 28;
    constexpr std::size_t kCategories = 18;
    constexpr std::size_t kFirst = kHeaderBytes + kCategories * kRecordBytes;

    const xsflib::Reader r(asn, true);
    const xsflib::Reader ids(asn, big_endian);
    const uint32_t count = asn.size() >= kHeaderBytes ? r.U32(12) : 0;
    if (asn.size() < kFirst || (asn.size() - kFirst) / kRecordBytes < count)
    {
        throw xsflib::FormatError("the name file is too short for its sounds");
    }

    std::vector<AsnSound> sounds;
    for (std::size_t i = 0; i < count; i++)
    {
        const std::size_t record = kFirst + i * kRecordBytes;
        sounds.push_back({ids.U32(record + kNameBytes), r.CString(record, kNameBytes)});
    }

    return sounds;
}

} // namespace ntdlib::jaudio
