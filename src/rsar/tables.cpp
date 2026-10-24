// SPDX-License-Identifier: MIT

#include "ntdlib/rsar/tables.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/rsar/reference.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"

namespace ntdlib::rsar
{
namespace
{

// An entry of a table of the INFO block: its place in the archive, and the data type of its reference.
struct Entry
{
    std::size_t place = 0;
    uint8_t data_type = 0;
};

// The entries of the table whose reference lies at `table_ref`. `base` is where the INFO block's references count
// from. Throws xsflib::FormatError for a table that isn't there, or that runs past the end.
std::vector<Entry> EntriesOf(const xsflib::Reader& r, std::size_t base, std::size_t table_ref, const char* name)
{
    const Reference ref = ReadRef(r, table_ref);
    if (!ref.IsOffset())
    {
        throw xsflib::FormatError(std::string("the sound archive has no ") + name + " table");
    }

    const std::size_t table = base + ref.offset;
    const uint32_t count = r.U32(table);
    std::vector<Entry> entries;
    for (uint32_t i = 0; i < count; i++)
    {
        const Reference entry = ReadRef(r, table + 4 + i * std::size_t{8});
        entries.push_back({base + entry.offset, entry.data_type});
    }

    return entries;
}

// Whether `magic` lies at `offset`. Throws xsflib::FormatError when the bytes run past the end.
bool MagicAt(const xsflib::Reader& r, std::size_t offset, const char* magic)
{
    return std::memcmp(r.Sub(offset, 4).data(), magic, 4) == 0;
}

} // namespace

void WriteTableValues(std::span<uint8_t> archive, std::span<const SoundInfo> sounds,
                      std::span<const PlayerInfo> players)
{
    // The header gives the version and the INFO block, as SoundArchive reads them.
    const xsflib::Reader r(archive, true);
    if (!MagicAt(r, 0, "RSAR"))
    {
        throw xsflib::FormatError("not an RSAR file");
    }
    const uint16_t version = r.U16(6);
    const uint32_t info = r.U32(0x18);
    if (!MagicAt(r, info, "INFO"))
    {
        throw xsflib::FormatError("the sound archive has no INFO block");
    }
    const std::size_t base = info + std::size_t{8};
    const std::vector<Entry> sound_entries = EntriesOf(r, base, base, "sound");
    const std::vector<Entry> player_entries =
        players.empty() ? std::vector<Entry>() : EntriesOf(r, base, base + 0x10, "player");
    if (sounds.size() > sound_entries.size() || players.size() > player_entries.size())
    {
        throw std::invalid_argument("more sounds or players than the archive's tables hold");
    }

    // Each place is checked before the first write. Tables that run past the end then change nothing. A sound's kind
    // comes from its data type before version 1.1.
    std::vector<std::optional<std::size_t>> details(sounds.size());
    for (std::size_t i = 0; i < sounds.size(); i++)
    {
        const std::size_t p = sound_entries[i].place;
        r.Sub(p, version >= 0x0102 ? 0x2A : 0x28);
        const Reference detail = ReadRef(r, p + 0x18);
        const uint8_t type = version >= 0x0101 ? r.U8(p + 0x16) : sound_entries[i].data_type;
        if (type == 1 && detail.IsOffset())
        {
            details[i] = base + detail.offset;
            r.Sub(*details[i], version >= 0x0103 ? 0xE : 0xD);
        }
    }
    for (std::size_t i = 0; i < players.size(); i++)
    {
        r.Sub(player_entries[i].place, 5);
    }

    for (std::size_t i = 0; i < sounds.size(); i++)
    {
        const SoundInfo& sound = sounds[i];
        const std::size_t p = sound_entries[i].place;
        xsflib::StoreBe32(&archive[p + 8], sound.player_id);
        archive[p + 0x14] = sound.volume;
        archive[p + 0x15] = sound.player_priority;
        if (version >= 0x0102)
        {
            archive[p + 0x28] = sound.pan_mode;
            archive[p + 0x29] = sound.pan_curve;
        }
        if (!details[i])
        {
            continue;
        }

        const std::size_t q = *details[i];
        xsflib::StoreBe32(&archive[q + 4], sound.bank);
        xsflib::StoreBe32(&archive[q + 8], sound.allocate_track_flags);
        archive[q + 0xC] = sound.channel_priority;
        if (version >= 0x0103 && sound.release_priority_fix != (archive[q + 0xD] != 0))
        {
            archive[q + 0xD] = sound.release_priority_fix ? 1 : 0;
        }
    }
    for (std::size_t i = 0; i < players.size(); i++)
    {
        archive[player_entries[i].place + 4] = players[i].sound_count;
    }
}

} // namespace ntdlib::rsar
