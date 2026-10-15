// SPDX-License-Identifier: MIT

#include "ntdlib/musyx/project.h"

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "xsflib/binary.h"

namespace ntdlib::musyx
{
namespace
{

// The end of a list of groups, of IDs, of pages or of MIDI setups.
constexpr uint32_t kEndOfGroups = 0xffffffff;
constexpr uint16_t kEndOfIds = 0xffff;
constexpr uint8_t kEndOfPages = 0xff;
constexpr uint16_t kEndOfSetups = 0xffff;

// Sizes of a group's header, a page, a channel's setup, a MIDI setup and an effect.
constexpr std::size_t kGroupHeader = 0x28;
constexpr std::size_t kPageSize = 6;
constexpr std::size_t kChannelSetupSize = 5;
constexpr std::size_t kMidiSetupSize = 4 + 16 * kChannelSetupSize;
constexpr std::size_t kEffectSize = 10;

// The bit of an ID-list entry that starts a range. The range runs from the entry's low 14 bits to the next entry.
constexpr uint16_t kRange = 0x8000;
constexpr uint16_t kRangeStart = 0x3fff;

// The kind bits of a keymap's and a layer's ID.
constexpr uint16_t kKeymapBit = 0x4000;
constexpr uint16_t kLayerBit = 0x8000;

// The most IDs a list can name: every 16-bit ID once.
constexpr std::size_t kMostIds = 0x10000;

// Reads the ID list at `offset`, adding `kind_bit` to each ID. A list ends with 0xffff. An entry with bit 15 set starts
// a range up to the next entry. No part of a group can start at offset 0. The first group's header is there. An offset
// of 0 gives no IDs.
std::vector<uint16_t> ReadIds(const xsflib::Reader& r, uint32_t offset, uint16_t kind_bit)
{
    std::vector<uint16_t> ids;
    for (std::size_t at = offset; at != 0 && r.U16(at) != kEndOfIds; at += 2)
    {
        const uint16_t entry = r.U16(at);
        if ((entry & kRange) == 0)
        {
            ids.push_back(static_cast<uint16_t>(entry | kind_bit));
            continue;
        }

        at += 2;
        const uint16_t last = r.U16(at);
        for (uint32_t id = entry & kRangeStart; id <= last; id++)
        {
            ids.push_back(static_cast<uint16_t>(id | kind_bit));
        }
        if (ids.size() > kMostIds)
        {
            throw xsflib::FormatError("project file has an ID list that never ends");
        }
    }

    return ids;
}

// Reads the pages at `offset`, up to the one whose program is 0xff. An offset of 0 gives none.
std::vector<Page> ReadPages(const xsflib::Reader& r, uint32_t offset)
{
    std::vector<Page> pages;
    for (std::size_t at = offset; at != 0 && r.U8(at + 4) != kEndOfPages; at += kPageSize)
    {
        pages.push_back({r.U16(at), r.U8(at + 2), r.U8(at + 3), r.U8(at + 4)});
    }

    return pages;
}

// Reads the MIDI setups at `offset`, up to the group's end at `end` or the setup whose song is 0xffff. An offset of 0
// gives none.
std::vector<MidiSetup> ReadMidiSetups(const xsflib::Reader& r, uint32_t offset, uint32_t end)
{
    std::vector<MidiSetup> setups;
    for (std::size_t at = offset; at != 0 && at + kMidiSetupSize <= end && r.U16(at) != kEndOfSetups;
         at += kMidiSetupSize)
    {
        MidiSetup& setup = setups.emplace_back();
        setup.song = r.U16(at);
        for (std::size_t c = 0; c < setup.channels.size(); c++)
        {
            const std::size_t channel = at + 4 + c * kChannelSetupSize;
            setup.channels[c] = {r.U8(channel), r.U8(channel + 1), r.U8(channel + 2), r.U8(channel + 3),
                                 r.U8(channel + 4)};
        }
    }

    return setups;
}

// Reads the effects table at `offset`: a 16-bit count, two bytes, then the effects. An offset of 0 gives none.
std::vector<Effect> ReadEffects(const xsflib::Reader& r, uint32_t offset)
{
    std::vector<Effect> effects;
    const uint16_t count = offset != 0 ? r.U16(offset) : 0;
    for (std::size_t i = 0; i < count; i++)
    {
        const std::size_t at = offset + 4 + i * kEffectSize;
        effects.push_back({r.U16(at), r.U16(at + 2), r.U8(at + 4), r.U8(at + 5), r.U8(at + 6), r.U8(at + 7),
                           r.U8(at + 8), r.U8(at + 9)});
    }

    return effects;
}

} // namespace

ObjectKind KindOf(uint16_t id)
{
    if ((id & kLayerBit) != 0)
    {
        return ObjectKind::kLayer;
    }
    if ((id & kKeymapBit) != 0)
    {
        return ObjectKind::kKeymap;
    }

    return ObjectKind::kMacro;
}

std::vector<Group> ReadProject(std::span<const uint8_t> file)
{
    const xsflib::Reader r(file, true);
    std::vector<Group> groups;
    std::set<uint32_t> seen;
    for (uint32_t at = 0; r.U32(at) != kEndOfGroups; at = r.U32(at))
    {
        // A group's header gives the offset of the next group, its ID and type, and the offsets of its lists, all from
        // the file's start.
        if (!seen.insert(at).second)
        {
            throw xsflib::FormatError("project file's groups run in a circle");
        }

        const uint32_t next = r.U32(at);
        r.Sub(at, kGroupHeader);
        Group& g = groups.emplace_back();
        g.id = r.U16(at + 4);
        const uint16_t type = r.U16(at + 6);
        if (type > static_cast<uint16_t>(GroupType::kEffects))
        {
            throw xsflib::FormatError("project file has a group of unknown type " + std::to_string(type));
        }

        g.type = static_cast<GroupType>(type);
        g.macros = ReadIds(r, r.U32(at + 8), 0);
        g.samples = ReadIds(r, r.U32(at + 0xc), 0);
        g.tables = ReadIds(r, r.U32(at + 0x10), 0);
        g.keymaps = ReadIds(r, r.U32(at + 0x14), kKeymapBit);
        g.layers = ReadIds(r, r.U32(at + 0x18), kLayerBit);

        // The rest of the header depends on the type: the pages, the drum pages and the MIDI setups of a song group, or
        // the effects table of an effects group.
        if (g.type == GroupType::kSongs)
        {
            g.pages = ReadPages(r, r.U32(at + 0x1c));
            g.drum_pages = ReadPages(r, r.U32(at + 0x20));
            g.midi_setups = ReadMidiSetups(r, r.U32(at + 0x24), next);
        }
        else
        {
            g.effects = ReadEffects(r, r.U32(at + 0x1c));
        }
    }

    return groups;
}

} // namespace ntdlib::musyx
